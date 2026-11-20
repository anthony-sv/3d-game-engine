#include "stpch.h"
#include "Strada/Script/ScriptGlue.h"

#include "Strada/Asset/Mixer.h"
#include "Strada/Core/Log.h"
#include "Strada/Renderer/QualityPresets.h"
#include "Strada/Script/PlayerSettings.h"
#include "Strada/Script/SaveSlots.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Script/ScriptFieldSerialization.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The bindings of Strada.SaveSlot and Strada.Settings (Strada/Script/SaveSlots.h and PlayerSettings.h say what crosses;
// Docs/Architecture.md §10.4 and §13.1). Main thread. What the stores refuse or find is logged through the script logger as
// "SaveSlot.<Member>: ..." or "Settings.<Member>: ...", never thrown; no binding takes or gives a path.
namespace Strada::ScriptGlue
{
	namespace
	{
		// The layout of Strada.Interop.NativePlayerWindow (no implicit padding): a PlayerWindowMode, the windowed size (a
		// negative size from a script is the smallest) and vsync.
		struct PlayerWindowValues
		{
			int32_t Mode;
			int32_t Width;
			int32_t Height;
			int32_t VSync;
		};

		static_assert(sizeof(PlayerWindowValues) == 16 && offsetof(PlayerWindowValues, Width) == 4 &&
		              offsetof(PlayerWindowValues, Height) == 8 && offsetof(PlayerWindowValues, VSync) == 12);
		// Strada.WindowMode and Strada.QualityPreset have these values (InteropTests pins them).
		static_assert(static_cast<int32_t>(PlayerWindowMode::Windowed) == 0 && static_cast<int32_t>(PlayerWindowMode::Fullscreen) == 1);
		static_assert(static_cast<int32_t>(QualityPreset::Low) == 0 && static_cast<int32_t>(QualityPreset::Medium) == 1 &&
		              static_cast<int32_t>(QualityPreset::High) == 2);
		// Strada.SaveSlotStatus likewise; SaveSlot_Read answers this for a name the store refuses.
		static_assert(static_cast<int32_t>(SaveSlotStatus::Missing) == 0 && static_cast<int32_t>(SaveSlotStatus::Unavailable) == 4);
		constexpr int32_t RefusedSlotName = -1;

		// The text the latest binding of this file gave a script: engine-owned, valid until the next call of one of them,
		// copied by the caller at once.
		std::string& GetAnswer()
		{
			static std::string s_Answer;
			return s_Answer;
		}

		char const* Answer(std::string text, int32_t* length)
		{
			std::string& answer = GetAnswer();
			answer = std::move(text);
			*length = static_cast<int32_t>(answer.size());
			return answer.data();
		}

		// The script field map a script sent ({ "<Name>": { "Type", "Value" } }); its problem otherwise.
		Result<ScriptFieldMap> ReadFieldMap(char const* text, int32_t length)
		{
			Result<Json> const json = ParseJson(ToStringView(text, length));
			if (!json)
			{
				return Error{json.GetError()};
			}
			ScriptFieldMap values;
			if (Result<void> read = JsonTraits<ScriptFieldMap>::FromJson(json.GetValue(), values, DeserializationContext{}); !read)
			{
				return Error{read.GetError()};
			}
			return values;
		}

		// A bus name a settings setter takes: the mixer's bounds, logged otherwise (getters answer the defaults silently).
		bool CheckBusName(std::string_view function, std::string_view bus)
		{
			if (bus.empty() || bus.size() > MaxMixerBusNameBytes)
			{
				Log::GetScriptLogger().error("{}: a bus's name has 1 to {} bytes (the master volume is Settings.MasterVolume)", function,
				                             MaxMixerBusNameBytes);
				return false;
			}
			return true;
		}

		// --- SaveSlot ---

