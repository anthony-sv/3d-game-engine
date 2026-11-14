#pragma once

#include "Strada/Core/Base.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Strada
{
	// The operating system whose conventions name the per-user folders. The functions that take one are pure, so each
	// platform's names are tested on every platform.
	enum class UserDataPlatform : uint8_t
	{
		Windows = 0,
		MacOS,
		Linux
	};

	// Who makes a game and what it is called, which names the folder its data goes in on each player's machine.
	struct GameIdentity
	{
		// Empty means no company level.
		std::string Company;
		// The game's name; empty only before Resolve.
		std::string Product;

		// The identity a project's settings give: an empty product is the project's name.
		[[nodiscard]] static GameIdentity Resolve(std::string_view company, std::string_view product, std::string_view name);

		bool operator==(GameIdentity const& other) const = default;
	};

	// Where per-user data goes, by the platform's conventions. Pure functions of their inputs, thread-safe.
	class UserDataLocations
	{
	public:
		// Longest name of a folder below the per-user root, in bytes (longer names are cut, keeping paths short enough
		// for every platform).
		static constexpr size_t MaxFolderNameLength = 64;

		using EnvironmentReader = std::function<std::optional<std::string>(std::string const& name)>;

		// The platform this program was built for.
		static constexpr UserDataPlatform GetHostPlatform()
		{
#if defined(ST_PLATFORM_WINDOWS)
			return UserDataPlatform::Windows;
#elif defined(ST_PLATFORM_MACOS)
			return UserDataPlatform::MacOS;
#else
			return UserDataPlatform::Linux;
#endif
		}

		// The per-user root, the folder that holds the engine's folder and every game's: Windows %APPDATA% (else
		// %USERPROFILE%/AppData/Roaming), macOS $HOME/Library/Application Support, Linux $XDG_DATA_HOME (else
		// $HOME/.local/share). A variable that is unset, empty or not an absolute path counts as unset, so the root is
		// never relative to the working directory. Nothing when no variable names one.
		[[nodiscard]] static std::optional<std::filesystem::path> FindRoot(UserDataPlatform platform,
		                                                                   EnvironmentReader const& readVariable);
		// The engine's own folder below a root: "Strada", "strada" on Linux.
		[[nodiscard]] static std::filesystem::path MakeEngineDirectory(std::filesystem::path const& root, UserDataPlatform platform);
		// A game's data folder below a root: <root>/<Company>/<Product> on Windows and macOS, <root>/<company>-<product>
		// lower-cased on Linux, and without the company when there is none. Each name is made portable
		// (FileSystem::MakePortableFileName) and cut to MaxFolderNameLength; a product with nothing portable is "Game",
		// a company with nothing portable is none. The folder never is the engine's own folder, nor inside it: a first
		// name that is the engine's folder in any letter case gets a trailing '_', as reserved device names do.
		[[nodiscard]] static std::filesystem::path MakeGameDirectory(std::filesystem::path const& root, GameIdentity const& identity,
		                                                             UserDataPlatform platform);
	};

	// The service that tells the engine where the running game keeps its data (input bindings, saves, the player's
	// settings): one folder per game and per user, which the host fills before the game starts. The exported game's
	// player fills it from the game's identity (GameData::GetUserDirectory), development runs (the editor's play mode,
	// a project the player runs) with the project's .strada/PlayData folder, and test runs with a temporary folder. It
	// is empty while no game runs (the editor while editing, tools): callers then persist nothing rather than guess a
	// folder. The service only names the folder; whoever writes into it first creates it.
	class GameData
	{
	public:
		// The folder of the running game; nothing when no host filled it. Thread-safe.
		[[nodiscard]] static std::optional<std::filesystem::path> GetDirectory();
		// Whether the folder is a test run's temporary one, removed when the run ends. Thread-safe.
		[[nodiscard]] static bool IsTemporary();

		// A game's data folder on this machine: UserDataLocations::MakeGameDirectory below this process's per-user root,
		// or below <executable directory>/UserData when the environment names none (as FileSystem::GetUserDataDirectory
		// falls back). Thread-safe.
		[[nodiscard]] static std::filesystem::path GetUserDirectory(GameIdentity const& identity);
		// A new, unique folder name below the system's temporary folder for a test run's data (not created). Empty when the
		// system names no temporary folder. Thread-safe.
		[[nodiscard]] static std::filesystem::path MakeTemporaryDirectory();
	};

	// What a host does with the folder it gives the game when the run ends.
	enum class GameDataLifetime : uint8_t
	{
		// The game's real folder, or a development run's: kept.
		Kept = 0,
		// A test run's: removed with everything in it.
		Temporary
	};

	// A host's hold on the game data service: fills it for the scope's lifetime and gives back what it held before
	// (nothing, unless scopes nest). An empty directory holds the service empty. Main thread; hosts keep one as a member
	// (std::optional::emplace) for as long as the game runs, which outlives the scripts' last chance to write.
	class GameDataScope
	{
	public:
		GameDataScope(std::filesystem::path directory, GameDataLifetime lifetime);
		// Removes a temporary folder (logging what could not be removed) and restores the earlier folder.
		~GameDataScope();

		GameDataScope(GameDataScope const&) = delete;
		GameDataScope& operator=(GameDataScope const&) = delete;

		std::filesystem::path const& GetDirectory() const { return m_Directory; }

	private:
		std::filesystem::path m_Directory;
		GameDataLifetime m_Lifetime;
		std::filesystem::path m_PreviousDirectory;
		GameDataLifetime m_PreviousLifetime = GameDataLifetime::Kept;
	};
}
