#include "Audio/AudioTestUtilities.h"
#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/MixerAsset.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/GameData.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/KeyCodes.h"
#include "Strada/Core/Log.h"
#include "Strada/Project/SceneRunner.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/SceneSerializer.h"
#include "Strada/Script/PlayerSettings.h"
#include "Strada/Script/VisualScriptDebugger.h"
#include "Strada/Script/VisualScriptEngine.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
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

TEST_CASE("SceneRunner: simulating runs no visual scripts")
{
	Testing::PhysicsSystemScope physics;
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	// A graph that runs when created.
	VisualScriptDocument document;
	VisualScriptNode create;
	create.ID = 1;
	create.Type = "Event.Create";
	document.Graph.Nodes.push_back(create);
	AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(document), "Created");
	Ref<Scene> scene = CreateRef<Scene>("Simulated");
	Entity ball = scene->CreateEntity("Ball");
	ball.AddComponent<VisualScriptComponent>().Graph = graph;

	SceneRunnerSettings settings;
	settings.Runtime.RunScripts = false;
	settings.Runtime.PlayAudio = false;
	settings.IsEditor = true;
	{
		SceneRunner runner(settings);
		runner.Start(scene);
		for (int frame = 0; frame < 3; frame++)
		{
			runner.Update(Timestep(1.0f / 60.0f));
		}
		CHECK(VisualScriptEngine::GetInstanceCount() == 0);
		CHECK(ScriptEngine::GetSceneContext() == nullptr);
	}

	// Playing runs it.
	settings.Runtime.RunScripts = true;
	SceneRunner player(settings);
	player.Start(scene);
	CHECK(VisualScriptEngine::HasInstance(ball.GetUUID()));
	player.Stop();
	CHECK(VisualScriptEngine::GetInstanceCount() == 0);
}

TEST_CASE("SceneRunner: ApplyRequests loads the scene a script asked for between frames, as Update would after its frame")
{
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	Testing::TemporaryDirectory directory;
	std::filesystem::path const scenes = directory.GetPath() / "Scenes";
	REQUIRE(FileSystem::CreateDirectories(scenes).IsOk());
	Scene second("Second");
	second.CreateEntity("Arrival");
	REQUIRE(SceneSerializer::SaveToFile(second, scenes / "Second.sscene").IsOk());
	REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());

	// Loads the second scene on every update, at node 11, where the debugger stops it.
	VisualScriptDocument loader;
	VisualScriptNode update;
	update.ID = 10;
	update.Type = "Event.Update";
	VisualScriptNode load;
	load.ID = 11;
	load.Type = "Call:Strada.SceneManager.LoadScene(string)";
	load.Inputs = Json::object({{"Path", "Scenes/Second.sscene"}});
	loader.Graph.Nodes = {update, load};
	loader.Graph.Links.push_back({{10, "Then"}, {11, "Execute"}});
	AssetHandle const graph = AssetManager::AddMemoryAsset(CreateRef<VisualScriptAsset>(std::move(loader)), "Loader");
	Ref<Scene> const first = CreateRef<Scene>("First");
	first->CreateEntity("Loader").AddComponent<VisualScriptComponent>().Graph = graph;
	REQUIRE(VisualScriptDebugger::Attach().IsOk());
	VisualScriptDebugConfiguration configuration;
	configuration.Revision = 1;
	VisualScriptBreakpointSetting breakpoint;
	breakpoint.Document = graph;
	breakpoint.Node = 11;
	breakpoint.Type = "Call:Strada.SceneManager.LoadScene(string)";
	configuration.Breakpoints.push_back(breakpoint);
	REQUIRE(VisualScriptDebugger::Configure(configuration).IsOk());

	SceneRunner runner({});
	first->OnViewportResize(320, 240);
	runner.Start(first);
	// Nothing asked: nothing happens.
	uint64_t const version = runner.GetSceneVersion();
	runner.ApplyRequests();
	CHECK(runner.GetSceneVersion() == version);

	// The frame stopped the loader before it asked; continuing between frames asks, and the request waits for the runner.
	runner.Update(Timestep(1.0f / 60.0f));
	CHECK(runner.GetScene()->GetName() == "First");
	Result<VisualScriptResumeOutcome> const resumed = VisualScriptDebugger::Resume(std::nullopt, VisualScriptDebugAction::Continue);
	REQUIRE_MESSAGE(resumed.IsOk(), resumed.GetError());
	CHECK(resumed.GetValue().Result == VisualScriptResumeResult::Ended);
	CHECK(runner.GetScene()->GetName() == "First");
	CHECK(runner.GetSceneVersion() == version);

	// Applying the requests loads it as the end of the frame that ran the call would have: the first scene stops, the second
	// runs in the same view.
	runner.ApplyRequests();
	REQUIRE(runner.IsRunning());
	CHECK(runner.GetScene()->GetName() == "Second");
	CHECK(runner.GetScene()->IsRunning());
	CHECK_FALSE(first->IsRunning());
	CHECK(runner.GetSceneVersion() != version);
	CHECK(runner.GetScene()->GetViewportWidth() == 320);
	uint64_t const loaded = runner.GetSceneVersion();
	runner.ApplyRequests();
	CHECK(runner.GetSceneVersion() == loaded);
	runner.Stop();
}

