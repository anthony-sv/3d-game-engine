#pragma once

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/UUID.h"

#include <doctest/doctest.h>

#include <filesystem>

namespace Strada::Testing
{
	// Initializes the AssetManager for as long as it lives (a test may shut it down earlier).
	class AssetManagerScope
	{
	public:
		AssetManagerScope() { AssetManager::Init(); }
		~AssetManagerScope()
		{
			if (AssetManager::IsInitialized())
			{
				AssetManager::Shutdown();
			}
		}

		AssetManagerScope(AssetManagerScope const&) = delete;
		AssetManagerScope& operator=(AssetManagerScope const&) = delete;
	};

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
