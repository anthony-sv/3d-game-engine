#include "Core/UserDataTestUtilities.h"
#include "GpuTestUtilities.h"
#include "Runtime/ResolutionRequest.h"
#include "Runtime/ScreenshotRequest.h"
#include "RuntimeTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/EngineLayout.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/GameData.h"
#include "Strada/Core/Image.h"
#include "Strada/Core/PlatformDetection.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Platform/Process.h"
#include "Strada/Project/GameExporter.h"
#include "Strada/Project/Project.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Serialization/ProfilerCaptureSerializer.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;
using namespace Strada::RuntimeTesting;

namespace
{
	constexpr uint32_t WindowWidth = 320;
	constexpr uint32_t WindowHeight = 200;

	// The log a program of the given executable writes below a data folder (Strada/Core/EntryPoint.cpp).
	std::filesystem::path GetLogFile(std::filesystem::path const& dataDirectory, std::filesystem::path const& executable)
	{
		return dataDirectory / "Logs" / (FileSystem::PathToUtf8(executable.stem()) + ".log");
	}

	// A file's text; empty when it cannot be read.
	std::string ReadText(std::filesystem::path const& file)
	{
		Result<std::string> text = FileSystem::ReadTextFile(file);
		return text ? text.TakeValue() : std::string();
	}

	size_t CountOccurrences(std::string_view text, std::string_view part)
	{
		size_t count = 0;
		for (size_t found = text.find(part); found != std::string_view::npos; found = text.find(part, found + part.size()))
		{
			++count;
		}
		return count;
	}

	// What every application logs at startup about the engine marker beside it (Strada/Core/EntryPoint.cpp).
	constexpr std::string_view EngineLayoutWarning = "Engine layout: ";

	// A game in a temporary directory: scenes in Assets/Scenes, the test scripts as its scripts, and its configuration, as
	// an exported game (Game.sgame, with the scripting API next to the scripts) or as a project.
	class TestGame
	{
	public:
		explicit TestGame(ProjectFileKind kind)
			: m_Kind(kind)
		{
		}

		std::filesystem::path const& GetDirectory() const { return m_Directory.GetPath(); }
		std::filesystem::path GetScriptDirectory() const
		{
			return GetDirectory() / (m_Kind == ProjectFileKind::Game ? "Scripts" : "Scripts/Binaries");
		}

		void AddScene(std::string const& name, Ref<Scene> const& scene) const
		{
			std::filesystem::path const directory = GetDirectory() / "Assets" / "Scenes";
			REQUIRE(FileSystem::CreateDirectories(directory).IsOk());
			REQUIRE(SceneSerializer::SaveToFile(*scene, directory / (name + ".sscene")).IsOk());
		}

		void AddScripts() const
		{
			std::filesystem::path const scripts = GetScriptDirectory();
			REQUIRE(FileSystem::CreateDirectories(scripts).IsOk());
			std::filesystem::path const testScripts = Testing::GetTestScriptsPath().parent_path();
			for (char const* file : {"Strada.TestScripts.dll", "Strada.TestScripts.deps.json"})
			{
				REQUIRE(FileSystem::Copy(testScripts / file, scripts / file, true).IsOk());
			}
			if (m_Kind == ProjectFileKind::Game)
			{
				// Exported games carry the scripting API next to their scripts.
				std::filesystem::path const engine = FileSystem::GetExecutableDirectory();
				for (char const* file : {"Strada.ScriptCore.dll", "Strada.ScriptCore.runtimeconfig.json", "Strada.ScriptCore.deps.json"})
				{
					REQUIRE(FileSystem::Copy(engine / file, scripts / file, true).IsOk());
				}
			}
		}

		// The window's icon: an asset path relative to Assets (empty: none).
		void SetIcon(std::string icon) { m_Icon = std::move(icon); }
		// Whether the game's window settings make it fullscreen.
		void SetFullscreen(bool fullscreen) { m_Fullscreen = fullscreen; }

		// Registers the scenes (as the editor did) and writes the configuration; startScene is relative to Assets, or empty.
		std::filesystem::path Write(std::string const& startScene) const
		{
			ProjectSettings settings;
			settings.Name = "Runtime Test";
			settings.ScriptModule =
				FileSystem::PathToUtf8(FileSystem::GetRelativePath(GetScriptDirectory(), GetDirectory()) / "Strada.TestScripts.dll");
			settings.Window.Width = WindowWidth;
			settings.Window.Height = WindowHeight;
			settings.Window.Fullscreen = m_Fullscreen;
			{
				Testing::AssetManagerScope assets;
				REQUIRE(FileSystem::CreateDirectories(GetDirectory() / "Assets").IsOk());
				REQUIRE(AssetManager::OpenAssetDirectory(GetDirectory() / "Assets").IsOk());
				if (!startScene.empty())
				{
					settings.StartScene = AssetManager::FindByPath(startScene);
					REQUIRE(settings.StartScene.IsValid());
				}
				if (!m_Icon.empty())
				{
					settings.Window.Icon = AssetManager::FindByPath(m_Icon);
					REQUIRE(settings.Window.Icon.IsValid());
				}
			}
			std::filesystem::path const file =
				GetDirectory() / FileSystem::PathFromUtf8(m_Kind == ProjectFileKind::Game ? Project::GameFileName : "Runtime Test.sproj");
			REQUIRE(FileSystem::WriteTextFile(file, DumpJson(Project::Serialize(settings, m_Kind))).IsOk());
			return file;
		}

	private:
		ProjectFileKind m_Kind;
		Testing::TemporaryDirectory m_Directory;
		std::string m_Icon;
		bool m_Fullscreen = false;
	};

	VisualScriptNode MakeNode(uint32_t id, std::string type, Json inputs = Json::object())
	{
		VisualScriptNode node;
		node.ID = id;
		node.Type = std::move(type);
		node.Inputs = std::move(inputs);
		return node;
	}

	// Checks as a visual script: one passes when it is created, one in its first update, which then finishes the test run.
	VisualScriptDocument MakeChecksDocument()
	{
		VisualScriptDocument document;
		document.Graph.Nodes.push_back(MakeNode(1, "Event.Create"));
		document.Graph.Nodes.push_back(
			MakeNode(2, "Call:Strada.Testing.TestReporter.Pass(string)", Json::object({{"Name", "graph created"}})));
		document.Graph.Nodes.push_back(MakeNode(3, "Event.Update"));
		document.Graph.Nodes.push_back(
			MakeNode(4, "Call:Strada.Testing.TestReporter.Pass(string)", Json::object({{"Name", "graph updated"}})));
		document.Graph.Nodes.push_back(MakeNode(5, "Call:Strada.Testing.TestReporter.Finish()"));
		document.Graph.Links.push_back({{1, "Then"}, {2, "Execute"}});
		document.Graph.Links.push_back({{3, "Then"}, {4, "Execute"}});
		document.Graph.Links.push_back({{4, "Then"}, {5, "Execute"}});
		return document;
	}

	// The checks graph with a check that passes only where development-only nodes run: Create -> Pass "development node ran"
	// (development only) -> Pass "graph created".
	VisualScriptDocument MakeDevelopmentChecksDocument()
	{
		VisualScriptDocument document = MakeChecksDocument();
		VisualScriptNode development =
			MakeNode(6, "Call:Strada.Testing.TestReporter.Pass(string)", Json::object({{"Name", "development node ran"}}));
		development.Enabled = VisualScriptNodeEnabled::DevelopmentOnly;
		document.Graph.Nodes.push_back(std::move(development));
		document.Graph.Links.front() = {{1, "Then"}, {6, "Execute"}};
		document.Graph.Links.push_back({{6, "Then"}, {2, "Execute"}});
		return document;
	}

	// Writes a graph into Scripts of the open asset directory and registers it.
	AssetHandle AddGraphAsset(VisualScriptDocument const& document, std::string const& name)
	{
		std::filesystem::path const file = AssetManager::GetAssetDirectory() / "Scripts" / FileSystem::PathFromUtf8(name + ".sgraph");
		REQUIRE(VisualScriptSerializer::SaveToFile(document, file).IsOk());
		Result<AssetHandle> graph = AssetManager::ImportFile(file);
		REQUIRE(graph.IsOk());
		REQUIRE(AssetManager::SaveRegistry().IsOk());
		return graph.GetValue();
	}

	// Writes a checks graph into the open asset directory and adds an entity running it to the scene.
	void AddChecksGraph(Scene& scene, VisualScriptDocument const& document = MakeChecksDocument())
	{
		scene.CreateEntity("Checker").AddComponent<VisualScriptComponent>().Graph = AddGraphAsset(document, "Checks");
	}

	VisualScriptNode MakeSetNode(uint32_t id, std::string type, Json settings, Json inputs = Json::object())
	{
		VisualScriptNode node = MakeNode(id, std::move(type), std::move(inputs));
		node.Settings = std::move(settings);
		return node;
	}

	// The parameters of the custom event Ping: Amount, an int.
	Json MakePingParameters()
	{
		return Json::array({Json::object({{"Name", "Amount"}, {"Type", "int"}})});
	}

