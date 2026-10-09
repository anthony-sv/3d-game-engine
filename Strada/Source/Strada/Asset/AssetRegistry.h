#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Strada
{
	// Normalizes a path relative to an asset directory: forward slashes, no empty, "." or ".." segments, no leading slash or
	// drive. Fails for paths that escape the directory, are empty, or contain characters some platforms cannot store
	// (control characters and <>:"|?*). Paths are case-sensitive everywhere so projects stay portable.
	[[nodiscard]] Result<std::string> NormalizeAssetPath(std::string_view path);

	// The persistent handle <-> file mapping of a project's asset directory (Assets/AssetRegistry.sreg). Handles stay stable
	// while files are moved through the AssetManager, so scenes keep their references.
	class AssetRegistry
	{
	public:
		static constexpr int FormatVersion = 1;
		static constexpr std::string_view FileName = "AssetRegistry.sreg";

		bool Contains(AssetHandle handle) const { return m_Assets.contains(handle); }
		// Null when not registered. The pointer is invalidated by any modification.
		AssetMetadata const* Find(AssetHandle handle) const;
		// Invalid handle when not registered. The path must be normalized.
		AssetHandle FindByPath(std::string_view path) const;

		// Registers a file asset. Fails for invalid or reserved handles, type None, non-normalized paths, and handles or paths
		// that are already registered.
		[[nodiscard]] Result<void> Add(AssetMetadata metadata);
		bool Remove(AssetHandle handle);
		// Changes the path of a registered asset (the file itself is not touched).
		[[nodiscard]] Result<void> SetPath(AssetHandle handle, std::string_view path);
		[[nodiscard]] Result<void> SetType(AssetHandle handle, AssetType type);

		size_t GetCount() const { return m_Assets.size(); }
		// Sorted by path.
		std::vector<AssetMetadata> GetAll() const;
		void Clear();

		Json Serialize() const;
		// Invalid or duplicate entries are skipped with a warning (reported through the context) so one bad entry does not
		// lose the whole registry; structural errors fail.
		[[nodiscard]] static Result<AssetRegistry> Deserialize(Json const& json, DeserializationContext const& context);
		[[nodiscard]] Result<void> SaveToFile(std::filesystem::path const& path) const;
		[[nodiscard]] static Result<AssetRegistry> LoadFromFile(std::filesystem::path const& path, DeserializationContext const& context);

	private:
		std::unordered_map<AssetHandle, AssetMetadata> m_Assets;
		std::map<std::string, AssetHandle, std::less<>> m_Paths;
	};
}
