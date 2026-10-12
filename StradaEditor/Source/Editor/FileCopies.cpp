#include "Editor/FileCopies.h"

#include "Strada/Core/FileSystem.h"

namespace Strada
{
	Result<void> FileCopies::Copy(std::filesystem::path const& from, std::filesystem::path const& to)
	{
		if (FileSystem::Exists(to))
		{
			return MakeError("'{}' already exists", FileSystem::PathToUtf8(to));
		}
		if (Result<void> created = CreateFolder(to.parent_path()); !created)
		{
			return created;
		}
		if (Result<void> copied = FileSystem::Copy(from, to, false); !copied)
		{
			return copied;
		}
		m_Copies.push_back(to);
		return {};
	}

	Result<void> FileCopies::CreateFolder(std::filesystem::path const& folder)
	{
		std::vector<std::filesystem::path> missing;
		for (std::filesystem::path current = folder; !current.empty() && !FileSystem::Exists(current); current = current.parent_path())
		{
			missing.push_back(current);
			// A root that does not exist has itself as its parent.
			if (current == current.parent_path())
			{
				break;
			}
		}
		for (auto current = missing.rbegin(); current != missing.rend(); ++current)
		{
			if (Result<void> created = FileSystem::CreateDirectories(*current); !created)
			{
				return created;
			}
			m_Folders.push_back(*current);
		}
		return {};
	}

	void FileCopies::Rollback()
	{
		for (auto copy = m_Copies.rbegin(); copy != m_Copies.rend(); ++copy)
		{
			(void)FileSystem::Remove(*copy);
		}
		// Innermost first, so that each folder is empty when it goes; one that is not (someone else's files) stays.
		for (auto folder = m_Folders.rbegin(); folder != m_Folders.rend(); ++folder)
		{
			(void)FileSystem::Remove(*folder);
		}
		m_Copies.clear();
		m_Folders.clear();
	}
}