	// A function library: Twice (1, pure: Value * 2 by a formula) and Note (2, impure: a check that passes only where
	// development-only nodes run).
	VisualScriptDocument MakeMathsLibrary()
	{
		VisualScriptDocument document;
		document.Kind = VisualScriptDocumentKind::Library;
		VisualScriptFunction twice;
		twice.ID = 1;
		twice.Name = "Twice";
		twice.Pure = true;
		twice.Inputs = {VisualScriptParameter("Value", "int")};
		twice.Outputs = {VisualScriptParameter("Result", "int")};
		twice.Graph.Nodes.push_back(MakeNode(10, "Function.Entry"));
		twice.Graph.Nodes.push_back(MakeSetNode(11, "Expression", Json::object({{"Formula", "value * 2"}})));
		twice.Graph.Nodes.push_back(MakeNode(12, "Function.Return"));
		twice.Graph.Links.push_back({{10, "Value"}, {11, "value"}});
		twice.Graph.Links.push_back({{11, "Result"}, {12, "Result"}});
		VisualScriptFunction note;
		note.ID = 2;
		note.Name = "Note";
		note.Graph.Nodes.push_back(MakeNode(20, "Function.Entry"));
		VisualScriptNode development =
			MakeNode(21, "Call:Strada.Testing.TestReporter.Pass(string)", Json::object({{"Name", "library development node ran"}}));
		development.Enabled = VisualScriptNodeEnabled::DevelopmentOnly;
		note.Graph.Nodes.push_back(std::move(development));
		note.Graph.Links.push_back({{20, "Then"}, {21, "Execute"}});
		document.Functions = {std::move(twice), std::move(note)};
		return document;
	}

	// Each receiver passes a check named after its entity when its Ping carries 42.
	VisualScriptDocument MakeReceiverDocument()
	{
		VisualScriptDocument document;
		document.Graph.Nodes.push_back(
			MakeSetNode(1, "Event.Custom", Json::object({{"Name", "Ping"}, {"Parameters", MakePingParameters()}})));
		document.Graph.Nodes.push_back(MakeSetNode(2, "Expression", Json::object({{"Formula", "amount == 42"}})));
		document.Graph.Nodes.push_back(MakeNode(3, "Flow.Branch"));
		document.Graph.Nodes.push_back(MakeNode(4, "Get:Strada.Entity.Name"));
		document.Graph.Nodes.push_back(MakeNode(5, "Call:Strada.Testing.TestReporter.Pass(string)"));
		document.Graph.Nodes.push_back(
			MakeNode(6, "Call:Strada.Testing.TestReporter.Fail(string,string)", Json::object({{"Message", "another amount came"}})));
		document.Graph.Links.push_back({{1, "Then"}, {3, "Execute"}});
		document.Graph.Links.push_back({{1, "Amount"}, {2, "amount"}});
		document.Graph.Links.push_back({{2, "Result"}, {3, "Condition"}});
		document.Graph.Links.push_back({{3, "True"}, {5, "Execute"}});
		document.Graph.Links.push_back({{3, "False"}, {6, "Execute"}});
		document.Graph.Links.push_back({{4, "Value"}, {5, "Name"}});
		document.Graph.Links.push_back({{4, "Value"}, {6, "Name"}});
		return document;
	}

	// In its first update: calls the library's Note, broadcasts Ping with Twice(21), checks that both receivers got it and
	// finishes the test run.
	VisualScriptDocument MakeBroadcasterDocument(AssetHandle library)
	{
		Json const graph = library.ToString();
		VisualScriptDocument document;
		document.Graph.Nodes.push_back(MakeNode(1, "Event.Update"));
		document.Graph.Nodes.push_back(MakeSetNode(2, "Function.Call", Json::object({{"Graph", graph}, {"Function", 2}})));
		document.Graph.Nodes.push_back(
			MakeSetNode(3, "Function.Call", Json::object({{"Graph", graph}, {"Function", 1}}), Json::object({{"Value", 21}})));
		document.Graph.Nodes.push_back(
			MakeSetNode(4, "Event.Broadcast", Json::object({{"Name", "Ping"}, {"Parameters", MakePingParameters()}})));
		document.Graph.Nodes.push_back(MakeSetNode(5, "Expression", Json::object({{"Formula", "count == 2"}})));
		document.Graph.Nodes.push_back(MakeNode(6, "Flow.Branch"));
		document.Graph.Nodes.push_back(
			MakeNode(7, "Call:Strada.Testing.TestReporter.Pass(string)", Json::object({{"Name", "the broadcast reached both receivers"}})));
		document.Graph.Nodes.push_back(
			MakeNode(8, "Call:Strada.Testing.TestReporter.Fail(string,string)",
		             Json::object({{"Name", "the broadcast reached both receivers"}, {"Message", "another number received it"}})));
		document.Graph.Nodes.push_back(MakeNode(9, "Call:Strada.Testing.TestReporter.Finish()"));
		document.Graph.Links.push_back({{1, "Then"}, {2, "Execute"}});
		document.Graph.Links.push_back({{2, "Then"}, {4, "Execute"}});
		document.Graph.Links.push_back({{3, "Result"}, {4, "Amount"}});
		document.Graph.Links.push_back({{4, "Then"}, {6, "Execute"}});
		document.Graph.Links.push_back({{4, "Count"}, {5, "count"}});
		document.Graph.Links.push_back({{5, "Result"}, {6, "Condition"}});
		document.Graph.Links.push_back({{6, "True"}, {7, "Execute"}});
		document.Graph.Links.push_back({{6, "False"}, {8, "Execute"}});
		document.Graph.Links.push_back({{7, "Then"}, {9, "Execute"}});
		document.Graph.Links.push_back({{8, "Then"}, {9, "Execute"}});
		return document;
	}

}

TEST_CASE("Runtime: test runs of an exported game exit with the number of failures")
{
	TestGame game(ProjectFileKind::Game);
	game.AddScene(
		"Pass",
		MakeScene("Pass", ScriptFieldMap{{"ExpectedWindowWidth", ScriptFieldValue::FromInt32(static_cast<int32_t>(WindowWidth))},
	                                     {"ExpectedWindowHeight", ScriptFieldValue::FromInt32(static_cast<int32_t>(WindowHeight))}}));
	game.AddScene("Fail", MakeScene("Fail", ScriptFieldMap{{"Fail", ScriptFieldValue::FromBool(true)}}));
	game.AddScripts();
	std::filesystem::path const file = game.Write("Scenes/Pass.sscene");

	// Test runs see the game's window size although they have no window.
	ProcessResult const passed = RunPlayer({"--game", Native(file), "--test"});
	CAPTURE(passed.Output);
	CHECK(passed.ExitCode == 0);
	CHECK(Contains(passed.Output, "Tests passed: 3 checks in 1 frame"));

	// A directory names the Game.sgame in it; --scene starts elsewhere.
	ProcessResult const failed = RunPlayer({"--game", Native(game.GetDirectory()), "--test", "--scene", "Scenes/Fail.sscene"});
	CAPTURE(failed.Output);
	CHECK(failed.ExitCode == 1);
	CHECK(Contains(failed.Output, "Test failed: fails on request: expected false: Fail is set"));
	CHECK(Contains(failed.Output, "Tests failed: 1 of 2 checks failed, 0 script exceptions"));
}

TEST_CASE("Runtime: test runs go on in the scenes the scripts load, and fail when they quit or time out")
{
	TestGame game(ProjectFileKind::Game);
	game.AddScene("First", MakeScene("First", ScriptFieldMap{{"NextScene", ScriptFieldValue::FromString("Scenes/Second.sscene")}}));
	game.AddScene("Second", MakeScene("Second", ScriptFieldMap{}));
	game.AddScene("Quitter", MakeScene("Quitter", ScriptFieldMap{{"Quit", ScriptFieldValue::FromBool(true)}}));
	game.AddScene("Idle", MakeScene("Idle", std::nullopt));
	game.AddScripts();
	std::string const file = Native(game.Write("Scenes/First.sscene"));

	ProcessResult const loaded = RunPlayer({"--game", file, "--test"});
	CAPTURE(loaded.Output);
	CHECK(loaded.ExitCode == 0);
	CHECK(Contains(loaded.Output, "Loaded scene 'Second'"));
	CHECK(Contains(loaded.Output, "Tests passed: 4 checks in 2 frames"));

	ProcessResult const quit = RunPlayer({"--game", file, "--test", "--scene", "Scenes/Quitter.sscene"});
	CAPTURE(quit.Output);
	CHECK(quit.ExitCode == 1);
	CHECK(Contains(quit.Output, "the scripts quit before they finished testing"));

	ProcessResult const timedOut =
		RunPlayer({"--game", file, "--test", "--scene", "Scenes/Idle.sscene", "--timeout", "0.5", "--timestep", "0.125"});
	CAPTURE(timedOut.Output);
	CHECK(timedOut.ExitCode == 1);
	CHECK(Contains(timedOut.Output, "the scripts did not finish testing in 4 frames"));

	// Closing the player before the run ends fails it too.
	ProcessResult const cut = RunPlayer({"--game", file, "--test", "--scene", "Scenes/Idle.sscene", "--frames", "2"});
	CAPTURE(cut.Output);
	CHECK(cut.ExitCode == 1);
	CHECK(Contains(cut.Output, "the scripts did not finish testing in 2 frames"));
}

