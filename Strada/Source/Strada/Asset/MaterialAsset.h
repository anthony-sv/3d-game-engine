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
			Field("BaseColor", &MaterialData::BaseColor).Range(0.0, 1.0).AsColor().Doc("Albedo (linear) and opacity."),
			Field("Metallic", &MaterialData::Metallic).Range(0.0, 1.0), Field("Roughness", &MaterialData::Roughness).Range(0.0, 1.0),
			Field("EmissiveColor", &MaterialData::EmissiveColor).Range(0.0, 1.0).AsColor().Doc("Emitted light color (linear)."),
			Field("EmissiveIntensity", &MaterialData::EmissiveIntensity).AtLeast(0.0).Doc("Multiplier of the emitted light."),
			Field("NormalStrength", &MaterialData::NormalStrength).AtLeast(0.0).Doc("Scales the bumps of the normal map."),
			Field("OcclusionStrength", &MaterialData::OcclusionStrength)
				.Range(0.0, 1.0)
				.Doc("How much the occlusion map darkens indirect light."),
			Field("BaseColorTexture", &MaterialData::BaseColorTexture).References("Texture").Doc("sRGB colors, opacity in alpha."),
			Field("NormalTexture", &MaterialData::NormalTexture).References("Texture").Doc("Tangent-space normal map (+Y up)."),
			Field("MetallicRoughnessTexture", &MaterialData::MetallicRoughnessTexture)
				.References("Texture")
				.Doc("glTF packing: roughness in green, metallic in blue; multiplied with the factors."),
			Field("OcclusionTexture", &MaterialData::OcclusionTexture).References("Texture").Doc("Ambient occlusion in red."),
			Field("EmissiveTexture", &MaterialData::EmissiveTexture)
				.References("Texture")
				.Doc("sRGB emitted color, multiplied with EmissiveColor and EmissiveIntensity."),
			Field("AlphaMode", &MaterialData::AlphaMode)
				.Doc("Opaque ignores alpha, Mask discards pixels below AlphaCutoff, Blend is transparent."),
			Field("AlphaCutoff", &MaterialData::AlphaCutoff).Range(0.0, 1.0),
			Field("DoubleSided", &MaterialData::DoubleSided).Doc("Renders back faces too."),
			Field("UVTiling", &MaterialData::UVTiling).Doc("Texture coordinate scale."),
			Field("UVOffset", &MaterialData::UVOffset).Doc("Texture coordinate offset."));
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
