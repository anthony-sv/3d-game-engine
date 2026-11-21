#include "EditorLocationsTestUtilities.h"
#include "Physics/PhysicsTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/Automation/EditorCommands.h"
#include "Editor/Commands/EntityCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorScripts.h"
#include "Editor/PlayMode.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Core/UUID.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace Strada;

// Live edits of a vehicle definition's values that track EN adds (Docs/Architecture.md §8, "The engine in neutral"), beside
// DataAssetCommandsTests' live edit of a torque curve and a shift point.
namespace
{
	// An editor model with every command of the editor registered and a project open.
	struct VehicleEditFixture
	{
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		CommandRegistry Registry;

		VehicleEditFixture()
		{
			EditorCommandEnvironment environment;
			environment.Locations = Testing::MakeEditorLocations(Directory.GetPath());
			REQUIRE(RegisterEditorCommands(Registry, Operations, std::move(environment)).IsOk());
			REQUIRE(Operations.CreateProject(Directory.GetPath() / "Game", "Game", Scene("Main")).IsOk());
		}

		Json Ok(std::string_view name, Json params = Json::object())
		{
			std::optional<CommandResult> result;
			Registry.Execute(name, params,
			                 [&result](CommandResult value)
			                 {
								 result.emplace(std::move(value));
							 });
			REQUIRE(result.has_value());
			REQUIRE_MESSAGE(result->IsOk(), (result->IsError() ? result->GetError().Message : std::string()));
			return result->TakeValue();
		}

		UUID Create(std::string name, Json components)
		{
			EntityCreateInfo info;
			info.Name = std::move(name);
			info.Components = std::move(components);
			Result<UUID> entity = Operations.CreateEntity(info);
			REQUIRE(entity.IsOk());
			return entity.GetValue();
		}
	};
}

