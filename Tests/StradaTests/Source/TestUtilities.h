#pragma once

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/UUID.h"

#include <doctest/doctest.h>

#include <filesystem>

namespace Strada::Testing
{
	// Unique temporary directory that is removed (with its contents) when the object goes out of scope.
	class TemporaryDirectory
	{
	public:
		TemporaryDirectory()
			: m_Path(std::filesystem::temp_directory_path() / ("StradaTests-" + UUID().ToString()))
		{
			REQUIRE(FileSystem::CreateDirectories(m_Path).IsOk());
		}

		~TemporaryDirectory()
		{
			std::error_code errorCode;
			std::filesystem::remove_all(m_Path, errorCode);
		}

		TemporaryDirectory(TemporaryDirectory const&) = delete;
		TemporaryDirectory& operator=(TemporaryDirectory const&) = delete;

		std::filesystem::path const& GetPath() const { return m_Path; }

	private:
		std::filesystem::path m_Path;
	};
}
