#pragma once

#include "Strada/Core/EngineLayout.h"

#include <filesystem>
#include <string_view>

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
		// "<Documents>/Strada Projects". Empty when the Documents folder is unknown: forms then start without a location.
		// Nothing creates it but creating a project in it.
		std::filesystem::path DefaultProjectLocation;

		// The only reader of the process's layout (EngineLayout::Get), user-data (FileSystem::GetUserDataDirectory) and
		// Documents (Platform::GetDocumentsDirectory) folders. Reads, never creates. Any thread.
		static EditorLocations FromProcess();

		// The file of this name in UserDataDirectory ("RecentProjects.json"); empty when nothing is persisted.
		std::filesystem::path GetUserDataFile(std::string_view fileName) const;
	};
}
