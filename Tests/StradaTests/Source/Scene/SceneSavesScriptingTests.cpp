#include "Audio/AudioTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/MixerAsset.h"
#include "Strada/Audio/AudioEngine.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/GameData.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Project/SceneRunner.h"
#include "Strada/Renderer/QualityPresets.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/PlayerSettings.h"
#include "Strada/Script/SaveSlots.h"
#include "Strada/Script/ScriptField.h"

#include <doctest/doctest.h>

#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;

// What scripts keep through Strada.SaveSlot and Strada.Settings (Tests/TestScripts' SaveSlotProbe and SettingsProbe): the
// scripting side of Docs/Architecture.md §13.1, with a game data folder and without one.
namespace
{
	Entity AddProbe(Scene& scene, std::string const& className, ScriptFieldMap fields = {})
	{
		Entity entity = scene.CreateEntity("Probe");
		ScriptComponent& script = entity.AddComponent<ScriptComponent>();
		script.ClassName = className;
		script.Fields = std::move(fields);
		return entity;
	}

	// The settings are the process's: each case starts and ends with none loaded.
	class SettingsScope
	{
	public:
		SettingsScope() { PlayerSettingsStore::Clear(); }
		~SettingsScope() { PlayerSettingsStore::Clear(); }

		SettingsScope(SettingsScope const&) = delete;
		SettingsScope& operator=(SettingsScope const&) = delete;
	};
}

