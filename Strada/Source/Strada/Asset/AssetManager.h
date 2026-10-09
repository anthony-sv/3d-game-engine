#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Strada
{
	struct AssetRefreshResult
	{
		// Files registered by this scan.
		std::vector<AssetHandle> Added;
		// Registered files that no longer exist. They stay registered so references survive if the file comes back; remove
		// them with DeleteAsset.
		std::vector<AssetHandle> Missing;
		// Loaded file assets whose file changed on disk; they were unloaded and load again on next use.
		std::vector<AssetHandle> Modified;
		// Problems found while reading the registry or scanning (invalid entries, unsupported file names).
		std::vector<std::string> Warnings;
	};

	// Owns asset metadata and loaded assets: built-in assets, the file assets of one asset directory (the open project's
	// Assets/ folder) with their persistent registry, memory assets created at runtime, and sub-assets created while
	// loading (the materials and embedded textures of a mesh, which live as long as the mesh stays loaded).
	// Assets load on first use. Main thread only.
	class AssetManager
	{
	public:
		static void Init();
		static void Shutdown();
		static bool IsInitialized();

		// --- Asset directory ---

		// Loads <directory>/AssetRegistry.sreg (when present), scans the directory, registers new files and saves the registry.
		// Fails when the directory does not exist or the registry file is corrupt (continuing would break references).
		[[nodiscard]] static Result<AssetRefreshResult> OpenAssetDirectory(std::filesystem::path const& directory);
		// Unloads and forgets every file, memory and sub-asset. Built-in assets stay.
		static void CloseAssetDirectory();
		static bool HasAssetDirectory();
		// Absolute path; empty when no directory is open.
		static std::filesystem::path const& GetAssetDirectory();
		// Rescans the open directory (new, missing and modified files).
		[[nodiscard]] static Result<AssetRefreshResult> Refresh();
		[[nodiscard]] static Result<void> SaveRegistry();

		// --- Registration ---

		// Registers an existing file inside the asset directory (absolute, or relative to the directory). Returns the existing
		// handle when the file is already registered.
		[[nodiscard]] static Result<AssetHandle> ImportFile(std::filesystem::path const& path);
		// Moves or renames a file asset on disk and in the registry; the handle (and every reference to it) stays valid.
		[[nodiscard]] static Result<void> MoveAsset(AssetHandle handle, std::string_view newPath);
		// Deletes a file asset from disk (when present) and from the registry.
		[[nodiscard]] static Result<void> DeleteAsset(AssetHandle handle);
		// Adds an asset that only exists in memory (runtime-created materials, ...). Returns its new handle.
		static AssetHandle AddMemoryAsset(Ref<Asset> asset, std::string name);
		// Removes a memory asset; other kinds of assets are not affected.
		static void RemoveMemoryAsset(AssetHandle handle);
		// Registers an engine-provided asset as "builtin://<name>". The handle must be reserved and unused.
		[[nodiscard]] static Result<void> RegisterBuiltInAsset(std::string_view name, AssetHandle handle, Ref<Asset> asset);

		// --- Queries ---

		// Registered (built-in, file, memory or loaded sub-asset).
		static bool IsValid(AssetHandle handle);
		static bool IsLoaded(AssetHandle handle);
		// A registered file asset whose file was not found by the last scan.
		static bool IsMissing(AssetHandle handle);
		static std::optional<AssetMetadata> GetMetadata(AssetHandle handle);
		static AssetType GetAssetType(AssetHandle handle);
		// Path relative to the asset directory (normalized first); invalid handle when not registered.
		static AssetHandle FindByPath(std::string_view path);
		static AssetHandle FindBuiltIn(std::string_view name);
		// Registered assets of a type (None = all): built-ins, then file assets sorted by path, then memory and sub-assets.
		static std::vector<AssetMetadata> GetAssets(AssetType type = AssetType::None);
		// "asset://<path>", "builtin://<name>" or a registered handle as a decimal string.
		[[nodiscard]] static Result<AssetHandle> ResolveReference(std::string_view reference);
		// "asset://<path>" for file assets, "builtin://<name>" for built-ins, the decimal handle otherwise.
		static std::string GetReference(AssetHandle handle);
		// Absolute path of a file asset; empty for other assets.
		static std::filesystem::path GetAbsolutePath(AssetHandle handle);
		// A context that resolves asset references through ResolveReference.
		static DeserializationContext CreateDeserializationContext(UnknownFieldPolicy unknownFields = UnknownFieldPolicy::Error,
		                                                           std::vector<std::string>* warnings = nullptr);

		// --- Loading ---

		// Loads the asset on first use. Load failures are logged once and remembered until the asset is reloaded or the
		// directory is refreshed, so per-frame lookups of a broken asset stay cheap and quiet.
		[[nodiscard]] static Result<Ref<Asset>> LoadAsset(AssetHandle handle);

		template<typename T>
		[[nodiscard]] static Result<Ref<T>> TryGetAsset(AssetHandle handle)
		{
			static_assert(std::is_base_of_v<Asset, T>, "T must derive from Strada::Asset");
			Result<Ref<Asset>> asset = LoadAsset(handle);
			if (!asset)
			{
				return Error{asset.GetError()};
			}
			if (asset.GetValue()->GetAssetType() != T::GetStaticType())
			{
				return MakeError("asset {} is a {}, not a {}", handle, AssetTypeToString(asset.GetValue()->GetAssetType()),
				                 AssetTypeToString(T::GetStaticType()));
			}
			return std::static_pointer_cast<T>(asset.GetValue());
		}

		// Null when the handle is invalid, the asset failed to load, or it has another type.
		template<typename T>
		static Ref<T> GetAsset(AssetHandle handle)
		{
			Result<Ref<T>> asset = TryGetAsset<T>(handle);
			return asset ? asset.GetValue() : nullptr;
		}

		// Reloads a file asset from disk (a sub-asset reloads its parent).
		[[nodiscard]] static Result<void> ReloadAsset(AssetHandle handle);
		// Drops a loaded file asset (and its sub-assets); it loads again on next use.
		static void UnloadAsset(AssetHandle handle);
	};
}
