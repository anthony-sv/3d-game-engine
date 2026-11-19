#pragma once

#include "Strada/Asset/Mixer.h"
#include "Strada/Core/Result.h"
#include "Strada/Renderer/QualityPresets.h"
#include "Strada/Script/SaveSlots.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Script/ScriptFieldSerialization.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/StructDocument.h"
#include "Strada/Serialization/StructSerialization.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// The player's settings of a game (Docs/Architecture.md §13.1, "The player's settings", and §7.3): one file per game and
// per user, <game data>/Settings.json, holding the window (mode, size, vsync), the graphics quality (QualityPreset), the
// master volume, each bus's volume and mute, and named values the game owns (its assists, its options), of the script
// field types. A versioned struct document, not an asset.
//
// When each applies:
// - the window and vsync: by the player before its window opens, over the game's window settings, and when a script
//   changes them, once the frame's update has returned (ScriptHost::RequestWindowSettings, below). The player's command
//   line wins over both (--resolution keeps a window of that size, --no-vsync keeps vsync off), and a run without a window
//   (--headless, --test) keeps the values without applying them. The editor's play mode keeps them and never applies them
//   to the editor's window;
// - the quality: in every frame of the game's views (SceneRenderOptions::Quality, Renderer/QualityPresets.h);
// - the volumes and mutes: as the game's values of the mix (AudioEngine::SetMasterVolume, SetBusVolume, SetBusMuted), at
//   each game's start right after AudioEngine::ResetGameValues and the project's mixer (SceneRunner::Start), and at once
//   when a script sets them. They are the same values Strada.Audio's MasterVolume, SetBusVolume and SetBusMuted set: a
//   script that sets those changes the run's mix and not the player's settings, which the next game's start applies again.
//
// A file of a newer version, or a damaged one, is logged once and read as the game's defaults, and the next Save replaces
// it (settings are cheap to choose again; saves are not, which is why a newer slot is never replaced).
//
// The scripting boundary (Strada-ScriptCore's Settings.cs; bindings in ScriptBindingsSaves.cpp and InternalCalls.cs):
//   public static class Settings
//     QualityPreset Quality { get; set; }                     public enum QualityPreset { Low, Medium, High }
//     WindowMode WindowMode { get; set; }                     public enum WindowMode { Windowed, Fullscreen }
//     int WindowWidth { get; }, int WindowHeight { get; }, void SetWindowSize(int width, int height)
//     bool VSync { get; set; }
//     float MasterVolume { get; set; }                         0 to 1, brought within it
//     float GetBusVolume(string bus) [PureNode], void SetBusVolume(string bus, float decibels),
//     bool IsBusMuted(string bus) [PureNode], void SetBusMuted(string bus, bool muted)
//     bool Has(string name) [PureNode], T Get<T>(string name, T fallback) [PureNode], void Set<T>(string name, T value),
//     bool Remove(string name)                                 the game's named values, with SaveSlot's rules
//     bool Save()                                              writes Settings.json durably; false (logged) without a
//                                                              game data folder or when the write fails
// Every setter applies at once as said above and changes the file only at Save. The bindings, all main thread, read and
// write PlayerSettingsStore: Settings_GetQuality/SetQuality (int32_t), Settings_GetWindow(NativePlayerWindow*) and
// Settings_SetWindow(NativePlayerWindow const*) (mode, width, height and vsync in one blittable struct, so a size and a
// mode change together), Settings_GetMasterVolume/SetMasterVolume (float), Settings_GetBusVolume/SetBusVolume and
// Settings_IsBusMuted/SetBusMuted (a bus name as char const* and int32_t length), Settings_GetValue(char const* name,
// int32_t length, int32_t* textLength) -> char const* (one value's { "Type", "Value" } JSON, null when none),
// Settings_SetValue(name, length, char const* value, int32_t valueLength) -> uint8_t, Settings_RemoveValue(name, length)
// -> uint8_t and Settings_Save() -> uint8_t. Settings_SetWindow then asks the application to apply the window through
// ScriptEngine::GetHost(): ScriptHost gains `virtual void RequestWindowSettings(PlayerWindowSettings const& window) = 0;`
// ("acted on once the scene's update has returned", as RequestSceneLoad is). SceneRunner records it and applies it in
// ApplyRequests through SceneRunnerSettings::ApplyWindow (a std::function the player sets to resize, make fullscreen and
// set vsync on its window, minding its command line; the editor sets none, so play mode keeps the values unapplied).
// Without a host the request is logged and ignored. No member exposes a path.
namespace Strada
{
	enum class PlayerWindowMode : uint8_t
	{
		Windowed = 0,
		// Borderless fullscreen on the window's monitor (Window::SetFullscreen).
		Fullscreen
	};

	template<>
	struct EnumTraits<PlayerWindowMode>
	{
		static constexpr std::array<std::pair<PlayerWindowMode, std::string_view>, 2> Values = {{
			{PlayerWindowMode::Windowed, "Windowed"},
			{PlayerWindowMode::Fullscreen, "Fullscreen"},
		}};
	};

