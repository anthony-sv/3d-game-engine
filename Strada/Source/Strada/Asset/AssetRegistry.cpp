#include "stpch.h"
#include "Strada/Asset/AssetRegistry.h"

#include "Strada/Core/FileSystem.h"

namespace Strada
{
	Result<std::string> NormalizeAssetPath(std::string_view path)
	{
		std::string normalized;
		normalized.reserve(path.size());

		size_t start = 0;
		while (start <= path.size())
		{
			size_t end = path.find_first_of("/\\", start);
			if (end == std::string_view::npos)
			{
				end = path.size();
			}
			std::string_view const segment = path.substr(start, end - start);
			start = end + 1;

			if (segment.empty() || segment == ".")
			{
				continue;
			}
			if (segment == "..")
			{
				return MakeError("asset path '{}' must not contain '..'", path);
			}
			if (segment.find(':') != std::string_view::npos)
			{
				return MakeError("asset path '{}' must be relative to the asset directory", path);
			}
			// Projects move between operating systems: reject names that Windows cannot store.
			for (char const character : segment)
			{
				if (static_cast<unsigned char>(character) < 0x20 || std::string_view("<>\"|?*").find(character) != std::string_view::npos)
				{
					return MakeError("asset path '{}' contains characters that are not portable", path);
				}
			}
			if (!normalized.empty())
			{
				normalized.push_back('/');
			}
			normalized.append(segment);
		}

		if (normalized.empty())
		{
			return MakeError("asset path '{}' is empty", path);
		}
		if (!path.empty() && (path.front() == '/' || path.front() == '\\'))
		{
			return MakeError("asset path '{}' must be relative to the asset directory", path);
		}
		return normalized;
	}

	AssetMetadata const* AssetRegistry::Find(AssetHandle handle) const
	{
		auto const it = m_Assets.find(handle);
		return it != m_Assets.end() ? &it->second : nullptr;
	}

	AssetHandle AssetRegistry::FindByPath(std::string_view path) const
	{
		auto const it = m_Paths.find(path);
		return it != m_Paths.end() ? it->second : AssetHandle();
	}

	Result<void> AssetRegistry::Add(AssetMetadata metadata)
	{
		if (!metadata.Handle.IsValid() || metadata.Handle.IsReserved())
		{
			return MakeError("invalid handle {} for '{}'", metadata.Handle, metadata.Path);
		}
		if (metadata.Type == AssetType::None)
		{
			return MakeError("asset '{}' has no type", metadata.Path);
		}
		Result<std::string> normalized = NormalizeAssetPath(metadata.Path);
		if (!normalized)
		{
			return Error{normalized.GetError()};
		}
		if (normalized.GetValue() != metadata.Path)
		{
			return MakeError("asset path '{}' is not normalized (expected '{}')", metadata.Path, normalized.GetValue());
		}
		if (m_Assets.contains(metadata.Handle))
		{
			return MakeError("handle {} is already registered", metadata.Handle);
		}
		if (m_Paths.contains(metadata.Path))
		{
			return MakeError("'{}' is already registered", metadata.Path);
		}

		metadata.Name.clear();
		metadata.Parent = AssetHandle();
		m_Paths.emplace(metadata.Path, metadata.Handle);
		m_Assets.emplace(metadata.Handle, std::move(metadata));
		return {};
	}

	bool AssetRegistry::Remove(AssetHandle handle)
	{
		auto const it = m_Assets.find(handle);
		if (it == m_Assets.end())
		{
			return false;
		}
		m_Paths.erase(it->second.Path);
		m_Assets.erase(it);
		return true;
	}

	Result<void> AssetRegistry::SetPath(AssetHandle handle, std::string_view path)
	{
		auto const it = m_Assets.find(handle);
		if (it == m_Assets.end())
		{
			return MakeError("asset {} is not registered", handle);
		}
		Result<std::string> normalized = NormalizeAssetPath(path);
		if (!normalized)
		{
			return Error{normalized.GetError()};
		}
		std::string const& newPath = normalized.GetValue();
		if (newPath == it->second.Path)
		{
			return {};
		}
		if (m_Paths.contains(newPath))
		{
			return MakeError("'{}' is already registered", newPath);
		}

		m_Paths.erase(it->second.Path);
		m_Paths.emplace(newPath, handle);
		it->second.Path = newPath;
		return {};
	}

