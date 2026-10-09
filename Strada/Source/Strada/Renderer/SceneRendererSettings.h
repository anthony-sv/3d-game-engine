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

		// Linear multiplier applied to scene radiance: 1 / (1.2 * 2^EV100).
		float GetExposure() const { return 1.0f / (1.2f * std::exp2(EV100)); }

		bool operator==(SceneRendererSettings const& other) const = default;
	};

	template<>
	struct StructTraits<SceneRendererSettings>
	{
		static constexpr std::string_view Name = "Renderer";
		static constexpr auto Fields =
			std::make_tuple(Field("EV100", &SceneRendererSettings::EV100), Field("Tonemapper", &SceneRendererSettings::Tonemapper),
		                    Field("Dithering", &SceneRendererSettings::Dithering));
	};
}