TEST_CASE("SceneRunner: a game mixes through its project's mixer, follows its edits, and ends with the mix's game values reset")
{
	Testing::AudioEngineScope engine;
	Testing::AssetManagerScope assets;
	MixerBus music;
	music.Name = "Music";
	music.VolumeDb = -6.0f;
	Mixer mixer;
	mixer.Buses.push_back(music);
	mixer.Snapshots.push_back(MixerSnapshot{
		.Name = "Paused",
		.Values = {MixerSnapshotValue{.Bus = "Music",
	                                  .Target = MixerSnapshotTarget::Volume,
	                                  .Effect = 0,
	                                  .Parameter = MixerEffectParameter::Frequency,
	                                  .Value = -30.0f}},
	});
	TypedDataAssetType<Mixer> const* type = DataAssetRegistry::Find<Mixer>();
	REQUIRE(type != nullptr);
	auto const asset = std::static_pointer_cast<TypedDataAsset<Mixer>>(type->CreateAsset());
	REQUIRE(asset->SetData(mixer).IsOk());
	AssetHandle const handle = AssetManager::AddMemoryAsset(asset, "Game mixer");

	SceneRunnerSettings settings;
	settings.Runtime.RunScripts = false;
	settings.Runtime.Mixer = handle;
	uint64_t const logStart = Log::GetNextEntryIndex();
	{
		SceneRunner runner(settings);
		// What an earlier game left is not this one's.
		AudioEngine::SetMixer(mixer);
		AudioEngine::SetBusVolume("Music", -20.0f);
		REQUIRE(AudioEngine::GetBusVolume("Music") == -20.0f);
		REQUIRE(AudioEngine::TransitionTo("Paused", 1.0f));
		runner.Start(CreateRef<Scene>("Mixed"));
		CHECK(AudioEngine::GetMixer() == mixer);
		CHECK(AudioEngine::GetBusVolume("Music") == 0.0f);
		CHECK(AudioEngine::GetCurrentSnapshot().empty());
		CHECK(Testing::WasLogged(logStart, "Mixer: 2 buses from"));
		CHECK(Testing::WasLogged(logStart, "(Master, Music)"));

		// An edit of the asset (the inspector's, mixer.set's) is mixed from the next update on.
		mixer.Buses[1].VolumeDb = -9.0f;
		REQUIRE(asset->SetData(mixer).IsOk());
		runner.Update(Timestep(1.0f / 60.0f));
		CHECK(AudioEngine::GetMixer().Buses[1].VolumeDb == -9.0f);
		AudioEngine::SetBusMuted("Music", true);
		runner.Update(Timestep(1.0f / 60.0f));
		CHECK(AudioEngine::IsBusMuted("Music"));
		// A snapshot the game blends to holds through the asset's edits.
		REQUIRE(AudioEngine::TransitionTo("paused", 0.5f));
		mixer.Buses[1].VolumeDb = -8.0f;
		REQUIRE(asset->SetData(mixer).IsOk());
		runner.Update(Timestep(1.0f / 60.0f));
		CHECK(AudioEngine::GetCurrentSnapshot() == "Paused");
	}
	// The game ended: its values are gone, the mix is back at the mixer's own values, and the mixer stays until the next
	// game applies its own.
	CHECK_FALSE(AudioEngine::IsBusMuted("Music"));
	CHECK(AudioEngine::GetCurrentSnapshot().empty());
	CHECK(AudioEngine::GetMixer() == mixer);

	// A game without a mixer mixes through Master alone; one whose mixer does not load does too, and says so once.
	settings.Runtime.Mixer = AssetHandle();
	{
		SceneRunner runner(settings);
		runner.Start(CreateRef<Scene>("Unmixed"));
		CHECK(AudioEngine::GetMixer() == Mixer{});
	}
	AudioEngine::SetMixer(mixer);
	settings.Runtime.Mixer = AssetHandle(UUID(12345));
	uint64_t const missingStart = Log::GetNextEntryIndex();
	{
		SceneRunner runner(settings);
		runner.Start(CreateRef<Scene>("Missing"));
		CHECK(AudioEngine::GetMixer() == Mixer{});
		runner.Update(Timestep(1.0f / 60.0f));
		runner.Update(Timestep(1.0f / 60.0f));
	}
	std::vector<LogEntry> const entries = Log::GetEntries(missingStart);
	CHECK(std::count_if(entries.begin(), entries.end(),
	                    [](LogEntry const& entry)
	                    {
							return entry.Message.find("does not load as a Mixer asset") != std::string::npos;
						}) == 1);
}