TEST_CASE("SceneSaves: a script saves a slot of every value type, reads it back in another letter case, is refused what a slot "
          "cannot hold, lists and deletes it")
{
	Testing::ScriptEngineScope scripting;
	Testing::TemporaryDirectory directory;
	GameDataScope const gameData(directory.GetPath(), GameDataLifetime::Kept);
	std::filesystem::path const file = directory.GetPath() / "Saves" / "probe.ssave";

	// Deleted at the end of its run.
	{
		Scene scene("Saves");
		Entity probe = AddProbe(scene, "Strada.Tests.SaveSlotProbe");
		uint64_t const logStart = Log::GetNextEntryIndex();
		scene.OnRuntimeStart();
		CHECK(probe.GetName() == "SaveSlots:ok");
		scene.OnRuntimeStop();
		// A value read as another type is warned about once per object and name.
		CHECK(Testing::CountLogged(logStart, "SaveSlot.Get: slot 'probe', value 'Text': it holds a value of type String, not Int32") == 1);
		CHECK_FALSE(FileSystem::Exists(file));
	}

	// Kept: the file holds every value in its type.
	Scene scene("Saves");
	Entity probe = AddProbe(scene, "Strada.Tests.SaveSlotProbe", {{"Keep", ScriptFieldValue::FromBool(true)}});
	scene.OnRuntimeStart();
	CHECK(probe.GetName() == "SaveSlots:ok");
	UUID const self = probe.GetUUID();
	scene.OnRuntimeStop();
	Result<std::string> const text = FileSystem::ReadTextFile(file);
	REQUIRE(text.IsOk());
	SaveSlotRead const read = ReadSaveSlotText(text.GetValue());
	REQUIRE(read.Status == SaveSlotStatus::Present);
	ScriptFieldMap const& values = read.Data.Values;
	CHECK(values.size() == 21);
	CHECK(values.at("Bool") == ScriptFieldValue::FromBool(true));
	CHECK(values.at("Int") == ScriptFieldValue::FromInt32(-3));
	CHECK(values.at("ULong") == ScriptFieldValue::FromUInt64(18'000'000'000'000'000'000ull));
	CHECK(values.at("Best") == ScriptFieldValue::FromFloat(92.25f));
	CHECK(values.at("Text") == ScriptFieldValue::FromString("Strada Moto"));
	CHECK(values.at("Self") == ScriptFieldValue::FromEntity(self));
	CHECK(values.at("Key").GetType() == ScriptFieldType::Int32);
	CHECK(values.at("Rotation") == ScriptFieldValue::FromQuaternion(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
	CHECK(values.at("Splits") ==
	      ScriptFieldValue::FromList(ScriptFieldType::Float, {ScriptFieldValue::FromFloat(30.5f), ScriptFieldValue::FromFloat(61.25f)}));
	CHECK(values.at("Points").GetType() == MakeScriptFieldListType(ScriptFieldType::Vector3));
	CHECK(values.at("Laps").GetType() == MakeScriptFieldListType(ScriptFieldType::Int32));
	CHECK_FALSE(values.contains("Gone"));
	CHECK(SaveSlotStore::GetNames() == std::vector<std::string>{"probe"});
}

TEST_CASE("SceneSaves: a slot whose file reads reaches a script whole, though the engine writes its values longer than a slot "
          "file may be")
{
	Testing::ScriptEngineScope scripting;
	Testing::TemporaryDirectory directory;
	GameDataScope const gameData(directory.GetPath(), GameDataLifetime::Kept);

	// Lists of 4,096 Vector4 written compact with short numbers, as a hand or another tool may write them: under 3 MB on
	// disk, and past MaxSaveSlotBytes as the engine writes them (indented, each 0.1 as the double the float is). One list's
	// share of the engine's text is measured, so that the case holds what it says.
	std::string list = "[";
	for (size_t index = 0; index < MaxScriptFieldListElements; index++)
	{
		list += index == 0 ? "[0.1,0.1,0.1,0.1]" : ",[0.1,0.1,0.1,0.1]";
	}
	list += "]";
	auto const makeText = [&list](size_t lists)
	{
		std::string made = R"({"Strada":{"Version":1,"Type":"SaveSlot"},"SaveSlot":{"Values":{)";
		for (size_t index = 0; index < lists; index++)
		{
			made += index == 0 ? "\"List" : ",\"List";
			made += std::to_string(index);
			made += R"(":{"Type":"Vector4[]","Value":)";
			made += list;
			made += "}";
		}
		made += "}}}";
		return made;
	};
	constexpr size_t Lists = 36;
	SaveSlotRead const one = ReadSaveSlotText(makeText(1));
	REQUIRE(one.Status == SaveSlotStatus::Present);
	Result<std::string> const written = WriteSaveSlotText(one.Data);
	Result<std::string> const empty = WriteSaveSlotText({});
	REQUIRE((written.IsOk() && empty.IsOk()));
	REQUIRE(Lists * (written.GetValue().size() - empty.GetValue().size()) > MaxSaveSlotBytes);
	std::string const text = makeText(Lists);
	REQUIRE(text.size() < MaxSaveSlotBytes);
	REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "Saves" / "large.ssave", text).IsOk());

	Scene scene("Saves");
	Entity probe = AddProbe(
		scene, "Strada.Tests.SaveSlotProbe",
		{{"Slot", ScriptFieldValue::FromString("Large")}, {"ExpectedLists", ScriptFieldValue::FromInt32(static_cast<int32_t>(Lists))}});
	uint64_t const logStart = Log::GetNextEntryIndex();
	scene.OnRuntimeStart();
	CHECK(probe.GetName() == "SaveSlots:ok");
	scene.OnRuntimeStop();
	CHECK_FALSE(Testing::WasLogged(logStart, "SaveSlot.Open"));
}

TEST_CASE("SceneSaves: without a game data folder a script's slot is Unavailable and logged, keeps its values in memory, and its "
          "saves and deletes answer false")
{
	Testing::ScriptEngineScope scripting;
	REQUIRE_FALSE(GameData::GetDirectory().has_value());
	Scene scene("Saves");
	Entity probe = AddProbe(scene, "Strada.Tests.SaveSlotProbe", {{"Available", ScriptFieldValue::FromBool(false)}});
	uint64_t const logStart = Log::GetNextEntryIndex();
	scene.OnRuntimeStart();
	CHECK(probe.GetName() == "SaveSlots:ok");
	scene.OnRuntimeStop();
	CHECK(Testing::WasLogged(logStart, "SaveSlot.Open: no game runs (no game data folder): slot 'Probe' is empty, and nothing is saved"));
	CHECK(Testing::WasLogged(logStart, "SaveSlot.Save: slot 'probe' is not saved: no game runs"));
	CHECK(Testing::WasLogged(logStart, "SaveSlot.Delete: slot 'probe' is not deleted: no game runs"));
}