TEST_CASE("Runtime: a test summary ends with the physics state hash, the same at a step at 30, 60 and 144 frames per second")
{
	// A stack the ball knocks over, a character landing on it with a collider of its own below it, and a script that makes
	// a body between two steps and finishes the run after 200 fixed steps, whatever the frame rate.
	TestGame game(ProjectFileKind::Game);
	Ref<Scene> const scene = MakeScene("Physics", std::nullopt);
	Entity ground = scene->CreateEntity("Ground");
	ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
	ground.AddComponent<BoxColliderComponent>().HalfExtents = {20.0f, 0.5f, 20.0f};
	for (int level = 0; level < 4; level++)
	{
		Entity crate = scene->CreateEntity("Crate");
		crate.GetComponent<TransformComponent>().Translation = {0.03f * static_cast<float>(level % 2), 0.5f + static_cast<float>(level),
		                                                        0.0f};
		crate.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		crate.AddComponent<BoxColliderComponent>();
	}
	Entity ball = scene->CreateEntity("Ball");
	ball.GetComponent<TransformComponent>().Translation = {-5.0f, 0.5f, 0.1f};
	RigidBodyComponent& ballBody = ball.AddComponent<RigidBodyComponent>();
	ballBody.Type = RigidBodyType::Dynamic;
	ballBody.Mass = 5.0f;
	ballBody.InitialLinearVelocity = {8.0f, 0.0f, 0.0f};
	ball.AddComponent<SphereColliderComponent>();
	Entity hero = scene->CreateEntity("Hero");
	hero.GetComponent<TransformComponent>().Translation = {0.2f, 6.0f, 0.0f};
	hero.AddComponent<CharacterControllerComponent>().Offset = {0.0f, 1.0f, 0.0f};
	Entity held = scene->CreateEntity("Held", hero);
	held.GetComponent<TransformComponent>().Translation = {0.0f, 1.0f, 0.7f};
	held.AddComponent<SphereColliderComponent>().Radius = 0.2f;
	// The script's entity becomes a body in its 41st fixed update, which at 30 frames per second is the first of a frame's
	// two: the body takes part in the very next step at every frame rate.
	Entity probe = scene->CreateEntity("Probe");
	probe.GetComponent<TransformComponent>().Translation = {1.5f, 3.0f, 0.0f};
	probe.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
	ScriptComponent& script = probe.AddComponent<ScriptComponent>();
	script.ClassName = "Strada.Tests.StepProbe";
	script.Fields = ScriptFieldMap{{"Steps", ScriptFieldValue::FromInt32(200)}, {"ColliderStep", ScriptFieldValue::FromInt32(41)}};
	game.AddScene("Physics", scene);
	game.AddScripts();
	std::string const file = Native(game.Write("Scenes/Physics.sscene"));

	// The hash after step 180 is the same whichever way the frames divide the steps.
	std::string expected;
	for (char const* timestep : {"0.0166666667", "0.0333333333", "0.0069444444"})
	{
		CAPTURE(timestep);
		ProcessResult const run = RunPlayer({"--game", file, "--test", "--timestep", timestep, "--hash-step", "180"});
		CAPTURE(run.Output);
		CHECK(run.ExitCode == 0);
		std::string_view const summary = TestSummary(run.Output);
		size_t const physics = summary.find("; physics state ");
		REQUIRE(physics != std::string_view::npos);
		std::string const state(summary.substr(physics));
		CHECK(state.ends_with(" at step 180"));
		if (expected.empty())
		{
			expected = state;
		}
		CHECK(state == expected);
	}

	// Without --hash-step it is the hash after the run's last step, which a second run repeats.
	ProcessResult const last = RunPlayer({"--game", file, "--test"});
	CAPTURE(last.Output);
	CHECK(last.ExitCode == 0);
	CHECK(TestSummary(last.Output).ends_with(" at step 201"));
	ProcessResult const again = RunPlayer({"--game", file, "--test"});
	CHECK(TestSummary(again.Output) == TestSummary(last.Output));
	CHECK(TestSummary(last.Output) != TestSummary(RunPlayer({"--game", file, "--test", "--hash-step", "200"}).Output));

	// A step the run never reaches is said, and a failed run's summary ends the same way.
	ProcessResult const unreached = RunPlayer({"--game", file, "--test", "--hash-step", "5000"});
	CHECK(unreached.ExitCode == 0);
	CHECK(Contains(unreached.Output, "; no physics state at step 5000: the run ended at step 201"));
	ProcessResult const timedOut = RunPlayer({"--game", file, "--test", "--timeout", "1", "--hash-step", "30"});
	CAPTURE(timedOut.Output);
	CHECK(timedOut.ExitCode == 1);
	CHECK(Contains(timedOut.Output, " frames; physics state "));
	CHECK(Contains(timedOut.Output, " at step 30"));

	// The option belongs to test runs, and takes a step.
	CHECK(RunPlayer({"--game", file, "--headless", "--frames", "1", "--hash-step", "10"}).ExitCode == 2);
	CHECK(RunPlayer({"--game", file, "--test", "--hash-step", "0"}).ExitCode == 2);
}

TEST_CASE("Runtime: projects run with their last script build, and games without scripts need no .NET")
{
	TestGame project(ProjectFileKind::Project);
	project.AddScene("Pass", MakeScene("Pass", ScriptFieldMap{}));
	project.AddScripts();
	std::string const file = Native(project.Write("Scenes/Pass.sscene"));

	ProcessResult const passed = RunPlayer({"--project", file, "--test"});
	CAPTURE(passed.Output);
	CHECK(passed.ExitCode == 0);
	CHECK(Contains(passed.Output, "Tests passed: 2 checks"));

	// A C# project whose assembly is missing has not been built.
	REQUIRE(FileSystem::WriteTextFile(project.GetDirectory() / "Scripts" / "Strada.TestScripts.csproj", "<Project />").IsOk());
	REQUIRE(FileSystem::Remove(project.GetScriptDirectory() / "Strada.TestScripts.dll").IsOk());
	ProcessResult const unbuilt = RunPlayer({"--project", file, "--test"});
	CAPTURE(unbuilt.Output);
	CHECK(unbuilt.ExitCode == 1);
	CHECK(Contains(unbuilt.Output, "the project's scripts are not built"));

	TestGame plain(ProjectFileKind::Game);
	plain.AddScene("Main", MakeScene("Main", std::nullopt));
	ProcessResult const headless = RunPlayer({"--game", Native(plain.Write("Scenes/Main.sscene")), "--headless", "--frames", "3"});
	CAPTURE(headless.Output);
	CHECK(headless.ExitCode == 0);
	CHECK(Contains(headless.Output, "The game has no scripts"));
	CHECK_FALSE(Contains(headless.Output, ".NET runtime started"));
}

TEST_CASE(
	"Runtime: an exported game runs with its own data folder, a project with its play data, a test run with a temporary one it removes")
{
	// What the player logs when it gives the game a folder that it removes at exit.
	std::string_view const removedAtExit = "', a test run's folder removed at exit";
	// The folder the player logs that it gave the game; empty when it logged none.
	auto const dataFolder = [](std::string_view output) -> std::string
	{
		std::string_view const prefix = "The game keeps its data in '";
		size_t const start = output.find(prefix);
		if (start == std::string_view::npos)
		{
			return {};
		}
		size_t const end = output.find('\'', start + prefix.size());
		return end == std::string_view::npos ? std::string()
		                                     : std::string(output.substr(start + prefix.size(), end - start - prefix.size()));
	};

	TestGame project(ProjectFileKind::Project);
	project.AddScene("Pass", MakeScene("Pass", ScriptFieldMap{}));
	project.AddScripts();
	std::string const file = Native(project.Write("Scenes/Pass.sscene"));

	std::filesystem::path const player = FileSystem::PathFromUtf8(STRADA_RUNTIME_PATH);

	// A development run: the project's play data, which nothing creates until the game writes there. The player of a build
	// logs in the engine's shared folder.
	Testing::IsolatedUserRoot const developmentRoot;
	ProcessResult const development = RunPlayer({"--project", file, "--headless", "--frames", "3"}, developmentRoot);
	CAPTURE(development.Output);
	CHECK(development.ExitCode == 0);
	std::filesystem::path const playData = (project.GetDirectory() / ".strada" / "PlayData").lexically_normal();
	CHECK(dataFolder(development.Output) == FileSystem::PathToUtf8(playData));
	CHECK_FALSE(Contains(development.Output, removedAtExit));
	CHECK_FALSE(FileSystem::Exists(project.GetDirectory() / ".strada"));
	CHECK(Contains(ReadText(GetLogFile(developmentRoot.GetEngineDirectory(), player)), "The game keeps its data in"));
	CHECK(FileSystem::IsDirectory(developmentRoot.GetEngineDirectory() / "Crashes"));
	CHECK_FALSE(FileSystem::Exists(developmentRoot.GetGameDirectory(GameIdentity{"", "Runtime Test"})));

	// A test run: a folder of its own, gone when the run ends. The run writes nothing there, so the line the player logs is
	// what shows that it gave the service a temporary folder (GameDataTests show that such a folder goes with its contents).
	ProcessResult const test = RunPlayer({"--project", file, "--test"});
	CAPTURE(test.Output);
	CHECK(test.ExitCode == 0);
	std::string const temporary = dataFolder(test.Output);
	REQUIRE_FALSE(temporary.empty());
	CHECK(temporary != FileSystem::PathToUtf8(playData));
	CHECK(Contains(temporary, "StradaGameData-"));
	CHECK(Contains(test.Output, "The game keeps its data in '" + temporary + std::string(removedAtExit)));
	CHECK_FALSE(FileSystem::Exists(FileSystem::PathFromUtf8(temporary)));
	CHECK_FALSE(FileSystem::Exists(project.GetDirectory() / ".strada"));

	// An exported game: its own folder below the per-user root, named by its identity (none here, so its name), which the
	// run names and does not create. The player of a build runs it, so the log stays in the shared folder.
	TestGame game(ProjectFileKind::Game);
	game.AddScene("Main", MakeScene("Main", std::nullopt));
	Testing::IsolatedUserRoot const exportedRoot;
	ProcessResult const exported =
		RunPlayer({"--game", Native(game.Write("Scenes/Main.sscene")), "--headless", "--frames", "3"}, exportedRoot);
	CAPTURE(exported.Output);
	CHECK(exported.ExitCode == 0);
	std::filesystem::path const gameData = exportedRoot.GetGameDirectory(GameIdentity{"", "Runtime Test"});
	CHECK(dataFolder(exported.Output) == FileSystem::PathToUtf8(gameData));
	CHECK_FALSE(Contains(exported.Output, removedAtExit));
	CHECK_FALSE(FileSystem::Exists(gameData));
	CHECK_FALSE(FileSystem::Exists(game.GetDirectory() / ".strada"));
	CHECK(Contains(ReadText(GetLogFile(exportedRoot.GetEngineDirectory(), player)), "The game keeps its data in"));
}