TEST_CASE("VehicleLiveEdit: vehicle.set of the engine's NeutralDamping while a bike plays takes at its next step, the ride and the "
          "structure kept, and undo takes it back")
{
	Testing::PhysicsSystemScope physics;
	VehicleEditFixture fixture;
	EditorScripts scripts{fixture.Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
	PlayMode play{fixture.Context, scripts};
	// A Manual bike waiting in neutral, its engine a superbike's (idle at 1,700 rpm, the red line at 16,000, a light crank)
	// with Moto's engine damping and no neutral damping.
	std::string const definition =
		fixture
			.Ok("vehicle.create",
	            Json::object({{"path", "Vehicles/Bike.svehicle"},
	                          {"fields",
	                           {{"Engine", {{"MinRPM", 1700}, {"MaxRPM", 16000}, {"Inertia", 0.05}, {"AngularDamping", 0.005}}},
	                            {"Transmission", {{"Mode", "Manual"}}},
	                            {"Lean", {{"SpringDamping", 800}}}}}}))["vehicle"]["id"]
			.get<std::string>();
	fixture.Create("Ground", Json::object({{"Transform", {{"Translation", {0.0, -0.5, 0.0}}}},
	                                       {"BoxCollider", {{"HalfExtents", {400.0, 0.5, 400.0}}}}}));
	// A chassis box whose roll inertia carries the lean spring's damping (26 kg m²).
	UUID const bike =
		fixture.Create("Bike", Json::object({{"Transform", {{"Translation", {0.0, 0.05, 0.0}}}},
	                                         {"RigidBody", {{"Type", "Dynamic"}, {"Mass", 250.0}, {"AllowSleep", false}}},
	                                         {"BoxCollider", {{"HalfExtents", {0.25, 0.5, 0.6}}, {"Offset", {0.0, 0.75, 0.0}}}},
	                                         {"Vehicle", {{"Definition", definition}}}}));

	REQUIRE(play.Start(EditorPlayState::Simulate, 640, 480).IsOk());
	Scene& scene = fixture.Context.GetScene();
	scene.ApplyPhysicsChanges();
	PhysicsScene* const world = scene.GetPhysicsScene();
	REQUIRE(world != nullptr);
	REQUIRE(world->HasVehicle(bike));
	auto const rpm = [world, bike]
	{
		std::optional<VehicleState> const state = world->GetVehicleState(bike);
		REQUIRE(state);
		return state->EngineSpeed;
	};
	auto const frame = [&play]
	{
		REQUIRE(play.Advance(1, Timestep(1.0f / 60.0f)) == 1);
	};
	// Revved to the red line in neutral, both brakes held, then the throttle closed: the engine's own damping alone.
	VehicleInputs inputs;
	inputs.Throttle = 1.0f;
	inputs.FrontBrake = 1.0f;
	inputs.RearBrake = 1.0f;
	world->SetVehicleInputs(bike, inputs);
	for (int frames = 0; frames < 600 && rpm() < 16000.0f; frames++)
	{
		frame();
	}
	REQUIRE(rpm() == 16000.0f);
	inputs.Throttle = 0.0f;
	world->SetVehicleInputs(bike, inputs);
	float const closed = rpm();
	frame();
	float const unedited = rpm();
	CHECK(unedited == doctest::Approx(closed * (1.0f - 0.005f / 60.0f)).epsilon(1e-6));

	// The neutral damping set while it plays: the next step takes it, in place.
	uint64_t const structure = world->GetStructureVersion();
	fixture.Ok("vehicle.set", Json::object({{"vehicle", definition}, {"fields", {{"Engine", {{"NeutralDamping", 2.245}}}}}}));
	frame();
	float const edited = rpm();
	CHECK(edited == doctest::Approx(unedited * (1.0f - (0.005f + 2.245f) / 60.0f)).epsilon(1e-6));
	REQUIRE(world->FindVehicleSettings(bike) != nullptr);
	CHECK(world->FindVehicleSettings(bike)->EngineNeutralDamping == 2.245f);
	CHECK(world->GetStructureVersion() == structure);
	CHECK(world->HasVehicle(bike));
	// It falls on to idle in about a second at that rate.
	int frames = 1;
	while (frames < 120 && rpm() > 1.01f * 1700.0f)
	{
		frame();
		frames++;
	}
	CHECK(frames >= 54);
	CHECK(frames <= 63);

	// Undone, the next steps are without it again: revved again, the closed throttle takes its own damping alone.
	CHECK(fixture.Ok("editor.undo")["undone"] == "Edit Vehicle");
	inputs.Throttle = 1.0f;
	world->SetVehicleInputs(bike, inputs);
	for (int revving = 0; revving < 600 && rpm() < 16000.0f; revving++)
	{
		frame();
	}
	REQUIRE(rpm() == 16000.0f);
	inputs.Throttle = 0.0f;
	world->SetVehicleInputs(bike, inputs);
	frame();
	CHECK(rpm() == doctest::Approx(16000.0f * (1.0f - 0.005f / 60.0f)).epsilon(1e-6));
	CHECK(world->FindVehicleSettings(bike)->EngineNeutralDamping == 0.0f);
	CHECK(world->GetStructureVersion() == structure);
	play.Stop();
}

TEST_CASE("VehicleLiveEdit: a bike whose definition vehicle.set edits and that is reset with no step between is made again with the "
          "new values")
{
	Testing::PhysicsSystemScope physics;
	VehicleEditFixture fixture;
	EditorScripts scripts{fixture.Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
	PlayMode play{fixture.Context, scripts};
	std::string const definition = fixture
	                                   .Ok("vehicle.create", Json::object({{"path", "Vehicles/Bike.svehicle"},
	                                                                       {"fields",
	                                                                        {{"Engine", {{"MinRPM", 1700}, {"MaxRPM", 16000}}},
	                                                                         {"Transmission", {{"Mode", "Manual"}}},
	                                                                         {"Lean", {{"SpringDamping", 800}}}}}}))["vehicle"]["id"]
	                                   .get<std::string>();
	fixture.Create("Ground", Json::object({{"Transform", {{"Translation", {0.0, -0.5, 0.0}}}},
	                                       {"BoxCollider", {{"HalfExtents", {400.0, 0.5, 400.0}}}}}));
	UUID const bike =
		fixture.Create("Bike", Json::object({{"Transform", {{"Translation", {0.0, 0.05, 0.0}}}},
	                                         {"RigidBody", {{"Type", "Dynamic"}, {"Mass", 250.0}, {"AllowSleep", false}}},
	                                         {"BoxCollider", {{"HalfExtents", {0.25, 0.5, 0.6}}, {"Offset", {0.0, 0.75, 0.0}}}},
	                                         {"Vehicle", {{"Definition", definition}}}}));

	REQUIRE(play.Start(EditorPlayState::Simulate, 640, 480).IsOk());
	Scene& scene = fixture.Context.GetScene();
	scene.ApplyPhysicsChanges();
	PhysicsScene* const world = scene.GetPhysicsScene();
	REQUIRE(world != nullptr);
	REQUIRE(world->HasVehicle(bike));
	auto const rpm = [world, bike]
	{
		std::optional<VehicleState> const state = world->GetVehicleState(bike);
		REQUIRE(state);
		return state->EngineSpeed;
	};
	// Idling in neutral, at the definition's 1,700 rpm.
	REQUIRE(play.Advance(30, Timestep(1.0f / 60.0f)) == 30);
	REQUIRE(rpm() == 1700.0f);
	uint64_t const structure = world->GetStructureVersion();

	// The idle lowered, then the bike reset before any step: it is made again with the new idle, as a bike made there with
	// it, and not with the old one, which the new range would keep.
	fixture.Ok("vehicle.set", Json::object({{"vehicle", definition}, {"fields", {{"Engine", {{"MinRPM", 1200}}}}}}));
	REQUIRE(scene.ResetVehicle(scene.GetEntityByUUID(bike)));
	REQUIRE(world->FindVehicleSettings(bike) != nullptr);
	CHECK(world->FindVehicleSettings(bike)->MinRPM == 1200.0f);
	CHECK(rpm() == 1200.0f);
	CHECK(world->GetStructureVersion() == structure);
	REQUIRE(play.Advance(1, Timestep(1.0f / 60.0f)) == 1);
	CHECK(rpm() == 1200.0f);
	play.Stop();
}