	// The player's window. The defaults are the game's window settings (the project's Window: Fullscreen, Width, Height,
	// VSync), which a missing file or a missing field keeps.
	struct PlayerWindowSettings
	{
		PlayerWindowMode Mode = PlayerWindowMode::Windowed;
		// The windowed size in screen coordinates (also kept while fullscreen, for the return to a window).
		uint32_t Width = 1280;
		uint32_t Height = 720;
		bool VSync = true;

		bool operator==(PlayerWindowSettings const& other) const = default;
	};

	// Sizes a window setting holds, as the project's window settings bound them.
	inline constexpr uint32_t MinPlayerWindowSize = 64;
	inline constexpr uint32_t MaxPlayerWindowSize = 16384;

	template<>
	struct StructTraits<PlayerWindowSettings>
	{
		static constexpr std::string_view Name = "PlayerWindow";
		static constexpr auto Fields =
			std::make_tuple(Field("Mode", &PlayerWindowSettings::Mode).Doc("Windowed, or borderless fullscreen on the window's monitor."),
		                    Field("Width", &PlayerWindowSettings::Width)
		                        .Range(static_cast<double>(MinPlayerWindowSize), static_cast<double>(MaxPlayerWindowSize))
		                        .Doc("The windowed width in screen coordinates."),
		                    Field("Height", &PlayerWindowSettings::Height)
		                        .Range(static_cast<double>(MinPlayerWindowSize), static_cast<double>(MaxPlayerWindowSize))
		                        .Doc("The windowed height in screen coordinates."),
		                    Field("VSync", &PlayerWindowSettings::VSync).Doc("Present in step with the display's refresh."));
	};

	// A bus's volume and mute as the player set them: a game value of the mix (§9.5), by the bus's name (matched ignoring
	// letter case, as the mixer's names are). A bus the game's mixer lacks is kept and applies nothing (AudioEngine logs
	// the name once).
	struct PlayerBusSettings
	{
		std::string Bus;
		float VolumeDb = 0.0f;
		bool Muted = false;

		bool operator==(PlayerBusSettings const& other) const = default;
	};

	template<>
	struct StructTraits<PlayerBusSettings>
	{
		static constexpr std::string_view Name = "PlayerBus";
		static constexpr auto Fields = std::make_tuple(
			Field("Bus", &PlayerBusSettings::Bus).Doc("The bus's name, as the game's mixer names it (any letter case)."),
			Field("VolumeDb", &PlayerBusSettings::VolumeDb)
				.Range(static_cast<double>(MinMixerVolumeDb), static_cast<double>(MaxMixerVolumeDb))
				.Doc("The player's volume of the bus in decibels, after the mixer's own (0 leaves it as the mixer sets it)."),
			Field("Muted", &PlayerBusSettings::Muted).Doc("Whether the player muted the bus."));
	};

	// The highest master volume a player's setting holds (Strada.Audio.MasterVolume itself may go above it).
	inline constexpr float MaxPlayerMasterVolume = 1.0f;

	struct PlayerSettings
	{
		PlayerWindowSettings Window;
		QualityPreset Quality = QualityPreset::High;
		// Linear, 0 (silence) to MaxPlayerMasterVolume.
		float MasterVolume = 1.0f;
		// Only the buses the player set, at most MaxMixerBuses, each named once.
		std::vector<PlayerBusSettings> Buses;
		// The game's own named values (ValidateSavedValueName, ValidateSavedValue).
		ScriptFieldMap Values;

		bool operator==(PlayerSettings const& other) const = default;
	};

	template<>
	struct StructTraits<PlayerSettings>
	{
		static constexpr std::string_view Name = "PlayerSettings";
		static constexpr auto Fields = std::make_tuple(
			Field("Window", &PlayerSettings::Window).Doc("The game's window as the player set it."),
			Field("Quality", &PlayerSettings::Quality).Doc("The graphics quality preset of the game's views."),
			Field("MasterVolume", &PlayerSettings::MasterVolume)
				.Range(0.0, static_cast<double>(MaxPlayerMasterVolume))
				.Doc("The master volume, linear: 0 is silence, 1 leaves the mix as it is."),
			Field("Buses", &PlayerSettings::Buses).MaxCount(MaxMixerBuses).Doc("The buses whose volume or mute the player set."),
			Field("Values", &PlayerSettings::Values).Doc("The game's own named values ({ \"Type\", \"Value\" }), such as its assists."));
	};

	inline constexpr std::array<DocumentMigration, 0> PlayerSettingsMigrations = {};

	// { "Strada": { "Version": 1, "Type": "PlayerSettings" }, "PlayerSettings": { "Window", "Quality", "MasterVolume",
	// "Buses", "Values" } }
	inline constexpr StructDocumentFormat PlayerSettingsFormat = {
		.Type = "PlayerSettings",
		.Version = 1,
		.Migrations = PlayerSettingsMigrations,
	};

	// The file's name in the game data folder, and its largest size (a larger file is Damaged without being read).
	inline constexpr std::string_view PlayerSettingsFileName = "Settings.json";
	inline constexpr uint64_t MaxPlayerSettingsBytes = 1024ull * 1024;

