#pragma once

#include "Strada/Renderer/SceneRendererSettings.h"
#include "Strada/Scene/Components.h"

#include <glm/glm.hpp>

#include <array>

namespace Strada
{
	class Scene;

	// How PostProcess volumes change a scene's renderer settings for a camera (see PostProcessComponent).
	namespace PostProcessVolumes
	{
		// The blend from which the switches that cannot fade (AmbientOcclusion, Tonemapper) take a volume's value.
		inline constexpr float SwitchBlend = 0.5f;

		// The box of a local volume: the unit cube centered on the entity, rotated and scaled by its world transform.
		struct Box
		{
			glm::vec3 Center = glm::vec3(0.0f);
			// Unit directions of the transform's axes. An axis scaled to nothing (a flat box) gets a direction square to the
			// others; a shearing transform leaves them oblique, and the box is measured along them.
			std::array<glm::vec3, 3> Axes = {glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)};
			// Half the box's size along each axis, in meters.
			glm::vec3 HalfExtents = glm::vec3(0.5f);
		};

		Box GetBox(glm::mat4 const& worldTransform);

		// Distance in meters from a point to a box, measured along its axes; 0 inside.
		float GetDistance(Box const& box, glm::vec3 const& point);

		// How much of a volume applies to a camera at cameraPosition, 0 to 1: its Weight, for local volumes times a fade
		// from 1 at its box to 0 at BlendDistance from it (0 anywhere outside the box without a blend distance).
		float GetBlend(PostProcessComponent const& volume, glm::mat4 const& worldTransform, glm::vec3 const& cameraPosition);

		// Applies volumes to renderer settings one after another, each over the ones before it (Resolve applies them lowest
		// priority first). Numbers move from the current values towards a volume's by its blend; AmbientOcclusion and
		// Tonemapper take its value from a blend of SwitchBlend. Bloom fades instead of switching: how much of it is on (0 to
		// 1) moves towards the volume's Bloom by the blend like a number and scales the bloom intensity, so a volume that
		// turns bloom off fades it out as the camera comes in, and one that turns it on fades it in, also over a volume
		// that faded it out.
		class Blender
		{
		public:
			explicit Blender(SceneRendererSettings const& settings);

			// Applies the settings a volume overrides with a blend (0 to 1; less does nothing, more counts as 1). Volume
			// values outside their field ranges (scripts can set any finite value) act as the nearest bound; numbers that
			// are not finite are ignored.
			void Apply(PostProcessComponent const& volume, float blend);

			// The settings with the volumes applied so far. Partly faded bloom is on with its intensity scaled; bloom
			// faded out completely is off and keeps the intensity the volumes set.
			SceneRendererSettings GetSettings() const;

		private:
			SceneRendererSettings m_Settings;
			// How much of the bloom is on, 0 to 1; m_Settings.BloomIntensity is its intensity when fully on.
			float m_BloomAmount = 0.0f;
		};

		// The scene's renderer settings with its PostProcess volumes applied for a camera at cameraPosition: in order of
		// Priority, lowest first, and in hierarchy order between equal priorities, each with its blend.
		SceneRendererSettings Resolve(Scene& scene, glm::vec3 const& cameraPosition);
	}
}
