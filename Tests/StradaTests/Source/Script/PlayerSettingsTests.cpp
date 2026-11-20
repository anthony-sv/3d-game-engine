#include "Fuzzing.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/Mixer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/GameData.h"
#include "Strada/Core/Log.h"
#include "Strada/Renderer/QualityPresets.h"
#include "Strada/Script/PlayerSettings.h"
#include "Strada/Script/SaveSlots.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

// The player's settings (Docs/Architecture.md §13.1, "The player's settings"; §7.3, "Player settings"): Settings.json,
// read over the game's defaults, its newer and damaged rules, and the store a running game reads and changes.
namespace
{
	// A game whose window is not PlayerSettings' own default, so that a missing field shows whose default it keeps.
	PlayerSettings MakeGameDefaults()
	{
		PlayerSettings defaults;
		defaults.Window.Mode = PlayerWindowMode::Fullscreen;
		defaults.Window.Width = 1920;
		defaults.Window.Height = 1080;
		defaults.Window.VSync = false;
		return defaults;
	}

	// Settings with every field away from its default.
	PlayerSettings MakeChosenSettings()
	{
		PlayerSettings settings;
		settings.Window = {PlayerWindowMode::Windowed, 1600, 900, true};
		settings.Quality = QualityPreset::Low;
		settings.MasterVolume = 0.75f;
		settings.Buses.push_back({"Music", -12.0f, false});
		settings.Buses.push_back({"Engine", 0.0f, true});
		settings.Values["LinkedBrakes"] = ScriptFieldValue::FromBool(true);
		settings.Values["BrakeShare"] = ScriptFieldValue::FromFloat(0.3f);
		settings.Values["Difficulty"] = ScriptFieldValue::FromString("Rider");
		settings.Values["Gears"] =
			ScriptFieldValue::FromList(ScriptFieldType::Int32, {ScriptFieldValue::FromInt32(1), ScriptFieldValue::FromInt32(6)});
		return settings;
	}

	std::string WriteText(PlayerSettings const& settings)
	{
		Result<std::string> text = WritePlayerSettingsText(settings);
		REQUIRE_MESSAGE(text.IsOk(), (text.IsOk() ? std::string() : text.GetError()));
		return text.TakeValue();
	}

	// A settings document of the version over the content (which may be anything).
	std::string MakeSettingsText(std::string_view version, std::string_view content)
	{
		return "{ \"Strada\": { \"Version\": " + std::string(version) +
		       ", \"Type\": \"PlayerSettings\" }, \"PlayerSettings\": " + std::string(content) + " }";
	}

	std::string ReadWholeFile(std::filesystem::path const& path)
	{
		Result<std::string> text = FileSystem::ReadTextFile(path);
		REQUIRE(text.IsOk());
		return text.TakeValue();
	}

	// Whether the document's header names settings of a version above 1 (the fuzz case's rule for Newer).
	bool NamesNewerSettings(Json const& document)
	{
		if (!document.is_object() || !document.contains("Strada") || !document["Strada"].is_object())
		{
			return false;
		}
		Json const& header = document["Strada"];
		if (!header.contains("Type") || header["Type"] != "PlayerSettings" || !header.contains("Version") ||
		    !header["Version"].is_number_integer())
		{
			return false;
		}
		return header["Version"].is_number_unsigned() ? header["Version"].get<uint64_t>() > 1 : header["Version"].get<int64_t>() > 1;
	}

	// The store is global: a case that loads it clears it again, whatever it checks.
	class PlayerSettingsScope
	{
	public:
		PlayerSettingsScope() { PlayerSettingsStore::Clear(); }
		~PlayerSettingsScope() { PlayerSettingsStore::Clear(); }

		PlayerSettingsScope(PlayerSettingsScope const&) = delete;
		PlayerSettingsScope& operator=(PlayerSettingsScope const&) = delete;
	};
}