TEST_CASE("Runtime: an exported game logs and keeps its crash reports below its own data folder, or in the shared folder "
          "when its game file is missing or names no identity")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path player;
	{
		Testing::AssetManagerScope assets;
		Result<Ref<Project>> created = Project::Create(directory.GetPath() / "Project", "Logging Test", *MakeScene("Main", std::nullopt));
		REQUIRE(created.IsOk());
		Ref<Project> const project = created.GetValue();
		REQUIRE(
			project->ApplySettings(Json::object({{"Identity", Json::object({{"Company", "Strada Tests"}, {"Product", "Logged Game"}})}}))
				.IsOk());
		Result<GameExportSettings> prepared = GameExporter::Prepare(*project, directory.GetPath() / "Export");
		REQUIRE_MESSAGE(prepared.IsOk(), (prepared ? std::string() : prepared.GetError()));
		Result<GameExportResult> exported = GameExporter::Export(prepared.GetValue());
		REQUIRE_MESSAGE(exported.IsOk(), (exported ? std::string() : exported.GetError()));
		REQUIRE(exported.GetValue().Succeeded);
		player = exported.GetValue().Executable;
		if (player.extension() == ".app")
		{
			player = player / "Contents" / "MacOS" / player.stem();
		}
	}
	GameIdentity const identity{"Strada Tests", "Logged Game"};

	// The executable is named after the game, its folders after the identity; the service gets the same folder.
	Testing::IsolatedUserRoot const userRoot;
	ProcessResult const run = RunProgram(player, {"--headless", "--frames", "2"}, userRoot);
	CAPTURE(run.Output);
	CHECK(run.ExitCode == 0);
	std::filesystem::path const gameData = userRoot.GetGameDirectory(identity);
	CHECK(Contains(ReadText(GetLogFile(gameData, player)), "The game keeps its data in '" + FileSystem::PathToUtf8(gameData) + "'"));
	CHECK(FileSystem::IsDirectory(gameData / "Crashes"));
	// Nothing went into the engine's folder.
	CHECK_FALSE(FileSystem::Exists(userRoot.GetEngineDirectory()));

	// A test run of the game logs there too, while its data goes into a temporary folder.
	ProcessResult const test = RunProgram(player, {"--test", "--timeout", "0.1"}, userRoot);
	CAPTURE(test.Output);
	CHECK(Contains(ReadText(GetLogFile(gameData, player)), "StradaGameData-"));
	CHECK_FALSE(FileSystem::Exists(userRoot.GetEngineDirectory()));

	std::filesystem::path gameFile = player.parent_path() / FileSystem::PathFromUtf8(Project::GameFileName);
	if (player.parent_path().filename() == "MacOS")
	{
		gameFile = player.parent_path().parent_path() / "Resources" / FileSystem::PathFromUtf8(Project::GameFileName);
	}
	Result<std::string> const configuration = FileSystem::ReadTextFile(gameFile);
	REQUIRE(configuration.IsOk());
	Result<Json> parsed = ParseJson(configuration.GetValue());
	REQUIRE(parsed.IsOk());
	Json const gameSettings = parsed.TakeValue();

	// A game file that names its identity but cannot run (here its window settings are damaged) still logs below the
	// game's own folder, where the player says why it cannot run: the entry point needs only the header, the name and the
	// identity.
	{
		Json damaged = gameSettings;
		damaged["Game"]["Window"] = "garbage";
		REQUIRE(FileSystem::WriteTextFile(gameFile, DumpJson(damaged)).IsOk());
		Testing::IsolatedUserRoot const damagedRoot;
		ProcessResult const refused = RunProgram(player, {"--headless", "--frames", "2"}, damagedRoot);
		CAPTURE(refused.Output);
		CHECK(refused.ExitCode == 1);
		std::string const log = ReadText(GetLogFile(damagedRoot.GetGameDirectory(identity), player));
		CHECK(Contains(log, "Cannot run the game"));
		CHECK(Contains(log, "Window"));
		CHECK(FileSystem::IsDirectory(damagedRoot.GetGameDirectory(identity) / "Crashes"));
		CHECK_FALSE(FileSystem::Exists(damagedRoot.GetEngineDirectory()));
	}

	// Without a game file beside it whose header, name and identity read, the program logs in the shared folder, where it
	// says why it cannot run (what the player read before the engine started, or what opening the game found).
	auto const runWithoutGame = [&player, &identity](char const* what, std::string_view reason)
	{
		CAPTURE(what);
		Testing::IsolatedUserRoot const sharedRoot;
		ProcessResult const refused = RunProgram(player, {"--headless", "--frames", "2"}, sharedRoot);
		CAPTURE(refused.Output);
		CHECK(refused.ExitCode == 1);
		CHECK(Contains(ReadText(GetLogFile(sharedRoot.GetEngineDirectory(), player)), reason));
		CHECK(FileSystem::IsDirectory(sharedRoot.GetEngineDirectory() / "Crashes"));
		CHECK_FALSE(FileSystem::Exists(sharedRoot.GetGameDirectory(identity)));
	};
	Json projectFile = gameSettings;
	projectFile["Strada"]["Type"] = "Project";
	Json unnamedIdentity = gameSettings;
	unnamedIdentity["Game"]["Identity"] = Json::array();

	REQUIRE(FileSystem::WriteTextFile(gameFile, "{ \"Strada\": ").IsOk());
	runWithoutGame("a game file that is not JSON", "Cannot run the game");
	REQUIRE(FileSystem::WriteTextFile(gameFile, DumpJson(unnamedIdentity)).IsOk());
	runWithoutGame("a game file whose identity is not an object", "Application initialization failed");
	REQUIRE(FileSystem::WriteTextFile(gameFile, DumpJson(projectFile)).IsOk());
	runWithoutGame("a project file in the game file's place", "Cannot run the game");
	REQUIRE(FileSystem::RemoveAll(gameFile).IsOk());
	runWithoutGame("no game file", "Cannot run the game");
}

TEST_CASE("Runtime: games in directories with non-ASCII names start from the command line")
{
	// Characters outside every ANSI code page: Windows passes them to main as '?', so the player reads the wide command line.
	TestGame game(ProjectFileKind::Game);
	game.AddScene("Main", MakeScene("Main", std::nullopt));
	static_cast<void>(game.Write("Scenes/Main.sscene"));
	Testing::TemporaryDirectory directory;
	std::filesystem::path const copy =
		directory.GetPath() / FileSystem::PathFromUtf8("\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0 \xC3\x9C\xCE\xB1");
	REQUIRE(FileSystem::CopyDirectory(game.GetDirectory(), copy).IsOk());

	ProcessResult const run =
		RunPlayer({"--game", Native(copy / FileSystem::PathFromUtf8(Project::GameFileName)), "--headless", "--frames", "2"});
	CAPTURE(run.Output);
	CHECK(run.ExitCode == 0);
	CHECK(Contains(run.Output, "The game has no scripts"));
}