	// What a reader finds for the settings file (the states of SaveSlotStatus, with the settings' own rules).
	enum class PlayerSettingsStatus : uint8_t
	{
		// No file: the defaults.
		Missing = 0,
		Present,
		// Read as the defaults, replaced by the next Save.
		Damaged,
		// Of a newer version: read as the defaults, replaced by the next Save.
		Newer,
		// No game runs: the defaults, and nothing is written.
		Unavailable
	};

	// The buses' names unique ignoring letter case, each 1 to MaxMixerBusNameBytes bytes; every value's name and value as
	// SaveSlots.h's checks say; the window's sizes and the master volume within their ranges.
	[[nodiscard]] Result<void> ValidatePlayerSettings(PlayerSettings const& settings);

	struct PlayerSettingsRead
	{
		PlayerSettingsStatus Status = PlayerSettingsStatus::Missing;
		// What applies: the file's settings over the defaults when Present (a field the file lacks keeps the default), the
		// defaults otherwise.
		PlayerSettings Settings;
		// The file's version when its header names the type; 0 otherwise.
		int Version = 0;
		// Why a Damaged or Newer file reads as the defaults.
		std::string Problem;
		// Unknown fields skipped while reading.
		std::vector<std::string> Warnings;
	};

	// Reads a settings document's text over the defaults, with SaveSlotRead's rules for Present, Newer and Damaged (a text
	// larger than MaxPlayerSettingsBytes is Damaged). Any thread.
	[[nodiscard]] PlayerSettingsRead ReadPlayerSettingsText(std::string_view text, PlayerSettings const& defaults);
	// Reads a settings file: Missing (the defaults) when there is none, else ReadPlayerSettingsText of it. Any thread: the
	// player reads it this way before its window opens, from the folder it is about to give the GameData service.
	[[nodiscard]] PlayerSettingsRead ReadPlayerSettingsFile(std::filesystem::path const& file, PlayerSettings const& defaults);
	// The document of the settings in the current version. Any thread.
	[[nodiscard]] Json WritePlayerSettings(PlayerSettings const& settings);
	// The text of the document; refuses settings ValidatePlayerSettings refuses and text longer than MaxPlayerSettingsBytes.
	[[nodiscard]] Result<std::string> WritePlayerSettingsText(PlayerSettings const& settings);

	// The running game's settings: loaded when a game starts and cleared when it stops (SceneRunner::Start and Stop), read
	// by the game's views and by scripts, changed by scripts, written only by Save. Main thread only. Outside a game it
	// holds the defaults of an empty project (PlayerSettings{}) with the status Unavailable.
	class PlayerSettingsStore
	{
	public:
		// Reads <game data>/Settings.json over the defaults (the game's window settings, PlayerSettings' own defaults for the
		// rest): Unavailable without a game data folder. Logs a Damaged or Newer file once, and the warnings. Replaces what
		// was loaded.
		static void Load(PlayerSettings const& defaults);
		// Back to PlayerSettings{} and Unavailable; writes nothing.
		static void Clear();

		static PlayerSettings const& Get();
		static PlayerSettingsStatus GetStatus();
		// Moves with every change (a load, a clear, a setter that changed something), for hosts that apply what changed.
		static uint64_t GetVersion();

		// The window: sizes are brought within MinPlayerWindowSize and MaxPlayerWindowSize. Applying it is the host's.
		static void SetWindow(PlayerWindowSettings const& window);
		static void SetQuality(QualityPreset quality);
		// Brought within 0 and MaxPlayerMasterVolume (NaN keeps the volume); applied to the mix at once.
		static void SetMasterVolume(float volume);
		// A bus's setting, its name matched ignoring letter case (0 dB, not muted, for a bus the player never set). A name
		// that is empty or longer than MaxMixerBusNameBytes changes nothing; setting both back to 0 dB and not muted removes
		// the bus's entry; a new bus past MaxMixerBuses entries changes nothing and is logged. Applied to the mix at once.
		static float GetBusVolume(std::string_view bus);
		static bool IsBusMuted(std::string_view bus);
		static void SetBusVolume(std::string_view bus, float decibels);
		static void SetBusMuted(std::string_view bus, bool muted);
		// The game's named value; null when it holds none. The pointer is valid until the next change of the store.
		[[nodiscard]] static ScriptFieldValue const* FindValue(std::string_view name);
		// Refuses a name or value SaveSlots.h's checks refuse, and a new name past MaxSaveSlotValues values.
		[[nodiscard]] static Result<void> SetValue(std::string_view name, ScriptFieldValue value);
		// Whether a value of the name was there.
		static bool RemoveValue(std::string_view name);

		// Applies the master volume and every bus's setting as the game's values of the mix: SceneRunner calls it at a game's
		// start, after AudioEngine::ResetGameValues and the project's mixer, so that each bus exists when it is set.
		static void ApplyAudio();
		// Writes Settings.json durably (FileSystem::WriteTextFileDurably), whatever was on disk (a newer or damaged file
		// included); the status is Present after it. Refuses without a game data folder. Opens the profiler zone
		// Saves.WriteSettings.
		[[nodiscard]] static Result<void> Save();
	};
}
