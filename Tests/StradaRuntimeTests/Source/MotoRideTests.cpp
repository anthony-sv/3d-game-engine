#include "RuntimeTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Core/Platform.h"
#include "Strada/Platform/Process.h"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;
using namespace Strada::RuntimeTesting;

// The Moto sample's ridden bike, played by the player in its ride test scene (Scenes/Ride.sscene: the main scene without
// its props). The scene's graph (Scripts/RideTests.sgraph, written by Tools/BikeChecks.py) rides the grid bike from its
// Event.FixedUpdate as a function of its count of fixed steps and of what the bike reads, never of a frame: a launch, two
// stops on one brake each, a coast on the asphalt and one in the gravel trap outside Turn 6, the shifts and a lean to the
// left; its checks are bands drawn from the figures the bike's definition gives (Tools/Vehicle.py). Then, with the rider
// assists on (Tools/RideChecks.py), the throttle alone shifts it out of neutral, the front lever with the linked brakes stops
// it, a lean request turns it, the autopilot (Scripts/Ride.sgraph) rides it from the grid through the corners after Turn 2,
// and braked to a stop on the assists it waits in first gear where it stopped; the rider's pose is read as the bike rides. So the ride is
// the same ride at every frame rate, step for step: its readings, which the graph logs in two lines, and the physics state at a step of it
// are compared exactly between the rates. Meanwhile the directors' checks (Tools/DirectorChecks.py) read every frame what the camera and
// audio directors set: the attract mode's cuts by the camera director's rule and the race's chase view (the test stands in for the race
// director's AttractStarted and RaceStarted), the listener on the camera that shows, and the ridden bike's engine voices, squeal, shift
// blips, gravel crunch and wind, the crowd and the voices mixed; those readings are of frames, so no line compared here holds
// them, and their checks pass at every rate. Then the best lap's checks (Tools/GhostChecks.py) stand in for the race
// director's LapStarted and LapFinished: the autopilot rides short laps from the grid, the ghost (Scripts/Ghost.sgraph) keeps
// the faster, refuses a lap too long for its samples, sets its Delta against the best at the same place and replays the best
// lap beside the next; their readings of the fixed steps are logged in a third line, compared as the others are.
// A ctest entry of its own (StradaRuntimeTests.Ride, Shards.cmake), which later rides of the sample join. The scene has no
// props, so a start of the player on it costs less than the 25 s one on Moto's main scene takes on CI's macOS runners: the
// shard's two starts stay well within its 200 s there.

namespace
{
	// The ride test scene's checks: the bike's seven (Tools/BikeChecks.py), the assists', the autopilot's and the rest in
	// gear's eight, the directors' nine, then the best lap's five.
	constexpr std::string_view RideChecks = "Tests passed: 29 checks";
	// The lap test scene's checks (Tools/LapChecks.py): the laps' times, the line, the ghost, the lean.
	constexpr std::string_view LapChecks = "Tests passed: 4 checks";
	// The ride test scene's test timeout (seconds of the game, Tools/Checks.py's SCENES): the rides take about 158 s. A Debug
	// run of its 9,500 frames on a slow runner can pass RunTimeout's two minutes, so it has a process timeout of its own.
	constexpr std::string_view RideTimeout = "200";
	constexpr std::chrono::seconds RideRunTimeout = std::chrono::minutes(5);
	// A fixed step of the ride after it was teleported into the gravel trap and back (by about step 1,650) and before it ends
	// (about step 1,870): the physics state there is the same at every frame rate.
	constexpr std::string_view HashStep = "1750";
	// The start of the line of the assists' and the autopilot's readings, and of the best lap's.
	constexpr std::string_view AssistReadings = "Moto's assists and autopilot: ";
	constexpr std::string_view GhostReadings = "Moto's ghost: ";

	// The line in which the ride's graph logs every reading of a part of the ride it took ("Moto's ride: ..." for the bike's,
	// "Moto's assists and autopilot: ..." for the assists' and the autopilot's, "Moto's ghost: ..." for the best lap's,
	// "Moto's laps: ..." for the lap test scene's); empty without one.
	std::string_view RideReadings(std::string_view output, std::string_view part = "Moto's ride: ")
	{
		size_t const start = output.find(part);
		if (start == std::string_view::npos)
		{
			return {};
		}
		size_t const end = output.find_first_of("\r\n", start);
		return output.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
	}

	// The test summary's physics state ("physics state <hash> at step <step>"); empty without one.
	std::string_view PhysicsState(std::string_view output)
	{
		// "no physics state at step <step>" when the run ended before the step.
		std::string_view const summary = TestSummary(output);
		size_t const start = summary.find("; physics state ");
		return start == std::string_view::npos ? std::string_view() : summary.substr(start + 2);
	}

	// A test run of the ride test scene of a copy of Moto: at 1/60 s a frame, a test run's, or at a frame's time step.
	std::vector<std::string> RideRun(std::filesystem::path const& project, char const* timestep = nullptr)
	{
		std::vector<std::string> arguments = {"--project", Native(project),          "--scene",     "Scenes/Ride.sscene", "--test",
		                                      "--timeout", std::string(RideTimeout), "--hash-step", std::string(HashStep)};
		if (timestep != nullptr)
		{
			arguments.insert(arguments.end(), {"--timestep", timestep});
		}
		return arguments;
	}
}