TEST_CASE("Runtime: games that cannot start and wrong command lines fail before running")
{
	TestGame game(ProjectFileKind::Game);
	game.AddScene("Main", MakeScene("Main", std::nullopt));
	std::string const withoutStartScene = Native(game.Write(""));

	ProcessResult const help = RunPlayer({"--help"});
	CHECK(help.ExitCode == 0);
	CHECK(Contains(help.Output, "--project"));

	for (std::vector<std::string> const& arguments : std::vector<std::vector<std::string>>{
			 {"--game", withoutStartScene, "--project", "Game.sproj"},
			 {"--game", withoutStartScene, "--test", "--screenshot", "Shot.png"},
			 {"--game", withoutStartScene, "--timeout", "5"},
			 {"--game", withoutStartScene, "--test", "--timestep", "0"},
			 {"--game", withoutStartScene, "--unknown"},
			 {"--game", Native(game.GetDirectory() / "Missing.sgame"), "--headless", "--screenshot", "Shot.png"},
			 {"--game", withoutStartScene, "--resolution", "1920"},
			 {"--game", withoutStartScene, "--resolution", "0x1080"},
			 {"--game", withoutStartScene, "--headless", "--resolution", "1920x1080"},
		 })
	{
		ProcessResult const result = RunPlayer(arguments);
		CAPTURE(result.Output);
		CHECK(result.ExitCode == 2);
	}

	ProcessResult const missing = RunPlayer({"--game", Native(game.GetDirectory() / "Missing.sgame"), "--test"});
	CAPTURE(missing.Output);
	CHECK(missing.ExitCode == 1);
	CHECK(Contains(missing.Output, "Cannot run the game"));

	ProcessResult const noStartScene = RunPlayer({"--game", withoutStartScene, "--test"});
	CAPTURE(noStartScene.Output);
	CHECK(noStartScene.ExitCode == 1);
	CHECK(Contains(noStartScene.Output, "the game has no start scene"));

	ProcessResult const noScene = RunPlayer({"--game", withoutStartScene, "--test", "--scene", "Scenes/Missing.sscene"});
	CAPTURE(noScene.Output);
	CHECK(noScene.ExitCode == 1);
	CHECK(Contains(noScene.Output, "the game has no scene 'Scenes/Missing.sscene'"));
}

#ifdef ST_ENABLE_PROFILING
TEST_CASE("Runtime: a profiled run saves a capture of its frames and logs each subsystem's median")
{
	TestGame game(ProjectFileKind::Game);
	game.AddScene("Pass", MakeScene("Pass", ScriptFieldMap{}));
	game.AddScripts();
	std::string const file = Native(game.Write("Scenes/Pass.sscene"));
	Testing::TemporaryDirectory const directory;
	std::filesystem::path const capturePath = directory.GetPath() / "Run.json";

	ProcessResult const run = RunPlayer({"--game", file, "--headless", "--frames", "20", "--profile", Native(capturePath)});
	CAPTURE(run.Output);
	CHECK(run.ExitCode == 0);
	CHECK(Contains(run.Output, "Profile: 20 frames, frame median "));
	CHECK(Contains(run.Output, " ms, 0 zones dropped"));
	CHECK(Contains(run.Output, "Profile subsystem Scene: "));
	CHECK(Contains(run.Output, "Profile subsystem Application: "));
	CHECK(Contains(run.Output, "Profile zone Script.Strada.Tests.SelfTest: "));
	CHECK(Contains(run.Output, "Saved the profile of 20 frames"));

	Result<ProfilerCapture> const capture = ProfilerCaptureSerializer::Load(capturePath);
	REQUIRE_MESSAGE(capture.IsOk(), (capture ? std::string() : capture.GetError()));
	REQUIRE(capture.GetValue().Frames.size() == 20);
	CHECK(capture.GetValue().Threads.front().Name == "Main");
	size_t sceneUpdates = 0;
	for (ProfilerFrame const& frame : capture.GetValue().Frames)
	{
		for (ProfilerZone const& zone : frame.Zones)
		{
			sceneUpdates += capture.GetValue().Names[zone.Name] == "Scene.Update" ? 1 : 0;
		}
	}
	CHECK(sceneUpdates == 20);

	// A test run is profiled too, and keeps its exit code.
	ProcessResult const tested = RunPlayer({"--game", file, "--test", "--profile", Native(directory.GetPath() / "Test.json")});
	CAPTURE(tested.Output);
	CHECK(tested.ExitCode == 0);
	CHECK(ProfilerCaptureSerializer::Load(directory.GetPath() / "Test.json").IsOk());

	// A capture needs a file in a folder that exists.
	for (std::vector<std::string> const& arguments : std::vector<std::vector<std::string>>{
			 {"--game", file, "--headless", "--profile"},
			 {"--game", file, "--headless", "--profile", Native(directory.GetPath() / "Missing" / "Run.json")},
			 {"--game", file, "--headless", "--profile", Native(directory.GetPath())},
		 })
	{
		ProcessResult const result = RunPlayer(arguments);
		CAPTURE(result.Output);
		CHECK(result.ExitCode == 2);
		CHECK(Contains(result.Output, "--profile"));
	}
}
#endif

#ifdef ST_ENABLE_PROFILING
TEST_CASE("Runtime: a profiled windowed run logs the renderer's draws, batches and instances per frame")
{
	{
		// The player needs a Vulkan device (this probe's is released at the end of the block) and a display.
		ST_REQUIRE_GPU();
	}
	ST_REQUIRE_DISPLAY();
	TestGame game(ProjectFileKind::Game);
	game.AddScene("Main", MakeScene("Main", std::nullopt));
	Testing::TemporaryDirectory const directory;
	std::filesystem::path const capturePath = directory.GetPath() / "Drawn.json";
	ProcessResult const run =
		RunPlayer({"--game", Native(game.Write("Scenes/Main.sscene")), "--frames", "10", "--profile", Native(capturePath)});
	CAPTURE(run.Output);
	REQUIRE(run.ExitCode == 0);
	// The exit's draws are the player's view's count.
	for (char const* const counter : {"DrawCalls", "Batches", "Instances", "Triangles", "ShadowDrawCalls", "Quads"})
	{
		CAPTURE(counter);
		CHECK(Contains(run.Output, std::string("Profile counter Game/") + counter + ": "));
	}

	Result<ProfilerCapture> const capture = ProfilerCaptureSerializer::Load(capturePath);
	REQUIRE_MESSAGE(capture.IsOk(), (capture ? std::string() : capture.GetError()));
	size_t recorded = 0;
	for (ProfilerFrame const& frame : capture.GetValue().Frames)
	{
		for (ProfilerCounterRecord const& record : frame.Counters)
		{
			CHECK(capture.GetValue().Names[record.View] == "Game");
			for (ProfilerCounterValue const& value : record.Values)
			{
				// Every frame tonemaps at least.
				if (capture.GetValue().Names[value.Name] == "DrawCalls")
				{
					CHECK(value.Value >= 1);
					recorded++;
				}
			}
		}
	}
	CHECK(recorded >= 1);
}
#endif