		// Reads the slot: *status a SaveSlotStatus and the slot's document text (SaveSlotFormat, its values in the script
		// field form), or RefusedSlotName and why the name is refused. A slot that is unavailable, damaged or newer is logged.
		char const* SaveSlot_Read(char const* name, int32_t length, int32_t* status, int32_t* textLength)
		{
			std::string_view const slot = ToStringView(name, length);
			Result<SaveSlotRead> read = SaveSlotStore::Read(slot);
			if (!read)
			{
				*status = RefusedSlotName;
				return Answer(read.GetError(), textLength);
			}
			SaveSlotRead& found = read.GetValue();
			*status = static_cast<int32_t>(found.Status);
			switch (found.Status)
			{
				case SaveSlotStatus::Unavailable:
					Log::GetScriptLogger().warn("SaveSlot.Open: no game runs (no game data folder): slot '{}' is empty, and nothing is "
					                            "saved",
					                            slot);
					break;
				case SaveSlotStatus::Damaged:
					Log::GetScriptLogger().warn("SaveSlot.Open: slot '{}' reads as empty, {}: only Save or Delete replaces it", slot,
					                            found.Problem);
					break;
				case SaveSlotStatus::Newer:
					Log::GetScriptLogger().warn("SaveSlot.Open: slot '{}' reads as empty, {}: it is never written over or deleted", slot,
					                            found.Problem);
					break;
				case SaveSlotStatus::Missing:
				case SaveSlotStatus::Present:
					break;
			}
			for (std::string const& warning : found.Warnings)
			{
				Log::GetScriptLogger().warn("SaveSlot.Open: slot '{}': {}", slot, warning);
			}
			Result<std::string> text = WriteSaveSlotText(found.Data);
			if (!text)
			{
				// What the reader read, it validated: a slot that reads always writes.
				Log::GetScriptLogger().error("SaveSlot.Open: slot '{}' is read as empty: {}", slot, text.GetError());
				*status = static_cast<int32_t>(SaveSlotStatus::Damaged);
				return Answer(std::string(), textLength);
			}
			return Answer(text.TakeValue(), textLength);
		}

		uint8_t SaveSlot_Write(char const* name, int32_t length, char const* values, int32_t valuesLength)
		{
			std::string_view const slot = ToStringView(name, length);
			Result<ScriptFieldMap> read = ReadFieldMap(values, valuesLength);
			if (!read)
			{
				Log::GetScriptLogger().error("SaveSlot.Save: slot '{}' is not saved: {}", slot, read.GetError());
				return 0;
			}
			SaveSlotData data;
			data.Values = read.TakeValue();
			if (Result<void> written = SaveSlotStore::Write(slot, data); !written)
			{
				Log::GetScriptLogger().error("SaveSlot.Save: slot '{}' is not saved: {}", slot, written.GetError());
				return 0;
			}
			return 1;
		}

		uint8_t SaveSlot_Delete(char const* name, int32_t length)
		{
			std::string_view const slot = ToStringView(name, length);
			if (Result<void> deleted = SaveSlotStore::Delete(slot); !deleted)
			{
				Log::GetScriptLogger().error("SaveSlot.Delete: slot '{}' is not deleted: {}", slot, deleted.GetError());
				return 0;
			}
			return 1;
		}

		// The slots' names joined by '\n'; empty for none.
		char const* SaveSlot_GetNames(int32_t* textLength)
		{
			std::string joined;
			for (std::string const& name : SaveSlotStore::GetNames())
			{
				if (!joined.empty())
				{
					joined += '\n';
				}
				joined += name;
			}
			return Answer(std::move(joined), textLength);
		}

		// --- Settings ---

		int32_t Settings_GetQuality()
		{
			return static_cast<int32_t>(PlayerSettingsStore::Get().Quality);
		}

		void Settings_SetQuality(int32_t quality)
		{
			if (quality < static_cast<int32_t>(QualityPreset::Low) || quality > static_cast<int32_t>(QualityPreset::High))
			{
				Log::GetScriptLogger().error("Settings.Quality: {} is no QualityPreset", quality);
				return;
			}
			PlayerSettingsStore::SetQuality(static_cast<QualityPreset>(quality));
		}

		void Settings_GetWindow(PlayerWindowValues* window)
		{
			PlayerWindowSettings const& settings = PlayerSettingsStore::Get().Window;
			window->Mode = static_cast<int32_t>(settings.Mode);
			window->Width = static_cast<int32_t>(settings.Width);
			window->Height = static_cast<int32_t>(settings.Height);
			window->VSync = settings.VSync ? 1 : 0;
		}

		void Settings_SetWindow(PlayerWindowValues const* window)
		{
			if (window->Mode != static_cast<int32_t>(PlayerWindowMode::Windowed) &&
			    window->Mode != static_cast<int32_t>(PlayerWindowMode::Fullscreen))
			{
				Log::GetScriptLogger().error("Settings.WindowMode: {} is no WindowMode", window->Mode);
				return;
			}
			// Sizes are brought within the store's range; a negative one is the smallest.
			PlayerWindowSettings settings;
			settings.Mode = static_cast<PlayerWindowMode>(window->Mode);
			settings.Width = static_cast<uint32_t>(std::max(window->Width, 0));
			settings.Height = static_cast<uint32_t>(std::max(window->Height, 0));
			settings.VSync = window->VSync != 0;
			PlayerSettingsStore::SetWindow(settings);
		}

