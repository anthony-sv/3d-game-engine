#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Project/ProjectSettings.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	class Scene;

	// The file a project's settings are kept in.
	enum class ProjectFileKind : uint8_t
	{
		// A project file (<Name>.sproj): { "Strada": { "Version": 1, "Type": "Project" }, "Project": { <settings> } }.
		Project = 0,
		// An exported game's configuration (Game.sgame): { "Strada": { "Version": 1, "Type": "Game" }, "Game": { <settings> } },
		// with the asset directory and the script assembly relative to the game's directory.
		Game,
	};

	// A game project: the project file (<Name>.sproj) with the project settings, next to the asset directory; or an exported
	// game, whose configuration (Game.sgame) holds the same settings. Opening or creating a project makes its asset
	// directory the AssetManager's. Main thread only.
	class Project
	{
	public:
		static constexpr int FormatVersion = 1;
		static constexpr std::string_view FileExtension = ".sproj";
		// The configuration of an exported game, next to the player executable (in Contents/Resources of a macOS app bundle).
		static constexpr std::string_view GameFileName = "Game.sgame";
		// The game data folder of development runs (the editor's play mode, the player's --project), relative to the
		// project directory: .strada/ is left out of version control and of project copies, and playing never writes into
		// the shipped game's real folder.
		static constexpr std::string_view PlayDataDirectory = ".strada/PlayData";

		// Writes a new project into an empty or missing directory: the project file, the asset directory and the start scene
		// (Assets/Scenes/Main.sscene), then opens it like Open. On failure nothing is left behind.
		[[nodiscard]] static Result<Ref<Project>> Create(std::filesystem::path const& directory, std::string const& name,
		                                                 Scene const& startScene);
		// Reads a project file and opens its asset directory in the AssetManager, closing the previous one. Settings from newer
		// versions that this one does not know are skipped and reported in warnings (may be null).
		[[nodiscard]] static Result<Ref<Project>> Open(std::filesystem::path const& file, std::vector<std::string>* warnings = nullptr);
		// Reads an exported game's configuration (Game.sgame) and opens its asset directory like Open.
		[[nodiscard]] static Result<Ref<Project>> OpenGame(std::filesystem::path const& file, std::vector<std::string>* warnings = nullptr);
		// The window settings of a project or game file, with the title resolved (the project's name when empty), read without
		// opening it: the player creates its window before the engine starts. Open and OpenGame check the whole file.
		[[nodiscard]] static Result<ProjectWindowSettings> ReadWindowSettings(std::filesystem::path const& file, ProjectFileKind kind);
		// The identity of a project or game file (GetGameIdentity of its settings), read without opening it and without
		// logging: the entry point reads an exported game's before logging starts, to log below the game's data folder. Fails
		// for a file that is missing, not JSON or of the other kind, or whose header, name or identity Open and OpenGame
		// would refuse. The rest of the file is not read, so a file whose other settings are damaged gives its identity and
		// still does not open; a file they open gives the identity of the settings they read.
		[[nodiscard]] static Result<GameIdentity> ReadGameIdentity(std::filesystem::path const& file, ProjectFileKind kind);
		// Where the configuration of the exported game a player runs by default is: GameFileName beside the executable, or
		// in Contents/Resources of a macOS app bundle when there is none beside it. The file may not exist.
		static std::filesystem::path FindExportedGameFile();

		// A project in memory; Create, Open and OpenGame also prepare its files and asset directory.
		Project(std::filesystem::path filePath, ProjectSettings settings, ProjectFileKind fileKind = ProjectFileKind::Project);

		// Writes the project file (the configuration, for an exported game).
		[[nodiscard]] Result<void> Save() const;

		ProjectSettings const& GetSettings() const { return m_Settings; }
		// Applies a partial patch in the project-file format ("Project" object); nothing changes when the result is invalid.
		// The asset directory cannot change while the project is open.
		[[nodiscard]] Result<void> ApplySettings(Json const& patch);

		// Absolute path of the project file (Game.sgame for an exported game).
		std::filesystem::path const& GetFilePath() const { return m_FilePath; }
		ProjectFileKind GetFileKind() const { return m_FileKind; }
		bool IsGame() const { return m_FileKind == ProjectFileKind::Game; }
		std::filesystem::path GetDirectory() const { return m_FilePath.parent_path(); }
		std::filesystem::path GetAssetDirectory() const;
		// <project directory>/.strada/PlayData (PlayDataDirectory); not created.
		std::filesystem::path GetPlayDataDirectory() const;
		// Whether the project's asset directory, open in the AssetManager (Open, OpenGame), registers visual scripts: running
		// the game then needs the scripting runtime, with or without C# scripts.
		bool UsesVisualScripts() const;

		static Json Serialize(ProjectSettings const& settings, ProjectFileKind kind = ProjectFileKind::Project);
		// Validates the file header and the settings.
		[[nodiscard]] static Result<ProjectSettings> Deserialize(Json const& document, DeserializationContext const& context,
		                                                         ProjectFileKind kind = ProjectFileKind::Project);

	private:
		std::filesystem::path m_FilePath;
		ProjectSettings m_Settings;
		ProjectFileKind m_FileKind;
	};
}
