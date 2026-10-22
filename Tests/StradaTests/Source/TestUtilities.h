#pragma once

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/UUID.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <system_error>
#include <utility>

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

	// Takes every write permission away from a file (on Windows, sets its read-only attribute) for as long as it lives, then
	// gives it back the permissions it had, also when a check fails: Windows keeps a TemporaryDirectory from removing a
	// read-only file.
	class ReadOnlyFileScope
	{
	public:
		explicit ReadOnlyFileScope(std::filesystem::path path)
			: m_Path(std::move(path))
		{
			std::error_code error;
			m_Permissions = std::filesystem::status(m_Path, error).permissions();
			REQUIRE_FALSE(error);
			std::filesystem::permissions(m_Path, WritePermissions, std::filesystem::perm_options::remove, error);
			REQUIRE_FALSE(error);
		}

		~ReadOnlyFileScope() { Restore(); }

		ReadOnlyFileScope(ReadOnlyFileScope const&) = delete;
		ReadOnlyFileScope& operator=(ReadOnlyFileScope const&) = delete;

		// Gives the file its permissions back before the scope ends.
		void Restore()
		{
			if (!m_Restored)
			{
				std::error_code error;
				std::filesystem::permissions(m_Path, m_Permissions, std::filesystem::perm_options::replace, error);
				m_Restored = true;
			}
		}

	private:
		static constexpr std::filesystem::perms WritePermissions =
			std::filesystem::perms::owner_write | std::filesystem::perms::group_write | std::filesystem::perms::others_write;

		std::filesystem::path m_Path;
		std::filesystem::perms m_Permissions = std::filesystem::perms::none;
		bool m_Restored = false;
	};

	// Whether the file system of a directory finds files under names written in another letter case, as those of Windows
	// and (by default) macOS do, unlike those of Linux.
	inline bool IgnoresLetterCase(std::filesystem::path const& directory)
	{
		std::filesystem::path const probe = directory / "LetterCaseProbe";
		REQUIRE(FileSystem::WriteTextFile(probe, "probe").IsOk());
		bool const ignores = FileSystem::Exists(directory / "lettercaseprobe");
		REQUIRE(FileSystem::Remove(probe).IsOk());
		return ignores;
	}
}
