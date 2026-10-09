#pragma once

#include "Strada/Serialization/StructSerialization.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <utility>

namespace Strada
{
	enum class TonemapOperator : uint8_t
	{
		// Stephen Hill's fit of the ACES RRT + ODT.
		ACES = 0,
		AgX,
		// Khronos PBR Neutral: faithful base colors, for product-style rendering.
		PBRNeutral,
		Reinhard,
		// Clamp only.
		None
	};

	template<>
	struct EnumTraits<TonemapOperator>
	{
		static constexpr std::array<std::pair<TonemapOperator, std::string_view>, 5> Values = {{
			{TonemapOperator::ACES, "ACES"},
			{TonemapOperator::AgX, "AgX"},
			{TonemapOperator::PBRNeutral, "PBRNeutral"},
			{TonemapOperator::Reinhard, "Reinhard"},
			{TonemapOperator::None, "None"},
		}};
	};

	// Per-scene rendering options, stored in scene files under "Settings": { "Renderer": { ... } }.
	struct SceneRendererSettings
	{
		// Exposure value at ISO 100. Light intensities are artist-friendly multipliers, so the default 0 suits them.
		float EV100 = 0.0f;
		TonemapOperator Tonemapper = TonemapOperator::ACES;
		// Adds sub-quantization noise against banding in gradients.
		bool Dithering = true;

		bool Shadows = true;
		// Percentage-closer soft shadows sized by the directional light's angular diameter; off uses a fixed filter.
		bool SoftShadows = true;
		// View distance covered by the directional light's cascades.
		float ShadowDistance = 100.0f;
		// 1-4.
		uint32_t CascadeCount = 4;
		// Blends uniform (0) and logarithmic (1) cascade splits.
		float CascadeSplitLambda = 0.75f;
		// Texels per side of each cascade and of each local light shadow map (rounded down to a power of two).
		uint32_t ShadowMapSize = 2048;
		uint32_t LocalShadowMapSize = 1024;

		// Ground-truth ambient occlusion of indirect light.
		bool AmbientOcclusion = true;
		// World-space search radius (0.01-10) and contrast exponent (0.1-8).
		float AmbientOcclusionRadius = 0.75f;
		float AmbientOcclusionIntensity = 1.5f;

		bool Bloom = true;
		// Fraction of the blurred image mixed into the scene (0-1).
		float BloomIntensity = 0.04f;

		// Fast approximate anti-aliasing after tonemapping.
		bool FXAA = true;

		// Linear multiplier applied to scene radiance: 1 / (1.2 * 2^EV100).
		float GetExposure() const { return 1.0f / (1.2f * std::exp2(EV100)); }

		bool operator==(SceneRendererSettings const& other) const = default;
	};

	template<>
	struct StructTraits<SceneRendererSettings>
	{
		static constexpr std::string_view Name = "Renderer";
		static constexpr auto Fields = std::make_tuple(
			Field("EV100", &SceneRendererSettings::EV100)
				.Range(-20.0, 20.0)
				.Doc("Exposure value at ISO 100; higher values darken the image."),
			Field("Tonemapper", &SceneRendererSettings::Tonemapper).Doc("Maps HDR colors to the display range."),
			Field("Dithering", &SceneRendererSettings::Dithering).Doc("Adds subtle noise against banding in gradients."),
			Field("Shadows", &SceneRendererSettings::Shadows),
			Field("SoftShadows", &SceneRendererSettings::SoftShadows)
				.Doc("Contact-hardening shadows sized by the directional light's LightSize; off uses a fixed filter."),
			Field("ShadowDistance", &SceneRendererSettings::ShadowDistance)
				.AtLeast(0.1)
				.Doc("View distance in meters covered by the directional light's shadows."),
			Field("CascadeCount", &SceneRendererSettings::CascadeCount).Range(1.0, 4.0).Doc("Number of directional shadow cascades."),
			Field("CascadeSplitLambda", &SceneRendererSettings::CascadeSplitLambda)
				.Range(0.0, 1.0)
				.Doc("Blends uniform (0) and logarithmic (1) cascade distribution."),
			Field("ShadowMapSize", &SceneRendererSettings::ShadowMapSize)
				.Range(256.0, 8192.0)
				.Doc("Texels per side of each cascade (rounded down to a power of two)."),
			Field("LocalShadowMapSize", &SceneRendererSettings::LocalShadowMapSize)
				.Range(256.0, 8192.0)
				.Doc("Texels per side of each spot and point light shadow map (rounded down to a power of two)."),
			Field("AmbientOcclusion", &SceneRendererSettings::AmbientOcclusion).Doc("Darkens indirect light in creases and corners."),
			Field("AmbientOcclusionRadius", &SceneRendererSettings::AmbientOcclusionRadius)
				.Range(0.01, 10.0)
				.Doc("World-space search radius of ambient occlusion in meters."),
			Field("AmbientOcclusionIntensity", &SceneRendererSettings::AmbientOcclusionIntensity)
				.Range(0.1, 8.0)
				.Doc("Contrast exponent of ambient occlusion."),
			Field("Bloom", &SceneRendererSettings::Bloom).Doc("Glow around bright areas."),
			Field("BloomIntensity", &SceneRendererSettings::BloomIntensity)
				.Range(0.0, 1.0)
				.Doc("Fraction of the blurred image mixed into the scene."),
			Field("FXAA", &SceneRendererSettings::FXAA).Doc("Fast approximate anti-aliasing."));
	};
}