TEST_CASE("Runtime: a windowed run shows the primary camera's view")
{
	{
		// The player needs a Vulkan device (this probe's is released at the end of the block) and a display.
		ST_REQUIRE_GPU();
	}
	ST_REQUIRE_DISPLAY();
	TestGame game(ProjectFileKind::Game);
	game.AddScene("Main", MakeScene("Main", std::nullopt));
	std::filesystem::path const screenshot = game.GetDirectory() / "Shot.png";
	ProcessResult const result =
		RunPlayer({"--game", Native(game.Write("Scenes/Main.sscene")), "--frames", "3", "--screenshot", Native(screenshot)});
	CAPTURE(result.Output);
	REQUIRE(result.ExitCode == 0);
	CHECK_FALSE(Contains(result.Output, "Graphics validation errors"));

	// The red sky fills the view, with the channels in order.
	Result<Image> image = Image::LoadFromFile(screenshot);
	REQUIRE(image.IsOk());
	uint8_t const* center = image.GetValue().GetPixel(image.GetValue().GetWidth() / 2, image.GetValue().GetHeight() / 2);
	CHECK(center[0] > 150);
	CHECK(center[1] < 40);
	CHECK(center[2] < 40);
	CHECK(Contains(result.Output, "Saved screenshot to"));
	CHECK_FALSE(Contains(result.Output, "No screenshot was saved"));
	CHECK_FALSE(Contains(result.Output, "icon is not used"));

	// --resolution sizes the drawing area in pixels, whatever the display's scale: the picture is of that size. The window
	// is resized before the first frame, and the framebuffer follows within the call on Windows and with the next events
	// elsewhere, so the run gives it a few frames.
	std::filesystem::path const sized = game.GetDirectory() / "Sized.png";
	ProcessResult const resolution = RunPlayer(
		{"--game", Native(game.Write("Scenes/Main.sscene")), "--frames", "10", "--resolution", "320x184", "--screenshot", Native(sized)});
	CAPTURE(resolution.Output);
	REQUIRE(resolution.ExitCode == 0);
	Result<Image> sizedImage = Image::LoadFromFile(sized);
	REQUIRE(sizedImage.IsOk());
	CHECK(sizedImage.GetValue().GetWidth() == 320);
	CHECK(sizedImage.GetValue().GetHeight() == 184);
	CHECK_FALSE(Contains(resolution.Output, "The run stops"));

	// A game whose window settings make it fullscreen runs in a window of that size too.
	game.SetFullscreen(true);
	std::filesystem::path const windowed = game.GetDirectory() / "Windowed.png";
	ProcessResult const fullscreen = RunPlayer({"--game", Native(game.Write("Scenes/Main.sscene")), "--frames", "10", "--resolution",
	                                            "320x184", "--screenshot", Native(windowed)});
	game.SetFullscreen(false);
	CAPTURE(fullscreen.Output);
	REQUIRE(fullscreen.ExitCode == 0);
	Result<Image> windowedImage = Image::LoadFromFile(windowed);
	REQUIRE(windowedImage.IsOk());
	CHECK(windowedImage.GetValue().GetWidth() == 320);
	CHECK(windowedImage.GetValue().GetHeight() == 184);

#if defined(ST_PLATFORM_WINDOWS)
	// Windows keeps a window within about the desktop's size, so no window has a drawing area this large: the run stops in
	// the frame that judges its size, naming both sizes, and takes no screenshot.
	std::filesystem::path const tooLarge = game.GetDirectory() / "TooLarge.png";
	ProcessResult const oversized = RunPlayer({"--game", Native(game.Write("Scenes/Main.sscene")), "--frames", "10", "--resolution",
	                                           "16384x16384", "--screenshot", Native(tooLarge)});
	CAPTURE(oversized.Output);
	CHECK(oversized.ExitCode == 1);
	CHECK(Contains(oversized.Output, "The run stops: --resolution asked for 16384x16384 pixels, but the window's drawing area is "));
	CHECK_FALSE(Contains(oversized.Output, "Saved screenshot"));
	CHECK_FALSE(FileSystem::Exists(tooLarge));
#endif

	// The project's icon becomes the window's; one that is not a texture is reported and left out.
	Image icon(32, 32, 3);
	REQUIRE(icon.WritePNG(game.GetDirectory() / "Assets" / "Icon.png").IsOk());
	game.SetIcon("Icon.png");
	ProcessResult const withIcon = RunPlayer({"--game", Native(game.Write("Scenes/Main.sscene")), "--frames", "2"});
	CAPTURE(withIcon.Output);
	CHECK(withIcon.ExitCode == 0);
	CHECK_FALSE(Contains(withIcon.Output, "icon is not used"));
	game.SetIcon("Scenes/Main.sscene");
	ProcessResult const wrongIcon = RunPlayer({"--game", Native(game.Write("Scenes/Main.sscene")), "--frames", "2"});
	CAPTURE(wrongIcon.Output);
	CHECK(wrongIcon.ExitCode == 0);
	CHECK(Contains(wrongIcon.Output, "icon is not used"));
}

TEST_CASE("Runtime: the player asks for its screenshot in the first frame drawn at or after the one requested, once")
{
	// Frame 4 is not drawn (a minimized window runs no update, or the swapchain gave no image): the request waits for frame 5.
	ScreenshotRequest request(FileSystem::PathFromUtf8("Shot.png"), 4);
	CHECK(request.IsWanted());
	CHECK_FALSE(request.TakeRequest(3, true));
	CHECK_FALSE(request.TakeRequest(4, false));
	CHECK(request.TakeRequest(5, true));
	CHECK(request.IsRequested());
	CHECK_FALSE(request.TakeRequest(6, true));
	CHECK(Contains(request.DescribeFailure(), "before the capture was made"));
	request.SetSaved();
	CHECK(request.IsSaved());
	CHECK(request.DescribeFailure().empty());

	// A frame passed by while nothing was drawn is not waited for again: the next drawn frame takes it.
	ScreenshotRequest late(FileSystem::PathFromUtf8("Late.png"), 2);
	CHECK(late.TakeRequest(40, true));

	// A run that never drew a frame at or after the one requested says so.
	ScreenshotRequest missed(FileSystem::PathFromUtf8("Missed.png"), 59);
	CHECK_FALSE(missed.TakeRequest(59, false));
	CHECK(Contains(missed.DescribeFailure(), "before a frame at or after frame 59 was drawn"));

	// A capture or a write that failed gives its reason.
	ScreenshotRequest failed(FileSystem::PathFromUtf8("Failed.png"), 0);
	CHECK(failed.TakeRequest(0, true));
	failed.SetFailed("the disk is full");
	CHECK(failed.DescribeFailure() == "the disk is full");

	// Without a path no screenshot is wanted, asked for or missed.
	ScreenshotRequest none;
	CHECK_FALSE(none.IsWanted());
	CHECK_FALSE(none.TakeRequest(100, true));
	CHECK(none.DescribeFailure().empty());
}

TEST_CASE("Runtime: a run asked for a resolution judges its drawing area once, in the first frame drawn from the settle frame on")
{
	// The frames before the settle frame may still be of the old size where a resize arrives with later events.
	ResolutionRequest request(glm::uvec2(1920, 1080));
	CHECK(request.IsWanted());
	CHECK(request.Judge(0, glm::uvec2(1600, 900)).empty());
	CHECK(request.Judge(ResolutionRequest::SettleFrame - 1, glm::uvec2(1600, 900)).empty());
	CHECK_FALSE(request.IsJudged());
	CHECK(request.Judge(ResolutionRequest::SettleFrame, glm::uvec2(1920, 1080)).empty());
	CHECK(request.IsJudged());
	// Judged once: a later resize by the user is the game's.
	CHECK(request.Judge(ResolutionRequest::SettleFrame + 1, glm::uvec2(800, 600)).empty());
	CHECK(request.DescribeUnjudged().empty());

	// A drawing area the system kept smaller ends the run, naming both sizes; the first frame drawn at or after the settle
	// frame judges it, whichever it is.
	ResolutionRequest clamped(glm::uvec2(3840, 2160));
	CHECK(clamped.Judge(ResolutionRequest::SettleFrame + 5, glm::uvec2(3840, 2124)) ==
	      "--resolution asked for 3840x2160 pixels, but the window's drawing area is 3840x2124 (the system keeps a window within "
	      "the size it allows, on Windows about the desktop's)");
	CHECK(clamped.IsJudged());
	CHECK(clamped.Judge(ResolutionRequest::SettleFrame + 6, glm::uvec2(3840, 2124)).empty());
	CHECK(clamped.DescribeUnjudged().empty());

	// A run too short to judge its drawing area is judged at its end by the last frame drawn.
	ResolutionRequest shortRun(glm::uvec2(320, 184));
	CHECK(shortRun.Judge(0, glm::uvec2(320, 200)).empty());
	CHECK(Contains(shortRun.DescribeUnjudged(), "but the window's drawing area is 320x200"));
	CHECK(shortRun.Judge(1, glm::uvec2(320, 184)).empty());
	CHECK(shortRun.DescribeUnjudged().empty());
	ResolutionRequest undrawn(glm::uvec2(320, 184));
	CHECK(undrawn.DescribeUnjudged().empty());

	// Without --resolution nothing is judged.
	ResolutionRequest none;
	CHECK_FALSE(none.IsWanted());
	CHECK(none.Judge(ResolutionRequest::SettleFrame, glm::uvec2(1, 1)).empty());
	CHECK(none.DescribeUnjudged().empty());
}

TEST_CASE("Runtime: a windowed run that saves no screenshot ends with an error line and exit code 1")
{
	{
		// The player needs a Vulkan device (this probe's is released at the end of the block) and a display.
		ST_REQUIRE_GPU();
	}
	ST_REQUIRE_DISPLAY();
	TestGame game(ProjectFileKind::Game);
	game.AddScene("Main", MakeScene("Main", std::nullopt));
	game.AddScene("Quitter", MakeScene("Quitter", ScriptFieldMap{{"Quit", ScriptFieldValue::FromBool(true)}}));
	game.AddScripts();
	std::string const file = Native(game.Write("Scenes/Main.sscene"));

	// The file cannot be written: its folder is a file.
	std::filesystem::path const blocker = game.GetDirectory() / "NotAFolder";
	REQUIRE(FileSystem::WriteTextFile(blocker, "a file").IsOk());
	std::filesystem::path const unwritable = blocker / "Shot.png";
	ProcessResult const written = RunPlayer({"--game", file, "--frames", "3", "--screenshot", Native(unwritable)});
	CAPTURE(written.Output);
	CHECK(written.ExitCode == 1);
	CHECK(Contains(written.Output, "No screenshot was saved to"));
	CHECK_FALSE(Contains(written.Output, "Saved screenshot"));
	CHECK_FALSE(FileSystem::Exists(unwritable));

	// The game quits at its first update, long before frame 60, so no frame is drawn to capture.
	std::filesystem::path const screenshot = game.GetDirectory() / "Quit.png";
	ProcessResult const quit = RunPlayer({"--game", file, "--scene", "Scenes/Quitter.sscene", "--screenshot", Native(screenshot)});
	CAPTURE(quit.Output);
	CHECK(quit.ExitCode == 1);
	CHECK(Contains(quit.Output, "before a frame at or after frame 60 was drawn"));
	CHECK_FALSE(FileSystem::Exists(screenshot));
}

