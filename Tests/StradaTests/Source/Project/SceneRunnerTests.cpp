#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/KeyCodes.h"
#include "Strada/Project/SceneRunner.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace Strada;

namespace
{
	// Input is global: tests that set it restore it.
	class InputScope
	{
	public:
		InputScope() { Input::Reset(); }
		~InputScope() { Input::Reset(); }

		InputScope(InputScope const&) = delete;
		InputScope& operator=(InputScope const&) = delete;
	};

	// A scene whose SceneSwitcher loads nextScene when L is pressed.
	void WriteSwitcherScene(std::filesystem::path const& path, std::string const& name, std::string const& nextScene)
	{
		Scene scene(name);
		Entity switcher = scene.CreateEntity("Switcher");
		ScriptComponent& script = switcher.AddComponent<ScriptComponent>();
		script.ClassName = "Strada.Tests.SceneSwitcher";
		script.Fields["NextScene"] = ScriptFieldValue::FromString(nextScene);
		REQUIRE(SceneSerializer::SaveToFile(scene, path).IsOk());
	}

	// One frame with the key pressed.
	void UpdateWithKey(SceneRunner& runner, KeyCode key)
	{
		Input::SetKeyState(key, true);
		runner.Update(Timestep(1.0f / 60.0f));
		Input::EndFrame();
		Input::SetKeyState(key, false);
		Input::EndFrame();
	}

	std::vector<std::string> GetResultNames(TestRunReport const& report)
	{
		std::vector<std::string> names;
		for (ScriptTestResult const& result : report.Results)
		{
			names.push_back(result.Name);
		}
		return names;
	}
}

TEST_CASE("SceneRunner: scenes run, load the scenes their scripts ask for and report tests")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	InputScope input;
	Testing::TemporaryDirectory directory;
	std::filesystem::path const scenes = directory.GetPath() / "Scenes";
	REQUIRE(FileSystem::CreateDirectories(scenes).IsOk());
	WriteSwitcherScene(scenes / "First.sscene", "First", "Scenes/Second.sscene");
	WriteSwitcherScene(scenes / "Second.sscene", "Second", "Scenes/Broken.sscene");
	REQUIRE(FileSystem::WriteTextFile(scenes / "Broken.sscene", "{ not a scene").IsOk());
	REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());

	// Scenes are read as assets of the asset directory.
	CHECK(LoadSceneAsset(AssetHandle()).IsError());
	CHECK(LoadSceneAsset(AssetManager::FindByPath("Scenes/Broken.sscene")).IsError());
	Result<Ref<Scene>> loaded = LoadSceneAsset(AssetManager::FindByPath("Scenes/First.sscene"));
	REQUIRE_MESSAGE(loaded.IsOk(), (loaded ? std::string() : loaded.GetError()));
	CHECK(loaded.GetValue()->GetName() == "First");

	SceneRunner runner({});
	Ref<Scene> const first = loaded.GetValue();
	first->OnViewportResize(640, 480);
	runner.Start(first);
	REQUIRE(runner.IsRunning());
	CHECK(ScriptEngine::GetHost() == &runner);
	CHECK_FALSE(runner.IsEditor());
	CHECK(GetResultNames(runner.GetTestReport()) == std::vector<std::string>{"First started"});

	// A load happens after the frame: the current scene stops first, the new one renders to the same view.
	uint64_t const version = runner.GetSceneVersion();
	UpdateWithKey(runner, KeyCode::L);
	REQUIRE(runner.IsRunning());
	CHECK(runner.GetScene()->GetName() == "Second");
	CHECK(runner.GetScene()->IsRunning());
	CHECK(runner.GetSceneVersion() != version);
	CHECK(runner.GetScene()->GetViewportWidth() == 640);
	CHECK(runner.GetScene()->GetViewportHeight() == 480);
	CHECK(GetResultNames(runner.GetTestReport()) == std::vector<std::string>{"First started", "First stopped", "Second started"});

	// A scene file that cannot be read keeps the current scene running.
	uint64_t const logStart = Log::GetNextEntryIndex();
	UpdateWithKey(runner, KeyCode::L);
	CHECK(runner.GetScene()->GetName() == "Second");
	CHECK(Testing::WasLogged(logStart, "SceneManager.LoadScene: "));

	// Quitting is the owner's decision; exceptions and the end of the run are counted.
	UpdateWithKey(runner, KeyCode::Q);
	CHECK(runner.IsQuitRequested());
	CHECK(runner.IsRunning());
	UpdateWithKey(runner, KeyCode::T);
	CHECK(runner.GetTestReport().ScriptExceptions == 1);
	CHECK_FALSE(runner.GetTestReport().Finished);
	UpdateWithKey(runner, KeyCode::F);
	CHECK(runner.GetTestReport().Finished);
	CHECK(runner.GetTestReport().GetFailedCount() == 0);
	CHECK(runner.GetTestReport().GetFailureCount() == 1);

	runner.Stop();
	CHECK_FALSE(runner.IsRunning());
	CHECK(ScriptEngine::GetHost() == nullptr);
	CHECK(ScriptEngine::GetInstanceCount() == 0);
	CHECK(GetResultNames(runner.GetTestReport()).back() == "Second stopped");
}

TEST_CASE("SceneRunner: simulating runs physics without scripts or sounds")
{
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting;
	SceneRunnerSettings settings;
	settings.Runtime.RunScripts = false;
	settings.Runtime.PlayAudio = false;
	settings.IsEditor = true;
	SceneRunner runner(settings);
	CHECK(runner.IsEditor());

	Ref<Scene> scene = CreateRef<Scene>("Simulated");
	Entity ball = scene->CreateEntity("Ball");
	ball.GetComponent<TransformComponent>().Translation = {0.0f, 5.0f, 0.0f};
	ball.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
	ball.AddComponent<SphereColliderComponent>();
	ball.AddComponent<ScriptComponent>().ClassName = "Strada.Tests.Mover";
	runner.Start(scene);
	CHECK(scene->GetPhysicsScene() != nullptr);
	CHECK(scene->GetAudioScene() == nullptr);
	CHECK(ScriptEngine::GetInstanceCount() == 0);
	for (int frame = 0; frame < 30; frame++)
	{
		runner.Update(Timestep(1.0f / 60.0f));
	}
	CHECK(ball.GetComponent<TransformComponent>().Translation.y < 5.0f);
}
