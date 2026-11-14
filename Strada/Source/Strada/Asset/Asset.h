#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Base.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	enum class AssetType : uint8_t
	{
		None = 0,
		Scene,
		Prefab,
		Mesh,
		Material,
		Texture,
		Environment,
		AudioClip,
		Font,
		VisualScript
	};

	// The name of a built-in or registered data asset type (DataAssetRegistry, §6.4 of Docs/Architecture.md); "None" for
	// values no type has. A data asset type's name stays valid as long as its type (the engine's: the whole process).
	char const* AssetTypeToString(AssetType type);
	// Built-in types and registered data asset types, by their exact names.
	std::optional<AssetType> AssetTypeFromString(std::string_view name);
	// Maps a file extension (".glb", ".PNG", ...) to the asset type it imports as, built-in or a registered data asset type;
	// None for unsupported files.
	AssetType GetAssetTypeForExtension(std::string_view extension);
	// Every extension that imports as an asset (lowercase, with the dot), in a stable order: the built-in types', then the
	// registered data asset types' in registration order.
	std::vector<std::string_view> GetSupportedAssetExtensions();
	// The lookups above for the built-in types only, which the data asset registry checks registrations against.
	std::optional<AssetType> BuiltInAssetTypeFromString(std::string_view name);
	AssetType GetBuiltInAssetTypeForExtension(std::string_view extension);

	// Base class of every loaded asset. Assets are CPU-side data; GPU resources are created from them by the renderer.
	class Asset
	{
	public:
		virtual ~Asset() = default;
		virtual AssetType GetAssetType() const = 0;

		// Assigned by the AssetManager when the asset is registered or loaded.
		AssetHandle Handle;
	};

	// Describes a registered asset.
	struct AssetMetadata
	{
		AssetHandle Handle;
		AssetType Type = AssetType::None;
		// File assets: path relative to the asset directory with forward slashes. Built-in assets: "builtin://<Name>".
		// Empty for memory assets and sub-assets.
		std::string Path;
		// Display name of memory assets and sub-assets (e.g. "Helmet.glb/Materials/Visor"); empty for file assets.
		std::string Name;
		// The file asset that owns a sub-asset (embedded textures and materials of a mesh); invalid otherwise.
		AssetHandle Parent;

		bool IsBuiltIn() const { return Path.starts_with("builtin://"); }
		bool IsFileAsset() const { return !Path.empty() && !IsBuiltIn(); }
		bool IsMemoryAsset() const { return Path.empty() && !Parent.IsValid(); }
		bool IsSubAsset() const { return Parent.IsValid(); }
		// Name, or the file name (without directories) of file and built-in assets.
		std::string GetDisplayName() const;
	};

	// Deterministic handle of the index-th sub-asset of a kind (e.g. the 3rd embedded texture) owned by a parent asset, so a
	// reloaded mesh hands out the same sub-asset handles. Never invalid or reserved.
	AssetHandle DeriveAssetHandle(AssetHandle parent, uint32_t kind, uint32_t index);
}
