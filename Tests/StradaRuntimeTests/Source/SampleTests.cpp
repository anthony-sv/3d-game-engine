#include "RuntimeTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Platform/Process.h"
#include "Strada/Project/GameExporter.h"
#include "Strada/Project/Project.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;
using namespace Strada::RuntimeTesting;

// The repository's sample projects, played by the player: their test scenes pass their pinned checks, again with the same
// summary, and at another frame rate, and their games run without a word from their graphs. A ctest entry of their own
// (StradaRuntimeTests.Samples, Shards.cmake), as they take the longest.

TEST_CASE("Runtime: the feature-test project passes its test run")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project = CopyRepositoryProject("FeatureTest", directory.GetPath());

	ProcessResult const run = RunPlayer({"--project", Native(project), "--test"});
	CAPTURE(run.Output);
	CHECK(run.ExitCode == 0);
	CHECK(Contains(run.Output, "Tests passed"));
}

TEST_CASE("Runtime: the Showcase sample plays and passes its tests")
{
	// Its Poly Haven models, sky and maps are only there after Tools/FetchAssets.py; the checks do not depend on them.
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project = CopyRepositoryProject("Showcase", directory.GetPath());

	ProcessResult const tests = RunPlayer({"--project", Native(project), "--scene", "Scenes/Tests.sscene", "--test"});
	CAPTURE(tests.Output);
	CHECK(tests.ExitCode == 0);
	CHECK(Contains(tests.Output, "Tests passed: 68 checks"));

	ProcessResult const game = RunPlayer({"--project", Native(project), "--headless", "--frames", "120"});
	CAPTURE(game.Output);
	CHECK(game.ExitCode == 0);

	// The Graph Garden's visual scripts run without a word, in the tests and in the game. A graph's messages name its file:
	// "<path>.sgraph: " before a node's error or warning, "<path>.sgraph on <entity>" before a warning of the API scripts
	// call it through (SendEvent, GetVariable) or of its variables. A graph that cannot run says "Visual script" or "runs no
	// visual script".
	constexpr std::string_view GraphMessages[] = {".sgraph", "Visual script", " runs no visual script"};
	for (std::string_view const message : GraphMessages)
	{
		CAPTURE(message);
		CHECK_FALSE(Contains(tests.Output, message));
		CHECK_FALSE(Contains(game.Output, message));
	}
}

TEST_CASE("Runtime: the Blocks sample plays and passes its tests")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project = CopyRepositoryProject("Blocks", directory.GetPath());

	ProcessResult const tests = RunPlayer({"--project", Native(project), "--scene", "Scenes/Tests.sscene", "--test"});
	CAPTURE(tests.Output);
	CHECK(tests.ExitCode == 0);
	CHECK(Contains(tests.Output, "Tests passed: 57 checks"));

	// The checks seed the game's random numbers, so every run plays the same frames.
	ProcessResult const again = RunPlayer({"--project", Native(project), "--scene", "Scenes/Tests.sscene", "--test"});
	CAPTURE(again.Output);
	CHECK(again.ExitCode == 0);
	CHECK_FALSE(TestSummary(tests.Output).empty());
	CHECK(TestSummary(again.Output) == TestSummary(tests.Output));

	// They wait in seconds of the game, so they pass at other frame rates too.
	for (char const* timestep : {"0.0333333", "0.00694444"})
	{
		ProcessResult const run =
			RunPlayer({"--project", Native(project), "--scene", "Scenes/Tests.sscene", "--test", "--timestep", timestep});
		CAPTURE(timestep);
		CAPTURE(run.Output);
		CHECK(run.ExitCode == 0);
		CHECK(Contains(run.Output, "Tests passed: 57 checks"));
	}

	// The game's own scene runs as well (two seconds, without a window).
	ProcessResult const game = RunPlayer({"--project", Native(project), "--headless", "--frames", "120"});
	CAPTURE(game.Output);
	CHECK(game.ExitCode == 0);
}

