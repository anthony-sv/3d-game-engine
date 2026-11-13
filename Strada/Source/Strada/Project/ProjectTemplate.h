#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	class Scene;

	// The checks template.json and catalog.json share. Thread-safe.
	namespace ProjectContent
	{
		inline constexpr size_t MaxIdBytes = 40;
		inline constexpr size_t MaxNameBytes = 64;
		inline constexpr size_t MaxDescriptionBytes = 400;
		inline constexpr int64_t MaxOrder = 1000;
		inline constexpr uint64_t MaxThumbnailBytes = 1024 * 1024;
		// A larger template.json or catalog.json is damaged: each is a few lines per entry.
		inline constexpr uint64_t MaxDocumentBytes = 256 * 1024;

		// An id: lowercase ASCII letters and digits in groups joined by single hyphens ("graph-starter"), at most MaxIdBytes.
		bool IsValidId(std::string_view id);
		// UTF-8 text of at most maxBytes without control characters; the message names the member.
		[[nodiscard]] Result<void> CheckText(std::string_view member, std::string_view text, size_t maxBytes);
		// A path relative to a folder that stays inside it: forward slashes, no "..", no root or drive, no empty, "." or ".."
		// names, and no characters some platform cannot store. Returns the path as given.
		[[nodiscard]] Result<std::string> CheckRelativePath(std::string_view member, std::string_view path);
		// The file at the relative path in the folder when it is a PNG image of at most MaxThumbnailBytes; empty otherwise
		// (also for an empty folder or path).
		std::filesystem::path FindThumbnail(std::filesystem::path const& folder, std::string_view relativePath);
		// Reads a small JSON document with a Strada header of this type and version (newer ones are refused).
		[[nodiscard]] Result<Json> ReadDocument(std::filesystem::path const& file, std::string_view type, int version);
	}

	struct TemplateOptions
	{
		// Write the root .gitignore (ProjectTemplate::GitIgnoreText) when the template brings none.
		bool GitIgnore = true;
	};

	// A project template: a folder Templates/<Id>/ holding template.json, a thumbnail and, unless it is the editor's default
	// scene, a complete project whose project file is ProjectFile (Template.sproj):
	//
	//   { "Strada": { "Version": 1, "Type": "ProjectTemplate" },
	//     "Template": { "Id": "graph-starter", "Name": "Graph Starter", "Order": 2, "Description": "...",
	//                   "Requires": { "Sdk": false }, "Tags": ["3D", "Visual scripts"], "Thumbnail": "thumbnail.png",
	//                   "ProjectFile": "Template.sproj" } }
	//
	// Id (equal to the folder's name), Name and Order are required; unknown members are ignored. Thread-safe: nothing touches
	// global state.
	struct ProjectTemplate
	{
		static constexpr int FormatVersion = 1;
		static constexpr std::string_view FileType = "ProjectTemplate";
		static constexpr std::string_view FileName = "template.json";
		// The template new projects start from when none is named: the editor's default scene.
		static constexpr std::string_view DefaultId = "empty-3d";
		static constexpr size_t MaxTags = 8;
		static constexpr size_t MaxTagBytes = 24;
		// The root .gitignore of a new project.
		static constexpr std::string_view GitIgnoreText = "# Strada: build output, caches and machine-specific files.\n"
														  "Build/\n"
														  ".strada/\n"
														  "Scripts/Binaries/\n"
														  "Scripts/bin/\n"
														  "Scripts/obj/\n"
														  "Scripts/Strada.props\n";

		std::string Id;
		std::string Name;
		std::string Description;
		int32_t Order = 0;
		std::vector<std::string> Tags;
		// The .NET 10 SDK is needed before the project plays (it has C# scripts).
		bool RequiresSdk = false;
		// Relative to Directory; empty when there is none.
		std::string Thumbnail;
		// Relative to Directory; empty for a template of the editor's default scene.
		std::string ProjectFile;
		// The template's folder; empty for one made in memory (MakeDefault).
		std::filesystem::path Directory;

		bool IsDefaultScene() const { return ProjectFile.empty(); }
		// The thumbnail's file when it is a PNG image of at most 1 MiB; empty otherwise.
		std::filesystem::path GetThumbnailPath() const;

		// The default-scene template empty-3d as the editor offers it when the engine content has none (no thumbnail, no
		// folder).
		static ProjectTemplate MakeDefault();
		// Reads <directory>/template.json. Fails for a damaged, foreign or newer file, an Id that is not the folder's name, a
		// member out of its limits, and a ProjectFile that is not a .sproj file of the folder.
		[[nodiscard]] static Result<ProjectTemplate> Load(std::filesystem::path const& directory);
		// The templates in the folders of root (folders without template.json are not templates), sorted by Order, then Id.
		// A damaged template is left out with a warning naming it; a missing root gives an empty list.
		static std::vector<ProjectTemplate> List(std::filesystem::path const& root, std::vector<std::string>& warnings);
		// Writes a new project from the template into a new or empty directory and returns its project file:
		// - a default-scene template writes what Project::Create writes (the start scene Assets/Scenes/Main.sscene, registered,
		//   and the project file) with startScene, which it requires;
		// - another copies the template's folder through the project copy filter (ProjectCopy) without template.json and the
		//   thumbnail, then renames the project file to <MakePortableFileName(name)>.sproj ("Project.sproj" when nothing
		//   portable remains), sets its Project.Name to name and its Window.Title to name when the title was the template's
		//   own Project.Name. Nothing else of any file is rewritten.
		// Then, with options.GitIgnore, it writes the root .gitignore when the project has none. Opens nothing (the
		// AssetManager is not touched); on failure nothing is left behind. Warnings of the copy are logged. Any thread (with
		// startScene, one that may read the scene).
		[[nodiscard]] static Result<std::filesystem::path> Instantiate(ProjectTemplate const& projectTemplate,
		                                                               std::filesystem::path const& directory, std::string const& name,
		                                                               TemplateOptions const& options, Scene const* startScene = nullptr);
	};
}
