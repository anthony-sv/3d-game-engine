#include "stpch.h"
#include "Strada/Scene/SceneCamera.h"

#include "Strada/Math/Math.h"

#include <algorithm>

namespace Strada
{
	namespace
	{
		constexpr float MinimumAspectRatio = 1e-4f;
		constexpr float MinimumNearPlane = 1e-4f;
	}

	float GetCameraAspectRatio(CameraComponent const& camera, float viewportAspectRatio)
	{
		float const aspectRatio = camera.FixedAspectRatio ? camera.AspectRatio : viewportAspectRatio;
		return std::max(aspectRatio, MinimumAspectRatio);
	}

	glm::mat4 ComputeCameraProjection(CameraComponent const& camera, float viewportAspectRatio)
	{
		float const aspectRatio = GetCameraAspectRatio(camera, viewportAspectRatio);
		if (camera.Projection == ProjectionType::Perspective)
		{
			float const fov = glm::radians(std::clamp(camera.PerspectiveFOV, 1.0f, 179.0f));
			return Math::PerspectiveReversedZ(fov, aspectRatio, std::max(camera.PerspectiveNear, MinimumNearPlane));
		}
		return Math::OrthographicReversedZ(std::max(camera.OrthographicSize, MinimumNearPlane), aspectRatio, camera.OrthographicNear,
		                                   camera.OrthographicFar);
	}

	glm::mat4 ComputeCameraGizmoProjection(CameraComponent const& camera, float viewportAspectRatio)
	{
		float const aspectRatio = GetCameraAspectRatio(camera, viewportAspectRatio);
		if (camera.Projection == ProjectionType::Perspective)
		{
			float const fov = glm::radians(std::clamp(camera.PerspectiveFOV, 1.0f, 179.0f));
			float const nearPlane = std::max(camera.PerspectiveNear, MinimumNearPlane);
			return Math::Perspective(fov, aspectRatio, nearPlane, std::max(camera.PerspectiveFar, nearPlane * 2.0f));
		}
		return Math::Orthographic(std::max(camera.OrthographicSize, MinimumNearPlane), aspectRatio, camera.OrthographicNear,
		                          camera.OrthographicFar);
	}
}
