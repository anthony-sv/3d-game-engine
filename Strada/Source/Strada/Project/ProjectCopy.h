#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	// How far a running project copy got, for a progress display on another thread. Totals are known once the source has
	// been listed (before the first file is copied); every member is written by the copying thread only.
	struct ProjectCopyProgress
	{
		std::atomic<uint64_t> Bytes = 0;
		std::atomic<uint32_t> Files = 0;
		std::atomic<uint64_t> TotalBytes = 0;
		std::atomic<uint32_t> TotalFiles = 0;
	};

	struct ProjectCopyOptions
	{
		// Files left out besides the filter's (paths relative to the source with forward slashes, compared exactly; folders of
		// these names are copied): a template's template.json and thumbnail.
		std::vector<std::string> ExcludedFiles;
		// Set from another thread to stop the copy; it then fails and removes what it wrote. May be null.
		std::atomic<bool> const* Cancel = nullptr;
		// Updated while copying. May be null.
		ProjectCopyProgress* Progress = nullptr;
	};

	struct ProjectCopyResult
	{
		// What was written: the files' sizes as written (script projects lose their repository-only blocks).
		uint64_t Bytes = 0;
		uint32_t Files = 0;
		// What the copy could not do as asked: an unclosed repository-only block, a symbolic link left out.
		std::vector<std::string> Warnings;
	};

	// The project copy filter, shared by templates and examples: what a copy of a project leaves out and how it writes script
	// projects. Thread-safe: no function touches global state.
	class ProjectCopy
	{
	public:
		// The line that opens and the line that closes a block of a script project (*.csproj) that a copy leaves out: a
		// fallback that only works inside the engine's repository (a ProjectReference to ../../../Strada-ScriptCore).
		static constexpr std::string_view RepositoryOnlyBegin = "<!-- strada:repository-only -->";
		static constexpr std::string_view RepositoryOnlyEnd = "<!-- /strada:repository-only -->";

		// Whether a copy leaves out the file or folder at this path relative to the project directory (forward slashes):
		// Tools/, Build/, .strada/, .git/, Scripts/bin/, Scripts/obj/, Scripts/Binaries/, Scripts/Strada.props, and any
		// __pycache__/ folder. Letter case counts, as everywhere in projects.
		static bool IsExcluded(std::string_view relativePath);

		// A script project's text without its repository-only blocks: each line that contains RepositoryOnlyBegin, up to and
		// including the next line that contains RepositoryOnlyEnd, goes. Line endings and a byte order mark stay as they are.
		// A block that is not closed leaves the whole text as it is, with a warning (naming the file as given).
		static std::string RemoveRepositoryOnlyBlocks(std::string_view text, std::string_view fileName, std::vector<std::string>& warnings);

		// Copies a project directory into a new or empty target through the filter: every regular file but those IsExcluded
		// and options.ExcludedFiles name, script projects without their repository-only blocks. Symbolic links are never
		// followed (a linked file or folder is left out with a warning), so nothing outside the source is read, and nothing
		// outside the target is written. Fails for a source that is not a directory, a target that is a file, is not empty,
		// or lies inside the source (or the source inside it), a cancelled copy and any file that cannot be read or written;
		// a failed copy leaves the target as it was (a new one is removed). Any thread.
		[[nodiscard]] static Result<ProjectCopyResult> Copy(std::filesystem::path const& source, std::filesystem::path const& target,
		                                                    ProjectCopyOptions const& options = {});
	};

	// Puts a directory that was new or empty back as it was when what was written into it must not stay: unless committed, it
	// removes the directory when it did not exist (with the folders above it that did not exist either), or else everything
	// inside it. Use it on targets that were checked new or empty (ProjectCopy::Copy, ProjectTemplate::Instantiate), and
	// construct it before anything is written there.
	class NewDirectoryCleanup
	{
	public:
		explicit NewDirectoryCleanup(std::filesystem::path const& directory);
		~NewDirectoryCleanup();

		NewDirectoryCleanup(NewDirectoryCleanup const&) = delete;
		NewDirectoryCleanup& operator=(NewDirectoryCleanup const&) = delete;

		void Commit() { m_Committed = true; }

	private:
		std::filesystem::path m_Directory;
		bool m_Existed;
		// The highest new folder: the directory, or one above it.
		std::filesystem::path m_NewRoot;
		bool m_Committed = false;
	};

	// Whether a target can take a new project: it does not exist, or is an empty directory. The message says why not.
	[[nodiscard]] Result<void> CheckNewProjectDirectory(std::filesystem::path const& directory);
}
