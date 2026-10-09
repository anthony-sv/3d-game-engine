#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/StructSerialization.h"

#include <glm/glm.hpp>

#include <array>
#include <filesystem>
#include <utility>

namespace Strada
{
	enum class MaterialAlphaMode : uint8_t
	{
		// Alpha is ignored.
		Opaque = 0,
		// Pixels with alpha below AlphaCutoff are discarded.
		Mask,
		// Alpha blending (drawn after opaque geometry, sorted back to front).
		Blend
	};

	// Metallic-roughness PBR material parameters (glTF 2.0 model). Colors are linear.
	struct MaterialData
	{
		glm::vec4 BaseColor = glm::vec4(1.0f);
		float Metallic = 0.0f;
		float Roughness = 0.5f;
		glm::vec3 EmissiveColor = glm::vec3(0.0f);
		float EmissiveIntensity = 1.0f;
		// Scales the tangent-space XY of the normal map.
		float NormalStrength = 1.0f;
		// Blends the occlusion map in (0 = ignored, 1 = full).
		float OcclusionStrength = 1.0f;
		// Base color (sRGB, alpha in A).
		AssetHandle BaseColorTexture;
		// Tangent-space normal map (linear, +Y up).
		AssetHandle NormalTexture;
		// glTF packing (linear): G = roughness, B = metallic; multiplied with the factors.
		AssetHandle MetallicRoughnessTexture;
		// Ambient occlusion in R (linear).
		AssetHandle OcclusionTexture;
		// Emissive color (sRGB); multiplied with EmissiveColor * EmissiveIntensity.
		AssetHandle EmissiveTexture;
		MaterialAlphaMode AlphaMode = MaterialAlphaMode::Opaque;
		float AlphaCutoff = 0.5f;
		bool DoubleSided = false;
		glm::vec2 UVTiling = glm::vec2(1.0f);
		glm::vec2 UVOffset = glm::vec2(0.0f);

		bool operator==(MaterialData const& other) const = default;
	};

	template<>
	struct EnumTraits<MaterialAlphaMode>
	{
		static constexpr std::array<std::pair<MaterialAlphaMode, std::string_view>, 3> Values = {{
			{MaterialAlphaMode::Opaque, "Opaque"},
			{MaterialAlphaMode::Mask, "Mask"},
			{MaterialAlphaMode::Blend, "Blend"},
		}};
	};

	template<>
	struct StructTraits<MaterialData>
	{
		static constexpr std::string_view Name = "Material";
		static constexpr auto Fields = std::make_tuple(
			Field("BaseColor", &MaterialData::BaseColor), Field("Metallic", &MaterialData::Metallic),
			Field("Roughness", &MaterialData::Roughness), Field("EmissiveColor", &MaterialData::EmissiveColor),
			Field("EmissiveIntensity", &MaterialData::EmissiveIntensity), Field("NormalStrength", &MaterialData::NormalStrength),
			Field("OcclusionStrength", &MaterialData::OcclusionStrength), Field("BaseColorTexture", &MaterialData::BaseColorTexture),
			Field("NormalTexture", &MaterialData::NormalTexture),
			Field("MetallicRoughnessTexture", &MaterialData::MetallicRoughnessTexture),
			Field("OcclusionTexture", &MaterialData::OcclusionTexture), Field("EmissiveTexture", &MaterialData::EmissiveTexture),
			Field("AlphaMode", &MaterialData::AlphaMode), Field("AlphaCutoff", &MaterialData::AlphaCutoff),
			Field("DoubleSided", &MaterialData::DoubleSided), Field("UVTiling", &MaterialData::UVTiling),
			Field("UVOffset", &MaterialData::UVOffset));
	};

	// A material asset (.smat file, mesh-embedded material or runtime-created material).
	class MaterialAsset final : public Asset
	{
	public:
		MaterialAsset() = default;
		explicit MaterialAsset(MaterialData data)
			: m_Data(std::move(data))
		{
		}

		static AssetType GetStaticType() { return AssetType::Material; }
		AssetType GetAssetType() const override { return GetStaticType(); }

		MaterialData const& GetData() const { return m_Data; }
		void SetData(MaterialData data);
		// Incremented by every change; renderers compare it to refresh GPU parameters.
		uint64_t GetVersion() const { return m_Version; }

	private:
		MaterialData m_Data;
		uint64_t m_Version = 0;
	};

	// .smat files: { "Strada": { "Version", "Type": "Material" }, "Material": { <MaterialData fields> } }
	class MaterialSerializer
	{
	public:
		static constexpr int FormatVersion = 1;

		static Json Serialize(MaterialData const& data);
		// Missing fields keep their defaults. Texture fields accept handles and asset references (resolved by the context).
		[[nodiscard]] static Result<MaterialData> Deserialize(Json const& json, DeserializationContext const& context);
		[[nodiscard]] static Result<void> SaveToFile(MaterialData const& data, std::filesystem::path const& path);
		[[nodiscard]] static Result<MaterialData> LoadFromFile(std::filesystem::path const& path, DeserializationContext const& context);
	};
}
