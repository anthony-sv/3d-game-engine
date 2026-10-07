#pragma once

#include "Strada/Scene/Components.h"

#include <glm/glm.hpp>

namespace Strada
{
	// Aspect ratio a camera renders with: its own when FixedAspectRatio is set, otherwise the viewport's.
	float GetCameraAspectRatio(CameraComponent const& camera, float viewportAspectRatio);

	// Projection used for rendering (reversed Z, infinite far plane for perspective cameras).
	glm::mat4 ComputeCameraProjection(CameraComponent const& camera, float viewportAspectRatio);

	// Conventional projection (depth 0..1, finite far plane) for tools such as gizmos.
	glm::mat4 ComputeCameraGizmoProjection(CameraComponent const& camera, float viewportAspectRatio);
}
