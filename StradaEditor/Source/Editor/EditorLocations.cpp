#include "Editor/EditorLocations.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Platform.h"

#include <optional>

namespace Strada
{
	EditorLocations EditorLocations::FromProcess()
	{
		EditorLocations locations;
		locations.Layout = EngineLayout::Get();
		locations.UserDataDirectory = FileSystem::GetUserDataDirectory() / "Editor";
		if (std::optional<std::filesystem::path> const documents = Platform::GetDocumentsDirectory())
		{
			locations.DefaultProjectLocation = *documents / "Strada Projects";
		}
		return locations;
	}

	std::filesystem::path EditorLocations::GetUserDataFile(std::string_view fileName) const
	{
		return UserDataDirectory.empty() ? std::filesystem::path() : UserDataDirectory / FileSystem::PathFromUtf8(fileName);
	}
}
