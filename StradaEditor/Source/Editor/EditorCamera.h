#pragma once

#include "Strada/Math/AABB.h"
#include "Strada/Renderer/SceneRenderer.h"

#include <glm/glm.hpp>

namespace Strada
{
	// One frame of viewport input (filled from ImGui by the viewport panel, or by tests).
	struct EditorCameraInput
	{
		// Mouse movement in pixels.
		glm::vec2 MouseDelta = glm::vec2(0.0f);
		float Scroll = 0.0f;
		// Right mouse: look around and fly with WASD/QE.
		bool Look = false;
		// Middle mouse: pan.
		bool Pan = false;
		// Alt + left mouse: orbit around the focal point.
		bool Orbit = false;
		// Fly directions while looking: x = right, y = up, z = forward (each -1, 0 or 1).
		glm::vec3 Move = glm::vec3(0.0f);
		bool Fast = false;
	};

	// Editor viewport camera: orbits, pans and dollies around a focal point, or flies in first person. Right-handed, Y-up,
	// looking down -Z at yaw = pitch = 0.
	class EditorCamera
	{
	public:
		// The distances from the focal point the camera keeps to, and the most it looks up or down (degrees).
		static constexpr float MinimumDistance = 0.05f;
		static constexpr float MaximumDistance = 10000.0f;
		static constexpr float PitchLimit = 89.0f;

		EditorCamera();

		void Update(EditorCameraInput const& input, float deltaTime);
		// Frames the bounds from the current viewing direction; invalid bounds leave the view as it is.
		void Focus(AABB const& bounds);

		void SetViewportSize(uint32_t width, uint32_t height);
		void SetFieldOfView(float degrees) { m_FieldOfView = degrees; }

		glm::vec3 GetPosition() const;
		glm::vec3 GetForward() const;
		glm::vec3 GetRight() const;
		glm::vec3 GetUp() const;
		glm::vec3 const& GetFocalPoint() const { return m_FocalPoint; }
		float GetDistance() const { return m_Distance; }
		float GetYaw() const { return m_Yaw; }
		float GetPitch() const { return m_Pitch; }

		glm::mat4 GetViewMatrix() const;
		// Reversed-Z, infinite far plane (rendering).
		glm::mat4 GetProjection() const;
		// Conventional projection (depth 0..1, finite far plane) for transform gizmos.
		glm::mat4 GetGizmoProjection() const;
		float GetAspectRatio() const { return m_AspectRatio; }
		SceneRendererCamera GetRendererCamera() const;

		void SetView(glm::vec3 const& focalPoint, float distance, float yawDegrees, float pitchDegrees);

		// Normalized direction of the ray from the camera position through a pixel of a viewport of the given size ((0, 0)
		// is its top-left corner; its aspect ratio is the one given to SetViewportSize).
		glm::vec3 GetRayDirection(glm::vec2 const& pixel, glm::vec2 const& viewportSize) const;
		// Where something dropped at a viewport pixel is placed: the point of the ground plane (y = 0) under the pixel when it
		// lies in front of the camera within MaxPlacementDistance, else the point at the focal distance along the ray.
		glm::vec3 GetPlacementPoint(glm::vec2 const& pixel, glm::vec2 const& viewportSize) const;

		static constexpr float MaxPlacementDistance = 500.0f;

	private:
		glm::vec3 m_FocalPoint = glm::vec3(0.0f);
		float m_Distance = 8.0f;
		// Degrees.
		float m_Yaw = 0.0f;
		float m_Pitch = -25.0f;
		float m_FieldOfView = 50.0f;
		float m_NearPlane = 0.05f;
		float m_AspectRatio = 16.0f / 9.0f;
	};
}
