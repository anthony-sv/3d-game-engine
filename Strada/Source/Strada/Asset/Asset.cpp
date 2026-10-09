#include "stpch.h"
#include "Strada/Asset/Asset.h"

#include <array>
#include <cctype>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr std::array<std::pair<AssetType, char const*>, 9> s_TypeNames = {{
			{AssetType::None, "None"},
			{AssetType::Scene, "Scene"},
			{AssetType::Prefab, "Prefab"},
			{AssetType::Mesh, "Mesh"},
			{AssetType::Material, "Material"},
			{AssetType::Texture, "Texture"},
			{AssetType::Environment, "Environment"},
			{AssetType::AudioClip, "AudioClip"},
			{AssetType::Font, "Font"},
		}};

		struct ExtensionMapping
		{
			std::string_view Extension;
			AssetType Type;
		};

		constexpr std::array<ExtensionMapping, 17> s_Extensions = {{
			{".sscene", AssetType::Scene},
			{".sprefab", AssetType::Prefab},
			{".gltf", AssetType::Mesh},
			{".glb", AssetType::Mesh},
			{".fbx", AssetType::Mesh},
			{".obj", AssetType::Mesh},
			{".smat", AssetType::Material},
			{".png", AssetType::Texture},
			{".jpg", AssetType::Texture},
			{".jpeg", AssetType::Texture},
			{".tga", AssetType::Texture},
			{".bmp", AssetType::Texture},
			{".hdr", AssetType::Environment},
			{".wav", AssetType::AudioClip},
			{".mp3", AssetType::AudioClip},
			{".flac", AssetType::AudioClip},
			{".ogg", AssetType::AudioClip},
		}};

		constexpr std::array<std::string_view, 2> s_FontExtensions = {".ttf", ".otf"};
	}

	char const* AssetTypeToString(AssetType type)
	{
		for (auto const& [value, name] : s_TypeNames)
		{
			if (value == type)
			{
				return name;
			}
		}
		return "None";
	}

	std::optional<AssetType> AssetTypeFromString(std::string_view name)
	{
		for (auto const& [value, typeName] : s_TypeNames)
		{
			if (name == typeName)
			{
				return value;
			}
		}
		return std::nullopt;
	}

	std::string AssetMetadata::GetDisplayName() const
	{
		if (!Name.empty())
		{
			return Name;
		}
		std::string_view path = Path;
		if (IsBuiltIn())
		{
			path.remove_prefix(std::string_view("builtin://").size());
		}
		size_t const separator = path.find_last_of('/');
		return std::string(separator == std::string_view::npos ? path : path.substr(separator + 1));
	}

	AssetHandle DeriveAssetHandle(AssetHandle parent, uint32_t kind, uint32_t index)
	{
		// SplitMix64 finalizer over the parent handle combined with (kind, index): well distributed and deterministic.
		uint64_t value = parent.GetUUID().GetValue() ^ ((static_cast<uint64_t>(kind) << 32 | index) * 0x9E3779B97F4A7C15ull);
		value ^= value >> 30;
		value *= 0xBF58476D1CE4E5B9ull;
		value ^= value >> 27;
		value *= 0x94D049BB133111EBull;
		value ^= value >> 31;
		if (value < AssetHandle::ReservedCount)
		{
			value += AssetHandle::ReservedCount;
		}
		return AssetHandle(UUID(value));
	}

	AssetType GetAssetTypeForExtension(std::string_view extension)
	{
		std::string lower(extension);
		for (char& character : lower)
		{
			character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		}

		for (ExtensionMapping const& mapping : s_Extensions)
		{
			if (mapping.Extension == lower)
			{
				return mapping.Type;
			}
		}
		for (std::string_view const fontExtension : s_FontExtensions)
		{
			if (fontExtension == lower)
			{
				return AssetType::Font;
			}
		}
		return AssetType::None;
	}
}
