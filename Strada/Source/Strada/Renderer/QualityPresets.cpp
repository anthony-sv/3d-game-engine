#include "stpch.h"
#include "Strada/Renderer/QualityPresets.h"

#include <algorithm>

namespace Strada
{
	namespace
	{
		// The shortest shadow distance a preset leaves: the field's own minimum.
		constexpr float MinShadowDistance = 0.1f;

		// A shadow map's size divided, never below the presets' minimum and never above the size itself.
		uint32_t LowerShadowMapSize(uint32_t size, uint32_t divisor)
		{
			return std::min(size, std::max(size / divisor, QualityPresetMinShadowMapSize));
		}

		float LowerShadowDistance(float distance, float scale)
		{
			return std::min(distance, std::max(distance * scale, MinShadowDistance));
		}
	}

	SceneRendererSettings ApplyQualityPreset(SceneRendererSettings settings, QualityPreset preset)
	{
		switch (preset)
		{
			case QualityPreset::High:
				// The resolved settings exactly: pictures and measurements at High are the scene's own.
				return settings;
			case QualityPreset::Medium:
				settings.ShadowMapSize = LowerShadowMapSize(settings.ShadowMapSize, 2);
				settings.LocalShadowMapSize = LowerShadowMapSize(settings.LocalShadowMapSize, 2);
				settings.CascadeCount = std::min(settings.CascadeCount, QualityPresetMediumMaxCascades);
				settings.ShadowDistance = LowerShadowDistance(settings.ShadowDistance, QualityPresetMediumShadowDistanceScale);
				return settings;
			case QualityPreset::Low:
				settings.ShadowMapSize = LowerShadowMapSize(settings.ShadowMapSize, 4);
				settings.LocalShadowMapSize = LowerShadowMapSize(settings.LocalShadowMapSize, 4);
				settings.CascadeCount = std::min(settings.CascadeCount, QualityPresetLowMaxCascades);
				settings.ShadowDistance = LowerShadowDistance(settings.ShadowDistance, QualityPresetLowShadowDistanceScale);
				settings.SoftShadows = false;
				settings.AmbientOcclusion = false;
				settings.Bloom = false;
				settings.FXAA = false;
				return settings;
		}
		// A value outside the enum (none the settings' reader or setters let through) renders as the scene's own.
		return settings;
	}
}