TEST_CASE("PlayerSettings: settings round trip over the game's defaults, a missing field keeps the game's default, and unknown "
          "fields warn")
{
	PlayerSettings const defaults = MakeGameDefaults();
	PlayerSettings const chosen = MakeChosenSettings();
	std::string const text = WriteText(chosen);
	CHECK(text == DumpJson(WritePlayerSettings(chosen)));
	PlayerSettingsRead const read = ReadPlayerSettingsText(text, defaults);
	REQUIRE_MESSAGE(read.Status == PlayerSettingsStatus::Present, read.Problem);
	CHECK(read.Version == 1);
	CHECK(read.Settings == chosen);
	CHECK(read.Warnings.empty());
	CHECK(WriteText(read.Settings) == text);

	// A file that sets the window's mode alone keeps the game's size and vsync, and PlayerSettings' own defaults for the rest.
	PlayerSettingsRead const partial =
		ReadPlayerSettingsText(MakeSettingsText("1", "{ \"Window\": { \"Mode\": \"Windowed\" } }"), defaults);
	REQUIRE_MESSAGE(partial.Status == PlayerSettingsStatus::Present, partial.Problem);
	PlayerSettings expected = defaults;
	expected.Window.Mode = PlayerWindowMode::Windowed;
	CHECK(partial.Settings == expected);
	CHECK(partial.Settings.Window.Width == 1920);
	CHECK_FALSE(partial.Settings.Window.VSync);
	CHECK(partial.Settings.Quality == QualityPreset::High);

	// A later engine's field is skipped with a warning; what this engine knows reads.
	PlayerSettingsRead const later = ReadPlayerSettingsText(
		MakeSettingsText("1", "{ \"Quality\": \"Medium\", \"RenderScale\": 0.5, \"Window\": { \"Hdr\": true } }"), defaults);
	REQUIRE_MESSAGE(later.Status == PlayerSettingsStatus::Present, later.Problem);
	CHECK(later.Settings.Quality == QualityPreset::Medium);
	REQUIRE(later.Warnings.size() == 2);
	CHECK(later.Warnings[0].find("RenderScale") != std::string::npos);
	CHECK(later.Warnings[1].find("Hdr") != std::string::npos);

	// The fixture of version 1 reads, and writes and reads back alike.
	Result<std::string> const fixture = FileSystem::ReadTextFile(Testing::GetTestDataPath("PlayerSettings/Version1.json"));
	REQUIRE(fixture.IsOk());
	PlayerSettingsRead const kept = ReadPlayerSettingsText(fixture.GetValue(), defaults);
	REQUIRE_MESSAGE(kept.Status == PlayerSettingsStatus::Present, kept.Problem);
	CHECK(kept.Settings.Window == PlayerWindowSettings{PlayerWindowMode::Fullscreen, 1600, 900, false});
	CHECK(kept.Settings.Quality == QualityPreset::Medium);
	CHECK(kept.Settings.MasterVolume == 0.8f);
	CHECK(kept.Settings.Buses.size() == 2);
	CHECK(kept.Settings.Values.size() == 3);
	PlayerSettingsRead const again = ReadPlayerSettingsText(WriteText(kept.Settings), defaults);
	CHECK(again.Settings == kept.Settings);
}

