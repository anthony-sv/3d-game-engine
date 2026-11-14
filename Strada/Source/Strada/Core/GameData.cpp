#include "stpch.h"
#include "Strada/Core/GameData.h"

#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Platform.h"
#include "Strada/Core/UUID.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <system_error>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr std::string_view EngineFolderName = "Strada";
		constexpr std::string_view ProductFallback = "Game";

		struct GameDataState
		{
			std::mutex Mutex;
			std::filesystem::path Directory;
			GameDataLifetime Lifetime = GameDataLifetime::Kept;
		};

		GameDataState& GetState()
		{
			static GameDataState s_State;
			return s_State;
		}

		std::string ToLower(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(),
			               [](unsigned char character)
			               {
							   return static_cast<char>(std::tolower(character));
						   });
			return text;
		}

		// An absolute path by the platform's rules, whatever platform reads it: a drive and a separator ("C:/", "C:\") or
		// a share ("\\server", "//server") on Windows, a leading '/' elsewhere.
		bool IsAbsoluteFor(std::string_view text, UserDataPlatform platform)
		{
			if (platform != UserDataPlatform::Windows)
			{
				return text.starts_with('/');
			}
			auto const isSeparator = [](char character)
			{
				return character == '/' || character == '\\';
			};
			bool const drive =
				text.size() >= 3 && std::isalpha(static_cast<unsigned char>(text[0])) != 0 && text[1] == ':' && isSeparator(text[2]);
			bool const share = text.size() >= 3 && isSeparator(text[0]) && isSeparator(text[1]) && !isSeparator(text[2]);
			return drive || share;
		}

		// A portable folder name of at most MaxFolderNameLength bytes; empty when nothing portable remains.
		std::string MakeFolderName(std::string_view name)
		{
			std::string portable = FileSystem::MakePortableFileName(name);
			if (portable.size() > UserDataLocations::MaxFolderNameLength)
			{
				// Portable names are ASCII, so the cut splits no character; the second pass trims what the cut left at the end
				// and marks a reserved name the cut made.
				portable = FileSystem::MakePortableFileName(std::string_view(portable).substr(0, UserDataLocations::MaxFolderNameLength));
			}
			return portable;
		}

		// The name of a folder directly below the root, kept apart from the engine's own folder in any letter case.
		std::string KeepApartFromEngine(std::string name)
		{
			return ToLower(name) == ToLower(std::string(EngineFolderName)) ? name + "_" : name;
		}
	}

	GameIdentity GameIdentity::Resolve(std::string_view company, std::string_view product, std::string_view name)
	{
		return GameIdentity{std::string(company), std::string(product.empty() ? name : product)};
	}

	std::optional<std::filesystem::path> UserDataLocations::FindRoot(UserDataPlatform platform, EnvironmentReader const& readVariable)
	{
		auto const read = [&readVariable, platform](std::string const& variable) -> std::optional<std::filesystem::path>
		{
			std::optional<std::string> const value = readVariable(variable);
			if (!value || !IsAbsoluteFor(*value, platform))
			{
				return std::nullopt;
			}
			return FileSystem::PathFromUtf8(*value);
		};
		switch (platform)
		{
			case UserDataPlatform::Windows:
				if (std::optional<std::filesystem::path> appData = read("APPDATA"))
				{
					return appData;
				}
				if (std::optional<std::filesystem::path> const profile = read("USERPROFILE"))
				{
					return *profile / "AppData" / "Roaming";
				}
				return std::nullopt;
			case UserDataPlatform::MacOS:
				if (std::optional<std::filesystem::path> const home = read("HOME"))
				{
					return *home / "Library" / "Application Support";
				}
				return std::nullopt;
			case UserDataPlatform::Linux:
				if (std::optional<std::filesystem::path> dataHome = read("XDG_DATA_HOME"))
				{
					return dataHome;
				}
				if (std::optional<std::filesystem::path> const home = read("HOME"))
				{
					return *home / ".local" / "share";
				}
				return std::nullopt;
		}
		return std::nullopt;
	}

	std::filesystem::path UserDataLocations::MakeEngineDirectory(std::filesystem::path const& root, UserDataPlatform platform)
	{
		std::string const name =
			platform == UserDataPlatform::Linux ? ToLower(std::string(EngineFolderName)) : std::string(EngineFolderName);
		return root / FileSystem::PathFromUtf8(name);
	}

	std::filesystem::path UserDataLocations::MakeGameDirectory(std::filesystem::path const& root, GameIdentity const& identity,
	                                                           UserDataPlatform platform)
	{
		std::string const company = MakeFolderName(identity.Company);
		std::string product = MakeFolderName(identity.Product);
		if (product.empty())
		{
			product = ProductFallback;
		}
		if (platform == UserDataPlatform::Linux)
		{
			std::string const name = ToLower(company.empty() ? product : company + "-" + product);
			return root / FileSystem::PathFromUtf8(KeepApartFromEngine(name));
		}
		if (company.empty())
		{
			return root / FileSystem::PathFromUtf8(KeepApartFromEngine(product));
		}
		return root / FileSystem::PathFromUtf8(KeepApartFromEngine(company)) / FileSystem::PathFromUtf8(product);
	}

	std::optional<std::filesystem::path> GameData::GetDirectory()
	{
		GameDataState& state = GetState();
		std::scoped_lock const lock(state.Mutex);
		if (state.Directory.empty())
		{
			return std::nullopt;
		}
		return state.Directory;
	}

	bool GameData::IsTemporary()
	{
		GameDataState& state = GetState();
		std::scoped_lock const lock(state.Mutex);
		return !state.Directory.empty() && state.Lifetime == GameDataLifetime::Temporary;
	}

	std::filesystem::path GameData::GetUserDirectory(GameIdentity const& identity)
	{
		constexpr UserDataPlatform platform = UserDataLocations::GetHostPlatform();
		std::optional<std::filesystem::path> const root = UserDataLocations::FindRoot(platform, &Platform::ReadEnvironmentVariable);
		return UserDataLocations::MakeGameDirectory(root ? *root : FileSystem::GetExecutableDirectory() / "UserData", identity, platform);
	}

	std::filesystem::path GameData::MakeTemporaryDirectory()
	{
		std::error_code error;
		std::filesystem::path const temporary = std::filesystem::temp_directory_path(error);
		if (error || temporary.empty())
		{
			return {};
		}
		return temporary / FileSystem::PathFromUtf8("StradaGameData-" + UUID().ToString());
	}

	GameDataScope::GameDataScope(std::filesystem::path directory, GameDataLifetime lifetime)
		: m_Directory(std::move(directory)),
		  m_Lifetime(lifetime)
	{
		GameDataState& state = GetState();
		std::scoped_lock const lock(state.Mutex);
		m_PreviousDirectory = std::exchange(state.Directory, m_Directory);
		m_PreviousLifetime = std::exchange(state.Lifetime, m_Lifetime);
	}

	GameDataScope::~GameDataScope()
	{
		{
			GameDataState& state = GetState();
			std::scoped_lock const lock(state.Mutex);
			state.Directory = std::move(m_PreviousDirectory);
			state.Lifetime = m_PreviousLifetime;
		}
		if (m_Lifetime == GameDataLifetime::Temporary && !m_Directory.empty())
		{
			if (Result<void> removed = FileSystem::RemoveAll(m_Directory); !removed)
			{
				ST_CORE_WARN("The test run's game data folder '{}' could not be removed: {}", FileSystem::PathToUtf8(m_Directory),
				             removed.GetError());
			}
		}
	}
}
