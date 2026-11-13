#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	// One example game of the catalog.
	struct ExampleEntry
	{
		std::string Id;
		std::string Name;
		std::string Description;
		int32_t Order = 0;
		// The .NET 10 SDK is needed before the example plays (it has C# scripts).
		bool RequiresSdk = false;
		// The example's folder, relative to the examples root.
		std::string Directory;
		// The project file, relative to the example's folder.
		std::string ProjectFile;
		// Relative to the examples root; empty when there is none.
		std::string Thumbnail;
		// Files and folders the example needs besides what the engine content carries (relative to its folder), and what to
		// do when they are missing (Needs.Hint).
		std::vector<std::string> NeedsPaths;
		std::string NeedsHint;
	};

	// The catalog of the example games (Projects/catalog.json in a source tree, Examples/catalog.json otherwise; see
	// EngineLayout::GetExampleCatalogPath). The examples lie in its folder, the examples root:
	//
	//   { "Strada": { "Version": 1, "Type": "ExampleCatalog" },
	//     "Examples": [ { "Id": "showcase", "Name": "Showcase", "Order": 3, "Directory": "Showcase",
	//                     "ProjectFile": "Showcase.sproj", "Description": "...", "Thumbnail": "Thumbnails/showcase.png",
	//                     "Requires": { "Sdk": true },
	//                     "Needs": { "Paths": ["Assets/Models"], "Hint": "Run: ..." } } ] }
	//
	// Id, Name, Order, Directory and ProjectFile are required; Id, Name, Description and Order follow template.json's limits
	// (ProjectContent); ProjectFile and the needed paths do not lie where a copy leaves files out (ProjectCopy::IsExcluded),
	// since an example is only ever opened as a copy. Unknown members are ignored. Thread-safe: a catalog is a value.
	class ExampleCatalog
	{
	public:
		static constexpr int FormatVersion = 1;
		static constexpr std::string_view FileType = "ExampleCatalog";
		static constexpr size_t MaxExamples = 256;
		static constexpr size_t MaxNeedsPaths = 16;
		static constexpr size_t MaxHintBytes = 400;

		ExampleCatalog() = default;

		// Reads a catalog. A damaged entry, or one whose Id an earlier entry has, is left out with a warning; a missing,
		// damaged, foreign or newer file is an error (the caller reports it once).
		[[nodiscard]] static Result<ExampleCatalog> Load(std::filesystem::path const& catalogPath);

		// Sorted by Order, then Id.
		std::vector<ExampleEntry> const& GetExamples() const { return m_Examples; }
		std::vector<std::string> const& GetWarnings() const { return m_Warnings; }
		// The catalog's folder, which the examples' folders and thumbnails are relative to.
		std::filesystem::path const& GetRoot() const { return m_Root; }
		// Null when no example has the id.
		ExampleEntry const* Find(std::string_view id) const;

		std::filesystem::path GetDirectory(ExampleEntry const& example) const;
		std::filesystem::path GetProjectFile(ExampleEntry const& example) const;
		// The thumbnail's file when it is a PNG image of at most 1 MiB; empty otherwise.
		std::filesystem::path GetThumbnailPath(ExampleEntry const& example) const;
		// Whether the example can be copied: its project file and every path it needs exist.
		bool IsAvailable(ExampleEntry const& example) const;

	private:
		std::filesystem::path m_Root;
		std::vector<ExampleEntry> m_Examples;
		std::vector<std::string> m_Warnings;
	};
}
