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
//   changes them, once the frame's update has returned (PlayerSettingsStore::SetWindow asks ScriptHost::
//   RequestWindowSettings, which SceneRunner hands to SceneRunnerSettings::ApplyWindow). The player's command line wins
//   over both (--resolution keeps a window of that size, --no-vsync keeps vsync off), and a run without a window
//   (--headless, --test) keeps the values without applying them. The editor's play mode keeps them and never applies them
//   to the editor's window. One pure function of the Project module (ResolvePlayerWindow, Project/PlayerWindow.h, over
//   these settings, whose defaults are the game's window settings, and the player's command line) gives the window the
//   player opens and each window a script asks for, so the precedence is tested without a window;
// - the quality: in every frame of the game's views (SceneRenderOptions::Quality, Renderer/QualityPresets.h);
// - the volumes and mutes: the player's stage of the mix (Docs/Architecture.md §9.5), which AudioEngine keeps by bus name
//   beside the game's values (the stage Strada.Audio's MasterVolume, SetBusVolume and SetBusMuted set) and combines with
//   them on the main thread (AudioEngine::SetPlayerMasterVolume, SetPlayerBusVolume, SetPlayerBusMuted and
//   ResetPlayerValues, with getters of their own): a bus's node gets the sum of the two volumes in decibels and either
//   mute, the master the product of the two volumes, through the commands the game's values already use, so the mixing
//   thread is unchanged. Settings' members write and read the player's stage only, Strada.Audio's the game's only, so
//   neither overwrites the other. AudioEngine::ResetGameValues resets the game's stage; ApplyAudio sets the player's at a
//   game's start, after the reset and the project's mixer, and the setters at once; Clear resets it.
//
// SceneRunner::Start calls Load whatever SceneRuntimeSettings::PlayAudio and the audio engine's state (a headless run,
// Simulate and a test run read the settings as a player's run does), and only ApplyAudio sits inside its audio branch;
// SceneRunner::Stop calls Clear the same way. Load logs once what applies and whence ("Player settings: quality Medium,
// window 1920x1080 fullscreen, vsync on, 2 buses, from Settings.json", or "... the game's defaults"), and the player's
// --profile writes the quality beside its medians, so a measurement or a capture says which preset it ran at.
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
//     bool Remove(string name)                                 the game's named values, with SaveSlot's rules; Set
//                                                              also refuses (ArgumentException, logged) a value that
//                                                              would bring the file past MaxPlayerSettingsBytes
//     bool Save()                                              writes Settings.json durably; false (logged) without a
//                                                              game data folder or when the write fails
// Every setter applies at once as said above and changes the file only at Save. The bindings, all main thread, read and
// write PlayerSettingsStore: Settings_GetQuality/SetQuality (int32_t), Settings_GetWindow(NativePlayerWindow*) and
// Settings_SetWindow(NativePlayerWindow const*) (mode, width, height and vsync in one blittable struct, so a size and a
// mode change together), Settings_GetMasterVolume/SetMasterVolume (float), Settings_GetBusVolume/SetBusVolume and
// Settings_IsBusMuted/SetBusMuted (a bus name as char const* and int32_t length), Settings_GetValue(char const* name,
// int32_t length, int32_t* textLength) -> char const* (one value's { "Type", "Value" } JSON, null when none; the caller
// frees it once copied, SaveSlot_ReleaseText),
// Settings_SetValue(char const* values, int32_t length, int32_t* errorLength) -> char const* (a one-member script field
// map; null when the value is kept, else why it is refused, which the managed side throws with), Settings_RemoveValue(name,
// length) -> uint8_t and Settings_Save() -> uint8_t. Settings_SetWindow calls PlayerSettingsStore::SetWindow, which asks the
// application to apply the window through ScriptEngine::GetHost() (ScriptHost::RequestWindowSettings, acted on once the
// scene's update has returned, as RequestSceneLoad is). SceneRunner records it and applies it in ApplyRequests through
// SceneRunnerSettings::ApplyWindow (a std::function the player sets to resize, make fullscreen and set vsync on its
// window, minding its command line; the editor sets none, so play mode keeps the values unapplied). Without a host the
// settings change and nothing applies them. No member exposes a path.
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

	// A bus's volume and mute as the player set them: the player's stage of the bus (§9.5), by the bus's name (matched
	// ignoring letter case, as the mixer's names are). A bus the game's mixer lacks is kept and applies nothing until a bus
	// of that name exists (AudioEngine logs the name once).
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
		// Read as the defaults, replaced by the next Save (which fails, logged, where no file stands at the path: a folder in
		// its place, PlayerSettingsRead::Kept; or where the system refuses, a file another program holds open).
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
		// A Damaged path where no file stands (a folder): a save cannot replace it until it is removed.
		bool Kept = false;
	};

	// Reads a settings document's text over the defaults (a field the content lacks keeps the default), with SaveSlotRead's
	// rules for Present, Newer and Damaged: the header is read first (ReadDocumentHeaderVersion), so a later version's is
	// Newer whatever follows it; otherwise a text larger than MaxPlayerSettingsBytes, one that is not JSON, of another
	// type or version, or whose content does not read or ValidatePlayerSettings refuses, is Damaged. Unknown fields are
	// skipped with warnings. Any thread.
	[[nodiscard]] PlayerSettingsRead ReadPlayerSettingsText(std::string_view text, PlayerSettings const& defaults);
	// Reads a settings file: Missing (the defaults) when there is none, Damaged without reading it when it is no file, or
	// larger than MaxPlayerSettingsBytes, or cannot be read, else ReadPlayerSettingsText of it. Creates nothing. Any thread:
	// the player reads it this way before its window opens, from the folder it is about to give the GameData service.
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
		// rest): Unavailable without a game data folder, which reads nothing and logs nothing. With one, logs a Damaged or
		// Newer file once, the warnings, and once what applies and whence. Replaces what was loaded; applies nothing to the
		// mix (ApplyAudio does).
		static void Load(PlayerSettings const& defaults);
		// Back to PlayerSettings{} and Unavailable, and the player's stage of the mix back to unchanged when ApplyAudio had
		// applied it; writes nothing.
		static void Clear();

		static PlayerSettings const& Get();
		static PlayerSettingsStatus GetStatus();
		// Moves with every change (a load, a clear, a setter that changed something), for hosts that apply what changed.
		static uint64_t GetVersion();

		// The window: sizes are brought within MinPlayerWindowSize and MaxPlayerWindowSize, a mode outside the enum is
		// Windowed. A change asks the running game's host to apply it after the frame (ScriptHost::RequestWindowSettings);
		// applying it is the host's.
		static void SetWindow(PlayerWindowSettings const& window);
		// A preset outside the enum changes nothing.
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
		// Refuses a name or value SaveSlots.h's checks refuse, a new name past MaxSaveSlotValues values, and a value that
		// would bring the settings' text past MaxPlayerSettingsBytes (measured by writing the text with the value, so that one
		// oversized value never keeps the rest from being saved). Each changed value costs one write of the whole settings'
		// text in memory, which a settings screen's few values make small; a value set to what it holds measures nothing.
		[[nodiscard]] static Result<void> SetValue(std::string_view name, ScriptFieldValue value);
		// Whether a value of the name was there.
		static bool RemoveValue(std::string_view name);

		// Applies the master volume and every bus's setting as the player's stage of the mix (AudioEngine's player values,
		// apart from the game's): SceneRunner calls it at a game's start, after AudioEngine::ResetGameValues and the
		// project's mixer, when the game plays audio and the engine is initialized. From then until Clear or the next Load
		// the volume setters apply their value the same way at once; before it (a game without sound, Simulate) they change
		// the settings alone.
		static void ApplyAudio();
		// Writes Settings.json durably (FileSystem::WriteTextFileDurably), whatever was on disk (a newer or damaged file
		// included); the status is Present after it. Refuses without a game data folder. Opens the profiler zone
		// Saves.WriteSettings.
		[[nodiscard]] static Result<void> Save();
	};
}