TEST_CASE("PlayerSettings: a newer or damaged file reads as the game's defaults, told apart by its header")
{
	PlayerSettings const defaults = MakeGameDefaults();
	for (std::string const& newer :
	     {MakeSettingsText("2", "{ \"Window\": \"a later form\" }"), MakeSettingsText("7", "[]"),
	      MakeSettingsText("2", "{ \"Pad\": \"" + std::string(MaxPlayerSettingsBytes, 'x') + "\" }"),
	      std::string("{ \"PlayerSettings\": 3, \"Strada\": { \"Type\": \"PlayerSettings\", \"Version\": 9 } }")})
	{
		PlayerSettingsRead const read = ReadPlayerSettingsText(newer, defaults);
		CHECK(read.Status == PlayerSettingsStatus::Newer);
		CHECK(read.Version > 1);
		CHECK(read.Settings == defaults);
		CHECK(read.Problem.find("newer") != std::string::npos);
	}

	std::vector<std::string> const damaged = {
		"",
		"not JSON",
		"{ \"Strada\": { \"Version\": 1, \"Type\": \"SaveSlot\" }, \"PlayerSettings\": {} }",
		MakeSettingsText("0", "{}"),
		MakeSettingsText("1", "[]"),
		MakeSettingsText("1", "{ \"Window\": { \"Width\": 10 } }"),
		MakeSettingsText("1", "{ \"Window\": { \"Mode\": \"Borderless\" } }"),
		MakeSettingsText("1", "{ \"Quality\": \"Ultra\" }"),
		MakeSettingsText("1", "{ \"MasterVolume\": 1.5 }"),
		MakeSettingsText("1", "{ \"MasterVolume\": null }"),
		MakeSettingsText("1", "{ \"Buses\": [{ \"Bus\": \"Music\" }, { \"Bus\": \"MUSIC\", \"Muted\": true }] }"),
		MakeSettingsText("1", "{ \"Buses\": [{ \"Bus\": \"\" }] }"),
		MakeSettingsText("1", "{ \"Buses\": [{ \"Bus\": \"Music\", \"VolumeDb\": 30 }] }"),
		MakeSettingsText("1", "{ \"Values\": { \"\": { \"Type\": \"Bool\", \"Value\": true } } }"),
		MakeSettingsText("1", "{ \"Values\": { \"Share\": { \"Type\": \"Float\", \"Value\": \"much\" } } }"),
		MakeSettingsText("1", "{ \"Values\": { \"Share\": { \"Type\": \"Float\", \"Value\": 0.5 } }, \"Pad\": \"") +
			std::string(MaxPlayerSettingsBytes, 'x') + "\" }",
	};
	for (size_t index = 0; index < damaged.size(); index++)
	{
		CAPTURE(index);
		PlayerSettingsRead const read = ReadPlayerSettingsText(damaged[index], defaults);
		CHECK(read.Status == PlayerSettingsStatus::Damaged);
		CHECK(read.Settings == defaults);
		CHECK_FALSE(read.Problem.empty());
	}

	// Settings the reader would refuse are not written either.
	PlayerSettings invalid = MakeChosenSettings();
	invalid.Buses.push_back({"MUSIC", -1.0f, false});
	CHECK_FALSE(WritePlayerSettingsText(invalid).IsOk());
	invalid = MakeChosenSettings();
	invalid.MasterVolume = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(WritePlayerSettingsText(invalid).IsOk());
	invalid = MakeChosenSettings();
	invalid.Window.Width = MaxPlayerWindowSize + 1;
	CHECK_FALSE(WritePlayerSettingsText(invalid).IsOk());
	invalid = MakeChosenSettings();
	invalid.Quality = static_cast<QualityPreset>(7);
	CHECK_FALSE(WritePlayerSettingsText(invalid).IsOk());
	invalid = MakeChosenSettings();
	invalid.Values["Huge"] = ScriptFieldValue::FromString(std::string(MaxPlayerSettingsBytes, 'x'));
	CHECK_FALSE(WritePlayerSettingsText(invalid).IsOk());
	invalid = MakeChosenSettings();
	invalid.Buses.clear();
	for (uint32_t index = 0; index <= MaxMixerBuses; index++)
	{
		invalid.Buses.push_back({"Bus " + std::to_string(index), -1.0f, false});
	}
	CHECK_FALSE(WritePlayerSettingsText(invalid).IsOk());
	invalid.Buses.pop_back();
	CHECK(WritePlayerSettingsText(invalid).IsOk());
}

