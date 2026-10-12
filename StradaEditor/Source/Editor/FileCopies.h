#pragma once

#include "Strada/Core/Result.h"

#include <filesystem>
#include <vector>

namespace Strada
{
	// Copies files into a project as one change: Rollback removes the copies and the folders made for them, so an import that
	// fails halfway leaves nothing behind.
	class FileCopies
	{
	public:
		// Copies a file, making the folders it needs; fails when the target exists.
		[[nodiscard]] Result<void> Copy(std::filesystem::path const& from, std::filesystem::path const& to);
		// Makes a folder with its missing parents.
		[[nodiscard]] Result<void> CreateFolder(std::filesystem::path const& folder);
		// Removes the copies and the folders made, newest first; folders that hold other files stay.
		void Rollback();

		// The copies, in order.
		std::vector<std::filesystem::path> const& GetCopies() const { return m_Copies; }

	private:
		std::vector<std::filesystem::path> m_Copies;
		// Outermost first.
		std::vector<std::filesystem::path> m_Folders;
	};
}