		float Settings_GetMasterVolume()
		{
			return PlayerSettingsStore::Get().MasterVolume;
		}

		void Settings_SetMasterVolume(float volume)
		{
			if (CheckFinite("Settings.MasterVolume", volume))
			{
				PlayerSettingsStore::SetMasterVolume(volume);
			}
		}

		float Settings_GetBusVolume(char const* bus, int32_t length)
		{
			return PlayerSettingsStore::GetBusVolume(ToStringView(bus, length));
		}

		void Settings_SetBusVolume(char const* bus, int32_t length, float decibels)
		{
			std::string_view const name = ToStringView(bus, length);
			if (CheckBusName("Settings.SetBusVolume", name) && CheckFinite("Settings.SetBusVolume", decibels))
			{
				PlayerSettingsStore::SetBusVolume(name, decibels);
			}
		}

		uint8_t Settings_IsBusMuted(char const* bus, int32_t length)
		{
			return PlayerSettingsStore::IsBusMuted(ToStringView(bus, length)) ? 1 : 0;
		}

		void Settings_SetBusMuted(char const* bus, int32_t length, uint8_t muted)
		{
			std::string_view const name = ToStringView(bus, length);
			if (CheckBusName("Settings.SetBusMuted", name))
			{
				PlayerSettingsStore::SetBusMuted(name, muted != 0);
			}
		}

		// The game's named value as { "Type", "Value" }; null when the settings hold none.
		char const* Settings_GetValue(char const* name, int32_t length, int32_t* textLength)
		{
			*textLength = 0;
			ScriptFieldValue const* const value = PlayerSettingsStore::FindValue(ToStringView(name, length));
			if (value == nullptr)
			{
				return nullptr;
			}
			Json field = Json::object();
			field["Type"] = ScriptFieldTypeToString(value->GetType());
			field["Value"] = ScriptFieldValueToJson(*value);
			return Answer(DumpJson(field), textLength);
		}

		// Sets the named value of a one-member script field map; null when it is kept, else why it is refused (the managed side
		// throws with it).
		char const* Settings_SetValue(char const* text, int32_t length, int32_t* errorLength)
		{
			*errorLength = 0;
			Result<ScriptFieldMap> read = ReadFieldMap(text, length);
			if (!read)
			{
				return Answer(read.GetError(), errorLength);
			}
			ScriptFieldMap& values = read.GetValue();
			if (values.size() != 1)
			{
				return Answer("expected one value", errorLength);
			}
			std::string const name = values.begin()->first;
			if (Result<void> set = PlayerSettingsStore::SetValue(name, std::move(values.begin()->second)); !set)
			{
				return Answer(set.GetError(), errorLength);
			}
			return nullptr;
		}

		uint8_t Settings_RemoveValue(char const* name, int32_t length)
		{
			return PlayerSettingsStore::RemoveValue(ToStringView(name, length)) ? 1 : 0;
		}

		uint8_t Settings_Save()
		{
			if (Result<void> saved = PlayerSettingsStore::Save(); !saved)
			{
				Log::GetScriptLogger().error("Settings.Save: the settings are not saved: {}", saved.GetError());
				return 0;
			}
			return 1;
		}
	}

	void RegisterSaveBindings(BindingTable& table)
	{
		table.Add("SaveSlot_Read", &SaveSlot_Read);
		table.Add("SaveSlot_Write", &SaveSlot_Write);
		table.Add("SaveSlot_Delete", &SaveSlot_Delete);
		table.Add("SaveSlot_GetNames", &SaveSlot_GetNames);
		table.Add("Settings_GetQuality", &Settings_GetQuality);
		table.Add("Settings_SetQuality", &Settings_SetQuality);
		table.Add("Settings_GetWindow", &Settings_GetWindow);
		table.Add("Settings_SetWindow", &Settings_SetWindow);
		table.Add("Settings_GetMasterVolume", &Settings_GetMasterVolume);
		table.Add("Settings_SetMasterVolume", &Settings_SetMasterVolume);
		table.Add("Settings_GetBusVolume", &Settings_GetBusVolume);
		table.Add("Settings_SetBusVolume", &Settings_SetBusVolume);
		table.Add("Settings_IsBusMuted", &Settings_IsBusMuted);
		table.Add("Settings_SetBusMuted", &Settings_SetBusMuted);
		table.Add("Settings_GetValue", &Settings_GetValue);
		table.Add("Settings_SetValue", &Settings_SetValue);
		table.Add("Settings_RemoveValue", &Settings_RemoveValue);
		table.Add("Settings_Save", &Settings_Save);
	}
}