TEST_CASE("SceneSaves: a script's settings apply at once and the window reaches the runner's owner after the frame; Save writes "
          "Settings.json, and without a game data folder it answers false")
{
	Testing::AudioEngineScope engine;
	Testing::AssetManagerScope assets;
	Testing::ScriptEngineScope scripting;
	SettingsScope const settingsScope;
	MixerBus music;
	music.Name = "Music";
	Mixer mixer;
	mixer.Buses.push_back(music);
	TypedDataAssetType<Mixer> const* type = DataAssetRegistry::Find<Mixer>();
	REQUIRE(type != nullptr);
	auto const asset = std::static_pointer_cast<TypedDataAsset<Mixer>>(type->CreateAsset());
	REQUIRE(asset->SetData(mixer).IsOk());
	AssetHandle const handle = AssetManager::AddMemoryAsset(asset, "Game mixer");

	SceneRunnerSettings settings;
	settings.Runtime.Mixer = handle;
	settings.PlayerDefaults.Window = {PlayerWindowMode::Windowed, 1280, 720, true};
	std::vector<PlayerWindowSettings> applied;
	settings.ApplyWindow = [&applied](PlayerWindowSettings const& window)
	{
		applied.push_back(window);
	};

	Testing::TemporaryDirectory directory;
	{
		GameDataScope const gameData(directory.GetPath(), GameDataLifetime::Kept);
		SceneRunner runner(settings);
		Ref<Scene> const scene = CreateRef<Scene>("Settings");
		Entity probe = AddProbe(*scene, "Strada.Tests.SettingsProbe");
		uint64_t const logStart = Log::GetNextEntryIndex();
		runner.Start(scene);
		CHECK(probe.GetName() == "Settings:ok");
		// A value read as another type is warned about once per name while the game's scripts are loaded.
		CHECK(Testing::CountLogged(logStart, "Settings.Get: value 'Share': it holds a value of type Float, not Int32") == 1);
		// At once: the quality and the player's stage of the mix.
		CHECK(PlayerSettingsStore::Get().Quality == QualityPreset::Low);
		CHECK(AudioEngine::GetPlayerMasterVolume() == 0.5f);
		CHECK(AudioEngine::GetPlayerBusVolume("Music") == -6.0f);
		CHECK(AudioEngine::IsPlayerBusMuted("Music"));
		// The game's own stage is untouched.
		CHECK(AudioEngine::GetBusVolume("Music") == 0.0f);
		CHECK(AudioEngine::GetMasterVolume() == 1.0f);
		// The window: the frame's last request, once its update has returned (the probe asked from OnCreate, before the
		// first frame).
		CHECK(applied.empty());
		runner.Update(Timestep(1.0f / 60.0f));
		REQUIRE(applied.size() == 1);
		CHECK(applied[0] == PlayerWindowSettings{PlayerWindowMode::Windowed, 400, 100, false});
		runner.Update(Timestep(1.0f / 60.0f));
		CHECK(applied.size() == 1);
		runner.Stop();
	}
	PlayerSettingsRead const saved = ReadPlayerSettingsFile(directory.GetPath() / "Settings.json", {});
	REQUIRE(saved.Status == PlayerSettingsStatus::Present);
	CHECK(saved.Settings.Quality == QualityPreset::Low);
	CHECK(saved.Settings.MasterVolume == 0.5f);
	CHECK(saved.Settings.Window == PlayerWindowSettings{PlayerWindowMode::Windowed, 400, 100, false});
	REQUIRE(saved.Settings.Buses.size() == 1);
	CHECK(saved.Settings.Buses[0] == PlayerBusSettings{"Music", -6.0f, true});
	CHECK(saved.Settings.Values == ScriptFieldMap{{"LinkedBrakes", ScriptFieldValue::FromBool(true)}});

	// No game data folder: everything applies, and the save is refused.
	applied.clear();
	{
		SceneRunner runner(settings);
		Ref<Scene> const scene = CreateRef<Scene>("Settings");
		Entity probe = AddProbe(*scene, "Strada.Tests.SettingsProbe", {{"Available", ScriptFieldValue::FromBool(false)}});
		uint64_t const logStart = Log::GetNextEntryIndex();
		runner.Start(scene);
		CHECK(probe.GetName() == "Settings:ok");
		CHECK(Testing::WasLogged(logStart, "Settings.Save: the settings are not saved: no game runs"));
		runner.Stop();
	}
	AssetManager::RemoveMemoryAsset(handle);
}
