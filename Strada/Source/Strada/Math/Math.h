#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Strada::Math
{
	// Builds translation * rotation * scale.
	glm::mat4 ComposeTransform(glm::vec3 const& translation, glm::quat const& rotation, glm::vec3 const& scale);

	// A child's world matrix: parentWorld * local for affine matrices, each bottom row taken as (0, 0, 0, 1), which the
	// result gets exactly. The products are glm's (each column summed left to right) without the terms the bottom rows
	// make zero. Every world matrix of a scene is made by this one function, kept out of line, so that the scene's cache,
	// its reads and their tests compute the same bits on every compiler (an inlined product may be contracted into fused
	// multiply-adds differently where it is inlined).
	glm::mat4 ComposeWorldTransform(glm::mat4 const& parentWorld, glm::mat4 const& local);

	// Splits an affine transform into translation, rotation and scale. A negative determinant (mirroring) is folded into
	// a negative X scale. Returns false (outputs unchanged) for degenerate matrices: a basis vector of zero length, or one
	// whose length is not finite.
	bool DecomposeTransform(glm::mat4 const& transform, glm::vec3& translation, glm::quat& rotation, glm::vec3& scale);

	// Euler angles in degrees (pitch about X, yaw about Y, roll about Z), applied in the order used by glm::quat(vec3).
	glm::vec3 QuaternionToEulerDegrees(glm::quat const& rotation);
	glm::quat EulerDegreesToQuaternion(glm::vec3 const& degrees);

	// The length of a quaternion as a 4D vector. Components near the float range do not overflow and tiny ones do not
	// underflow (their squares would); only a length beyond the float range is infinite.
	float QuaternionLength(glm::quat const& quaternion);
	// The unit quaternion with the same direction, identity for zero: glm::normalize's result, except that components
	// near the float range (which it turns into zero) and tiny ones (which it turns into the identity) keep their
	// direction. Expects finite components.
	glm::quat NormalizeRotation(glm::quat const& rotation);

	// The sRGB transfer function (IEC 61966-2-1) for one color channel in [0, 1].
	float SrgbToLinear(float value);
	float LinearToSrgb(float value);

	// Right-handed perspective projection with reversed Z (near maps to 1, infinity to 0) and D3D clip conventions
	// (depth 0..1, +Y up). The far plane is at infinity.
	glm::mat4 PerspectiveReversedZ(float verticalFovRadians, float aspectRatio, float nearPlane);

	// Right-handed orthographic projection with reversed Z (near maps to 1, far to 0). size is the full view height.
	glm::mat4 OrthographicReversedZ(float size, float aspectRatio, float nearPlane, float farPlane);

	// Conventional (non-reversed) projections with depth 0..1, for tools such as gizmos that expect them.
	glm::mat4 Perspective(float verticalFovRadians, float aspectRatio, float nearPlane, float farPlane);
	glm::mat4 Orthographic(float size, float aspectRatio, float nearPlane, float farPlane);
}