TEST_CASE("PlayerSettingsStore: settings load from the game data folder, change in memory, and reach the file only when saved")
{
	PlayerSettingsScope const scope;
	PlayerSettings const defaults = MakeGameDefaults();

	// No game runs: the game's defaults, nothing read, nothing logged, nothing written.
	REQUIRE_FALSE(GameData::GetDirectory().has_value());
	uint64_t const quiet = Log::GetNextEntryIndex();
	PlayerSettingsStore::Load(defaults);
	CHECK(PlayerSettingsStore::GetStatus() == PlayerSettingsStatus::Unavailable);
	CHECK(PlayerSettingsStore::Get() == defaults);
	CHECK_FALSE(Testing::WasLogged(quiet, "Player settings"));
	Result<void> const unavailable = PlayerSettingsStore::Save();
	REQUIRE_FALSE(unavailable.IsOk());
	CHECK(unavailable.GetError().find("no game data folder") != std::string::npos);

	Testing::TemporaryDirectory directory;
	std::filesystem::path const data = directory.GetPath() / "Data";
	std::filesystem::path const file = data / "Settings.json";
	{
		GameDataScope const gameData(data, GameDataLifetime::Kept);
		uint64_t const missingStart = Log::GetNextEntryIndex();
		PlayerSettingsStore::Load(defaults);
		CHECK(PlayerSettingsStore::GetStatus() == PlayerSettingsStatus::Missing);
		CHECK(PlayerSettingsStore::Get() == defaults);
		CHECK(Testing::WasLogged(missingStart,
		                         "Player settings: quality High, window 1920x1080 fullscreen, vsync off, 0 buses, the game's defaults"));
		// Reading creates nothing.
		CHECK_FALSE(FileSystem::Exists(data));

		// Changes stay in memory until a save writes them, durably and whole.
		uint64_t const version = PlayerSettingsStore::GetVersion();
		PlayerSettingsStore::SetQuality(QualityPreset::Medium);
		CHECK(PlayerSettingsStore::GetVersion() != version);
		PlayerSettingsStore::SetBusVolume("Music", -6.0f);
		CHECK_FALSE(FileSystem::Exists(file));
		REQUIRE(PlayerSettingsStore::Save().IsOk());
		CHECK(PlayerSettingsStore::GetStatus() == PlayerSettingsStatus::Present);
		CHECK(ReadWholeFile(file) == WriteText(PlayerSettingsStore::Get()));
		CHECK_FALSE(FileSystem::Exists(data / "Saves"));

		// The next game reads them back; a scene load or a frame does not.
		PlayerSettingsStore::Clear();
		CHECK(PlayerSettingsStore::GetStatus() == PlayerSettingsStatus::Unavailable);
		CHECK(PlayerSettingsStore::Get() == PlayerSettings{});
		uint64_t const presentStart = Log::GetNextEntryIndex();
		PlayerSettingsStore::Load(defaults);
		CHECK(PlayerSettingsStore::GetStatus() == PlayerSettingsStatus::Present);
		CHECK(PlayerSettingsStore::Get().Quality == QualityPreset::Medium);
		CHECK(PlayerSettingsStore::GetBusVolume("MUSIC") == -6.0f);
		CHECK(Testing::WasLogged(presentStart, "Player settings: quality Medium, window 1920x1080 fullscreen, vsync off, 1 bus, from "
		                                       "Settings.json"));

		// A newer file is logged, read as the defaults, kept until the game saves, and then replaced.
		std::string const newer = MakeSettingsText("2", "{ \"Window\": \"a later form\" }");
		REQUIRE(FileSystem::WriteTextFile(file, newer).IsOk());
		uint64_t const newerStart = Log::GetNextEntryIndex();
		PlayerSettingsStore::Load(defaults);
		CHECK(PlayerSettingsStore::GetStatus() == PlayerSettingsStatus::Newer);
		CHECK(PlayerSettingsStore::Get() == defaults);
		CHECK(Testing::CountLogged(newerStart, "newer than this engine's 1") == 1);
		PlayerSettingsStore::SetMasterVolume(0.5f);
		CHECK(ReadWholeFile(file) == newer);
		REQUIRE(PlayerSettingsStore::Save().IsOk());
		CHECK(ReadPlayerSettingsFile(file, defaults).Settings.MasterVolume == 0.5f);

		// A damaged file likewise; a folder in the file's place is damaged, and a save that cannot replace it fails.
		REQUIRE(FileSystem::WriteTextFile(file, "{ broken").IsOk());
		uint64_t const damagedStart = Log::GetNextEntryIndex();
		PlayerSettingsStore::Load(defaults);
		CHECK(PlayerSettingsStore::GetStatus() == PlayerSettingsStatus::Damaged);
		CHECK(Testing::WasLogged(damagedStart, "the game's defaults apply, and the game's next save of its settings replaces it"));
		REQUIRE(PlayerSettingsStore::Save().IsOk());
		CHECK(ReadPlayerSettingsFile(file, defaults).Status == PlayerSettingsStatus::Present);
		REQUIRE(FileSystem::Remove(file).IsOk());
		REQUIRE(FileSystem::WriteTextFile(file / "inside.txt", "x").IsOk());
		uint64_t const folderStart = Log::GetNextEntryIndex();
		PlayerSettingsStore::Load(defaults);
		CHECK(PlayerSettingsStore::GetStatus() == PlayerSettingsStatus::Damaged);
		CHECK(ReadPlayerSettingsFile(file, defaults).Kept);
		CHECK(Testing::WasLogged(folderStart, "Player settings: Settings.json is not read, it is no file: the game's defaults apply, and "
		                                      "the game's saves of its settings fail until it is removed"));
		CHECK_FALSE(PlayerSettingsStore::Save().IsOk());
		CHECK(ReadWholeFile(file / "inside.txt") == "x");
	}
	// The game stopped: nothing is read or written.
	PlayerSettingsStore::Load(defaults);
	CHECK(PlayerSettingsStore::GetStatus() == PlayerSettingsStatus::Unavailable);
}

