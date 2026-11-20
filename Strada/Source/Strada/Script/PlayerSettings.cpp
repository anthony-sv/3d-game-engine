#include "stpch.h"
#include "Strada/Script/PlayerSettings.h"

#include "Strada/Audio/AudioEngine.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/GameData.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Core/Utf8.h"
#include "Strada/Script/ScriptEngine.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace Strada
{
	// The settings' named values are script fields: a change to the script field form takes the next version of the settings
	// (and of slots, scenes and prefabs; Docs/Architecture.md §7.3, "Adding a version", step 3), so that an older engine reads
	// a later game's settings as Newer. These pin the form version 1 holds: raise PlayerSettingsFormat.Version, with its
	// migration, and these checks with it.
	static_assert(PlayerSettingsFormat.Version == 1 && LastScriptFieldElementType == ScriptFieldType::Asset &&
	                  static_cast<uint8_t>(LastScriptFieldElementType) == 16 && MaxScriptFieldListElements == 4096,
	              "the script field form changed: the player's settings need their next version");
	static_assert(PlayerSettingsMigrations.size() + 1 == PlayerSettingsFormat.Version, "one migration per older version");

	namespace
	{
		bool IsValidWindowMode(PlayerWindowMode mode)
		{
			return !EnumToString(mode).empty();
		}

		bool IsValidQuality(QualityPreset quality)
		{
			return !EnumToString(quality).empty();
		}

		Result<void> ValidateBus(PlayerBusSettings const& bus)
		{
			if (bus.Bus.empty() || bus.Bus.size() > MaxMixerBusNameBytes)
			{
				return MakeError("a bus's name has 1 to {} bytes, not {}", MaxMixerBusNameBytes, bus.Bus.size());
			}
			if (!Utf8::IsValid(bus.Bus))
			{
				return Error{"a bus's name is not UTF-8"};
			}
			if (!(bus.VolumeDb >= MinMixerVolumeDb && bus.VolumeDb <= MaxMixerVolumeDb))
			{
				return MakeError("bus '{}': its volume is {} to {} dB", bus.Bus, MinMixerVolumeDb, MaxMixerVolumeDb);
			}
			return {};
		}

		// Reads the content of a document of the settings' type and version over the defaults: a field the content lacks
		// keeps the default (ReadStructDocument would start from PlayerSettings{} instead).
		Result<PlayerSettings> ReadSettingsDocument(Json const& document, PlayerSettings const& defaults,
		                                            std::vector<std::string>& warnings)
		{
			Json migrated;
			Result<Json const*> const content = FindDocumentContent(document, PlayerSettingsFormat, migrated);
			if (!content)
			{
				return Error{content.GetError()};
			}
			DeserializationContext context;
			context.UnknownFields = UnknownFieldPolicy::Warn;
			context.Warnings = &warnings;
			PlayerSettings settings = defaults;
			if (Result<void> read = DeserializeFields<StructTraits<PlayerSettings>>(*content.GetValue(), settings, context, "document");
			    !read)
			{
				return Error{read.GetError()};
			}
			if (Result<void> valid = ValidatePlayerSettings(settings); !valid)
			{
				return MakeError("{}: {}", PlayerSettingsFormat.Type, valid.GetError());
			}
			return settings;
		}

		PlayerSettingsRead MakeDefaultsRead(PlayerSettingsStatus status, PlayerSettings const& defaults)
		{
			PlayerSettingsRead read;
			read.Status = status;
			read.Settings = defaults;
			return read;
		}

		// The running game's settings (PlayerSettingsStore).
		struct PlayerSettingsStoreData
		{
			PlayerSettings Settings;
			PlayerSettingsStatus Status = PlayerSettingsStatus::Unavailable;
			uint64_t Version = 0;
			// ApplyAudio ran since the last Load or Clear: the volume setters reach the mix.
			bool AppliesAudio = false;
		};

		PlayerSettingsStoreData s_Data;

		void MarkChanged()
		{
			s_Data.Version++;
		}

		// The bus's entry, matched ignoring letter case as the mixer's names are; null when the player never set it.
		PlayerBusSettings* FindBus(std::string_view bus)
		{
			auto const found = std::find_if(s_Data.Settings.Buses.begin(), s_Data.Settings.Buses.end(),
			                                [bus](PlayerBusSettings const& entry)
			                                {
												return IsSameMixerBus(entry.Bus, bus);
											});
			return found != s_Data.Settings.Buses.end() ? &*found : nullptr;
		}

		// Changes a bus's entry by `change`, made when the player never set the bus and removed when it is back at 0 dB and
		// not muted; returns whether the settings changed. A name the mixer could never hold changes nothing.
		template<typename TChange>
		bool ChangeBus(std::string_view bus, TChange const& change)
		{
			if (bus.empty() || bus.size() > MaxMixerBusNameBytes || !Utf8::IsValid(bus))
			{
				return false;
			}
			PlayerBusSettings updated;
			updated.Bus = std::string(bus);
			PlayerBusSettings* const entry = FindBus(bus);
			if (entry != nullptr)
			{
				updated = *entry;
			}
			change(updated);
			bool const atDefaults = updated.VolumeDb == 0.0f && !updated.Muted;
			std::vector<PlayerBusSettings>& buses = s_Data.Settings.Buses;
			if (entry == nullptr)
			{
				if (atDefaults)
				{
					return false;
				}
				if (buses.size() >= MaxMixerBuses)
				{
					ST_CORE_WARN("Player settings: bus '{}' is not kept: the settings hold at most {} buses", bus, MaxMixerBuses);
					return false;
				}
				buses.push_back(std::move(updated));
				return true;
			}
			if (atDefaults)
			{
				std::erase_if(buses,
				              [bus](PlayerBusSettings const& candidate)
				              {
								  return IsSameMixerBus(candidate.Bus, bus);
							  });
				return true;
			}
			if (updated == *entry)
			{
				return false;
			}
			*entry = std::move(updated);
			return true;
		}

		std::string DescribeSettings(PlayerSettings const& settings)
		{
			return fmt::format("quality {}, window {}x{} {}, vsync {}, {} bus{}", EnumToString(settings.Quality), settings.Window.Width,
			                   settings.Window.Height, settings.Window.Mode == PlayerWindowMode::Fullscreen ? "fullscreen" : "windowed",
			                   settings.Window.VSync ? "on" : "off", settings.Buses.size(), settings.Buses.size() == 1 ? "" : "es");
		}
	}

	Result<void> ValidatePlayerSettings(PlayerSettings const& settings)
	{
		if (!IsValidWindowMode(settings.Window.Mode))
		{
			return Error{"the window's mode is neither Windowed nor Fullscreen"};
		}
		if (settings.Window.Width < MinPlayerWindowSize || settings.Window.Width > MaxPlayerWindowSize ||
		    settings.Window.Height < MinPlayerWindowSize || settings.Window.Height > MaxPlayerWindowSize)
		{
			return MakeError("the window's sizes are {} to {}, not {}x{}", MinPlayerWindowSize, MaxPlayerWindowSize, settings.Window.Width,
			                 settings.Window.Height);
		}
		if (!IsValidQuality(settings.Quality))
		{
			return Error{"the quality is none of Low, Medium and High"};
		}
		if (!(settings.MasterVolume >= 0.0f && settings.MasterVolume <= MaxPlayerMasterVolume))
		{
			return MakeError("the master volume is 0 to {}", MaxPlayerMasterVolume);
		}
		if (settings.Buses.size() > MaxMixerBuses)
		{
			return MakeError("the settings hold at most {} buses, not {}", MaxMixerBuses, settings.Buses.size());
		}
		std::unordered_set<std::string> keys;
		for (PlayerBusSettings const& bus : settings.Buses)
		{
			if (Result<void> valid = ValidateBus(bus); !valid)
			{
				return valid;
			}
			if (!keys.insert(MakeMixerBusKey(bus.Bus)).second)
			{
				return MakeError("bus '{}' is named twice (names ignore letter case)", bus.Bus);
			}
		}
		if (settings.Values.size() > MaxSaveSlotValues)
		{
			return MakeError("the settings hold at most {} values, not {}", MaxSaveSlotValues, settings.Values.size());
		}
		for (auto const& [name, value] : settings.Values)
		{
			if (Result<void> valid = ValidateSavedValueName(name); !valid)
			{
				return valid;
			}
			if (Result<void> valid = ValidateSavedValue(value); !valid)
			{
				return MakeError("value '{}': {}", name, valid.GetError());
			}
		}
		return {};
	}

	PlayerSettingsRead ReadPlayerSettingsText(std::string_view text, PlayerSettings const& defaults)
	{
		std::optional<int> const version = ReadDocumentHeaderVersion(text, PlayerSettingsFormat.Type);
		if (version && *version > static_cast<int>(PlayerSettingsFormat.Version))
		{
			PlayerSettingsRead read = MakeDefaultsRead(PlayerSettingsStatus::Newer, defaults);
			read.Version = *version;
			read.Problem = fmt::format("it is of version {}, newer than this engine's {}", *version, PlayerSettingsFormat.Version);
			return read;
		}
		PlayerSettingsRead read = MakeDefaultsRead(PlayerSettingsStatus::Damaged, defaults);
		read.Version = version.value_or(0);
		if (text.size() > MaxPlayerSettingsBytes)
		{
			read.Problem = fmt::format("it has more than {} bytes", MaxPlayerSettingsBytes);
			return read;
		}
		Result<Json> const document = ParseJson(text);
		if (!document)
		{
			read.Problem = document.GetError();
			return read;
		}
		std::vector<std::string> warnings;
		Result<PlayerSettings> settings = ReadSettingsDocument(document.GetValue(), defaults, warnings);
		if (!settings)
		{
			read.Problem = settings.GetError();
			return read;
		}
		read.Status = PlayerSettingsStatus::Present;
		read.Version = static_cast<int>(PlayerSettingsFormat.Version);
		read.Settings = settings.TakeValue();
		read.Warnings = std::move(warnings);
		return read;
	}

	PlayerSettingsRead ReadPlayerSettingsFile(std::filesystem::path const& file, PlayerSettings const& defaults)
	{
		std::error_code errorCode;
		std::filesystem::file_status const status = std::filesystem::status(file, errorCode);
		if (!std::filesystem::exists(status))
		{
			return MakeDefaultsRead(PlayerSettingsStatus::Missing, defaults);
		}
		PlayerSettingsRead damaged = MakeDefaultsRead(PlayerSettingsStatus::Damaged, defaults);
		if (!std::filesystem::is_regular_file(status))
		{
			damaged.Problem = "it is no file";
			damaged.Kept = true;
			return damaged;
		}
		std::optional<uint64_t> const size = FileSystem::GetFileSize(file);
		if (!size)
		{
			damaged.Problem = "its size cannot be read";
			return damaged;
		}
		if (*size > MaxPlayerSettingsBytes)
		{
			damaged.Problem = fmt::format("it has {} bytes, more than {}", *size, MaxPlayerSettingsBytes);
			return damaged;
		}
		Result<std::string> const text = FileSystem::ReadTextFile(file);
		if (!text)
		{
			damaged.Problem = text.GetError();
			return damaged;
		}
		return ReadPlayerSettingsText(text.GetValue(), defaults);
	}

	Json WritePlayerSettings(PlayerSettings const& settings)
	{
		return WriteStructDocument(settings, PlayerSettingsFormat);
	}

	Result<std::string> WritePlayerSettingsText(PlayerSettings const& settings)
	{
		if (Result<void> valid = ValidatePlayerSettings(settings); !valid)
		{
			return Error{valid.GetError()};
		}
		std::string text = DumpJson(WritePlayerSettings(settings));
		if (text.size() > MaxPlayerSettingsBytes)
		{
			return MakeError("the settings' text has {} bytes, more than {}", text.size(), MaxPlayerSettingsBytes);
		}
		return text;
	}

	void PlayerSettingsStore::Load(PlayerSettings const& defaults)
	{
		s_Data.AppliesAudio = false;
		MarkChanged();
		std::optional<std::filesystem::path> const folder = GameData::GetDirectory();
		if (!folder)
		{
			s_Data.Settings = defaults;
			s_Data.Status = PlayerSettingsStatus::Unavailable;
			return;
		}
		PlayerSettingsRead read = ReadPlayerSettingsFile(*folder / FileSystem::PathFromUtf8(PlayerSettingsFileName), defaults);
		if (read.Status == PlayerSettingsStatus::Damaged || read.Status == PlayerSettingsStatus::Newer)
		{
			ST_CORE_WARN("Player settings: {} is not read, {}: the game's defaults apply, and {}", PlayerSettingsFileName, read.Problem,
			             read.Kept ? "the game's saves of its settings fail until it is removed"
			                       : "the game's next save of its settings replaces it");
		}
		for (std::string const& warning : read.Warnings)
		{
			ST_CORE_WARN("Player settings: {}: {}", PlayerSettingsFileName, warning);
		}
		s_Data.Settings = std::move(read.Settings);
		s_Data.Status = read.Status;
		ST_CORE_INFO("Player settings: {}, {}", DescribeSettings(s_Data.Settings),
		             s_Data.Status == PlayerSettingsStatus::Present ? fmt::format("from {}", PlayerSettingsFileName)
		                                                            : std::string("the game's defaults"));
	}

	void PlayerSettingsStore::Clear()
	{
		if (s_Data.AppliesAudio)
		{
			AudioEngine::ResetPlayerValues();
		}
		s_Data.AppliesAudio = false;
		s_Data.Settings = PlayerSettings{};
		s_Data.Status = PlayerSettingsStatus::Unavailable;
		MarkChanged();
	}

	PlayerSettings const& PlayerSettingsStore::Get()
	{
		return s_Data.Settings;
	}

	PlayerSettingsStatus PlayerSettingsStore::GetStatus()
	{
		return s_Data.Status;
	}

	uint64_t PlayerSettingsStore::GetVersion()
	{
		return s_Data.Version;
	}

	void PlayerSettingsStore::SetWindow(PlayerWindowSettings const& window)
	{
		PlayerWindowSettings fitted = window;
		if (!IsValidWindowMode(fitted.Mode))
		{
			fitted.Mode = PlayerWindowMode::Windowed;
		}
		fitted.Width = std::clamp(fitted.Width, MinPlayerWindowSize, MaxPlayerWindowSize);
		fitted.Height = std::clamp(fitted.Height, MinPlayerWindowSize, MaxPlayerWindowSize);
		if (fitted == s_Data.Settings.Window)
		{
			return;
		}
		s_Data.Settings.Window = fitted;
		MarkChanged();
		if (ScriptHost* const host = ScriptEngine::GetHost())
		{
			host->RequestWindowSettings(fitted);
		}
	}

	void PlayerSettingsStore::SetQuality(QualityPreset quality)
	{
		if (!IsValidQuality(quality) || quality == s_Data.Settings.Quality)
		{
			return;
		}
		s_Data.Settings.Quality = quality;
		MarkChanged();
	}

	void PlayerSettingsStore::SetMasterVolume(float volume)
	{
		if (std::isnan(volume))
		{
			return;
		}
		volume = std::clamp(volume, 0.0f, MaxPlayerMasterVolume);
		if (volume == s_Data.Settings.MasterVolume)
		{
			return;
		}
		s_Data.Settings.MasterVolume = volume;
		MarkChanged();
		if (s_Data.AppliesAudio)
		{
			AudioEngine::SetPlayerMasterVolume(volume);
		}
	}

	float PlayerSettingsStore::GetBusVolume(std::string_view bus)
	{
		PlayerBusSettings const* const entry = FindBus(bus);
		return entry != nullptr ? entry->VolumeDb : 0.0f;
	}

	bool PlayerSettingsStore::IsBusMuted(std::string_view bus)
	{
		PlayerBusSettings const* const entry = FindBus(bus);
		return entry != nullptr && entry->Muted;
	}

	void PlayerSettingsStore::SetBusVolume(std::string_view bus, float decibels)
	{
		if (std::isnan(decibels))
		{
			return;
		}
		float const volume = std::clamp(decibels, MinMixerVolumeDb, MaxMixerVolumeDb);
		bool const changed = ChangeBus(bus,
		                               [volume](PlayerBusSettings& entry)
		                               {
										   entry.VolumeDb = volume;
									   });
		if (!changed)
		{
			return;
		}
		MarkChanged();
		if (s_Data.AppliesAudio)
		{
			AudioEngine::SetPlayerBusVolume(bus, volume);
		}
	}

	void PlayerSettingsStore::SetBusMuted(std::string_view bus, bool muted)
	{
		bool const changed = ChangeBus(bus,
		                               [muted](PlayerBusSettings& entry)
		                               {
										   entry.Muted = muted;
									   });
		if (!changed)
		{
			return;
		}
		MarkChanged();
		if (s_Data.AppliesAudio)
		{
			AudioEngine::SetPlayerBusMuted(bus, muted);
		}
	}

	ScriptFieldValue const* PlayerSettingsStore::FindValue(std::string_view name)
	{
		auto const found = s_Data.Settings.Values.find(std::string(name));
		return found != s_Data.Settings.Values.end() ? &found->second : nullptr;
	}

	Result<void> PlayerSettingsStore::SetValue(std::string_view name, ScriptFieldValue value)
	{
		if (Result<void> valid = ValidateSavedValueName(name); !valid)
		{
			return valid;
		}
		if (Result<void> valid = ValidateSavedValue(value); !valid)
		{
			return MakeError("value '{}': {}", name, valid.GetError());
		}
		std::string key(name);
		auto const found = s_Data.Settings.Values.find(key);
		if (found != s_Data.Settings.Values.end() && found->second == value)
		{
			return {};
		}
		if (found == s_Data.Settings.Values.end() && s_Data.Settings.Values.size() >= MaxSaveSlotValues)
		{
			return MakeError("the settings hold at most {} values", MaxSaveSlotValues);
		}
		// Measured by writing the settings with the value, so that one value too large never keeps the rest from being saved.
		PlayerSettings measured = s_Data.Settings;
		measured.Values[key] = value;
		if (Result<std::string> const text = WritePlayerSettingsText(measured); !text)
		{
			return MakeError("value '{}' is not kept: {}", name, text.GetError());
		}
		s_Data.Settings.Values[key] = std::move(value);
		MarkChanged();
		return {};
	}

	bool PlayerSettingsStore::RemoveValue(std::string_view name)
	{
		if (s_Data.Settings.Values.erase(std::string(name)) == 0)
		{
			return false;
		}
		MarkChanged();
		return true;
	}

	void PlayerSettingsStore::ApplyAudio()
	{
		s_Data.AppliesAudio = true;
		AudioEngine::SetPlayerMasterVolume(s_Data.Settings.MasterVolume);
		for (PlayerBusSettings const& bus : s_Data.Settings.Buses)
		{
			AudioEngine::SetPlayerBusVolume(bus.Bus, bus.VolumeDb);
			AudioEngine::SetPlayerBusMuted(bus.Bus, bus.Muted);
		}
	}

	Result<void> PlayerSettingsStore::Save()
	{
		ST_PROFILE_SCOPE("Saves.WriteSettings");
		std::optional<std::filesystem::path> const folder = GameData::GetDirectory();
		if (!folder)
		{
			return Error{"no game runs (no game data folder): nothing is written"};
		}
		Result<std::string> const text = WritePlayerSettingsText(s_Data.Settings);
		if (!text)
		{
			return Error{text.GetError()};
		}
		if (Result<void> written =
		        FileSystem::WriteTextFileDurably(*folder / FileSystem::PathFromUtf8(PlayerSettingsFileName), text.GetValue());
		    !written)
		{
			return written;
		}
		s_Data.Status = PlayerSettingsStatus::Present;
		return {};
	}
}
