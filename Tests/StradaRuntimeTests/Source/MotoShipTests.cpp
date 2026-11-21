#include "RuntimeTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Platform/Process.h"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;
using namespace Strada::RuntimeTesting;

// The Moto sample as it ships, played by the player at the frame rates a player's machine runs it at: its test scene's
// pinned checks at 1/30 and 1/144 s a frame (SampleTests.cpp plays them at 1/60), and the physics state of the first
// minute of a ride from the grid, every fixed step of it, the same at 1/30, 1/60 and 1/144 s a frame
// (Docs/Architecture.md section 15, "Moto shipped"). A ctest entry of its own (StradaRuntimeTests.Ship, Shards.cmake): its
// starts are on scenes without the props.

namespace
{
	// Tests.sscene's checks (Tools/Checks.py), as SampleTests.cpp pins them at 1/60 s a frame.
	constexpr std::string_view TestChecks = "Tests passed: 24 checks";
	// A frame of 1/144 s runs a fixed step only now and then, so a run's frames are 2.4 times those at 1/60 s: on a slow
	// runner a Debug run can pass RunTimeout's two minutes, so these runs have a process timeout of their own.
	constexpr std::chrono::seconds ShipRunTimeout = std::chrono::minutes(5);
	// The ride's minute: 3,600 fixed steps of the project's 1/60 s step. The run's timeout gives it a quarter of a second
	// more of the game (15 steps), since 8,640 frames of the player's 1/144 s, a float a frame, may fall short of step 3,600.
	constexpr std::string_view RideStep = "3600";
	constexpr std::string_view RideTimeout = "60.25";

	// The test run's summary line, passed or failed, to its end; empty without one.
	std::string_view SummaryLine(std::string_view output)
	{
		size_t start = output.find("Tests passed: ");
		if (start == std::string_view::npos)
		{
			start = output.find("Tests failed: ");
		}
		if (start == std::string_view::npos)
		{
			return {};
		}
		size_t const end = output.find_first_of("\r\n", start);
		return output.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
	}

	// The summary's physics state, from "physics state" to its end (the fold included); empty without one.
	std::string_view PhysicsState(std::string_view output)
	{
		std::string_view const summary = SummaryLine(output);
		size_t const start = summary.find("; physics state ");
		return start == std::string_view::npos ? std::string_view() : summary.substr(start + 2);
	}
}

TEST_CASE("Runtime: the Moto sample's test scene passes its pinned checks at 1/30 and at 1/144 s a frame")
{
	// Its checks wait in seconds of the game, so they pass at other frame rates too.
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project = CopyRepositoryProject("Moto", directory.GetPath());
	Testing::IsolatedUserRoot const userRoot;
	for (char const* timestep : {"0.0333333", "0.00694444"})
	{
		CAPTURE(timestep);
		ProcessResult const run =
			RunPlayer({"--project", Native(project), "--scene", "Scenes/Tests.sscene", "--test", "--timeout", "90", "--timestep", timestep},
		              userRoot, ShipRunTimeout);
		CAPTURE(run.Output);
		CHECK(run.ExitCode == 0);
		CHECK(Contains(run.Output, TestChecks));
	}
}

TEST_CASE("Runtime: the first minute of Moto's ride from the grid has the same physics state at every fixed step at 1/30, 1/60 "
          "and 1/144 s a frame")
{
	// The lap test scene (Scenes/Lap.sscene: the main scene without its props, Tools/LapChecks.py) rides the bike on the
	// autopilot from the grid as the scene starts, its inputs set in its fixed steps from what the bike reads, never from a
	// frame. Its checks report after two whole laps, so a minute of it is a test run cut by its timeout: it ends unfinished
	// (exit code 1) with no check failed, and its summary gives the state hash of step 3,600 and the fold of every step's
	// hash from the first to it (--hash-every-step), compared exactly between the rates. A test run's game data folder is
	// temporary, so every run starts from no kept best lap and no settings.
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project = CopyRepositoryProject("Moto", directory.GetPath());
	Testing::IsolatedUserRoot const userRoot;
	std::string expected;
	for (char const* timestep : {"0.0166666667", "0.0333333333", "0.0069444444"})
	{
		CAPTURE(timestep);
		ProcessResult const run =
			RunPlayer({"--project", Native(project), "--scene", "Scenes/Lap.sscene", "--test", "--timeout", std::string(RideTimeout),
		               "--timestep", timestep, "--hash-step", std::string(RideStep), "--hash-every-step"},
		              userRoot, ShipRunTimeout);
		CAPTURE(run.Output);
		CHECK(run.ExitCode == 1);
		std::string_view const summary = SummaryLine(run.Output);
		CHECK(summary.starts_with("Tests failed: 0 of "));
		CHECK(Contains(summary, " failed, 0 script exceptions; the scripts did not finish testing in "));
		std::string const state(PhysicsState(run.Output));
		REQUIRE(Contains(state, std::string(" at step ") + std::string(RideStep) + "; steps 1 to " + std::string(RideStep) + " folded "));
		if (expected.empty())
		{
			expected = state;
		}
		CHECK(state == expected);
	}
}
