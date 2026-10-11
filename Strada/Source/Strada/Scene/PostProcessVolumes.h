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
		// The blend from which switches (Bloom, AmbientOcclusion, Tonemapper) take a volume's value.
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

		// Applies the settings a volume overrides with a blend (0 to 1): numbers move from the current values towards the
		// volume's by the blend, switches take the volume's value from a blend of SwitchBlend. Volume values outside their
		// field ranges (scripts can set any finite value) act as the nearest bound; numbers that are not finite are ignored.
		void Apply(SceneRendererSettings& settings, PostProcessComponent const& volume, float blend);

		// The scene's renderer settings with its PostProcess volumes applied for a camera at cameraPosition: in order of
		// Priority, lowest first, and in hierarchy order between equal priorities, each with its blend.
		SceneRendererSettings Resolve(Scene& scene, glm::vec3 const& cameraPosition);
	}
}
