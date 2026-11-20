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
// Moto's main scene takes on CI's macOS runners: the shard's two starts stay well within its 200 s there.

namespace
{
	// The ride test scene's checks.
	constexpr std::string_view RideChecks = "Tests passed: 7 checks";
	// A fixed step of the ride after it was teleported into the gravel trap and back (by about step 1,650) and before it ends
	// (about step 1,870): the physics state there is the same at every frame rate.
	constexpr std::string_view HashStep = "1750";

	// The line in which the ride's graph logs every reading it took ("Moto's ride: ..."); empty without one.
	std::string_view RideReadings(std::string_view output)
	{
		size_t const start = output.find("Moto's ride: ");
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
		std::string_view const summary = TestSummary(output);
		size_t const start = summary.find("physics state ");
		return start == std::string_view::npos ? std::string_view() : summary.substr(start);
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
}