TEST_CASE("PlayerSettingsStore: setters bring values within range, match buses ignoring case and keep only what differs from "
          "the defaults")
{
	PlayerSettingsScope const scope;
	PlayerSettingsStore::Load(MakeGameDefaults());

	PlayerSettingsStore::SetWindow({PlayerWindowMode::Windowed, 10, 100000, true});
	CHECK(PlayerSettingsStore::Get().Window ==
	      PlayerWindowSettings{PlayerWindowMode::Windowed, MinPlayerWindowSize, MaxPlayerWindowSize, true});
	PlayerSettingsStore::SetWindow({static_cast<PlayerWindowMode>(9), 800, 600, false});
	CHECK(PlayerSettingsStore::Get().Window.Mode == PlayerWindowMode::Windowed);

	// A preset outside the enum changes nothing; an unchanged value does not move the version.
	PlayerSettingsStore::SetQuality(QualityPreset::Low);
	uint64_t const version = PlayerSettingsStore::GetVersion();
	PlayerSettingsStore::SetQuality(static_cast<QualityPreset>(5));
	PlayerSettingsStore::SetQuality(QualityPreset::Low);
	CHECK(PlayerSettingsStore::Get().Quality == QualityPreset::Low);
	CHECK(PlayerSettingsStore::GetVersion() == version);

	PlayerSettingsStore::SetMasterVolume(3.0f);
	CHECK(PlayerSettingsStore::Get().MasterVolume == MaxPlayerMasterVolume);
	PlayerSettingsStore::SetMasterVolume(-1.0f);
	CHECK(PlayerSettingsStore::Get().MasterVolume == 0.0f);
	PlayerSettingsStore::SetMasterVolume(std::numeric_limits<float>::quiet_NaN());
	CHECK(PlayerSettingsStore::Get().MasterVolume == 0.0f);

	// One entry per bus, whatever the letter case; back at 0 dB and not muted, it is gone.
	PlayerSettingsStore::SetBusVolume("Music", -100.0f);
	CHECK(PlayerSettingsStore::GetBusVolume("music") == MinMixerVolumeDb);
	PlayerSettingsStore::SetBusMuted("MUSIC", true);
	REQUIRE(PlayerSettingsStore::Get().Buses.size() == 1);
	CHECK(PlayerSettingsStore::Get().Buses[0].Bus == "Music");
	CHECK(PlayerSettingsStore::IsBusMuted("Music"));
	PlayerSettingsStore::SetBusVolume("music", 0.0f);
	PlayerSettingsStore::SetBusMuted("Music", false);
	CHECK(PlayerSettingsStore::Get().Buses.empty());
	CHECK(PlayerSettingsStore::GetBusVolume("Music") == 0.0f);
	// Names a mixer could not hold change nothing, nor do NaN and a bus left at its defaults.
	PlayerSettingsStore::SetBusVolume("", -3.0f);
	PlayerSettingsStore::SetBusVolume(std::string(MaxMixerBusNameBytes + 1, 'b'), -3.0f);
	PlayerSettingsStore::SetBusVolume("Music", std::numeric_limits<float>::quiet_NaN());
	PlayerSettingsStore::SetBusMuted("Engine", false);
	CHECK(PlayerSettingsStore::Get().Buses.empty());
	// At most as many buses as a mixer has; the next is logged and not kept.
	for (uint32_t index = 0; index < MaxMixerBuses; index++)
	{
		PlayerSettingsStore::SetBusVolume("Bus " + std::to_string(index), -1.0f);
	}
	uint64_t const logStart = Log::GetNextEntryIndex();
	PlayerSettingsStore::SetBusVolume("One more", -1.0f);
	CHECK(PlayerSettingsStore::Get().Buses.size() == MaxMixerBuses);
	CHECK(Testing::WasLogged(logStart, "bus 'One more' is not kept"));
	CHECK(ValidatePlayerSettings(PlayerSettingsStore::Get()).IsOk());
}

