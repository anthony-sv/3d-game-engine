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
// lap beside the next; their readings of the fixed steps are logged in a third line, compared as the others are. The scene's
// race director stays idle and its screens hidden (Tools/Checks.py's start switches), so nothing but these checks speaks
// for the race there.
//
// The flow test scene (Scenes/Flow.sscene, the main scene without its props too) runs the race director itself, so it is a
// scene of its own: the bike's ride again (the same graph's functions, Scripts/FlowTests.sgraph), and beside it, from the
// scene's first step, a graph of the race's checks (Tools/RaceChecks.py) sends a box through the race's trigger gates, the
// bike's top speed among the speeds, and the race director two scripted three-lap races and a third that the box's
// crossing of the line begins, then abandons; the director keeps its best lap in the game's save slot once the results show
// (never in a fixed step), with the samples of the scene's ghost (Scripts/Ghost.sgraph), and the checks report after the
// ride's. Its readings are logged in one line too ("Moto's race: ...", by about step 480), compared between the frame
// rates, and two short runs that keep their data (development runs, not test runs) show the slot the first saved loaded by
// the second, which gives the ghost its time and its samples, and the ghost rides them. Last come the HUD's and the pause's
// checks (Tools/HudChecks.py): a graph presses the Pause action in the middle of the ride's lean and reads the stopped game,
// the mix the race director's TogglePause muffles and the HUD against the bike (a text written over while the game stands
// is left as it is), and a fourth race shows the ghost's live delta and the split delta at a sector gate, and a fifth's
// countdown neither. Then the menus' checks (Tools/MenuChecks.py) press the menus as a player does: the title's RACE, a race
// to its results, the settings screen's buttons read back through Settings and given to the bike's riding graph, a binding
// that listens, the pad's selection, the pause screen, and a race whose best lap is kept when it is left by the pause
// screen's QUIT TO TITLE. The pause inserts frames without fixed steps, so the ride's and the race's readings and the state
// at the hash's step stay what they are.
//
// A ctest entry of its own (StradaRuntimeTests.Ride, Shards.cmake), which later rides of the sample join. The scenes have
// no props, so a start of the player on one costs less than the 25 s one on Moto's main scene takes on CI's macOS runners:
// the shard's starts stay well within its 200 s there.

namespace
{
	// The ride test scene's checks: the bike's seven (Tools/BikeChecks.py), the assists', the autopilot's and the rest in
	// gear's eight, the directors' nine, then the best lap's five.
	constexpr std::string_view RideChecks = "Tests passed: 29 checks";
	// The flow test scene's checks: the bike's seven, the race's nine, the HUD's and the pause's eight, the menus' seven.
	constexpr std::string_view FlowChecks = "Tests passed: 31 checks";
	// The lap test scene's checks (Tools/LapChecks.py): the laps' times, the line, the ghost, the lean.
	constexpr std::string_view LapChecks = "Tests passed: 4 checks";
	// The test scenes and their test timeouts (seconds of the game, Tools/Checks.py's SCENES): the ride scene's rides take
	// about 158 s, the flow scene's checks about 36 s. A Debug run of the ride scene's 9,500 frames on a slow runner can pass
	// RunTimeout's two minutes, so its runs have a process timeout of their own.
	constexpr std::string_view RideScene = "Scenes/Ride.sscene";
	constexpr std::string_view RideTimeout = "200";
	constexpr std::string_view FlowScene = "Scenes/Flow.sscene";
	constexpr std::string_view FlowTimeout = "60";
	constexpr std::chrono::seconds RideRunTimeout = std::chrono::minutes(5);
	// A fixed step of the ride after it was teleported into the gravel trap and back (by about step 1,650) and before it ends
	// (about step 1,870): the physics state there is the same at every frame rate.
	constexpr std::string_view HashStep = "1750";
	// The start of the line of the assists' and the autopilot's readings, and of the best lap's.
	constexpr std::string_view AssistReadings = "Moto's assists and autopilot: ";
	constexpr std::string_view GhostReadings = "Moto's ghost: ";

	// The line that begins with a prefix, to its end; empty without one.
	std::string_view LineFrom(std::string_view output, std::string_view prefix)
	{
		size_t const start = output.find(prefix);
		if (start == std::string_view::npos)
		{
			return {};
		}
		size_t const end = output.find_first_of("\r\n", start);
		return output.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
	}

