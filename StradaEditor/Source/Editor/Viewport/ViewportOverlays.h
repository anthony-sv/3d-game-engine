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
			inline constexpr uint32_t Joints = 1u << 3;
			// Boxes of local post-process volumes.
			inline constexpr uint32_t Volumes = 1u << 4;
			inline constexpr uint32_t All = Cameras | Lights | Colliders | Joints | Volumes;
		}

		// Distance up to which camera frustums are drawn (their far planes are usually far away).
		inline constexpr float CameraFrustumLength = 3.0f;
		inline constexpr float DirectionalLightArrowLength = 1.5f;
		inline constexpr uint32_t CircleSegments = 32;

		inline constexpr glm::vec4 CameraColor = glm::vec4(0.9f, 0.9f, 0.9f, 1.0f);
		inline constexpr glm::vec4 LightColor = glm::vec4(1.0f, 0.85f, 0.3f, 1.0f);
		inline constexpr glm::vec4 ColliderColor = glm::vec4(0.35f, 1.0f, 0.45f, 1.0f);
		inline constexpr glm::vec4 TriggerColor = glm::vec4(0.35f, 0.8f, 1.0f, 1.0f);
		inline constexpr glm::vec4 JointColor = glm::vec4(1.0f, 0.4f, 0.9f, 1.0f);
		// A saturated violet, darker than light floors and brighter than dark ones.
		inline constexpr glm::vec4 VolumeColor = glm::vec4(0.58f, 0.32f, 1.0f, 1.0f);
		// The outline where a local volume's blend distance ends.
		inline constexpr glm::vec4 VolumeBlendColor = glm::vec4(0.58f, 0.32f, 1.0f, 0.55f);
		// Length of the drawn hinge and slider axes, and size of the anchor's cross.
		inline constexpr float JointAxisLength = 0.6f;
		inline constexpr float JointAnchorSize = 0.1f;

		// Shapes of an entity's camera, lights, colliders, joint and post-process volume (the kinds in shapes): camera
		// frustums up to CameraFrustumLength, light directions, ranges and cones, collider outlines, the joint's anchor (a
		// cross), its hinge or slider axis and a line to where it joins the connected body (or the world), and a local
		// volume's box with the rounded outline where its blend distance ends. aspectRatio is used by cameras without a fixed
		// aspect ratio.
		void AppendEntityShapes(Scene& scene, Entity entity, float aspectRatio, std::vector<OverlayLine>& lines,
		                        uint32_t shapes = Shapes::All);
	}
}
