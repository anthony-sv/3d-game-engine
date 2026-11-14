#pragma once

#include "TestUtilities.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/GameData.h"
#include "Strada/Core/Platform.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Strada::Testing
{
	// A per-user root of its own for the programs a test starts (players, exported games), which then write their logs,
	// crash reports and game data below a temporary directory instead of the user's folders: the environment variable that
	// names the root on this platform (APPDATA on Windows, HOME on macOS, XDG_DATA_HOME on Linux), as StradaToolTests'
	// IsolatedUserData sets it. Give GetEnvironment to ProcessSpecification::Environment.
	class IsolatedUserRoot
	{
	public:
		IsolatedUserRoot()
		{
			std::string const value = FileSystem::PathToUtf8(m_Directory.GetPath());
			constexpr UserDataPlatform platform = UserDataLocations::GetHostPlatform();
			char const* const variable = platform == UserDataPlatform::Windows ? "APPDATA"
			                             : platform == UserDataPlatform::MacOS ? "HOME"
			                                                                   : "XDG_DATA_HOME";
			m_Environment = {{variable, value}};
			// What the programs see: the variable over the rest of this process's environment.
			std::optional<std::filesystem::path> const root =
				UserDataLocations::FindRoot(platform,
			                                [this](std::string const& name) -> std::optional<std::string>
			                                {
												for (std::pair<std::string, std::string> const& entry : m_Environment)
												{
													if (entry.first == name)
													{
														return entry.second;
													}
												}
												return Platform::ReadEnvironmentVariable(name);
											});
			REQUIRE(root.has_value());
			m_Root = *root;
		}

		IsolatedUserRoot(IsolatedUserRoot const&) = delete;
		IsolatedUserRoot& operator=(IsolatedUserRoot const&) = delete;

		std::vector<std::pair<std::string, std::string>> const& GetEnvironment() const { return m_Environment; }
		// The temporary directory everything goes below.
		std::filesystem::path const& GetDirectory() const { return m_Directory.GetPath(); }
		// The per-user root the programs find (UserDataLocations::FindRoot): the directory itself, or below it on macOS.
		std::filesystem::path const& GetRoot() const { return m_Root; }
		// The engine's shared folder there: the editor's, the player's of a build, and an exported game's without a usable
		// game file.
		std::filesystem::path GetEngineDirectory() const
		{
			return UserDataLocations::MakeEngineDirectory(m_Root, UserDataLocations::GetHostPlatform());
		}
		// A game's data folder there (GameData::GetUserDirectory as the programs compute it).
		std::filesystem::path GetGameDirectory(GameIdentity const& identity) const
		{
			return UserDataLocations::MakeGameDirectory(m_Root, identity, UserDataLocations::GetHostPlatform());
		}

	private:
		TemporaryDirectory m_Directory;
		std::vector<std::pair<std::string, std::string>> m_Environment;
		std::filesystem::path m_Root;
	};
}
