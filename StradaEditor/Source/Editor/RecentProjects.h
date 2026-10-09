#pragma once

#include "Strada/Core/Result.h"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace Strada
{
	// Recently opened project files, most recent first, persisted in a small JSON file (in the user data directory).
	// Main thread only.
	class RecentProjects
	{
	public:
		static constexpr size_t MaxCount = 10;
		static constexpr int FormatVersion = 1;

		explicit RecentProjects(std::filesystem::path file);

		// Reads the list; a missing file is an empty list. Entries that are not valid paths are skipped.
		[[nodiscard]] Result<void> Load();
		[[nodiscard]] Result<void> Save() const;

		// Moves the project to the front (adding it when new) and drops the oldest entries beyond MaxCount.
		void Add(std::filesystem::path const& projectFile);
		void Remove(std::filesystem::path const& projectFile);
		void Clear() { m_Projects.clear(); }

		std::vector<std::filesystem::path> const& GetProjects() const { return m_Projects; }

	private:
		std::filesystem::path m_File;
		std::vector<std::filesystem::path> m_Projects;
	};
}