TEST_CASE("PlayerSettingsStore: the game's named values follow a slot's rules and never bring the file past its size")
{
	PlayerSettingsScope const scope;
	PlayerSettingsStore::Load({});
	CHECK(PlayerSettingsStore::FindValue("LinkedBrakes") == nullptr);
	REQUIRE(PlayerSettingsStore::SetValue("LinkedBrakes", ScriptFieldValue::FromBool(true)).IsOk());
	ScriptFieldValue const* const linked = PlayerSettingsStore::FindValue("LinkedBrakes");
	REQUIRE(linked != nullptr);
	CHECK(linked->GetBool());
	// The same value again changes nothing.
	uint64_t const version = PlayerSettingsStore::GetVersion();
	REQUIRE(PlayerSettingsStore::SetValue("LinkedBrakes", ScriptFieldValue::FromBool(true)).IsOk());
	CHECK(PlayerSettingsStore::GetVersion() == version);

	CHECK_FALSE(PlayerSettingsStore::SetValue("", ScriptFieldValue::FromBool(true)).IsOk());
	CHECK_FALSE(PlayerSettingsStore::SetValue("Share", ScriptFieldValue::FromFloat(std::numeric_limits<float>::infinity())).IsOk());
	CHECK_FALSE(PlayerSettingsStore::SetValue("Share", ScriptFieldValue()).IsOk());
	// A value that would bring the settings' text past their size is refused, and the rest stays savable.
	Result<void> const huge =
		PlayerSettingsStore::SetValue("Notes", ScriptFieldValue::FromString(std::string(MaxPlayerSettingsBytes, 'x')));
	REQUIRE_FALSE(huge.IsOk());
	CHECK(huge.GetError().find("more than") != std::string::npos);
	CHECK(PlayerSettingsStore::FindValue("Notes") == nullptr);
	CHECK(WritePlayerSettingsText(PlayerSettingsStore::Get()).IsOk());

	// At most as many values as a slot holds: settings read with that many take no new name, but a new value of a name
	// they hold, and a new name once one is removed.
	PlayerSettings full;
	full.Values["LinkedBrakes"] = ScriptFieldValue::FromBool(true);
	for (size_t index = full.Values.size(); index < MaxSaveSlotValues; index++)
	{
		full.Values["Value " + std::to_string(index)] = ScriptFieldValue::FromInt32(1);
	}
	Testing::TemporaryDirectory directory;
	REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "Settings.json", WriteText(full)).IsOk());
	GameDataScope const gameData(directory.GetPath(), GameDataLifetime::Kept);
	PlayerSettingsStore::Load({});
	REQUIRE(PlayerSettingsStore::GetStatus() == PlayerSettingsStatus::Present);
	CHECK_FALSE(PlayerSettingsStore::SetValue("One more", ScriptFieldValue::FromInt32(1)).IsOk());
	CHECK(PlayerSettingsStore::SetValue("LinkedBrakes", ScriptFieldValue::FromBool(false)).IsOk());
	CHECK(PlayerSettingsStore::RemoveValue("LinkedBrakes"));
	CHECK_FALSE(PlayerSettingsStore::RemoveValue("LinkedBrakes"));
	CHECK(PlayerSettingsStore::SetValue("One more", ScriptFieldValue::FromInt32(1)).IsOk());
	CHECK(PlayerSettingsStore::Get().Values.size() == MaxSaveSlotValues);
}