TEST_CASE("Runtime: the Prism Break sample, made of visual scripts alone, plays and passes its tests, and so does its exported game")
{
	// Thirteen graphs and no C#: the player starts .NET for them, and its 33 checks are graphs too (spec-slice4 section 18.10).
	// Four of them drive the game through its input actions' overrides and rebind Launch, which a test run keeps in a
	// temporary game data folder.
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project = CopyRepositoryProject("PrismBreak", directory.GetPath());
	std::vector<std::string> const testRun = {"--project", Native(project), "--scene", "Scenes/Tests.sscene", "--test", "--timeout", "90"};

	ProcessResult const tests = RunPlayer(testRun);
	CAPTURE(tests.Output);
	CHECK(tests.ExitCode == 0);
	CHECK(Contains(tests.Output, "Tests passed: 33 checks"));
	CHECK_FALSE(Contains(tests.Output, "The game has no scripts"));

	// The test scene seeds the game's random numbers, so every run plays the same frames.
	ProcessResult const again = RunPlayer(testRun);
	CAPTURE(again.Output);
	CHECK(again.ExitCode == 0);
	CHECK_FALSE(TestSummary(tests.Output).empty());
	CHECK(TestSummary(again.Output) == TestSummary(tests.Output));

	// The checks wait in seconds of the game, so they pass at another frame rate too.
	std::vector<std::string> slower = testRun;
	slower.insert(slower.end(), {"--timestep", "0.0333333"});
	ProcessResult const slow = RunPlayer(slower);
	CAPTURE(slow.Output);
	CHECK(slow.ExitCode == 0);
	CHECK(Contains(slow.Output, "Tests passed: 33 checks"));

	// The game's own scene, whose title plays itself, runs five seconds without a window and without a word from its graphs.
	ProcessResult const game = RunPlayer({"--project", Native(project), "--headless", "--frames", "300"});
	CAPTURE(game.Output);
	CHECK(game.ExitCode == 0);
	CHECK_FALSE(Contains(game.Output, "Visual script"));
	CHECK_FALSE(Contains(game.Output, " runs no visual script"));
	// What the graphs' nodes report while they run (an aborting error, a node's warning) starts with the graph's path.
	CHECK_FALSE(Contains(game.Output, ".sgraph:"));

	// Its exported game carries the scripting API and no game scripts, and passes the same checks.
	std::filesystem::path player;
	std::filesystem::path scripts;
	{
		Testing::AssetManagerScope assets;
		Result<Ref<Project>> opened = Project::Open(project);
		REQUIRE_MESSAGE(opened.IsOk(), (opened ? std::string() : opened.GetError()));
		Result<GameExportSettings> prepared = GameExporter::Prepare(*opened.GetValue(), directory.GetPath() / "Export");
		REQUIRE_MESSAGE(prepared.IsOk(), (prepared ? std::string() : prepared.GetError()));
		CHECK(prepared.GetValue().UsesVisualScripts);
		CHECK(prepared.GetValue().ScriptProjectFile.empty());
		Result<GameExportResult> exported = GameExporter::Export(prepared.GetValue());
		REQUIRE_MESSAGE(exported.IsOk(), (exported ? std::string() : exported.GetError()));
		REQUIRE(exported.GetValue().Succeeded);
		player = exported.GetValue().Executable;
		scripts = directory.GetPath() / "Export" / "Scripts";
		if (player.extension() == ".app")
		{
			scripts = player / "Contents" / "Resources" / "Scripts";
			player = player / "Contents" / "MacOS" / player.stem();
		}
	}
	CHECK(FileSystem::IsRegularFile(scripts / "Strada.ScriptCore.dll"));
	CHECK_FALSE(FileSystem::Exists(scripts / "Game.dll"));
	ProcessResult const exportedTests = RunProgram(player, {"--test", "--scene", "Scenes/Tests.sscene", "--timeout", "90"});
	CAPTURE(exportedTests.Output);
	CHECK(exportedTests.ExitCode == 0);
	CHECK(Contains(exportedTests.Output, "Tests passed: 33 checks"));
}

TEST_CASE("Runtime: the Moto sample, made of graphs alone, flies its camera tour and passes its tests")
{
	// Eight graphs and no C#: the main camera's tour is a timeline (Scripts/Tour.sgraph), so is the ghost bike's ride round
	// the racing line (Scripts/Ghost.sgraph); its engine's sound, the camera director, the audio director and the grid bike's
	// riding graph (Scripts/Bike.sgraph, the bike a vehicle that stands on the grid while nobody rides it) are graphs, and
	// the test scene's 24 checks of the circuit (its surfaces and their physics materials, a crate at rest, two blocks
	// sliding on asphalt and on gravel), of the tour, of the ghost and its engine's sound, of the cuts and of the pause are
	// graphs too.
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project = CopyRepositoryProject("Moto", directory.GetPath());
	std::vector<std::string> const testRun = {"--project", Native(project), "--scene", "Scenes/Tests.sscene", "--test", "--timeout", "90"};

	ProcessResult const tests = RunPlayer(testRun);
	CAPTURE(tests.Output);
	CHECK(tests.ExitCode == 0);
	CHECK(Contains(tests.Output, "Tests passed: 24 checks"));
	CHECK_FALSE(Contains(tests.Output, "The game has no scripts"));

	// Nothing in the scene is random, so every run plays the same frames.
	ProcessResult const again = RunPlayer(testRun);
	CAPTURE(again.Output);
	CHECK(again.ExitCode == 0);
	CHECK_FALSE(TestSummary(tests.Output).empty());
	CHECK(TestSummary(again.Output) == TestSummary(tests.Output));

	// The checks wait in seconds of the game, so they pass at another frame rate too.
	std::vector<std::string> slower = testRun;
	slower.insert(slower.end(), {"--timestep", "0.0333333"});
	ProcessResult const slow = RunPlayer(slower);
	CAPTURE(slow.Output);
	CHECK(slow.ExitCode == 0);
	CHECK(Contains(slow.Output, "Tests passed: 24 checks"));

	// The game's own scene, whose tour takes off at once, runs five seconds without a window and without a word from its
	// graphs.
	ProcessResult const game = RunPlayer({"--project", Native(project), "--headless", "--frames", "300"});
	CAPTURE(game.Output);
	CHECK(game.ExitCode == 0);
	CHECK_FALSE(Contains(game.Output, "Visual script"));
	CHECK_FALSE(Contains(game.Output, " runs no visual script"));
	CHECK_FALSE(Contains(game.Output, ".sgraph:"));
}
