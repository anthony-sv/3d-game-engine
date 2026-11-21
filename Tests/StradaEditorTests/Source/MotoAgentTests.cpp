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

	// Turn 1's turn-in as an agent reads it from the telemetry of a ride (a value per step: the vehicle's LeanAngle and the
	// Ride graph's Ride.Distance): the first step past TurnInFrom meters along the lap leaned TurnInLean degrees or more, and
	// the seconds from there to TurnLean degrees, the lean's roll over the turn-in (none when the ride reaches neither).
	constexpr float TurnInFrom = 500.0f;
	constexpr float TurnInTo = 700.0f;
	constexpr float TurnInLean = 2.0f;
	constexpr float TurnLean = 10.0f;

	std::optional<float> TurnInSeconds(Json const& lean, Json const& along, float stepSeconds)
	{
		std::optional<size_t> turnIn;
		for (size_t row = 0; row < lean.size() && row < along.size(); row++)
		{
			if (lean[row].is_null() || along[row].is_null())
			{
				continue;
			}
			float const distance = along[row].get<float>();
			float const angle = std::abs(lean[row].get<float>());
			if (!turnIn && distance > TurnInFrom && distance < TurnInTo && angle >= TurnInLean)
			{
				turnIn = row;
			}
			if (turnIn && angle >= TurnLean)
			{
				return static_cast<float>(row - *turnIn) * stepSeconds;
			}
		}
		return std::nullopt;
	}
}

