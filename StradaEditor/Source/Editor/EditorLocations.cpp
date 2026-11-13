#include "Editor/EditorLocations.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Platform.h"

#include <spdlog/fmt/fmt.h>

namespace Strada
{
	EditorLocations EditorLocations::FromProcess()
	{
		EditorLocations locations;
		locations.Layout = EngineLayout::Get();
		locations.UserDataDirectory = FileSystem::GetUserDataDirectory() / "Editor";
		std::vector<std::string> warnings;
		locations.DefaultProjectLocation = ResolveDefaultProjectLocation(Platform::ReadEnvironmentVariable(ProjectsDirectoryVariable),
		                                                                 Platform::GetDocumentsDirectory(), &warnings);
		for (std::string const& warning : warnings)
		{
			ST_WARN("{}", warning);
		}
		return locations;
	}

	std::filesystem::path EditorLocations::ResolveDefaultProjectLocation(std::optional<std::string> const& projectsDirectory,
	                                                                     std::optional<std::filesystem::path> const& documents,
	                                                                     std::vector<std::string>* warnings)
	{
		if (projectsDirectory && !projectsDirectory->empty())
		{
			std::filesystem::path location = FileSystem::PathFromUtf8(*projectsDirectory).lexically_normal();
			if (location.is_absolute())
			{
				// "C:/Games/" names the folder "C:/Games" (lexically_normal leaves one trailing separator); a root stays as it is.
				if (!location.has_filename() && location.has_relative_path())
				{
					location = location.parent_path();
				}
				return location;
			}
			if (warnings != nullptr)
			{
				warnings->push_back(
					fmt::format("{} is ignored: '{}' is not an absolute path", ProjectsDirectoryVariable, *projectsDirectory));
			}
		}
		return documents ? *documents / "Strada Projects" : std::filesystem::path();
	}

	std::filesystem::path EditorLocations::GetUserDataFile(std::string_view fileName) const
	{
		return UserDataDirectory.empty() ? std::filesystem::path() : UserDataDirectory / FileSystem::PathFromUtf8(fileName);
	}
}
