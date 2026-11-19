#pragma once

#include "Strada/Renderer/SceneRendererSettings.h"
#include "Strada/Serialization/StructSerialization.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

// The player's graphics quality (Docs/Architecture.md §6.2, "Quality presets", and §13.1): presets made only of the scene
// renderer's existing settings, applied over a view's resolved settings (the scene's, with its post-process volumes) in the
// game's views alone: the player's window and the editor's Game View while a game plays, never the editor's Scene View or
// asset thumbnails. High leaves the resolved settings as they are, so pictures, goldens and measurements at High are those
// of the scene itself; Medium and Low only lower a setting, never raise one, so a scene that turns something off keeps it
// off at every preset. Pure functions, any thread.
//
// Where it applies: SceneRenderOptions gains `QualityPreset Quality = QualityPreset::High;` (Scene/SceneRendering.h), which
// RenderScene applies to PostProcessVolumes::Resolve's result before BeginScene when the view draws the world (a
// screen-space-only frame keeps its own settings). Its default is the identity, so a view that never sets it (the Scene
// View, thumbnails, tests) renders as before by construction; the player's RuntimeLayer and the editor's Game View set it
// from the player's settings (Script/PlayerSettings.h, PlayerSettingsStore::Get().Quality) every frame. The renderer
// already takes a change of shadow map size or cascade count from one frame to the next (it recreates the shadow maps
// it needs), as post-process volumes may change settings per frame.
namespace Strada
{
	enum class QualityPreset : uint8_t
	{
		Low = 0,
		Medium,
		// The resolved settings, unchanged.
		High
	};

	template<>
	struct EnumTraits<QualityPreset>
	{
		static constexpr std::array<std::pair<QualityPreset, std::string_view>, 3> Values = {{
			{QualityPreset::Low, "Low"},
			{QualityPreset::Medium, "Medium"},
			{QualityPreset::High, "High"},
		}};
	};

	// What each preset makes of the resolved settings, each value only ever lowered (a minimum with the resolved value,
	// or a switch that can only turn off):
	//
	// | Setting | Medium | Low |
	// |---|---|---|
	// | ShadowMapSize, LocalShadowMapSize | half, at least 256 | a quarter, at least 256 |
	// | CascadeCount | at most 3 | at most 2 |
	// | ShadowDistance | three quarters | half |
	// | SoftShadows | as resolved | off |
	// | AmbientOcclusion | as resolved | off |
	// | Bloom | as resolved | off |
	// | FXAA | as resolved | off |
	//
	// Shadows themselves are never turned off by a preset (a scene without them reads flat); everything else (exposure,
	// tonemapping, grading, lens effects, dithering) is the scene's at every preset. A value already below a preset's is
	// kept: 256 stays 256, a distance of 0.1 stays 0.1 (the field's minimum), a cascade count of 1 stays 1.
	inline constexpr uint32_t QualityPresetMinShadowMapSize = 256;
	inline constexpr uint32_t QualityPresetMediumMaxCascades = 3;
	inline constexpr uint32_t QualityPresetLowMaxCascades = 2;
	inline constexpr float QualityPresetMediumShadowDistanceScale = 0.75f;
	inline constexpr float QualityPresetLowShadowDistanceScale = 0.5f;

	// The resolved settings at a preset: High returns them unchanged; Medium and Low as the table above says.
	[[nodiscard]] SceneRendererSettings ApplyQualityPreset(SceneRendererSettings settings, QualityPreset preset);
}