TEST_CASE("Runtime: a windowed run of a menu without a camera shows its UI over black")
{
	{
		// The player needs a Vulkan device (this probe's is released at the end of the block) and a display.
		ST_REQUIRE_GPU();
	}
	ST_REQUIRE_DISPLAY();
	TestGame game(ProjectFileKind::Game);
	// A title screen without a camera (a sky, which needs one to show, included): a red panel over the middle half of the
	// screen with a green button at its bottom and a title.
	Ref<Scene> const title = CreateRef<Scene>("Title");
	title->CreateEntity("Sky").AddComponent<SkyLightComponent>().AmbientColor = glm::vec3(1.0f);
	Entity canvas = title->CreateEntity("Canvas");
	canvas.AddComponent<UICanvasComponent>();
	Entity panel = title->CreateEntity("Panel", canvas);
	UIRectComponent& panelRect = panel.AddComponent<UIRectComponent>();
	panelRect.AnchorMin = {0.25f, 0.25f};
	panelRect.AnchorMax = {0.75f, 0.75f};
	panelRect.Size = {0.0f, 0.0f};
	panel.AddComponent<UIImageComponent>().Color = {1.0f, 0.0f, 0.0f, 1.0f};
	Entity heading = title->CreateEntity("Heading", panel);
	UIRectComponent& headingRect = heading.AddComponent<UIRectComponent>();
	headingRect.AnchorMin = {0.0f, 0.0f};
	headingRect.AnchorMax = {1.0f, 0.0f};
	headingRect.Pivot = {0.5f, 0.0f};
	headingRect.Size = {0.0f, 150.0f};
	heading.AddComponent<UITextComponent>().Text = "TITLE";
	Entity button = title->CreateEntity("Play", panel);
	UIRectComponent& buttonRect = button.AddComponent<UIRectComponent>();
	buttonRect.AnchorMin = {0.5f, 1.0f};
	buttonRect.AnchorMax = {0.5f, 1.0f};
	buttonRect.Pivot = {0.5f, 1.0f};
	buttonRect.Position = {0.0f, -20.0f};
	buttonRect.Size = {400.0f, 120.0f};
	button.AddComponent<UIImageComponent>();
	button.AddComponent<UIButtonComponent>().NormalColor = {0.0f, 1.0f, 0.0f, 1.0f};
	game.AddScene("Title", title);
	// A scene with nothing to show without a camera.
	Ref<Scene> const empty = CreateRef<Scene>("Empty");
	empty->CreateEntity("Sky").AddComponent<SkyLightComponent>();
	game.AddScene("Empty", empty);

	std::filesystem::path const screenshot = game.GetDirectory() / "Title.png";
	ProcessResult const result =
		RunPlayer({"--game", Native(game.Write("Scenes/Title.sscene")), "--frames", "3", "--screenshot", Native(screenshot)});
	CAPTURE(result.Output);
	REQUIRE(result.ExitCode == 0);
	CHECK_FALSE(Contains(result.Output, "nothing is drawn"));
	CHECK_FALSE(Contains(result.Output, "Graphics validation errors"));

	// The 1920 x 1080 canvas fits the 16:10 window's width: the panel spans the middle half of the window, with the button
	// a tenth of the window high at its bottom (from 63% to 73% of the height) and a fifth of it wide. The rest is black.
	// The window's framebuffer follows the display's scale, so places are fractions of the image.
	Result<Image> image = Image::LoadFromFile(screenshot);
	REQUIRE(image.IsOk());
	auto const checkPixel = [&image](float x, float y, glm::ivec3 const& expected)
	{
		Image const& shot = image.GetValue();
		uint32_t const column = std::min(static_cast<uint32_t>(x * static_cast<float>(shot.GetWidth())), shot.GetWidth() - 1);
		uint32_t const row = std::min(static_cast<uint32_t>(y * static_cast<float>(shot.GetHeight())), shot.GetHeight() - 1);
		uint8_t const* pixel = shot.GetPixel(column, row);
		CAPTURE(column);
		CAPTURE(row);
		for (int channel = 0; channel < 3; channel++)
		{
			CAPTURE(channel);
			CHECK(std::abs(static_cast<int>(pixel[channel]) - expected[channel]) <= 8);
		}
	};
	checkPixel(0.02f, 0.02f, {0, 0, 0});
	checkPixel(0.98f, 0.98f, {0, 0, 0});
	checkPixel(0.3f, 0.5f, {255, 0, 0});
	checkPixel(0.5f, 0.68f, {0, 255, 0});

	// Without a camera or screen-space content nothing is drawn, and the player says so.
	ProcessResult const nothing = RunPlayer({"--game", Native(game.Write("Scenes/Empty.sscene")), "--frames", "2"});
	CAPTURE(nothing.Output);
	CHECK(nothing.ExitCode == 0);
	CHECK(Contains(nothing.Output, "has no primary camera and no screen-space UI: nothing is drawn"));
}

TEST_CASE("Runtime: exported games run from their own directory")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path player;
	{
		Testing::AssetManagerScope assets;
		Ref<Scene> const pass = MakeScene("Pass", ScriptFieldMap{{"ExpectedWindowWidth", ScriptFieldValue::FromInt32(1280)},
		                                                         {"ExpectedWindowHeight", ScriptFieldValue::FromInt32(720)}});
		Result<Ref<Project>> created = Project::Create(directory.GetPath() / "Project", "Exported Test", *pass);
		REQUIRE(created.IsOk());
		Ref<Project> const project = created.GetValue();
		std::filesystem::path const scenes = project->GetAssetDirectory() / "Scenes";
		REQUIRE(SceneSerializer::SaveToFile(*MakeScene("Fail", ScriptFieldMap{{"Fail", ScriptFieldValue::FromBool(true)}}),
		                                    scenes / "Fail.sscene")
		            .IsOk());
		// Scripts built elsewhere: the test scripts.
		std::filesystem::path const binaries = project->GetDirectory() / "Scripts" / "Binaries";
		for (char const* file : {"Strada.TestScripts.dll", "Strada.TestScripts.deps.json"})
		{
			REQUIRE(FileSystem::Copy(Testing::GetTestScriptsPath().parent_path() / file, binaries / file, true).IsOk());
		}
		REQUIRE(project->ApplySettings(Json::object({{"ScriptModule", "Scripts/Binaries/Strada.TestScripts.dll"}})).IsOk());

		Result<GameExportSettings> prepared = GameExporter::Prepare(*project, directory.GetPath() / "Export");
		REQUIRE_MESSAGE(prepared.IsOk(), (prepared ? std::string() : prepared.GetError()));
		Result<GameExportResult> exported = GameExporter::Export(prepared.GetValue());
		REQUIRE_MESSAGE(exported.IsOk(), (exported ? std::string() : exported.GetError()));
		REQUIRE(exported.GetValue().Succeeded);
		player = exported.GetValue().Executable;
		if (player.extension() == ".app")
		{
			player = player / "Contents" / "MacOS" / player.stem();
		}
	}
	// Nothing is left of the project: the game carries everything it needs.
	REQUIRE(FileSystem::RemoveAll(directory.GetPath() / "Project").IsOk());

	Testing::IsolatedUserRoot const userRoot;
	ProcessResult const passed = RunProgram(player, {"--test"}, userRoot);
	CAPTURE(passed.Output);
	CHECK(passed.ExitCode == 0);
	CHECK(Contains(passed.Output, "Tests passed: 3 checks in 1 frame"));
	// Its log is below its own data folder, named by its name (the project sets no identity).
	CHECK(Contains(ReadText(GetLogFile(userRoot.GetGameDirectory(GameIdentity{"", "Exported Test"}), player)),
	               "Tests passed: 3 checks in 1 frame"));

	ProcessResult const failed = RunProgram(player, {"--test", "--scene", "Scenes/Fail.sscene"});
	CAPTURE(failed.Output);
	CHECK(failed.ExitCode == 1);
	CHECK(Contains(failed.Output, "Tests failed: 1 of 2 checks failed"));
}

TEST_CASE("Runtime: players tell once at startup what is wrong with the engine marker beside them, and still run")
{
	// The build tree's marker names this checkout: nothing to tell.
	ProcessResult const help = RunPlayer({"--help"});
	CAPTURE(help.Output);
	CHECK(help.ExitCode == 0);
	CHECK_FALSE(Contains(help.Output, EngineLayoutWarning));

	Testing::TemporaryDirectory directory;
	std::filesystem::path player;
	{
		Testing::AssetManagerScope assets;
		Result<Ref<Project>> created = Project::Create(directory.GetPath() / "Project", "Marker Test", *MakeScene("Main", std::nullopt));
		REQUIRE(created.IsOk());
		Result<GameExportSettings> prepared = GameExporter::Prepare(*created.GetValue(), directory.GetPath() / "Export");
		REQUIRE_MESSAGE(prepared.IsOk(), (prepared ? std::string() : prepared.GetError()));
		Result<GameExportResult> exported = GameExporter::Export(prepared.GetValue());
		REQUIRE_MESSAGE(exported.IsOk(), (exported ? std::string() : exported.GetError()));
		REQUIRE(exported.GetValue().Succeeded);
		player = exported.GetValue().Executable;
		if (player.extension() == ".app")
		{
			player = player / "Contents" / "MacOS" / player.stem();
		}
	}

	// An exported game carries no marker: nothing to tell.
	ProcessResult const plain = RunProgram(player, {"--headless", "--frames", "2"});
	CAPTURE(plain.Output);
	CHECK(plain.ExitCode == 0);
	CHECK_FALSE(Contains(plain.Output, EngineLayoutWarning));

	// A damaged marker where the player reads one (Contents/Resources of a macOS bundle) is told once, and the game runs.
	std::filesystem::path resources = player.parent_path();
	if (resources.filename() == "MacOS")
	{
		resources = resources.parent_path() / "Resources";
	}
	REQUIRE(FileSystem::CreateDirectories(resources).IsOk());
	REQUIRE(FileSystem::WriteTextFile(resources / EngineLayout::ManifestFileName, "{ \"Strada\": ").IsOk());
	ProcessResult const damaged = RunProgram(player, {"--headless", "--frames", "2"});
	CAPTURE(damaged.Output);
	CHECK(damaged.ExitCode == 0);
	CHECK(CountOccurrences(damaged.Output, EngineLayoutWarning) == 1);
	CHECK(Contains(damaged.Output, std::string(EngineLayout::ManifestFileName) + "' is ignored: "));
}

