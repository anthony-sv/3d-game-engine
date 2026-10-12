#include "stpch.h"
#include "Strada/Core/Events/ApplicationEvent.h"

#include "Strada/Core/FileSystem.h"

namespace Strada
{
	WindowFileDropEvent WindowFileDropEvent::FromUtf8(std::span<char const* const> paths)
	{
		std::vector<std::filesystem::path> converted;
		converted.reserve(paths.size());
		for (char const* const path : paths)
		{
			if (path != nullptr && *path != '\0')
			{
				converted.push_back(FileSystem::PathFromUtf8(path));
			}
		}
		return WindowFileDropEvent(std::move(converted));
	}
}
