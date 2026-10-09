#pragma once

#include "Strada/Asset/Asset.h"

#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	// Content browser logic independent of the UI. Folders and asset paths are relative to the open asset directory with
	// forward slashes; "" is the asset directory itself. All functions need the AssetManager.
	namespace AssetBrowsing
	{
		// "Textures/Wood" + "Oak.png" -> "Textures/Wood/Oak.png"; "" + "Oak.png" -> "Oak.png".
		std::string JoinPath(std::string_view folder, std::string_view name);
		// "Textures/Wood/Oak.png" -> "Textures/Wood"; "Oak.png" -> "".
		std::string GetParentFolder(std::string_view path);
		// "Textures/Wood/Oak.png" -> "Oak.png".
		std::string GetFileName(std::string_view path);

		// The folders directly inside a folder of the asset directory, sorted by name ignoring case; hidden folders (names
		// starting with '.') are skipped. Empty when there is no asset directory or the folder does not exist.
		std::vector<std::string> ListFolders(std::string_view folder);
		// The registered file assets directly inside a folder, sorted by file name ignoring case.
		std::vector<AssetMetadata> ListAssets(std::string_view folder);
		// Registered file assets whose path contains the text (ignoring case), sorted by path.
		std::vector<AssetMetadata> SearchAssets(std::string_view text);
		// The first of "<name><extension>", "<name> 1<extension>", ... inside the folder that is neither on disk nor registered.
		std::string MakeUniqueName(std::string_view folder, std::string_view name, std::string_view extension);
	}
}
