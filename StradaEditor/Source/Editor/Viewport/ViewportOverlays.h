#pragma once

#include "Strada/Scene/Entity.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Strada
{
	class Scene;

	struct OverlayLine
	{
		glm::vec3 From = glm::vec3(0.0f);
		glm::vec3 To = glm::vec3(0.0f);
		// sRGB-encoded with straight alpha.
		glm::vec4 Color = glm::vec4(1.0f);
		bool DepthTest = true;
	};

	namespace ViewportOverlays
	{
		// Kinds of shapes AppendEntityShapes draws (bit flags).
		namespace Shapes
		{
			inline constexpr uint32_t Cameras = 1u << 0;
			inline constexpr uint32_t Lights = 1u << 1;
			inline constexpr uint32_t Colliders = 1u << 2;
			inline constexpr uint32_t All = Cameras | Lights | Colliders;
		}

		// Distance up to which camera frustums are drawn (their far planes are usually far away).
		inline constexpr float CameraFrustumLength = 3.0f;
		inline constexpr float DirectionalLightArrowLength = 1.5f;
		inline constexpr uint32_t CircleSegments = 32;

		inline constexpr glm::vec4 CameraColor = glm::vec4(0.9f, 0.9f, 0.9f, 1.0f);
		inline constexpr glm::vec4 LightColor = glm::vec4(1.0f, 0.85f, 0.3f, 1.0f);
		inline constexpr glm::vec4 ColliderColor = glm::vec4(0.35f, 1.0f, 0.45f, 1.0f);
		inline constexpr glm::vec4 TriggerColor = glm::vec4(0.35f, 0.8f, 1.0f, 1.0f);

		// Shapes of an entity's camera, lights and colliders (the kinds in shapes): camera frustums up to
		// CameraFrustumLength, light directions, ranges and cones, and collider outlines. aspectRatio is used by cameras
		// without a fixed aspect ratio.
		void AppendEntityShapes(Scene& scene, Entity entity, float aspectRatio, std::vector<OverlayLine>& lines,
		                        uint32_t shapes = Shapes::All);
	}
}