TEST_CASE("PlayerSettings: damaged settings are read, refused or found newer, and what is read writes and reads back alike")
{
	PlayerSettings const defaults = MakeGameDefaults();
	std::vector<Json> seeds;
	seeds.push_back(WritePlayerSettings(MakeChosenSettings()));
	seeds.push_back(Testing::LoadTestDataDocument("PlayerSettings/Version1.json"));
	// Settings at their defaults, whose header is a large part of the document, so that mutations reach its version often.
	seeds.push_back(WritePlayerSettings({}));

	Testing::DocumentMutator mutator(6300);
	int read = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(400); round < rounds; round++)
	{
		CAPTURE(round);
		Json const damaged = mutator.Damage(seeds[static_cast<size_t>(round) % seeds.size()], round);
		PlayerSettingsRead const loaded = ReadPlayerSettingsText(DumpJson(damaged), defaults);
		// A header that names settings of a later version always reads as Newer, whatever else the mutation did.
		CHECK((loaded.Status == PlayerSettingsStatus::Newer) == NamesNewerSettings(damaged));
		if (loaded.Status != PlayerSettingsStatus::Present)
		{
			REQUIRE((loaded.Status == PlayerSettingsStatus::Damaged || loaded.Status == PlayerSettingsStatus::Newer));
			CHECK(loaded.Settings == defaults);
			CHECK_FALSE(loaded.Problem.empty());
			refused++;
			continue;
		}
		read++;
		CHECK(ValidatePlayerSettings(loaded.Settings).IsOk());
		Result<std::string> const text = WritePlayerSettingsText(loaded.Settings);
		REQUIRE_MESSAGE(text.IsOk(), (text.IsOk() ? std::string() : text.GetError()));
		PlayerSettingsRead const again = ReadPlayerSettingsText(text.GetValue(), defaults);
		REQUIRE(again.Status == PlayerSettingsStatus::Present);
		CHECK(again.Settings == loaded.Settings);
		CHECK(WriteText(again.Settings) == text.GetValue());
	}
	CHECK(read > 0);
	CHECK(refused > 0);
}
