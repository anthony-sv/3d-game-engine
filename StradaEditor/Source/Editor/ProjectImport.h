#pragma once

#include "Strada/Asset/AssetHandle.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Strada
{
	class EditorOperations;
	class EditorScripts;

	// Files and folders added to the open project from outside it: dropped onto the editor from the OS, or chosen in the
	// content browser's import dialog. Main thread only.
	namespace ProjectImport
	{
		// Where the files go: folders relative to the asset directory and to the scripts directory (ScriptProject::
		// GetSourceDirectory), with forward slashes ("" for the directories themselves).
		struct Destination
		{
			std::string AssetFolder;
			std::string ScriptFolder;
		};

		struct Report
		{
			// Registered assets: the dropped files' in their order, then those of the dropped folders.
			std::vector<AssetHandle> Assets;
			// The C# sources in the scripts directory.
			std::vector<std::filesystem::path> Scripts;
			// Folders of the asset directory made from dropped folders.
			std::vector<std::string> Folders;
			// Files of dropped folders that are not assets, copied along (the .bin buffers of a glTF scene, licenses).
			std::vector<std::string> KeptFiles;
			// Files that are neither assets nor C# sources: not added.
			std::vector<std::string> Skipped;
			// What failed, one message each.
			std::vector<std::string> Errors;
		};

		// Assets are copied into the asset folder and registered, C# sources into the script folder (through scripts, which
		// also creates the C# project when the project has none; without scripts they are refused). A folder becomes a folder
		// of the asset directory with everything inside except hidden files and links, and its assets are registered. Taken
		// names get a number; files and folders already in the project are registered where they are. One failure does not
		// stop the others.
		Report Import(EditorOperations& operations, EditorScripts* scripts, std::span<std::filesystem::path const> paths,
		              Destination const& destination);
		// Logs what an import did: a line for what was added, a warning for skipped files, an error per failure.
		void Log(Report const& report);
	}
}
