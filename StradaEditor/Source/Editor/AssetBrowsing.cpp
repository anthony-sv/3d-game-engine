#include "Editor/AssetBrowsing.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/AssetRegistry.h"
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

			// A regular file of the asset directory's folder that is neither hidden, nor the registry, nor a registered asset.
			bool IsOtherFile(std::filesystem::directory_entry const& entry, std::string_view folder, std::string const& name)
			{
				std::error_code errorCode;
				if (name.starts_with('.') || !entry.is_regular_file(errorCode) || (folder.empty() && name == AssetRegistry::FileName))
				{
					return false;
				}
				return !AssetManager::FindByPath(JoinPath(folder, name)).IsValid();
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

		bool LessIgnoringCase(std::string_view a, std::string_view b)
		{
			std::string const lowerA = ToLower(a);
			std::string const lowerB = ToLower(b);
			return lowerA != lowerB ? lowerA < lowerB : a < b;
		}

		bool ContainsIgnoringCase(std::string_view text, std::string_view needle)
		{
			return ToLower(text).find(ToLower(needle)) != std::string::npos;
		}

		std::vector<std::string> ListFolderNames(std::filesystem::path const& directory)
		{
			std::vector<std::string> folders;
			std::error_code errorCode;
			for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(directory, errorCode))
			{
				std::string name = FileSystem::PathToUtf8(entry.path().filename());
				if (entry.is_directory(errorCode) && !name.starts_with("."))
				{
					folders.push_back(std::move(name));
				}
			}
			std::sort(folders.begin(), folders.end(), LessIgnoringCase);
			return folders;
		}

		std::vector<std::string> ListFolders(std::string_view folder)
		{
			std::vector<std::string> folders;
			if (!AssetManager::HasAssetDirectory())
			{
				return folders;
			}
			for (std::string const& name : ListFolderNames(AssetManager::GetAssetDirectory() / FileSystem::PathFromUtf8(folder)))
			{
				folders.push_back(JoinPath(folder, name));
			}
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
			std::vector<AssetMetadata> assets;
			for (AssetMetadata& asset : AssetManager::GetAssets())
			{
				if (asset.IsFileAsset() && ContainsIgnoringCase(asset.Path, text))
				{
					assets.push_back(std::move(asset));
				}
			}
			return assets;
		}

		std::vector<std::string> ListOtherFiles(std::string_view folder)
		{
			std::vector<std::string> names;
			if (!AssetManager::HasAssetDirectory())
			{
				return names;
			}
			std::error_code errorCode;
			for (std::filesystem::directory_entry const& entry :
			     std::filesystem::directory_iterator(AssetManager::GetAssetDirectory() / FileSystem::PathFromUtf8(folder), errorCode))
			{
				std::string name = FileSystem::PathToUtf8(entry.path().filename());
				if (IsOtherFile(entry, folder, name))
				{
					names.push_back(std::move(name));
				}
			}
			std::sort(names.begin(), names.end(), LessIgnoringCase);
			std::vector<std::string> files;
			files.reserve(names.size());
			for (std::string const& name : names)
			{
				files.push_back(JoinPath(folder, name));
			}
			return files;
		}

		std::vector<std::string> SearchOtherFiles(std::string_view text)
		{
			std::vector<std::string> files;
			if (!AssetManager::HasAssetDirectory())
			{
				return files;
			}
			std::filesystem::path const& directory = AssetManager::GetAssetDirectory();
			std::error_code errorCode;
			std::filesystem::recursive_directory_iterator iterator(directory, std::filesystem::directory_options::skip_permission_denied,
			                                                       errorCode);
			for (std::filesystem::recursive_directory_iterator const end; !errorCode && iterator != end; iterator.increment(errorCode))
			{
				std::filesystem::directory_entry const& entry = *iterator;
				std::string const name = FileSystem::PathToUtf8(entry.path().filename());
				std::error_code entryError;
				if (name.starts_with('.') && entry.is_directory(entryError))
				{
					iterator.disable_recursion_pending();
					continue;
				}
				std::string const path = FileSystem::PathToUtf8(entry.path().lexically_relative(directory));
				if (IsOtherFile(entry, GetParentFolder(path), name) && ContainsIgnoringCase(path, text))
				{
					files.push_back(path);
				}
			}
			std::sort(files.begin(), files.end(), LessIgnoringCase);
			return files;
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

		std::string MakeUniqueFileName(std::filesystem::path const& directory, std::string_view name, std::string_view extension)
		{
			std::string candidate = std::string(name) + std::string(extension);
			for (int index = 1; FileSystem::Exists(directory / FileSystem::PathFromUtf8(candidate)); index++)
			{
				candidate = fmt::format("{} {}{}", name, index, extension);
			}
			return candidate;
		}
	}
}