TEST_CASE("SceneRunner: a game loads the player's settings, applies their volumes after the game's reset, hands the window "
          "to its owner after the frame, and clears them at its end")
{
	Testing::AudioEngineScope engine;
	Testing::AssetManagerScope assets;
	Testing::ScriptRuntimeScope runtime;
	MixerBus music;
	music.Name = "Music";
	Mixer mixer;
	mixer.Buses.push_back(music);
	TypedDataAssetType<Mixer> const* type = DataAssetRegistry::Find<Mixer>();
	REQUIRE(type != nullptr);
	auto const asset = std::static_pointer_cast<TypedDataAsset<Mixer>>(type->CreateAsset());
	REQUIRE(asset->SetData(mixer).IsOk());
	AssetHandle const handle = AssetManager::AddMemoryAsset(asset, "Game mixer");

	// What the player chose in an earlier run, in the game data folder.
	Testing::TemporaryDirectory directory;
	PlayerSettings saved;
	saved.Window = {PlayerWindowMode::Fullscreen, 800, 600, false};
	saved.Quality = QualityPreset::Low;
	saved.MasterVolume = 0.5f;
	saved.Buses.push_back({"music", -6.0f, false});
	Result<std::string> const text = WritePlayerSettingsText(saved);
	REQUIRE(text.IsOk());
	REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "Settings.json", text.GetValue()).IsOk());
	GameDataScope const gameData(directory.GetPath(), GameDataLifetime::Kept);

	SceneRunnerSettings settings;
	settings.Runtime.RunScripts = false;
	settings.Runtime.Mixer = handle;
	settings.PlayerDefaults.Window = {PlayerWindowMode::Windowed, 1280, 720, true};
	std::vector<PlayerWindowSettings> applied;
	settings.ApplyWindow = [&applied](PlayerWindowSettings const& window)
	{
		applied.push_back(window);
	};
	{
		SceneRunner runner(settings);
		// What an earlier game left is not this one's: the game's values start reset, the player's from the file.
		AudioEngine::SetMixer(mixer);
		AudioEngine::SetBusVolume("Music", -20.0f);
		AudioEngine::SetMasterVolume(0.25f);
		runner.Start(CreateRef<Scene>("Settled"));
		CHECK(PlayerSettingsStore::GetStatus() == PlayerSettingsStatus::Present);
		CHECK(PlayerSettingsStore::Get() == saved);
		CHECK(AudioEngine::GetBusVolume("Music") == 0.0f);
		CHECK(AudioEngine::GetMasterVolume() == 1.0f);
		CHECK(AudioEngine::GetPlayerBusVolume("Music") == -6.0f);
		CHECK(AudioEngine::GetPlayerMasterVolume() == 0.5f);
		// The settings' setters reach the mix at once while the game plays.
		PlayerSettingsStore::SetBusVolume("MUSIC", -3.0f);
		PlayerSettingsStore::SetMasterVolume(0.75f);
		CHECK(AudioEngine::GetPlayerBusVolume("Music") == -3.0f);
		CHECK(AudioEngine::GetPlayerMasterVolume() == 0.75f);
		// The opening window was the player's to apply before the game started: nothing is asked of the owner for it.
		runner.Update(Timestep(1.0f / 60.0f));
		CHECK(applied.empty());

		// A changed window reaches the owner after the frame, once; the last of a frame's changes is the one applied.
		PlayerWindowSettings const windowed = {PlayerWindowMode::Windowed, 1024, 768, true};
		PlayerSettingsStore::SetWindow(windowed);
		CHECK(applied.empty());
		runner.Update(Timestep(1.0f / 60.0f));
		REQUIRE(applied.size() == 1);
		CHECK(applied[0] == windowed);
		runner.Update(Timestep(1.0f / 60.0f));
		CHECK(applied.size() == 1);
		PlayerSettingsStore::SetWindow({PlayerWindowMode::Fullscreen, 1024, 768, true});
		PlayerSettingsStore::SetWindow({PlayerWindowMode::Fullscreen, 1600, 900, false});
		runner.ApplyRequests();
		REQUIRE(applied.size() == 2);
		CHECK(applied[1] == PlayerWindowSettings{PlayerWindowMode::Fullscreen, 1600, 900, false});
		// A change the game makes as it stops is dropped with it.
		PlayerSettingsStore::SetWindow(windowed);
	}
	CHECK(applied.size() == 2);
	// The game ended: the settings are cleared, and the player's stage of the mix is back to unchanged.
	CHECK(PlayerSettingsStore::GetStatus() == PlayerSettingsStatus::Unavailable);
	CHECK(PlayerSettingsStore::Get() == PlayerSettings{});
	CHECK(AudioEngine::GetPlayerBusVolume("Music") == 0.0f);
	CHECK(AudioEngine::GetPlayerMasterVolume() == 1.0f);

	// Simulating (no sound) and an owner that applies no window (the editor's play mode): the settings load and change, the
	// mix and the window do not.
	settings.Runtime.PlayAudio = false;
	settings.ApplyWindow = {};
	{
		SceneRunner runner(settings);
		runner.Start(CreateRef<Scene>("Simulated"));
		CHECK(PlayerSettingsStore::Get().Quality == QualityPreset::Low);
		CHECK(AudioEngine::GetPlayerMasterVolume() == 1.0f);
		PlayerSettingsStore::SetMasterVolume(0.2f);
		CHECK(AudioEngine::GetPlayerMasterVolume() == 1.0f);
		PlayerSettingsStore::SetWindow({PlayerWindowMode::Windowed, 640, 480, true});
		runner.Update(Timestep(1.0f / 60.0f));
		CHECK(PlayerSettingsStore::Get().Window == PlayerWindowSettings{PlayerWindowMode::Windowed, 640, 480, true});
	}
	CHECK(applied.size() == 2);
	CHECK(PlayerSettingsStore::GetStatus() == PlayerSettingsStatus::Unavailable);
	// Nothing was written: only a game's save writes the file.
	CHECK(ReadPlayerSettingsFile(directory.GetPath() / "Settings.json", {}).Settings == saved);
}
