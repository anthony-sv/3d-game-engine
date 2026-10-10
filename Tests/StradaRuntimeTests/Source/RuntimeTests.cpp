#include "GpuTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Image.h"
#include "Strada/Platform/Process.h"
#include "Strada/Project/Project.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>

#include <chrono>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	constexpr uint32_t WindowWidth = 320;
	constexpr uint32_t WindowHeight = 200;

	// Runs the player to completion.
	ProcessResult RunPlayer(std::vector<std::string> arguments)
	{
		ProcessSpecification specification;
		specification.Executable = FileSystem::PathFromUtf8(STRADA_RUNTIME_PATH);
		specification.Arguments = std::move(arguments);
		specification.Timeout = std::chrono::minutes(2);
		Result<ProcessResult> result = Process::Run(specification);
		REQUIRE_MESSAGE(result.IsOk(), (result ? std::string() : result.GetError()));
		REQUIRE_FALSE(result.GetValue().TimedOut);
		return result.TakeValue();
	}

	bool Contains(std::string_view text, std::string_view part)
	{
		return text.find(part) != std::string_view::npos;
	}

	// A scene seen by a primary camera against a red sky; with fields, an entity runs Strada.Tests.SelfTest with them.
	Ref<Scene> MakeScene(std::string const& name, std::optional<ScriptFieldMap> selfTest)
	{
		Ref<Scene> scene = CreateRef<Scene>(name);
		scene->CreateEntity("Camera").AddComponent<CameraComponent>();
		scene->CreateEntity("Sky").AddComponent<SkyLightComponent>().AmbientColor = glm::vec3(1.0f, 0.0f, 0.0f);
		scene->GetSettings().Renderer.Tonemapper = TonemapOperator::None;
		scene->GetSettings().Renderer.Dithering = false;
		if (selfTest)
		{
			ScriptComponent& script = scene->CreateEntity("Tester").AddComponent<ScriptComponent>();
			script.ClassName = "Strada.Tests.SelfTest";
			script.Fields = std::move(*selfTest);
		}
		return scene;
	}

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

		// Registers the scenes (as the editor did) and writes the configuration; startScene is relative to Assets, or empty.
		std::filesystem::path Write(std::string const& startScene) const
		{
			ProjectSettings settings;
			settings.Name = "Runtime Test";
			settings.ScriptModule =
				FileSystem::PathToUtf8(FileSystem::GetRelativePath(GetScriptDirectory(), GetDirectory()) / "Strada.TestScripts.dll");
			settings.Window.Width = WindowWidth;
			settings.Window.Height = WindowHeight;
			{
				Testing::AssetManagerScope assets;
				REQUIRE(FileSystem::CreateDirectories(GetDirectory() / "Assets").IsOk());
				REQUIRE(AssetManager::OpenAssetDirectory(GetDirectory() / "Assets").IsOk());
				if (!startScene.empty())
				{
					settings.StartScene = AssetManager::FindByPath(startScene);
					REQUIRE(settings.StartScene.IsValid());
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
	};

	std::string Native(std::filesystem::path const& path)
	{
		return FileSystem::PathToNativeUtf8(path);
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

TEST_CASE("Runtime: a windowed run shows the primary camera's view")
{
	TestGame game(ProjectFileKind::Game);
	game.AddScene("Main", MakeScene("Main", std::nullopt));
	std::filesystem::path const screenshot = game.GetDirectory() / "Shot.png";
	ProcessResult const result =
		RunPlayer({"--game", Native(game.Write("Scenes/Main.sscene")), "--frames", "3", "--screenshot", Native(screenshot)});
	CAPTURE(result.Output);
	if (result.ExitCode != 0 && Testing::IsMissingDisplayAllowed())
	{
		MESSAGE("Skipping the windowed run: the player could not start (no display or GPU)");
		return;
	}
	REQUIRE(result.ExitCode == 0);
	CHECK_FALSE(Contains(result.Output, "Graphics validation errors"));

	// The red sky fills the view, with the channels in order.
	Result<Image> image = Image::LoadFromFile(screenshot);
	REQUIRE(image.IsOk());
	uint8_t const* center = image.GetValue().GetPixel(image.GetValue().GetWidth() / 2, image.GetValue().GetHeight() / 2);
	CHECK(center[0] > 150);
	CHECK(center[1] < 40);
	CHECK(center[2] < 40);
}
