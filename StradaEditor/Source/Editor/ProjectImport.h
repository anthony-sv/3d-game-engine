#pragma once

#include "Strada/Asset/AssetHandle.h"

#include <cstddef>
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
		// Dropped folders are copied while the editor waits: larger ones belong in the file manager.
		inline constexpr size_t MaxFolderFiles = 10000;

		// Where the files go: folders relative to the asset directory and to the scripts directory (ScriptProject::
		// GetSourceDirectory), with forward slashes ("" for the directories themselves).
		struct Destination
		{
			std::string AssetFolder;
			std::string ScriptFolder;
		};

		struct Report
		{
			// Registered assets: those of the dropped files (models first, with the textures they read), then those of the
			// dropped folders.
			std::vector<AssetHandle> Assets;
			// The C# sources that came in, or were in the scripts already.
			std::vector<std::filesystem::path> Scripts;
			// Folders made in the asset directory: from dropped folders, and for models whose files' names were taken.
			std::vector<std::string> Folders;
			// Folders made in the scripts directory from dropped folders, relative to it.
			std::vector<std::string> ScriptFolders;
			// Files that are not assets copied into the asset directory: the buffers and material libraries models read, the
			// other files of dropped folders, and files dropped with models (a license).
			std::vector<std::string> KeptFiles;
			// Files that are neither assets nor C# sources, dropped without a model: not added.
			std::vector<std::string> Skipped;
			// Where parts of the import went when that is not where they were dropped.
			std::vector<std::string> Notes;
			// What failed, one message each.
			std::vector<std::string> Errors;
		};

		// Imports into the destination; one failure does not stop the others, what is in the project already is registered
		// where it is, and what lies inside a dropped folder comes with that folder.
		// - Assets are copied into the asset folder and registered; taken names get a number. A model comes with the files it
		//   reads (MeshImporter::FindExternalFiles: glTF buffers and images, OBJ material libraries and their textures) at
		//   the same paths relative to it, which keeps their names; when one of them is taken, the model and its files go
		//   into a new folder named after it.
		// - C# sources go to the script folder through scripts (EditorScripts::AddScripts, which refuses sources the C#
		//   project cannot take and creates the C# project when the project has none); without scripts they are refused.
		// - A dropped folder comes whole or not at all, without hidden files, links and the build output of C# projects
		//   inside it, as a new folder named like it (numbered when taken): its C# sources go to the script folder and the
		//   rest to the asset folder, or all of it to the script folder when it holds C# sources and no assets. Folders of
		//   more than maxFolderFiles files are refused.
		// - Other files are kept beside the assets when dropped with a model, else skipped.
		Report Import(EditorOperations& operations, EditorScripts* scripts, std::span<std::filesystem::path const> paths,
		              Destination const& destination, size_t maxFolderFiles = MaxFolderFiles);
		// Which files can be imported: "Strada imports SSCENE, ..., TTF and OTF files as assets, and CS files as C# scripts."
		std::string DescribeImportableFiles();
		// What skipped files a report has, and which files can be imported; empty without skipped files.
		std::string DescribeSkipped(Report const& report);
		// Logs what an import did: a line for what was added and one per note, a warning for skipped files, an error per
		// failure.
		void Log(Report const& report);
	}
}
