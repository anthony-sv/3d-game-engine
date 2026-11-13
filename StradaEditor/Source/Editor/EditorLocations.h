#pragma once

#include "Strada/Core/EngineLayout.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	// The folders an editor works with besides the open project. The application's come from the process; tests name
	// folders of their own, so nothing a test does reaches the user's. A default-constructed value names no folder at all:
	// an empty layout, nothing persisted and no default project location.
	struct EditorLocations
	{
		// The engine content: templates, examples, docs, the agent kit.
		EngineLayout Layout;
		// "<user data>/Editor": recent projects and view settings. Empty: nothing is persisted.
		std::filesystem::path UserDataDirectory;
		// Where new projects go unless the user picks another folder: the folder STRADA_PROJECTS_DIR names, else
		// "<Documents>/Strada Projects" (ResolveDefaultProjectLocation). Empty when neither is known: forms then start without
		// a location. Nothing creates it but creating a project in it.
		std::filesystem::path DefaultProjectLocation;

		// The environment variable that sets DefaultProjectLocation.
		static constexpr char const* ProjectsDirectoryVariable = "STRADA_PROJECTS_DIR";

		// The editor's only reader of the process's layout (EngineLayout::Get), user-data (FileSystem::GetUserDataDirectory)
		// and Documents (Platform::GetDocumentsDirectory) folders and of STRADA_PROJECTS_DIR; besides it, only the entry
		// point reads the layout, once at startup to log its warnings. Reads, never creates; logs a warning when
		// STRADA_PROJECTS_DIR is ignored. Any thread.
		static EditorLocations FromProcess();

		// The default project location from STRADA_PROJECTS_DIR's value and the Documents folder. An absolute path is the
		// location itself (not a folder that "Strada Projects" goes into), without a trailing separator; an unset or empty
		// value gives "<documents>/Strada Projects", or nothing without a Documents folder. A relative value is ignored, and
		// a warning naming the variable is added to warnings when given. Pure: FromProcess passes the process's values.
		[[nodiscard]] static std::filesystem::path ResolveDefaultProjectLocation(std::optional<std::string> const& projectsDirectory,
		                                                                         std::optional<std::filesystem::path> const& documents,
		                                                                         std::vector<std::string>* warnings = nullptr);

		// The file of this name in UserDataDirectory ("RecentProjects.json"); empty when nothing is persisted.
		std::filesystem::path GetUserDataFile(std::string_view fileName) const;
	};
}
