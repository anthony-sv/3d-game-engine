#include "RuntimeTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Platform/Process.h"
#include "Strada/Project/Project.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace Strada;
using namespace Strada::RuntimeTesting;

// The repository's templates, each through a project made from it as the editor makes one, played by the player: their
// test scenes pass their pinned checks, again with the same summary, and at another frame rate, and their games run
// without a word from their scripts (StradaRuntimeTests.Templates, Shards.cmake).

TEST_CASE("Runtime: a project made from the empty-3d template loads and runs")
{
	Testing::TemporaryDirectory directory;
	Ref<Scene> const scene = MakeScene("Main", std::nullopt);
	std::filesystem::path const project =
		InstantiateRepositoryTemplate("empty-3d", directory.GetPath() / "Empty", "First Light", scene.get());
	CHECK(project.filename() == "First Light.sproj");

	ProcessResult const run = RunPlayer({"--project", Native(project), "--headless", "--frames", "30"});
	CAPTURE(run.Output);
	CHECK(run.ExitCode == 0);
}

TEST_CASE("Runtime: a project made from the graph-starter template, Orb Run in visual scripts alone, plays and passes its tests")
{
	// Seven graphs and no C#: the project takes the name it was made with, the player starts .NET for its graphs, and its 13
	// checks are graphs too.
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project =
		InstantiateRepositoryTemplate("graph-starter", directory.GetPath() / "Spark Chase", "Spark Chase");
	CHECK(project.filename() == "Spark Chase.sproj");
	{
		Testing::AssetManagerScope assets;
		Result<Ref<Project>> opened = Project::Open(project);
		REQUIRE_MESSAGE(opened.IsOk(), (opened ? std::string() : opened.GetError()));
		CHECK(opened.GetValue()->GetSettings().Name == "Spark Chase");
		CHECK(opened.GetValue()->GetSettings().Window.Title == "Spark Chase");
	}
	std::vector<std::string> const testRun = {"--project", Native(project), "--scene", "Scenes/Tests.sscene", "--test", "--timeout", "90"};

	ProcessResult const tests = RunPlayer(testRun);
	CAPTURE(tests.Output);
	CHECK(tests.ExitCode == 0);
	CHECK(Contains(tests.Output, "Tests passed: 13 checks"));
	CHECK_FALSE(Contains(tests.Output, "The game has no scripts"));

	// The checks place what they test, so every run plays the same frames.
	ProcessResult const again = RunPlayer(testRun);
	CAPTURE(again.Output);
	CHECK(again.ExitCode == 0);
	CHECK_FALSE(TestSummary(tests.Output).empty());
	CHECK(TestSummary(again.Output) == TestSummary(tests.Output));

	// They wait in seconds of the game, so they pass at another frame rate too.
	std::vector<std::string> slower = testRun;
	slower.insert(slower.end(), {"--timestep", "0.0333333"});
	ProcessResult const slow = RunPlayer(slower);
	CAPTURE(slow.Output);
	CHECK(slow.ExitCode == 0);
	CHECK(Contains(slow.Output, "Tests passed: 13 checks"));

	// The game's own scene runs five seconds without a window and without a word from its graphs.
	ProcessResult const game = RunPlayer({"--project", Native(project), "--headless", "--frames", "300"});
	CAPTURE(game.Output);
	CHECK(game.ExitCode == 0);
	CHECK_FALSE(Contains(game.Output, "Visual script"));
	CHECK_FALSE(Contains(game.Output, " runs no visual script"));
	CHECK_FALSE(Contains(game.Output, ".sgraph:"));
}

TEST_CASE("Runtime: a project made from the csharp-starter template, Orb Run in C#, plays and passes its tests")
{
	// C# scripts (the assembly and namespace Game) and one graph that calls a C# member: the project takes the name it was
	// made with, its C# project names nothing of the engine's repository, and its 14 checks are C#.
	Testing::TemporaryDirectory directory;
	std::filesystem::path const project =
		InstantiateRepositoryTemplate("csharp-starter", directory.GetPath() / "Spark Chase", "Spark Chase");
	CHECK(project.filename() == "Spark Chase.sproj");
	std::filesystem::path const scripts = project.parent_path() / "Scripts";
	{
		Testing::AssetManagerScope assets;
		Result<Ref<Project>> opened = Project::Open(project);
		REQUIRE_MESSAGE(opened.IsOk(), (opened ? std::string() : opened.GetError()));
		CHECK(opened.GetValue()->GetSettings().Name == "Spark Chase");
		CHECK(opened.GetValue()->GetSettings().Window.Title == "Spark Chase");
		CHECK(opened.GetValue()->GetSettings().ScriptModule == "Scripts/Binaries/Game.dll");
	}
	Result<std::string> const scriptProject = FileSystem::ReadTextFile(scripts / "Game.csproj");
	REQUIRE(scriptProject.IsOk());
	CHECK(Contains(scriptProject.GetValue(), "<RootNamespace>Game</RootNamespace>"));
	CHECK_FALSE(Contains(scriptProject.GetValue(), "repository-only"));
	CHECK_FALSE(Contains(scriptProject.GetValue(), "Strada-ScriptCore"));
	CHECK(FileSystem::IsRegularFile(scripts / "Source" / "OrbRun.cs"));
	CHECK_FALSE(FileSystem::Exists(scripts / "Binaries"));
	CHECK_FALSE(FileSystem::Exists(scripts / "Strada.props"));

	// The editor builds the scripts when the project opens; here they are the ones CMake built for the template.
	REQUIRE(FileSystem::CopyDirectory(FileSystem::GetExecutableDirectory() / "CSharpStarter", scripts / "Binaries").IsOk());
	std::vector<std::string> const testRun = {"--project", Native(project), "--scene", "Scenes/Tests.sscene", "--test", "--timeout", "90"};

	ProcessResult const tests = RunPlayer(testRun);
	CAPTURE(tests.Output);
	CHECK(tests.ExitCode == 0);
	CHECK(Contains(tests.Output, "Tests passed: 14 checks"));

	// The checks place what they test, so every run plays the same frames.
	ProcessResult const again = RunPlayer(testRun);
	CAPTURE(again.Output);
	CHECK(again.ExitCode == 0);
	CHECK_FALSE(TestSummary(tests.Output).empty());
	CHECK(TestSummary(again.Output) == TestSummary(tests.Output));

	// They wait in seconds of the game, so they pass at another frame rate too.
	std::vector<std::string> slower = testRun;
	slower.insert(slower.end(), {"--timestep", "0.0333333"});
	ProcessResult const slow = RunPlayer(slower);
	CAPTURE(slow.Output);
	CHECK(slow.ExitCode == 0);
	CHECK(Contains(slow.Output, "Tests passed: 14 checks"));

	// The game's own scene runs five seconds without a window, without an exception and without a word from its graph.
	ProcessResult const game = RunPlayer({"--project", Native(project), "--headless", "--frames", "300"});
	CAPTURE(game.Output);
	CHECK(game.ExitCode == 0);
	CHECK_FALSE(Contains(game.Output, "Exception"));
	CHECK_FALSE(Contains(game.Output, "Visual script"));
	CHECK_FALSE(Contains(game.Output, ".sgraph:"));
}