TEST_CASE("Runtime: projects whose only scripts are visual scripts run them without a C# project")
{
	TestGame project(ProjectFileKind::Project);
	{
		Testing::AssetManagerScope assets;
		REQUIRE(FileSystem::CreateDirectories(project.GetDirectory() / "Assets").IsOk());
		REQUIRE(AssetManager::OpenAssetDirectory(project.GetDirectory() / "Assets").IsOk());
		Ref<Scene> const scene = MakeScene("Graphs", std::nullopt);
		AddChecksGraph(*scene);
		project.AddScene("Main", scene);
	}
	std::string const file = Native(project.Write("Scenes/Main.sscene"));

	// The scripting runtime starts without game scripts, and the graph's checks pass and finish the run.
	ProcessResult const passed = RunPlayer({"--project", file, "--test"});
	CAPTURE(passed.Output);
	CHECK(passed.ExitCode == 0);
	CHECK(Contains(passed.Output, ".NET runtime started"));
	CHECK(Contains(passed.Output, "Test passed: graph created"));
	CHECK(Contains(passed.Output, "Tests passed: 2 checks in 1 frame"));
	CHECK_FALSE(Contains(passed.Output, "The game has no scripts"));
}

TEST_CASE("Runtime: exported games whose only scripts are visual scripts carry the scripting API and run them")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path player;
	std::filesystem::path scripts;
	{
		Testing::AssetManagerScope assets;
		Result<Ref<Project>> created = Project::Create(directory.GetPath() / "Project", "Visual Test", *MakeScene("Main", std::nullopt));
		REQUIRE(created.IsOk());
		Ref<Project> const project = created.GetValue();
		Ref<Scene> const main = MakeScene("Main", std::nullopt);
		AddChecksGraph(*main);
		REQUIRE(SceneSerializer::SaveToFile(*main, project->GetAssetDirectory() / "Scenes" / "Main.sscene").IsOk());

		Result<GameExportSettings> prepared = GameExporter::Prepare(*project, directory.GetPath() / "Export");
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
	// Nothing is left of the project: the game carries the scripting API, without game scripts.
	REQUIRE(FileSystem::RemoveAll(directory.GetPath() / "Project").IsOk());
	CHECK(FileSystem::IsRegularFile(scripts / "Strada.ScriptCore.dll"));
	CHECK(FileSystem::IsRegularFile(scripts / "Strada.ScriptCore.runtimeconfig.json"));
	CHECK_FALSE(FileSystem::Exists(scripts / "Game.dll"));

	ProcessResult const passed = RunProgram(player, {"--test"});
	CAPTURE(passed.Output);
	CHECK(passed.ExitCode == 0);
	CHECK(Contains(passed.Output, "Tests passed: 2 checks in 1 frame"));
}

TEST_CASE("Runtime: an exported game skips the development-only nodes of its visual scripts, which its project runs")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path projectFile;
	std::filesystem::path player;
	{
		Testing::AssetManagerScope assets;
		Result<Ref<Project>> created =
			Project::Create(directory.GetPath() / "Project", "Development Test", *MakeScene("Main", std::nullopt));
		REQUIRE(created.IsOk());
		Ref<Project> const project = created.GetValue();
		Ref<Scene> const main = MakeScene("Main", std::nullopt);
		AddChecksGraph(*main, MakeDevelopmentChecksDocument());
		REQUIRE(SceneSerializer::SaveToFile(*main, project->GetAssetDirectory() / "Scenes" / "Main.sscene").IsOk());
		projectFile = project->GetFilePath();

		Result<GameExportSettings> prepared = GameExporter::Prepare(*project, directory.GetPath() / "Export");
		REQUIRE_MESSAGE(prepared.IsOk(), (prepared ? std::string() : prepared.GetError()));
		Result<GameExportResult> exported = GameExporter::Export(prepared.GetValue());
		REQUIRE_MESSAGE(exported.IsOk(), (exported ? std::string() : exported.GetError()));
		REQUIRE(exported.GetValue().Succeeded);
		player = exported.GetValue().Executable;
		if (player.extension() == ".app")
		{
			player = player / "Contents" / "MacOS" / player.stem();
		}
	}

	// The project runs the development-only node.
	ProcessResult const project = RunPlayer({"--project", Native(projectFile), "--test"});
	CAPTURE(project.Output);
	CHECK(project.ExitCode == 0);
	CHECK(Contains(project.Output, "Test passed: development node ran"));
	CHECK(Contains(project.Output, "Test passed: graph created"));
	CHECK(Contains(project.Output, "Tests passed: 3 checks in 1 frame"));

	// The exported game passes the flow through it.
	ProcessResult const game = RunProgram(player, {"--test"});
	CAPTURE(game.Output);
	CHECK(game.ExitCode == 0);
	CHECK_FALSE(Contains(game.Output, "development node ran"));
	CHECK(Contains(game.Output, "Test passed: graph created"));
	CHECK(Contains(game.Output, "Tests passed: 2 checks in 1 frame"));
}

TEST_CASE("Runtime: an exported game of graphs calls a function library and broadcasts, and skips the library's development-only "
          "nodes, which its project runs")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path projectFile;
	std::filesystem::path player;
	{
		Testing::AssetManagerScope assets;
		Result<Ref<Project>> created = Project::Create(directory.GetPath() / "Project", "Library Test", *MakeScene("Main", std::nullopt));
		REQUIRE(created.IsOk());
		Ref<Project> const project = created.GetValue();
		AssetHandle const library = AddGraphAsset(MakeMathsLibrary(), "Maths");
		AssetHandle const receiver = AddGraphAsset(MakeReceiverDocument(), "Receiver");
		AssetHandle const broadcaster = AddGraphAsset(MakeBroadcasterDocument(library), "Broadcaster");
		Ref<Scene> const main = MakeScene("Main", std::nullopt);
		main->CreateEntity("Broadcaster").AddComponent<VisualScriptComponent>().Graph = broadcaster;
		for (char const* name : {"Receiver A", "Receiver B"})
		{
			main->CreateEntity(name).AddComponent<VisualScriptComponent>().Graph = receiver;
		}
		REQUIRE(SceneSerializer::SaveToFile(*main, project->GetAssetDirectory() / "Scenes" / "Main.sscene").IsOk());
		projectFile = project->GetFilePath();

		Result<GameExportSettings> prepared = GameExporter::Prepare(*project, directory.GetPath() / "Export");
		REQUIRE_MESSAGE(prepared.IsOk(), (prepared ? std::string() : prepared.GetError()));
		CHECK(prepared.GetValue().UsesVisualScripts);
		CHECK(prepared.GetValue().ScriptProjectFile.empty());
		Result<GameExportResult> exported = GameExporter::Export(prepared.GetValue());
		REQUIRE_MESSAGE(exported.IsOk(), (exported ? std::string() : exported.GetError()));
		REQUIRE(exported.GetValue().Succeeded);
		player = exported.GetValue().Executable;
		if (player.extension() == ".app")
		{
			player = player / "Contents" / "MacOS" / player.stem();
		}
	}
	char const* const checks[] = {"Test passed: Receiver A", "Test passed: Receiver B",
	                              "Test passed: the broadcast reached both receivers"};

	// The project runs the library's development-only node.
	ProcessResult const project = RunPlayer({"--project", Native(projectFile), "--test"});
	CAPTURE(project.Output);
	CHECK(project.ExitCode == 0);
	CHECK(Contains(project.Output, "Test passed: library development node ran"));
	for (char const* check : checks)
	{
		CAPTURE(check);
		CHECK(Contains(project.Output, check));
	}
	CHECK(Contains(project.Output, "Tests passed: 4 checks in 1 frame"));

	// The exported game passes the flow through it, and the library's functions run in the graph that calls them.
	ProcessResult const game = RunProgram(player, {"--test"});
	CAPTURE(game.Output);
	CHECK(game.ExitCode == 0);
	CHECK_FALSE(Contains(game.Output, "development node ran"));
	for (char const* check : checks)
	{
		CAPTURE(check);
		CHECK(Contains(game.Output, check));
	}
	CHECK(Contains(game.Output, "Tests passed: 3 checks in 1 frame"));
}
