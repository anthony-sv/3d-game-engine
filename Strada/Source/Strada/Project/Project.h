#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Project/ProjectSettings.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	class Scene;

	// A game project: the project file (<Name>.sproj) with the project settings, next to the asset directory. Opening or
	// creating a project makes its asset directory the AssetManager's. Main thread only.
	//
	// Project file: { "Strada": { "Version": 1, "Type": "Project" }, "Project": { <ProjectSettings fields> } }.
	class Project
	{
	public:
		static constexpr int FormatVersion = 1;
		static constexpr std::string_view FileExtension = ".sproj";

		// Writes a new project into an empty or missing directory: the project file, the asset directory and the start scene
		// (Assets/Scenes/Main.sscene), then opens it like Open. On failure nothing is left behind.
		[[nodiscard]] static Result<Ref<Project>> Create(std::filesystem::path const& directory, std::string const& name,
		                                                 Scene const& startScene);
		// Reads a project file and opens its asset directory in the AssetManager, closing the previous one. Settings from newer
		// versions that this one does not know are skipped and reported in warnings (may be null).
		[[nodiscard]] static Result<Ref<Project>> Open(std::filesystem::path const& file, std::vector<std::string>* warnings = nullptr);

		// A project in memory; Create and Open also prepare its files and asset directory.
		Project(std::filesystem::path filePath, ProjectSettings settings);

		// Writes the project file.
		[[nodiscard]] Result<void> Save() const;

		ProjectSettings const& GetSettings() const { return m_Settings; }
		// Applies a partial patch in the project-file format ("Project" object); nothing changes when the result is invalid.
		// The asset directory cannot change while the project is open.
		[[nodiscard]] Result<void> ApplySettings(Json const& patch);

		// Absolute path of the project file.
		std::filesystem::path const& GetFilePath() const { return m_FilePath; }
		std::filesystem::path GetDirectory() const { return m_FilePath.parent_path(); }
		std::filesystem::path GetAssetDirectory() const;

		static Json Serialize(ProjectSettings const& settings);
		// Validates the file header and the settings.
		[[nodiscard]] static Result<ProjectSettings> Deserialize(Json const& document, DeserializationContext const& context);

	private:
		std::filesystem::path m_FilePath;
		ProjectSettings m_Settings;
	};
}
