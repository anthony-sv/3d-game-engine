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
// left; its checks are bands drawn from the figures the bike's definition gives (Tools/Vehicle.py). So the ride is the same
// ride at every frame rate, step for step: its readings, which the graph logs in one line, and the physics state at a step
// of it are compared exactly between the rates. A ctest entry of its own (StradaRuntimeTests.Ride, Shards.cmake), which
// later rides of the sample join. The scene has no props, so a start of the player on it costs less than the 25 s one on
// Moto's main scene takes on CI's macOS runners: the shard's starts stay well within its 200 s there.
//
// Beside the ride, from the scene's first step, a graph of the race's checks (Tools/RaceChecks.py) sends a box through the
// race's trigger gates, the bike's top speed among the speeds, and the race director two scripted three-lap races and a
// third that the box's crossing of the line begins, then abandons; the director keeps its best lap in the game's save
// slot, and the checks report after the ride's. Its readings are logged in one line too ("Moto's race: ...", by about step
// 480), compared between the frame rates, and two short runs that keep their data (development runs, not test runs) show
// the slot the first saved loaded by the second. Last come the HUD's and the pause's checks (Tools/HudChecks.py): a graph
// presses the Pause action in the middle of the ride's lean and reads the stopped game and the HUD against the bike, and
// a fourth race shows the ghost's live delta and the split delta at a sector gate, and a fifth's countdown neither. Then the
// menus' checks (Tools/MenuChecks.py) press the menus as a player does: the title's RACE, a race to its results, the
// settings screen's buttons read back through Settings, a binding that listens, the pad's selection and the pause screen.
// The pause inserts frames without fixed steps, so the ride's and the race's readings and the state at the hash's step stay
// what they are.

namespace
{
	// The ride test scene's checks.
	constexpr std::string_view RideChecks = "Tests passed: 29 checks";
	// A fixed step of the ride after it was teleported into the gravel trap and back (by about step 1,650) and before it ends
	// (about step 1,870): the physics state there is the same at every frame rate.
	constexpr std::string_view HashStep = "1750";

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

	// The line in which the ride's graph logs every reading it took ("Moto's ride: ..."); empty without one.
	std::string_view RideReadings(std::string_view output)
	{
		return LineFrom(output, "Moto's ride: ");
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

	// A test run of the ride test scene of a copy of Moto: at 1/60 s a frame, a test run's, or at a frame's time step.
	std::vector<std::string> RideRun(std::filesystem::path const& project, char const* timestep = nullptr)
	{
		std::vector<std::string> arguments = {
			"--project",   Native(project),      "--scene", "Scenes/Ride.sscene", "--test", "--timeout", "60",
			"--hash-step", std::string(HashStep)};
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

	ProcessResult const sixty = RunPlayer(RideRun(project));
	CAPTURE(sixty.Output);
	CHECK(sixty.ExitCode == 0);
	CHECK(Contains(sixty.Output, RideChecks));
	REQUIRE_FALSE(RideReadings(sixty.Output).empty());
	REQUIRE_FALSE(PhysicsState(sixty.Output).empty());

	// Two fixed steps a frame: the ride's inputs change between them as they do between frames at 1/60 s.
	ProcessResult const thirty = RunPlayer(RideRun(project, "0.0333333"));
	CAPTURE(thirty.Output);
	CHECK(thirty.ExitCode == 0);
	CHECK(Contains(thirty.Output, RideChecks));
	CHECK(RideReadings(thirty.Output) == RideReadings(sixty.Output));
	CHECK(PhysicsState(thirty.Output) == PhysicsState(sixty.Output));
	CHECK(Contains(PhysicsState(sixty.Output), std::string("at step ") + std::string(HashStep)));
	// The race's readings: its gates' crossings and its laps, in steps and fractions of steps, the same at both rates.
	REQUIRE_FALSE(RaceReadings(sixty.Output).empty());
	CHECK(RaceReadings(thirty.Output) == RaceReadings(sixty.Output));
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
	REQUIRE_FALSE(PhysicsState(sixty.Output).empty());
	CHECK(PhysicsState(fast.Output) == PhysicsState(sixty.Output));
	REQUIRE_FALSE(RaceReadings(sixty.Output).empty());
	CHECK(RaceReadings(fast.Output) == RaceReadings(sixty.Output));
}

TEST_CASE("Runtime: Moto's race keeps its best lap in the game's save slot, which the next run loads and gives the ghost")
{
	// Development runs of the ride test scene, which keep their game data in the project's folder (a test run's is
	// temporary), each 560 frames at 1/60 s, past the race's readings: the first finds no best lap and saves the scripted
	// races' best at their results, the second race's with the stand-in ghost's samples; the second run loads it as the scene
	// starts and tells the ghost, and its races beat nothing, so the slot keeps the first run's. A run without a window that
	// is not a test run is paced at 60 frames a second, so the runs stop well before the ride's end.
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project = CopyRepositoryProject("Moto", directory.GetPath());
	Testing::IsolatedUserRoot const userRoot;
	std::vector<std::string> const arguments = {
		"--project", Native(project), "--scene", "Scenes/Ride.sscene", "--headless", "--timestep", "0.0166666667", "--frames", "560"};

	ProcessResult const first = RunPlayer(arguments, userRoot);
	CAPTURE(first.Output);
	CHECK(first.ExitCode == 0);
	std::string_view const saving = RaceReadings(first.Output);
	REQUIRE_FALSE(saving.empty());
	CHECK(Contains(saving, "loaded at start: 0 s, the ghost told 0 times"));
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
	CHECK(Contains(loading, "the ghost told 1 times"));
	CHECK(RaceReading(loading, "the slot keeps ", " s, ") == saved);
}
