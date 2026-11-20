#include "Audio/AudioTestUtilities.h"
#include "Core/ProfilerTestUtilities.h"
#include "Core/TelemetryTestUtilities.h"
#include "EditorLocationsTestUtilities.h"
#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/Automation/EditorCommands.h"
#include "Editor/Automation/PlayCommands.h"
#include "Editor/Automation/ProfilerCommands.h"
#include "Editor/Automation/TelemetryCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/EditorProfiler.h"
#include "Editor/EditorScripts.h"
#include "Editor/EditorTelemetry.h"
#include "Editor/PlayMode.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace Strada;

namespace
{
	// The editor model as the editor layer sets it up for an agent: every command of the editor, play mode's, telemetry's and
	// the profiler's, on one registry.
	struct AgentFixture
	{
		Testing::AssetManagerScope Assets;
		Testing::TemporaryDirectory Directory;
		EditorContext Context;
		EditorOperations Operations{Context};
		EditorScripts Scripts{Context, FileSystem::GetExecutableDirectory() / "Strada.ScriptCore.dll"};
		PlayMode Play{Context, Scripts};
		EditorTelemetry TelemetryOwner;
		EditorProfiler ProfilerOwner;
		CommandRegistry Registry;

		AgentFixture()
		{
			EditorCommandEnvironment environment;
			environment.Locations = Testing::MakeEditorLocations(Directory.GetPath());
			REQUIRE(RegisterEditorCommands(Registry, Operations, std::move(environment)).IsOk());
			REQUIRE(RegisterPlayCommands(Registry, Play, Context,
			                             []
			                             {
											 return glm::uvec2(640, 480);
										 })
			            .IsOk());
			REQUIRE(RegisterTelemetryCommands(Registry, TelemetryOwner, Context).IsOk());
			REQUIRE(RegisterProfilerCommands(Registry, ProfilerOwner, Context, {}).IsOk());
		}

		CommandResult Run(std::string_view name, Json params = Json::object())
		{
			std::optional<CommandResult> result;
			Registry.Execute(name, params,
			                 [&result](CommandResult value)
			                 {
								 result.emplace(std::move(value));
							 });
			REQUIRE(result.has_value());
			return std::move(*result);
		}

		Json Ok(std::string_view name, Json params = Json::object())
		{
			CommandResult result = Run(name, std::move(params));
			REQUIRE_MESSAGE(result.IsOk(), (result.IsError() ? result.GetError().Message : std::string()));
			return result.TakeValue();
		}

		// Frames of the game, each a frame of the profiler as the editor's loop makes it.
		void Advance(int frames)
		{
			for (int frame = 0; frame < frames; frame++)
			{
				Profiler::BeginFrame();
				Ok("play.advance", Json::object({{"frames", 1}}));
				Profiler::EndFrame();
			}
		}
	};
}

