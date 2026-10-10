#include "stpch.h"
#include "Strada/Math/Math.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Strada::Math
{
	namespace
	{
		// Whether glm computes the quaternion's length accurately: its squared length neither overflows (components near
		// the float range) nor underflows into subnormal numbers or zero (tiny components).
		bool HasRepresentableSquaredLength(glm::quat const& quaternion)
		{
			float const lengthSquared = glm::dot(quaternion, quaternion);
			return lengthSquared >= std::numeric_limits<float>::min() && lengthSquared <= std::numeric_limits<float>::max();
		}

		float GetLargestComponent(glm::quat const& quaternion)
		{
			return std::max({std::abs(quaternion.x), std::abs(quaternion.y), std::abs(quaternion.z), std::abs(quaternion.w)});
		}
	}

	glm::mat4 ComposeTransform(glm::vec3 const& translation, glm::quat const& rotation, glm::vec3 const& scale)
	{
		glm::mat4 const rotationMatrix = glm::mat4_cast(NormalizeRotation(rotation));
		glm::mat4 result = rotationMatrix;
		result[0] *= scale.x;
		result[1] *= scale.y;
		result[2] *= scale.z;
		result[3] = glm::vec4(translation, 1.0f);
		return result;
	}

	bool DecomposeTransform(glm::mat4 const& transform, glm::vec3& translation, glm::quat& rotation, glm::vec3& scale)
	{
		glm::vec3 columns[3] = {glm::vec3(transform[0]), glm::vec3(transform[1]), glm::vec3(transform[2])};
		glm::vec3 lengths(glm::length(columns[0]), glm::length(columns[1]), glm::length(columns[2]));

		// Zero-length basis vectors have no direction, and ones whose length is not finite (huge components overflow
		// when squared) none that can be computed.
		constexpr float Epsilon = 1e-8f;
		auto const usable = [](float length)
		{
			return length >= Epsilon && std::isfinite(length);
		};
		if (!usable(lengths.x) || !usable(lengths.y) || !usable(lengths.z))
		{
			return false;
		}

		// A mirrored basis cannot be represented by a rotation; express it as a negative X scale.
		if (glm::dot(glm::cross(columns[0], columns[1]), columns[2]) < 0.0f)
		{
			lengths.x = -lengths.x;
		}

		glm::mat3 const rotationMatrix(columns[0] / lengths.x, columns[1] / lengths.y, columns[2] / lengths.z);
		translation = glm::vec3(transform[3]);
		rotation = glm::normalize(glm::quat_cast(rotationMatrix));
		scale = lengths;
		return true;
	}

	glm::vec3 QuaternionToEulerDegrees(glm::quat const& rotation)
	{
		return glm::degrees(glm::eulerAngles(NormalizeRotation(rotation)));
	}

	glm::quat EulerDegreesToQuaternion(glm::vec3 const& degrees)
	{
		return glm::normalize(glm::quat(glm::radians(degrees)));
	}

	float QuaternionLength(glm::quat const& quaternion)
	{
		if (HasRepresentableSquaredLength(quaternion))
		{
			return glm::length(quaternion);
		}
		// Divided by its largest component, the quaternion's squared length is between 1 and 4.
		float const largest = GetLargestComponent(quaternion);
		return largest > 0.0f ? largest * glm::length(quaternion / largest) : 0.0f;
	}

	glm::quat NormalizeRotation(glm::quat const& rotation)
	{
		// Rotations glm normalizes correctly keep its exact result: Euler angles near +-90 degrees of yaw (computed with
		// asin) change visibly when a component changes by an ulp.
		if (HasRepresentableSquaredLength(rotation))
		{
			return glm::normalize(rotation);
		}
		float const largest = GetLargestComponent(rotation);
		if (!(largest > 0.0f))
		{
			return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		}
		glm::quat const scaled = rotation / largest;
		return scaled / glm::length(scaled);
	}

	float SrgbToLinear(float value)
	{
		return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
	}

	float LinearToSrgb(float value)
	{
		return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
	}

	glm::mat4 PerspectiveReversedZ(float verticalFovRadians, float aspectRatio, float nearPlane)
	{
		float const focalLength = 1.0f / std::tan(verticalFovRadians * 0.5f);
		glm::mat4 result(0.0f);
		result[0][0] = focalLength / aspectRatio;
		result[1][1] = focalLength;
		// z_ndc = near / z_view_distance: 1 at the near plane, approaching 0 at infinity.
		result[2][3] = -1.0f;
		result[3][2] = nearPlane;
		return result;
	}

	glm::mat4 OrthographicReversedZ(float size, float aspectRatio, float nearPlane, float farPlane)
	{
		float const halfHeight = size * 0.5f;
		float const halfWidth = halfHeight * aspectRatio;
		// Swapping near and far in a 0..1 depth projection maps near to 1 and far to 0.
		return glm::orthoRH_ZO(-halfWidth, halfWidth, -halfHeight, halfHeight, farPlane, nearPlane);
	}

	glm::mat4 Perspective(float verticalFovRadians, float aspectRatio, float nearPlane, float farPlane)
	{
		return glm::perspectiveRH_ZO(verticalFovRadians, aspectRatio, nearPlane, farPlane);
	}

	glm::mat4 Orthographic(float size, float aspectRatio, float nearPlane, float farPlane)
	{
		float const halfHeight = size * 0.5f;
		float const halfWidth = halfHeight * aspectRatio;
		return glm::orthoRH_ZO(-halfWidth, halfWidth, -halfHeight, halfHeight, nearPlane, farPlane);
	}
}