	Result<void> AssetRegistry::SetType(AssetHandle handle, AssetType type)
	{
		auto const it = m_Assets.find(handle);
		if (it == m_Assets.end())
		{
			return MakeError("asset {} is not registered", handle);
		}
		if (type == AssetType::None)
		{
			return MakeError("asset '{}' needs a type", it->second.Path);
		}
		it->second.Type = type;
		return {};
	}

	std::vector<AssetMetadata> AssetRegistry::GetAll() const
	{
		std::vector<AssetMetadata> assets;
		assets.reserve(m_Paths.size());
		for (auto const& [path, handle] : m_Paths)
		{
			assets.push_back(m_Assets.at(handle));
		}
		return assets;
	}

	void AssetRegistry::Clear()
	{
		m_Assets.clear();
		m_Paths.clear();
	}

	Json AssetRegistry::Serialize() const
	{
		Json document = Json::object();
		document["Strada"] = MakeFileHeader("AssetRegistry", FormatVersion);
		Json assets = Json::array();
		for (auto const& [path, handle] : m_Paths)
		{
			AssetMetadata const& metadata = m_Assets.at(handle);
			Json entry = Json::object();
			entry["Handle"] = JsonTraits<AssetHandle>::ToJson(metadata.Handle);
			entry["Type"] = AssetTypeToString(metadata.Type);
			entry["Path"] = metadata.Path;
			assets.push_back(std::move(entry));
		}
		document["Assets"] = std::move(assets);
		return document;
	}

	Result<AssetRegistry> AssetRegistry::Deserialize(Json const& json, DeserializationContext const& context)
	{
		if (Result<int> header = ReadFileHeader(json, "AssetRegistry", FormatVersion); !header)
		{
			return Error{header.GetError()};
		}
		auto const assets = json.find("Assets");
		if (assets == json.end() || !assets->is_array())
		{
			return Error{"asset registry has no \"Assets\" array"};
		}

		AssetRegistry registry;
		for (size_t i = 0; i < assets->size(); i++)
		{
			Json const& entry = (*assets)[i];
			if (!entry.is_object())
			{
				context.Warn(fmt::format("Assets[{}]: expected an object; entry skipped", i));
				continue;
			}

			AssetMetadata metadata;
			auto const handle = entry.find("Handle");
			auto const type = entry.find("Type");
			auto const path = entry.find("Path");
			if (handle == entry.end() || type == entry.end() || path == entry.end() || !type->is_string() || !path->is_string())
			{
				context.Warn(fmt::format("Assets[{}]: expected \"Handle\", \"Type\" and \"Path\"; entry skipped", i));
				continue;
			}

			UUID id = UUID::Invalid();
			if (Result<void> result = JsonTraits<UUID>::FromJson(*handle, id, context); !result)
			{
				context.Warn(fmt::format("Assets[{}].Handle: {}; entry skipped", i, result.GetError()));
				continue;
			}
			std::optional<AssetType> const assetType = AssetTypeFromString(type->get_ref<std::string const&>());
			if (!assetType || *assetType == AssetType::None)
			{
				context.Warn(fmt::format("Assets[{}]: unknown asset type '{}'; entry skipped", i, type->get_ref<std::string const&>()));
				continue;
			}

			metadata.Handle = AssetHandle(id);
			metadata.Type = *assetType;
			metadata.Path = path->get<std::string>();
			if (Result<void> result = registry.Add(std::move(metadata)); !result)
			{
				context.Warn(fmt::format("Assets[{}]: {}; entry skipped", i, result.GetError()));
			}
		}
		return registry;
	}

	Result<void> AssetRegistry::SaveToFile(std::filesystem::path const& path) const
	{
		return FileSystem::WriteTextFile(path, DumpJson(Serialize()));
	}

	Result<AssetRegistry> AssetRegistry::LoadFromFile(std::filesystem::path const& path, DeserializationContext const& context)
	{
		Result<std::string> text = FileSystem::ReadTextFile(path);
		if (!text)
		{
			return Error{text.GetError()};
		}
		Result<Json> json = ParseJson(text.GetValue());
		if (!json)
		{
			return MakeError("'{}': {}", FileSystem::PathToUtf8(path), json.GetError());
		}
		Result<AssetRegistry> registry = Deserialize(json.GetValue(), context);
		if (!registry)
		{
			return MakeError("'{}': {}", FileSystem::PathToUtf8(path), registry.GetError());
		}
		return registry;
	}
}