	// The line in which the ride's graph logs every reading of a part of the ride it took ("Moto's ride: ..." for the bike's,
	// "Moto's assists and autopilot: ..." for the assists' and the autopilot's, "Moto's ghost: ..." for the best lap's,
	// "Moto's laps: ..." for the lap test scene's); empty without one.
	std::string_view RideReadings(std::string_view output, std::string_view part = "Moto's ride: ")
	{
		return LineFrom(output, part);
	}

	// The line in which the race checks' graph logs every reading it took ("Moto's race: ..."); empty without one.
	std::string_view RaceReadings(std::string_view output)
	{
		return LineFrom(output, "Moto's race: ");
	}

	// A reading of the race's line, from after its prefix to the next suffix ("the slot keeps " to " s, "); empty without one.
	std::string_view RaceReading(std::string_view readings, std::string_view prefix, std::string_view suffix)
	{
		size_t const start = readings.find(prefix);
		if (start == std::string_view::npos)
		{
			return {};
		}
		size_t const from = start + prefix.size();
		size_t const end = readings.find(suffix, from);
		return end == std::string_view::npos ? std::string_view() : readings.substr(from, end - from);
	}

	// The test summary's physics state ("physics state <hash> at step <step>"); empty without one.
	std::string_view PhysicsState(std::string_view output)
	{
		// "no physics state at step <step>" when the run ended before the step.
		std::string_view const summary = TestSummary(output);
		size_t const start = summary.find("; physics state ");
		return start == std::string_view::npos ? std::string_view() : summary.substr(start + 2);
	}

	// A test run of a test scene of a copy of Moto: at 1/60 s a frame, a test run's, or at a frame's time step.
	std::vector<std::string> SceneRun(std::filesystem::path const& project, std::string_view scene, std::string_view timeout,
	                                  char const* timestep = nullptr)
	{
		std::vector<std::string> arguments = {"--project", Native(project),      "--scene",     std::string(scene),   "--test",
		                                      "--timeout", std::string(timeout), "--hash-step", std::string(HashStep)};
		if (timestep != nullptr)
		{
			arguments.insert(arguments.end(), {"--timestep", timestep});
		}
		return arguments;
	}

	std::vector<std::string> RideRun(std::filesystem::path const& project, char const* timestep = nullptr)
	{
		return SceneRun(project, RideScene, RideTimeout, timestep);
	}

