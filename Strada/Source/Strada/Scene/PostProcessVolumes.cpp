#include "stpch.h"
#include "Strada/Scene/PostProcessVolumes.h"

#include "Strada/Scene/ComponentTraits.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace Strada::PostProcessVolumes
{
	namespace
	{
		// Box axes shorter than this are flat.
		constexpr float MinAxisLength = 1e-6f;

		// Scene files, the editor and scripts only give finite numbers; others (set by native code) are ignored.
		void BlendNumber(float& value, float target, float blend)
		{
			if (std::isfinite(target))
			{
				value = glm::mix(value, target, blend);
			}
		}

		void BlendColor(glm::vec3& value, glm::vec3 const& target, float blend)
		{
			if (std::isfinite(target.r) && std::isfinite(target.g) && std::isfinite(target.b))
			{
				value = glm::mix(value, target, blend);
			}
		}

		// Ambient occlusion and the tonemapper have no strength the renderer could fade them by.
		template<typename T>
		void BlendSwitch(T& value, T target, float blend)
		{
			if (blend >= SwitchBlend)
			{
				value = target;
			}
		}

		struct ActiveVolume
		{
			PostProcessComponent const* Volume = nullptr;
			float Blend = 0.0f;
		};

		// A priority that is not a number sorts first, keeping the order strict.
		float GetSortKey(ActiveVolume const& volume)
		{
			return std::isnan(volume.Volume->Priority) ? -std::numeric_limits<float>::infinity() : volume.Volume->Priority;
		}
	}

	Box GetBox(glm::mat4 const& worldTransform)
	{
		Box box;
		box.Center = glm::vec3(worldTransform[3]);
		std::array<bool, 3> known{};
		for (glm::length_t axis = 0; axis < 3; axis++)
		{
			glm::vec3 const column(worldTransform[axis]);
			// Divided by its largest component first, so measuring a huge axis does not overflow.
			float const largest = std::max({std::abs(column.x), std::abs(column.y), std::abs(column.z)});
			if (!(largest >= MinAxisLength))
			{
				box.HalfExtents[axis] = 0.0f;
				continue;
			}
			glm::vec3 const scaled = column / largest;
			float const scaledLength = glm::length(scaled);
			box.Axes[static_cast<size_t>(axis)] = scaled / scaledLength;
			box.HalfExtents[axis] = 0.5f * largest * scaledLength;
			known[static_cast<size_t>(axis)] = true;
		}
		// A flat axis takes the first world axis that keeps a clear direction once the known axes are taken out of it.
		for (size_t axis = 0; axis < 3; axis++)
		{
			if (known[axis])
			{
				continue;
			}
			for (glm::vec3 candidate : {glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)})
			{
				for (size_t other = 0; other < 3; other++)
				{
					if (known[other])
					{
						candidate -= glm::dot(candidate, box.Axes[other]) * box.Axes[other];
					}
				}
				float const length = glm::length(candidate);
				if (length > 0.1f)
				{
					box.Axes[axis] = candidate / length;
					known[axis] = true;
					break;
				}
			}
		}
		return box;
	}

	float GetDistance(Box const& box, glm::vec3 const& point)
	{
		glm::vec3 const offset = point - box.Center;
		float outsideSquared = 0.0f;
		for (size_t axis = 0; axis < 3; axis++)
		{
			float const along = glm::dot(offset, box.Axes[axis]);
			float const outside = std::max(std::abs(along) - box.HalfExtents[static_cast<glm::length_t>(axis)], 0.0f);
			outsideSquared += outside * outside;
		}
		return std::sqrt(outsideSquared);
	}

	float GetBlend(PostProcessComponent const& volume, glm::mat4 const& worldTransform, glm::vec3 const& cameraPosition)
	{
		float const weight = std::clamp(volume.Weight, 0.0f, 1.0f);
		if (!(weight > 0.0f))
		{
			return 0.0f;
		}
		if (volume.Global)
		{
			return weight;
		}
		float const distance = GetDistance(GetBox(worldTransform), cameraPosition);
		if (distance <= 0.0f)
		{
			return weight;
		}
		// Distances that are not finite (transforms beyond the float range) fail the comparison: the volume stays out.
		float const blendDistance = std::max(volume.BlendDistance, 0.0f);
		return distance < blendDistance ? weight * (1.0f - distance / blendDistance) : 0.0f;
	}

	Blender::Blender(SceneRendererSettings const& settings)
		: m_Settings(settings),
		  m_BloomAmount(settings.Bloom ? 1.0f : 0.0f)
	{
	}

	void Blender::Apply(PostProcessComponent const& volume, float blend)
	{
		if (!(blend > 0.0f))
		{
			return;
		}
		blend = std::min(blend, 1.0f);
		PostProcessComponent values = volume;
		ClampToFieldRanges<ComponentTraits<PostProcessComponent>>(values);

		if (values.OverrideEV100)
		{
			BlendNumber(m_Settings.EV100, values.EV100, blend);
		}
		if (values.OverrideTonemapper)
		{
			BlendSwitch(m_Settings.Tonemapper, values.Tonemapper, blend);
		}
		if (values.OverrideAmbientOcclusion)
		{
			BlendSwitch(m_Settings.AmbientOcclusion, values.AmbientOcclusion, blend);
		}
		if (values.OverrideAmbientOcclusionIntensity)
		{
			BlendNumber(m_Settings.AmbientOcclusionIntensity, values.AmbientOcclusionIntensity, blend);
		}
		if (values.OverrideBloom)
		{
			// Clamped so that rounding never takes it past fully on or off.
			m_BloomAmount = std::clamp(glm::mix(m_BloomAmount, values.Bloom ? 1.0f : 0.0f, blend), 0.0f, 1.0f);
		}
		if (values.OverrideBloomIntensity)
		{
			BlendNumber(m_Settings.BloomIntensity, values.BloomIntensity, blend);
		}
		if (values.OverrideContrast)
		{
			BlendNumber(m_Settings.Contrast, values.Contrast, blend);
		}
		if (values.OverrideSaturation)
		{
			BlendNumber(m_Settings.Saturation, values.Saturation, blend);
		}
		if (values.OverrideTemperature)
		{
			BlendNumber(m_Settings.Temperature, values.Temperature, blend);
		}
		if (values.OverrideTint)
		{
			BlendNumber(m_Settings.Tint, values.Tint, blend);
		}
		if (values.OverrideColorFilter)
		{
			BlendColor(m_Settings.ColorFilter, values.ColorFilter, blend);
		}
		if (values.OverrideChromaticAberration)
		{
			BlendNumber(m_Settings.ChromaticAberration, values.ChromaticAberration, blend);
		}
		if (values.OverrideVignette)
		{
			BlendNumber(m_Settings.Vignette, values.Vignette, blend);
		}
		if (values.OverrideVignetteSmoothness)
		{
			BlendNumber(m_Settings.VignetteSmoothness, values.VignetteSmoothness, blend);
		}
		if (values.OverrideFilmGrain)
		{
			BlendNumber(m_Settings.FilmGrain, values.FilmGrain, blend);
		}
	}

	SceneRendererSettings Blender::GetSettings() const
	{
		// The renderer treats bloom that is off like bloom with no intensity, so the fade reaches it without a step.
		SceneRendererSettings settings = m_Settings;
		settings.Bloom = m_BloomAmount > 0.0f;
		if (settings.Bloom)
		{
			settings.BloomIntensity *= m_BloomAmount;
		}
		return settings;
	}

	SceneRendererSettings Resolve(Scene& scene, glm::vec3 const& cameraPosition)
	{
		SceneRendererSettings const& settings = scene.GetSettings().Renderer;
		// Most scenes have no volumes: they skip the walk through the hierarchy.
		if (scene.GetAllEntitiesWith<PostProcessComponent>().empty())
		{
			return settings;
		}

		std::vector<ActiveVolume> volumes;
		scene.ForEachEntityInHierarchyOrder(
			[&](Entity entity)
			{
				if (PostProcessComponent const* volume = entity.TryGetComponent<PostProcessComponent>())
				{
					float const blend = GetBlend(*volume, scene.GetWorldTransform(entity), cameraPosition);
					if (blend > 0.0f)
					{
						volumes.push_back({volume, blend});
					}
				}
			});
		std::stable_sort(volumes.begin(), volumes.end(),
		                 [](ActiveVolume const& a, ActiveVolume const& b)
		                 {
							 return GetSortKey(a) < GetSortKey(b);
						 });
		Blender blender(settings);
		for (ActiveVolume const& active : volumes)
		{
			blender.Apply(*active.Volume, active.Blend);
		}
		return blender.GetSettings();
	}
}