// M1's sixth step (Docs/Roadmap.md, M1), as an agent takes it: told "make the bike shift later and lean faster", it edits
// the bike's definition with vehicle.set while the bike rides, then checks the ride with telemetry.read and
// profiler.capture. The Moto sample's lap test scene (Scenes/Lap.sscene: the main scene without its props) starts with the
// autopilot riding the bike from the grid (Tools/LapChecks.py); the rider assists shift for it by the definition's
// ShiftUpRPM, read at every step (Tools/Vehicle.py). The agent rides the scene twice through Turn 1's turn-in, each time
// editing the definition a second in: the first ride raises the shift point alone (with the red line above it, which the
// engine's speed never passes), the second the shift point and red line the same and the lean spring doubled, its damping
// left where it is (raised with the spring, towards the roll inertia's bound, it slows the lean: Docs/Architecture.md
// section 15, the feel pass). The two rides differ by the spring alone, so the speed the bike turns in at is the same in
// both and the quicker roll is the spring's. In the second, the first upshift, about 6 s from the grid, comes at a road
// speed whose engine speed in first gear is above the old shift point, with the engine past the old red line; the riding
// vehicle takes the new spring in place, before its next step; and at Turn 1's turn-in, about 16 s from the grid, the bike
// rolls from 2 to 10 degrees of lean in less time than in the first ride, on either side of RollSeconds (read 0.650 and
// 0.533 s in Release, 0.650 and 0.517 s in Debug). The capture holds the physics' zones.
TEST_CASE("MotoAgent: vehicle.set while the autopilot rides makes the bike shift later and lean faster, telemetry.read shows "
          "the next upshift above the old shift point and a quicker roll into Turn 1, and profiler.capture holds the physics")
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
	float const oldLeanSpring = fields["Lean"]["SpringConstant"].get<float>();
	float const leanDamping = fields["Lean"]["SpringDamping"].get<float>();
	float const firstGear = fields["Transmission"]["GearRatios"][0].get<float>() * fields["Transmission"]["FinalDrive"].get<float>();
	float const rearRadius = fields["Wheels"][1]["Radius"].get<float>();
	float const newShiftUp = oldShiftUp + 1000.0f;
	float const newLeanSpring = 2.0f * oldLeanSpring;
	// Each ride from the grid through Turn 1's turn-in (17.5 s), edited at EditFrame, and the roll's time halfway between the
	// two rides' readings.
	constexpr int RideFrames = 1050;
	constexpr int EditFrame = 60;
	constexpr float RollSeconds = 0.59f;
	Json const shiftLater = Json::object(
		{{"Engine", Json::object({{"MaxRPM", oldRedLine + 1000.0f}})}, {"Transmission", Json::object({{"ShiftUpRPM", newShiftUp}})}});
	fixture.Ok("telemetry.start");

	// The first ride, shifting later alone: Turn 1's turn-in from the vehicle's lean and the ride's distance along the lap.
	CHECK(fixture.Ok("play.start", Json::object({{"paused", true}}))["state"] == "play");
	fixture.Ok("play.advance", Json::object({{"frames", EditFrame}}));
	fixture.Ok("vehicle.set", Json::object({{"vehicle", Definition}, {"fields", shiftLater}}));
	fixture.Ok("play.advance", Json::object({{"frames", RideFrames - EditFrame}}));
	Entity const first = fixture.Context.GetScene().FindEntityByName("Bike");
	REQUIRE(first);
	std::string const leanChannel = "Vehicle." + std::to_string(first.GetUUID().GetValue()) + ".LeanAngle";
	Json const shifted =
		fixture.Ok("telemetry.read", Json::object({{"channels", Json::array({leanChannel, "Ride.Distance"})}, {"maxSamples", 4096}}));
	REQUIRE(shifted["stride"] == 1);
	std::optional<float> const shiftedRoll =
		TurnInSeconds(shifted["channels"][0]["samples"], shifted["channels"][1]["samples"], shifted["stepSeconds"].get<float>());
	REQUIRE(shiftedRoll.has_value());
	CHECK(fixture.Ok("play.stop")["state"] == "edit");
	// The definition as it was, so that the second ride starts as the first did.
	fixture.Ok("vehicle.set",
	           Json::object({{"vehicle", Definition},
	                         {"fields", {{"Engine", {{"MaxRPM", oldRedLine}}}, {"Transmission", {{"ShiftUpRPM", oldShiftUp}}}}}}));

	// The second ride, edited at the same frame: shifting later as the first, and leaning faster.
	fixture.Ok("profiler.start");
	CHECK(fixture.Ok("play.start", Json::object({{"paused", true}}))["state"] == "play");
	fixture.Advance(EditFrame);
	Entity const bike = fixture.Context.GetScene().FindEntityByName("Bike");
	REQUIRE(bike);
	Json const before = fixture.Ok("vehicle.state", Json::object({{"entity", bike.GetUUID().ToString()}}))["vehicles"][0];
	CAPTURE(before.dump());
	REQUIRE(before["gear"] == 1);
	REQUIRE(before["applied"]["throttle"].get<float>() > 0.9f);

	// The agent's edit, while the bike rides.
	Json both = shiftLater;
	both["Lean"] = Json::object({{"SpringConstant", newLeanSpring}});
	fixture.Ok("vehicle.set", Json::object({{"vehicle", Definition}, {"fields", both}}));
	// In place, before the next step: the riding vehicle has the new spring with its damping as it was, and the ride goes on
	// from where it was.
	fixture.Advance(1);
	PhysicsScene* const world = fixture.Context.GetScene().GetPhysicsScene();
	REQUIRE(world != nullptr);
	VehicleDesc const* const riding = world->FindVehicleSettings(bike.GetUUID());
	REQUIRE(riding != nullptr);
	CHECK(riding->LeanSpringConstant == newLeanSpring);
	CHECK(riding->LeanSpringDamping == leanDamping);
	Json const after = fixture.Ok("vehicle.state", Json::object({{"entity", bike.GetUUID().ToString()}}))["vehicles"][0];
	CHECK(after["forwardSpeed"].get<float>() > before["forwardSpeed"].get<float>());
	// On to 8 s from the grid frame by frame, for the profiler (the first upshift comes at about 6.8 s), then on through Turn
	// 1's turn-in.
	constexpr int ProfiledFrames = 480;
	fixture.Advance(ProfiledFrames - EditFrame - 1);
	fixture.Ok("play.advance", Json::object({{"frames", RideFrames - ProfiledFrames}}));

	// The telemetry the ride recorded, its own session: the vehicle's channels at every step (Vehicle.<entity>.<quantity>), and
	// the ride's own (Scripts/Ride.sgraph records them while the editor records: M1's fifth step plots laps against
	// Ride.Distance).
	std::string const prefix = "Vehicle." + std::to_string(bike.GetUUID().GetValue()) + ".";
	Json const read = fixture.Ok("telemetry.read",
	                             Json::object({{"channels", Json::array({prefix + "Gear", prefix + "ForwardSpeed", prefix + "EngineSpeed",
	                                                                     prefix + "LeanAngle", "Ride.Distance", "Ride.Speed"})},
	                                           {"maxSamples", 4096}}));
	CHECK(read["stride"] == 1);
	CHECK(read["session"] != shifted["session"]);
	Json const& gears = read["channels"][0]["samples"];
	Json const& speeds = read["channels"][1]["samples"];
	Json const& engine = read["channels"][2]["samples"];
	REQUIRE(gears.size() >= static_cast<size_t>(RideFrames));
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
	                                << ", the new " << newShiftUp << "); the engine at " << engine[*upshift - 1].get<float>() << " rpm");
	CHECK(roadEngineSpeed > oldShiftUp + 500.0f);
	// The engine at the new red line before the upshift (the rear tyre's drive slip holds the road's speed a little under
	// the shift point while the engine runs at its limit), which only the edit lets it reach.
	CHECK(engine[*upshift - 1].get<float>() > oldRedLine);

	// Leaning faster: from 2 to 10 degrees at Turn 1's turn-in in less time than the ride that only shifted later took, on
	// either side of the time between the two rides' readings.
	std::optional<float> const editedRoll =
		TurnInSeconds(read["channels"][3]["samples"], read["channels"][4]["samples"], read["stepSeconds"].get<float>());
	REQUIRE(editedRoll.has_value());
	MESSAGE("Turn 1's turn-in: from " << TurnInLean << " to " << TurnLean << " degrees in " << *shiftedRoll << " s with the lean spring at "
	                                  << oldLeanSpring << ", in " << *editedRoll << " s at " << newLeanSpring << " (its damping "
	                                  << leanDamping << " in both); the lean " << read["channels"][3]["summary"].dump());
	CHECK(*shiftedRoll > RollSeconds);
	CHECK(*editedRoll < RollSeconds);
	// Within the line's lean limit through the ride.
	CHECK(std::abs(read["channels"][3]["summary"]["minimum"].get<float>()) < 50.0f);
	CHECK(std::abs(read["channels"][3]["summary"]["maximum"].get<float>()) < 50.0f);
	// The ride's channels, of the same steps: the bike in Turn 1 (569 to 627 m from the start and finish line), at its speed in
	// km/h (a step apart at most: the Ride graph and the vehicle's channels sample in their own order).
	float const along = read["channels"][4]["summary"]["last"].get<float>();
	CHECK(along > TurnInFrom);
	CHECK(along < TurnInTo);
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
