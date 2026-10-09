#include "Editor/AssetBrowsing.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace Strada
{
	namespace AssetBrowsing
	{
		namespace
		{
			std::string ToLower(std::string_view text)
			{
				std::string lower(text);
				std::transform(lower.begin(), lower.end(), lower.begin(),
				               [](unsigned char c)
				               {
								   return static_cast<char>(std::tolower(c));
							   });
				return lower;
			}

			bool LessIgnoringCase(std::string_view a, std::string_view b)
			{
				std::string const lowerA = ToLower(a);
				std::string const lowerB = ToLower(b);
				return lowerA != lowerB ? lowerA < lowerB : a < b;
			}
		}

		std::string JoinPath(std::string_view folder, std::string_view name)
		{
			if (folder.empty())
			{
				return std::string(name);
			}
			return std::string(folder) + "/" + std::string(name);
		}

		std::string GetParentFolder(std::string_view path)
		{
			size_t const slash = path.rfind('/');
			return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
		}

		std::string GetFileName(std::string_view path)
		{
			size_t const slash = path.rfind('/');
			return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
		}

		std::vector<std::string> ListFolders(std::string_view folder)
		{
			std::vector<std::string> folders;
			if (!AssetManager::HasAssetDirectory())
			{
				return folders;
			}
			std::filesystem::path const directory = AssetManager::GetAssetDirectory() / FileSystem::PathFromUtf8(folder);
			std::error_code errorCode;
			for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(directory, errorCode))
			{
				std::string name = FileSystem::PathToUtf8(entry.path().filename());
				if (entry.is_directory(errorCode) && !name.starts_with("."))
				{
					folders.push_back(JoinPath(folder, name));
				}
			}
			std::sort(folders.begin(), folders.end(), LessIgnoringCase);
			return folders;
		}

		std::vector<AssetMetadata> ListAssets(std::string_view folder)
		{
			std::vector<AssetMetadata> assets;
			for (AssetMetadata& asset : AssetManager::GetAssets())
			{
				if (asset.IsFileAsset() && GetParentFolder(asset.Path) == folder)
				{
					assets.push_back(std::move(asset));
				}
			}
			std::sort(assets.begin(), assets.end(),
			          [](AssetMetadata const& a, AssetMetadata const& b)
			          {
						  return LessIgnoringCase(GetFileName(a.Path), GetFileName(b.Path));
					  });
			return assets;
		}

		std::vector<AssetMetadata> SearchAssets(std::string_view text)
		{
			std::string const needle = ToLower(text);
			std::vector<AssetMetadata> assets;
			for (AssetMetadata& asset : AssetManager::GetAssets())
			{
				if (asset.IsFileAsset() && ToLower(asset.Path).find(needle) != std::string::npos)
				{
					assets.push_back(std::move(asset));
				}
			}
			return assets;
		}

		std::string MakeUniqueName(std::string_view folder, std::string_view name, std::string_view extension)
		{
			std::filesystem::path const directory = AssetManager::HasAssetDirectory()
			                                            ? AssetManager::GetAssetDirectory() / FileSystem::PathFromUtf8(folder)
			                                            : std::filesystem::path();
			std::string candidate = std::string(name) + std::string(extension);
			for (int index = 1; FileSystem::Exists(directory / FileSystem::PathFromUtf8(candidate)) ||
			                    AssetManager::FindByPath(JoinPath(folder, candidate)).IsValid();
			     index++)
			{
				candidate = fmt::format("{} {}{}", name, index, extension);
			}
			return candidate;
		}
	}
}