	std::vector<std::string> FlowRun(std::filesystem::path const& project, char const* timestep = nullptr)
	{
		return SceneRun(project, FlowScene, FlowTimeout, timestep);
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

TEST_CASE("Runtime: Moto's flow test scene rides the grid bike and plays the race, the HUD, the pause and the menus through "
          "its checks at 1/60 and 1/30 s a frame, with the same readings and physics state at both")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project = CopyRepositoryProject("Moto", directory.GetPath());
	Testing::IsolatedUserRoot const userRoot;

	ProcessResult const sixty = RunPlayer(FlowRun(project), userRoot);
	CAPTURE(sixty.Output);
	CHECK(sixty.ExitCode == 0);
	CHECK(Contains(sixty.Output, FlowChecks));
	REQUIRE_FALSE(RideReadings(sixty.Output).empty());
	REQUIRE_FALSE(PhysicsState(sixty.Output).empty());

	// Two fixed steps a frame: the ride's inputs change between them as they do between frames at 1/60 s.
	ProcessResult const thirty = RunPlayer(FlowRun(project, "0.0333333"), userRoot);
	CAPTURE(thirty.Output);
	CHECK(thirty.ExitCode == 0);
	CHECK(Contains(thirty.Output, FlowChecks));
	CHECK(RideReadings(thirty.Output) == RideReadings(sixty.Output));
	CHECK(PhysicsState(thirty.Output) == PhysicsState(sixty.Output));
	CHECK(Contains(PhysicsState(sixty.Output), std::string("at step ") + std::string(HashStep)));
	// The race's readings: its gates' crossings and its laps, in steps and fractions of steps, the same at both rates.
	REQUIRE_FALSE(RaceReadings(sixty.Output).empty());
	CHECK(RaceReadings(thirty.Output) == RaceReadings(sixty.Output));
}

TEST_CASE("Runtime: Moto's flow test scene plays its checks at 1/144 s a frame too, with the readings and physics state it "
          "has at 1/60" *
          doctest::skip(!Platform::ReadEnvironmentVariable("STRADA_LONG_RIDES")))
{
	constexpr std::chrono::seconds LongRunTimeout = std::chrono::minutes(10);
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project = CopyRepositoryProject("Moto", directory.GetPath());
	Testing::IsolatedUserRoot const userRoot;

	ProcessResult const sixty = RunPlayer(FlowRun(project), userRoot, LongRunTimeout);
	CAPTURE(sixty.Output);
	CHECK(sixty.ExitCode == 0);
	ProcessResult const fast = RunPlayer(FlowRun(project, "0.00694444"), userRoot, LongRunTimeout);
	CAPTURE(fast.Output);
	CHECK(fast.ExitCode == 0);
	CHECK(Contains(fast.Output, FlowChecks));
	REQUIRE_FALSE(RideReadings(sixty.Output).empty());
	CHECK(RideReadings(fast.Output) == RideReadings(sixty.Output));
	REQUIRE_FALSE(PhysicsState(sixty.Output).empty());
	CHECK(PhysicsState(fast.Output) == PhysicsState(sixty.Output));
	REQUIRE_FALSE(RaceReadings(sixty.Output).empty());
	CHECK(RaceReadings(fast.Output) == RaceReadings(sixty.Output));
}

TEST_CASE("Runtime: Moto's race keeps its best lap in the game's save slot, which the next run loads and gives the ghost")
{
	// Development runs of the flow test scene, which keep their game data in the project's folder (a test run's is
	// temporary), each 560 frames at 1/60 s, past the race's readings: the first finds no best lap and saves the scripted
	// races' best once their results show, the second race's with the samples the scene's ghost was given; the second run
	// loads it as the scene starts, gives the ghost its samples and its time and sends it BestLapLoaded, the ghost takes
	// them and rides them (the line's "the ghost riding it True", read before the first race), and its races beat nothing,
	// so the slot keeps the first run's and nothing is saved again. A run without a window that is not a test run is paced
	// at 60 frames a second, so the runs stop well before the ride's end.
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project = CopyRepositoryProject("Moto", directory.GetPath());
	Testing::IsolatedUserRoot const userRoot;
	std::vector<std::string> const arguments = {
		"--project", Native(project), "--scene", std::string(FlowScene), "--headless", "--timestep", "0.0166666667", "--frames", "560"};

	ProcessResult const first = RunPlayer(arguments, userRoot);
	CAPTURE(first.Output);
	CHECK(first.ExitCode == 0);
	std::string_view const saving = RaceReadings(first.Output);
	REQUIRE_FALSE(saving.empty());
	CHECK(Contains(saving, "loaded at start: 0 s, the ghost riding it False with a best lap of 0 s; the best lap saved 2 times, "
	                       "in a finish's step False"));
	std::string const saved(RaceReading(saving, "the slot keeps ", " s, "));
	REQUIRE_FALSE(saved.empty());
	CHECK(saved != "-1");
	CHECK(Contains(saving, "], 850 samples; a real crossing"));
	CHECK(std::filesystem::is_regular_file(project.parent_path() / ".strada" / "PlayData" / "Saves" / "bestlap.ssave"));

	ProcessResult const second = RunPlayer(arguments, userRoot);
	CAPTURE(second.Output);
	CHECK(second.ExitCode == 0);
	std::string_view const loading = RaceReadings(second.Output);
	REQUIRE_FALSE(loading.empty());
	CHECK(RaceReading(loading, "loaded at start: ", " s, ") == saved);
	// The ghost's graph took the samples and rides them (its RidesBestLap), with the lap's time the director gave it.
	CHECK(Contains(loading, "the ghost riding it True"));
	CHECK(RaceReading(loading, "with a best lap of ", " s; ") == saved);
	CHECK(Contains(loading, "the best lap saved 0 times"));
	CHECK(RaceReading(loading, "the slot keeps ", " s, ") == saved);
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