TEST_CASE("Runtime: Moto's ride test scene rides the grid bike through its checks at 1/60 and 1/30 s a frame, with the same "
          "readings and physics state at both")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project = CopyRepositoryProject("Moto", directory.GetPath());
	Testing::IsolatedUserRoot const userRoot;

	ProcessResult const sixty = RunPlayer(RideRun(project), userRoot, RideRunTimeout);
	CAPTURE(sixty.Output);
	CHECK(sixty.ExitCode == 0);
	CHECK(Contains(sixty.Output, RideChecks));
	REQUIRE_FALSE(RideReadings(sixty.Output).empty());
	REQUIRE_FALSE(RideReadings(sixty.Output, AssistReadings).empty());
	REQUIRE_FALSE(RideReadings(sixty.Output, GhostReadings).empty());
	REQUIRE_FALSE(PhysicsState(sixty.Output).empty());

	// Two fixed steps a frame: the ride's inputs change between them as they do between frames at 1/60 s.
	ProcessResult const thirty = RunPlayer(RideRun(project, "0.0333333"), userRoot, RideRunTimeout);
	CAPTURE(thirty.Output);
	CHECK(thirty.ExitCode == 0);
	CHECK(Contains(thirty.Output, RideChecks));
	CHECK(RideReadings(thirty.Output) == RideReadings(sixty.Output));
	CHECK(RideReadings(thirty.Output, AssistReadings) == RideReadings(sixty.Output, AssistReadings));
	CHECK(RideReadings(thirty.Output, GhostReadings) == RideReadings(sixty.Output, GhostReadings));
	CHECK(PhysicsState(thirty.Output) == PhysicsState(sixty.Output));
	CHECK(Contains(PhysicsState(sixty.Output), std::string("at step ") + std::string(HashStep)));
}

TEST_CASE("Runtime: Moto's ride test scene rides the grid bike through its checks at 1/144 s a frame too, with the readings "
          "and physics state it has at 1/60" *
          doctest::skip(!Platform::ReadEnvironmentVariable("STRADA_LONG_RIDES")))
{
	// A frame at 1/144 s runs a fixed step only now and then: the ride's frames, and the run's time, are 2.4 times those at
	// 1/60 s, which is why the run is not on CI and has a longer timeout of its own.
	constexpr std::chrono::seconds LongRunTimeout = std::chrono::minutes(10);
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project = CopyRepositoryProject("Moto", directory.GetPath());
	Testing::IsolatedUserRoot const userRoot;

	ProcessResult const sixty = RunPlayer(RideRun(project), userRoot, LongRunTimeout);
	CAPTURE(sixty.Output);
	CHECK(sixty.ExitCode == 0);
	ProcessResult const fast = RunPlayer(RideRun(project, "0.00694444"), userRoot, LongRunTimeout);
	CAPTURE(fast.Output);
	CHECK(fast.ExitCode == 0);
	CHECK(Contains(fast.Output, RideChecks));
	REQUIRE_FALSE(RideReadings(sixty.Output).empty());
	CHECK(RideReadings(fast.Output) == RideReadings(sixty.Output));
	REQUIRE_FALSE(RideReadings(sixty.Output, AssistReadings).empty());
	CHECK(RideReadings(fast.Output, AssistReadings) == RideReadings(sixty.Output, AssistReadings));
	REQUIRE_FALSE(RideReadings(sixty.Output, GhostReadings).empty());
	CHECK(RideReadings(fast.Output, GhostReadings) == RideReadings(sixty.Output, GhostReadings));
	REQUIRE_FALSE(PhysicsState(sixty.Output).empty());
	CHECK(PhysicsState(fast.Output) == PhysicsState(sixty.Output));
}

TEST_CASE("Runtime: Moto's lap test scene rides two whole laps on the autopilot within the band of the racing line's lap, "
          "and the ghost keeps the faster" *
          doctest::skip(!Platform::ReadEnvironmentVariable("STRADA_LONG_RIDES")))
{
	// The scene (Scenes/Lap.sscene, Tools/LapChecks.py) is the main scene without its props: the autopilot rides from the grid
	// as it starts, two laps of about 80 s, while its graph stands in for the race director's lap events; a run of 10,000
	// frames and more, which is why it is not on CI and has a timeout of its own.
	constexpr std::chrono::seconds LongRunTimeout = std::chrono::minutes(10);
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project = CopyRepositoryProject("Moto", directory.GetPath());
	Testing::IsolatedUserRoot const userRoot;

	ProcessResult const laps =
		RunPlayer({"--project", Native(project), "--scene", "Scenes/Lap.sscene", "--test", "--timeout", "240"}, userRoot, LongRunTimeout);
	CAPTURE(laps.Output);
	CHECK(laps.ExitCode == 0);
	CHECK(Contains(laps.Output, LapChecks));
	CHECK_FALSE(RideReadings(laps.Output, "Moto's laps: ").empty());
}