// M1's sixth step (slice 4's spec, section 0.1), as an agent takes it: told "make the bike shift later and lean faster", it
// edits the bike's definition with vehicle.set while the bike rides, then checks the ride with telemetry.read and
// profiler.capture. The Moto sample's lap test scene (Scenes/Lap.sscene: the main scene without its props) starts with the
// autopilot riding the bike from the grid (Tools/LapChecks.py); the rider assists shift for it by the definition's
// ShiftUpRPM, read at every step (Tools/Vehicle.py). A second into the ride the agent raises the shift point (with the red
// line above it, which the engine's speed never passes) and the lean spring (with its damping, within the roll inertia's
// bound, Docs/Architecture.md section 8). The first upshift, about 6 s from the grid, then comes at a road speed whose
// engine speed in first gear is above the old shift point (the ride read 42.3 m/s before the edit, 45.0 after it: 15,500 and
// 16,500 rpm); the riding vehicle takes the new spring in place, before its next step, and rides on upright. Whether the
// bike leans faster is not measured: its lean follows its target within about 0.08 s at either spring, and that target is
// the ground's impulse, which reads a few degrees where the turn balances at 35 (the feel pass's finding, Docs/Architecture.md
// section 15), so the test logs the lean the telemetry holds. The capture holds the physics' zones.
TEST_CASE("MotoAgent: vehicle.set while the autopilot rides makes the bike shift later and takes the lean spring in place, "
          "telemetry.read shows the next upshift above the old shift point, and profiler.capture holds the physics")
{
	Testing::AudioEngineScope audio;
	Testing::PhysicsSystemScope physics;
	Testing::ScriptRuntimeScope scripting;
	Testing::TelemetryScope telemetry;
	Testing::ProfilerScope profiler;
	AgentFixture fixture;
	std::filesystem::path const sample = FileSystem::PathFromUtf8(STRADA_SAMPLE_PROJECTS_DIR) / "Moto";
	std::filesystem::path const project = fixture.Directory.GetPath() / "Moto";
	REQUIRE(FileSystem::CopyDirectory(sample / "Assets", project / "Assets").IsOk());
	REQUIRE(FileSystem::Copy(sample / "Moto.sproj", project / "Moto.sproj", false).IsOk());
	REQUIRE(fixture.Operations.OpenProject(project / "Moto.sproj").IsOk());
	fixture.Ok("scene.open", Json::object({{"path", "Scenes/Lap.sscene"}}));

	constexpr std::string_view Definition = "asset://Vehicles/Moto.svehicle";
	Json const fields = fixture.Ok("vehicle.get", Json::object({{"vehicle", Definition}}))["fields"];
	float const oldShiftUp = fields["Transmission"]["ShiftUpRPM"].get<float>();
	float const oldRedLine = fields["Engine"]["MaxRPM"].get<float>();
	float const firstGear = fields["Transmission"]["GearRatios"][0].get<float>() * fields["Transmission"]["FinalDrive"].get<float>();
	float const rearRadius = fields["Wheels"][1]["Radius"].get<float>();
	// The raised shift point and red line, and the lean spring twice as stiff, its damping raised by as much as the roll
	// inertia's bound allows (9.82 kg m² at 60 steps a second: below 589).
	float const newShiftUp = oldShiftUp + 1000.0f;
	constexpr float NewLeanSpring = 10000.0f;
	constexpr float NewLeanDamping = 560.0f;

	fixture.Ok("telemetry.start");
	fixture.Ok("profiler.start");
	CHECK(fixture.Ok("play.start")["state"] == "play");
	fixture.Ok("play.pause");
	fixture.Advance(60);
	Entity const bike = fixture.Context.GetScene().FindEntityByName("Bike");
	REQUIRE(bike);
	Json const before = fixture.Ok("vehicle.state", Json::object({{"entity", bike.GetUUID().ToString()}}))["vehicles"][0];
	CAPTURE(before.dump());
	REQUIRE(before["gear"] == 1);
	REQUIRE(before["applied"]["throttle"].get<float>() > 0.9f);

	// The agent's edit, while the bike rides.
	fixture.Ok("vehicle.set", Json::object({{"vehicle", Definition},
	                                        {"fields",
	                                         {{"Engine", {{"MaxRPM", oldRedLine + 1000.0f}}},
	                                          {"Transmission", {{"ShiftUpRPM", newShiftUp}}},
	                                          {"Lean", {{"SpringConstant", NewLeanSpring}, {"SpringDamping", NewLeanDamping}}}}}}));
	// In place, before the next step: the riding vehicle has the new spring, and the ride goes on from where it was.
	fixture.Advance(1);
	PhysicsScene* const world = fixture.Context.GetScene().GetPhysicsScene();
	REQUIRE(world != nullptr);
	VehicleDesc const* const riding = world->FindVehicleSettings(bike.GetUUID());
	REQUIRE(riding != nullptr);
	CHECK(riding->LeanSpringConstant == NewLeanSpring);
	CHECK(riding->LeanSpringDamping == NewLeanDamping);
	Json const after = fixture.Ok("vehicle.state", Json::object({{"entity", bike.GetUUID().ToString()}}))["vehicles"][0];
	CHECK(after["forwardSpeed"].get<float>() > before["forwardSpeed"].get<float>());
	// On to 8 s from the grid: the first upshift comes at about 6.8 s after the edit (6.3 s without it).
	fixture.Advance(419);

	// The telemetry the ride recorded: the vehicle's channels at every step (Vehicle.<entity>.<quantity>), and the ride's own
	// (Scripts/Ride.sgraph records them while the editor records: M1's fifth step plots laps against Ride.Distance).
	std::string const prefix = "Vehicle." + std::to_string(bike.GetUUID().GetValue()) + ".";
	Json const read = fixture.Ok("telemetry.read",
	                             Json::object({{"channels", Json::array({prefix + "Gear", prefix + "ForwardSpeed", prefix + "EngineSpeed",
	                                                                     prefix + "LeanAngle", "Ride.Distance", "Ride.Speed"})},
	                                           {"maxSamples", 4096}}));
	CHECK(read["stride"] == 1);
	Json const& gears = read["channels"][0]["samples"];
	Json const& speeds = read["channels"][1]["samples"];
	Json const& engine = read["channels"][2]["samples"];
	REQUIRE(gears.size() >= 480);
	std::optional<size_t> upshift;
	for (size_t row = 1; row < gears.size() && !upshift; row++)
	{
		if (!gears[row].is_null() && !gears[row - 1].is_null() && gears[row - 1].get<float>() == 1.0f && gears[row].get<float>() == 2.0f)
		{
			upshift = row;
		}
	}
	REQUIRE(upshift.has_value());
	// The engine's speed that the road's speed gives in first gear, the step before the upshift (the assist shifts by it).
	float const roadSpeed = speeds[*upshift - 1].get<float>();
	float const roadEngineSpeed = roadSpeed / rearRadius * firstGear * 60.0f / (2.0f * std::numbers::pi_v<float>);
	MESSAGE("the first upshift at " << roadSpeed << " m/s: " << roadEngineSpeed << " rpm in first gear (the old shift point " << oldShiftUp
	                                << ", the new " << newShiftUp << "); the engine at " << engine[*upshift - 1].get<float>()
	                                << " rpm; the lean " << read["channels"][3]["summary"].dump());
	CHECK(roadEngineSpeed > oldShiftUp + 500.0f);
	CHECK(engine[*upshift - 1].get<float>() > oldShiftUp);
	// Upright through the ride: within the line's lean limit.
	CHECK(std::abs(read["channels"][3]["summary"]["minimum"].get<float>()) < 50.0f);
	CHECK(std::abs(read["channels"][3]["summary"]["maximum"].get<float>()) < 50.0f);
	// The ride's channels, of the same steps: 8 s from the grid, 9 m behind the start and finish line, the bike is a couple of
	// hundred meters down the main straight (the autopilot's line read 214 m), at its speed in km/h (a step apart at most:
	// the Ride graph and the vehicle's channels sample in their own order).
	float const along = read["channels"][4]["summary"]["last"].get<float>();
	CHECK(along > 100.0f);
	CHECK(along < 400.0f);
	CHECK(std::abs(read["channels"][5]["summary"]["last"].get<float>() - read["channels"][1]["summary"]["last"].get<float>() * 3.6f) <
	      2.0f);

	// The capture of the frames the profiler held: its medians' subsystems hold the physics' (Physics.Step and
	// Physics.Vehicles).
	Json const capture = fixture.Ok("profiler.capture", Json::object({{"path", "Captures/Ride.json"}}));
	CHECK(FileSystem::Exists(project / "Captures" / "Ride.json"));
	CHECK(capture["frames"].get<uint64_t>() > 0);
	bool physicsHeld = false;
	for (Json const& subsystem : capture["medians"]["subsystems"])
	{
		physicsHeld = physicsHeld || subsystem["name"] == "Physics";
	}
	CHECK(physicsHeld);

	CHECK(fixture.Ok("play.stop")["state"] == "edit");
	fixture.Ok("profiler.stop");
	fixture.Ok("telemetry.stop");
}
