#include "Editor/EditorCamera.h"

#include "Strada/Math/Math.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace Strada
{
	namespace
	{
		constexpr float LookDegreesPerPixel = 0.2f;
		constexpr float MinimumDistance = 0.05f;
		constexpr float MaximumDistance = 10000.0f;
		constexpr float PitchLimit = 89.0f;
	}

	EditorCamera::EditorCamera() = default;

	void EditorCamera::SetView(glm::vec3 const& focalPoint, float distance, float yawDegrees, float pitchDegrees)
	{
		m_FocalPoint = focalPoint;
		m_Distance = std::clamp(distance, MinimumDistance, MaximumDistance);
		m_Yaw = yawDegrees;
		m_Pitch = std::clamp(pitchDegrees, -PitchLimit, PitchLimit);
	}

	glm::vec3 EditorCamera::GetForward() const
	{
		float const yaw = glm::radians(m_Yaw);
		float const pitch = glm::radians(m_Pitch);
		return glm::normalize(glm::vec3(-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)));
	}

	glm::vec3 EditorCamera::GetRight() const
	{
		return glm::normalize(glm::cross(GetForward(), glm::vec3(0.0f, 1.0f, 0.0f)));
	}

	glm::vec3 EditorCamera::GetUp() const
	{
		return glm::cross(GetRight(), GetForward());
	}

	glm::vec3 EditorCamera::GetPosition() const
	{
		return m_FocalPoint - GetForward() * m_Distance;
	}

	void EditorCamera::Update(EditorCameraInput const& input, float deltaTime)
	{
		// Panning and flying scale with the distance so the camera feels the same at every size.
		float const scale = std::max(m_Distance, 1.0f);

		if (input.Look)
		{
			// First person: rotate around the eye, then fly.
			glm::vec3 const eye = GetPosition();
			m_Yaw -= input.MouseDelta.x * LookDegreesPerPixel;
			m_Pitch = std::clamp(m_Pitch - input.MouseDelta.y * LookDegreesPerPixel, -PitchLimit, PitchLimit);
			float const speed = scale * (input.Fast ? 3.0f : 1.0f) * deltaTime;
			glm::vec3 const offset =
				(GetRight() * input.Move.x + glm::vec3(0.0f, 1.0f, 0.0f) * input.Move.y + GetForward() * input.Move.z) * speed;
			m_FocalPoint = eye + offset + GetForward() * m_Distance;
		}
		else if (input.Orbit)
		{
			m_Yaw -= input.MouseDelta.x * LookDegreesPerPixel;
			m_Pitch = std::clamp(m_Pitch - input.MouseDelta.y * LookDegreesPerPixel, -PitchLimit, PitchLimit);
		}
		else if (input.Pan)
		{
			float const unitsPerPixel = 2.0f * m_Distance * std::tan(glm::radians(m_FieldOfView) * 0.5f) / 600.0f;
			m_FocalPoint += (-GetRight() * input.MouseDelta.x + GetUp() * input.MouseDelta.y) * unitsPerPixel;
		}

		if (input.Scroll != 0.0f)
		{
			// Each wheel step moves 15% of the way towards (or away from) the focal point.
			m_Distance = std::clamp(m_Distance * std::pow(0.85f, input.Scroll), MinimumDistance, MaximumDistance);
		}
	}

	void EditorCamera::Focus(AABB const& bounds)
	{
		if (!bounds.IsValid())
		{
			return;
		}
		m_FocalPoint = bounds.GetCenter();
		float const radius = std::max(glm::length(bounds.GetExtents()), 0.5f);
		m_Distance = std::clamp(radius / std::sin(glm::radians(m_FieldOfView) * 0.5f), MinimumDistance, MaximumDistance);
	}

	void EditorCamera::SetViewportSize(uint32_t width, uint32_t height)
	{
		m_AspectRatio = static_cast<float>(std::max(width, 1u)) / static_cast<float>(std::max(height, 1u));
	}

	glm::mat4 EditorCamera::GetViewMatrix() const
	{
		return glm::lookAt(GetPosition(), m_FocalPoint, glm::vec3(0.0f, 1.0f, 0.0f));
	}

	glm::mat4 EditorCamera::GetProjection() const
	{
		return Math::PerspectiveReversedZ(glm::radians(m_FieldOfView), m_AspectRatio, m_NearPlane);
	}

	glm::mat4 EditorCamera::GetGizmoProjection() const
	{
		constexpr float GizmoFarPlane = 10000.0f;
		return Math::Perspective(glm::radians(m_FieldOfView), m_AspectRatio, m_NearPlane, GizmoFarPlane);
	}

	SceneRendererCamera EditorCamera::GetRendererCamera() const
	{
		SceneRendererCamera camera;
		camera.View = GetViewMatrix();
		camera.Projection = GetProjection();
		camera.Position = GetPosition();
		return camera;
	}
}
