#include "Editor/RecentProjects.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <algorithm>
#include <string>
#include <system_error>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr char const* FileType = "RecentProjects";

		// Comparable form of a path: absolute, normalized, with the real case of existing parts on case-insensitive systems.
		std::filesystem::path Normalize(std::filesystem::path const& path)
		{
			std::error_code errorCode;
			std::filesystem::path normalized = std::filesystem::weakly_canonical(path, errorCode);
			if (errorCode)
			{
				normalized = std::filesystem::absolute(path, errorCode);
			}
			return (errorCode ? path : normalized).lexically_normal();
		}
	}

	RecentProjects::RecentProjects(std::filesystem::path file)
		: m_File(std::move(file))
	{
	}

	Result<void> RecentProjects::Load()
	{
		m_Projects.clear();
		if (!FileSystem::Exists(m_File))
		{
			return {};
		}
		Result<std::string> text = FileSystem::ReadTextFile(m_File);
		if (!text)
		{
			return Error{text.GetError()};
		}
		Result<Json> document = ParseJson(text.GetValue());
		if (!document)
		{
			return MakeError("'{}' is not valid JSON: {}", FileSystem::PathToUtf8(m_File), document.GetError());
		}
		if (Result<int> header = ReadFileHeader(document.GetValue(), FileType, FormatVersion); !header)
		{
			return MakeError("'{}': {}", FileSystem::PathToUtf8(m_File), header.GetError());
		}
		Json const& projects = document.GetValue().contains("Projects") ? document.GetValue()["Projects"] : Json::array();
		if (!projects.is_array())
		{
			return MakeError("'{}': \"Projects\" must be an array", FileSystem::PathToUtf8(m_File));
		}
		for (Json const& entry : projects)
		{
			if (entry.is_string() && !entry.get_ref<std::string const&>().empty() && m_Projects.size() < MaxCount)
			{
				m_Projects.push_back(FileSystem::PathFromUtf8(entry.get_ref<std::string const&>()));
			}
		}
		return {};
	}

	Result<void> RecentProjects::Save() const
	{
		Json projects = Json::array();
		for (std::filesystem::path const& project : m_Projects)
		{
			projects.push_back(FileSystem::PathToUtf8(project));
		}
		Json document = Json::object();
		document["Strada"] = MakeFileHeader(FileType, FormatVersion);
		document["Projects"] = std::move(projects);
		if (Result<void> created = FileSystem::CreateDirectories(m_File.parent_path()); !created)
		{
			return created;
		}
		return FileSystem::WriteTextFile(m_File, DumpJson(document));
	}

	void RecentProjects::Add(std::filesystem::path const& projectFile)
	{
		Remove(projectFile);
		m_Projects.insert(m_Projects.begin(), Normalize(projectFile));
		if (m_Projects.size() > MaxCount)
		{
			m_Projects.resize(MaxCount);
		}
	}

	void RecentProjects::Remove(std::filesystem::path const& projectFile)
	{
		std::filesystem::path const normalized = Normalize(projectFile);
		std::erase_if(m_Projects,
		              [&normalized](std::filesystem::path const& project)
		              {
						  return Normalize(project) == normalized;
					  });
	}
}
