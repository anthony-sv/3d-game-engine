#include "stpch.h"
#include "Strada/Renderer/ShadowMath.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace Strada::Shadows
{
	namespace
	{
		glm::mat4 LightView(glm::vec3 const& eye, glm::vec3 const& direction)
		{
			// Any up vector that is not parallel to the direction works; the choice only rotates the map.
			glm::vec3 const up = std::abs(direction.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
			return glm::lookAtRH(eye, eye + direction, up);
		}

		// Perspective with reversed Z between nearPlane and farPlane: swapping the planes of a 0..1 projection maps the
		// near plane to 1 and the far plane to 0.
		glm::mat4 PerspectiveReversedZ(float fieldOfView, float nearPlane, float farPlane)
		{
			return glm::perspectiveRH_ZO(fieldOfView, 1.0f, farPlane, nearPlane);
		}
	}

	std::array<float, MaxCascades> ComputeCascadeSplits(float nearPlane, float farPlane, uint32_t cascadeCount, float lambda)
	{
		cascadeCount = std::clamp(cascadeCount, 1u, MaxCascades);
		lambda = std::clamp(lambda, 0.0f, 1.0f);
		// The logarithmic scheme needs a positive near plane (orthographic cameras may have a negative one).
		float const nearDistance = std::max(nearPlane, 0.01f);
		float const farDistance = std::max(farPlane, nearDistance * 1.001f);

		std::array<float, MaxCascades> splits{};
		for (uint32_t i = 0; i < MaxCascades; i++)
		{
			if (i + 1 >= cascadeCount)
			{
				splits[i] = farDistance;
				continue;
			}
			float const fraction = static_cast<float>(i + 1) / static_cast<float>(cascadeCount);
			float const logarithmic = nearDistance * std::pow(farDistance / nearDistance, fraction);
			float const uniform = nearDistance + (farDistance - nearDistance) * fraction;
			splits[i] = uniform + (logarithmic - uniform) * lambda;
		}
		return splits;
	}

	std::array<glm::vec3, 8> ComputeFrustumSliceCorners(glm::mat4 const& view, glm::mat4 const& projection, float nearDistance,
	                                                    float farDistance)
	{
		glm::mat4 const inverseProjection = glm::inverse(projection);
		glm::mat4 const inverseView = glm::inverse(view);
		glm::vec2 const ndcCorners[4] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};

		std::array<glm::vec3, 8> corners{};
		for (uint32_t i = 0; i < 4; i++)
		{
			// Two points on the corner's ray in view space: the near plane (depth 1) and a point behind it (depth 0.5;
			// depth 0 is at infinity for an infinite perspective projection).
			glm::vec4 const nearPoint = inverseProjection * glm::vec4(ndcCorners[i], 1.0f, 1.0f);
			glm::vec4 const innerPoint = inverseProjection * glm::vec4(ndcCorners[i], 0.5f, 1.0f);
			glm::vec3 const a = glm::vec3(nearPoint) / nearPoint.w;
			glm::vec3 const b = glm::vec3(innerPoint) / innerPoint.w;
			float const deltaZ = b.z - a.z;
			auto const pointAt = [&](float distance)
			{
				// The camera looks down -Z.
				float const t = std::abs(deltaZ) > 1e-12f ? (-distance - a.z) / deltaZ : 0.0f;
				return glm::vec3(inverseView * glm::vec4(a + (b - a) * t, 1.0f));
			};
			corners[i] = pointAt(nearDistance);
			corners[i + 4] = pointAt(farDistance);
		}
		return corners;
	}

	CascadeProjection FitCascade(std::span<glm::vec3 const, 8> sliceCorners, glm::vec3 const& lightDirection, AABB const& casterBounds,
	                             uint32_t mapSize)
	{
		glm::vec3 center(0.0f);
		for (glm::vec3 const& corner : sliceCorners)
		{
			center += corner;
		}
		center /= 8.0f;
		float radius = 0.0f;
		for (glm::vec3 const& corner : sliceCorners)
		{
			radius = std::max(radius, glm::length(corner - center));
		}
		// Quantize so floating-point noise in the corners cannot change the projection scale between frames.
		radius = std::max(std::ceil(radius * 16.0f) / 16.0f, 0.0625f);

		glm::vec3 const direction = glm::normalize(lightDirection);
		glm::mat4 const lightView = LightView(glm::vec3(0.0f), direction);
		float const texelSize = 2.0f * radius / static_cast<float>(mapSize);

		glm::vec3 lightCenter = glm::vec3(lightView * glm::vec4(center, 1.0f));
		lightCenter.x = std::floor(lightCenter.x / texelSize) * texelSize;
		lightCenter.y = std::floor(lightCenter.y / texelSize) * texelSize;

		// Light space looks down -Z: larger z is closer to the light.
		float closest = lightCenter.z + radius;
		float const farthest = lightCenter.z - radius;
		if (casterBounds.IsValid())
		{
			for (uint32_t i = 0; i < 8; i++)
			{
				glm::vec3 const corner((i & 1) ? casterBounds.Max.x : casterBounds.Min.x, (i & 2) ? casterBounds.Max.y : casterBounds.Min.y,
				                       (i & 4) ? casterBounds.Max.z : casterBounds.Min.z);
				closest = std::max(closest, (lightView * glm::vec4(corner, 1.0f)).z);
			}
		}

		// Distances along -Z; swapping them in a 0..1 projection gives reversed Z.
		float const nearPlane = -closest;
		float const farPlane = -farthest;
		glm::mat4 const projection = glm::orthoRH_ZO(lightCenter.x - radius, lightCenter.x + radius, lightCenter.y - radius,
		                                             lightCenter.y + radius, farPlane, nearPlane);

		CascadeProjection cascade;
		cascade.ViewProjection = projection * lightView;
		cascade.TexelSize = texelSize;
		cascade.Width = 2.0f * radius;
		cascade.DepthRange = farPlane - nearPlane;
		return cascade;
	}

	float GetLocalShadowNearPlane(float range)
	{
		return std::clamp(range * 0.005f, 0.02f, 0.5f);
	}

	glm::mat4 ComputeSpotLightViewProjection(glm::vec3 const& position, glm::vec3 const& lightDirection, float outerConeAngle, float range)
	{
		// A small margin keeps the cone's edge (and its filter kernel) inside the map.
		float const fieldOfView = std::min(2.0f * outerConeAngle + glm::radians(4.0f), glm::radians(178.0f));
		float const farPlane = std::max(range, GetLocalShadowNearPlane(range) * 2.0f);
		return PerspectiveReversedZ(fieldOfView, GetLocalShadowNearPlane(range), farPlane) *
		       LightView(position, glm::normalize(lightDirection));
	}

	std::array<glm::mat4, 6> ComputePointLightViewProjections(glm::vec3 const& position, float range, uint32_t mapSize,
	                                                          uint32_t guardTexels)
	{
		// A face covering 90 degrees spans tan(45) = 1 from its center; widen it so the inner mapSize - 2 * guard texels
		// cover exactly that.
		float const usable = static_cast<float>(std::max<int64_t>(static_cast<int64_t>(mapSize) - 2 * guardTexels, 1));
		float const fieldOfView = 2.0f * std::atan(static_cast<float>(mapSize) / usable);
		float const nearPlane = GetLocalShadowNearPlane(range);
		glm::mat4 const projection = PerspectiveReversedZ(fieldOfView, nearPlane, std::max(range, nearPlane * 2.0f));

		glm::vec3 const directions[6] = {{1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
		                                 {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f}};
		std::array<glm::mat4, 6> faces{};
		for (uint32_t face = 0; face < 6; face++)
		{
			faces[face] = projection * LightView(position, directions[face]);
		}
		return faces;
	}

	uint32_t GetCubeFace(glm::vec3 const& direction)
	{
		glm::vec3 const magnitude = glm::abs(direction);
		if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z)
		{
			return direction.x >= 0.0f ? 0u : 1u;
		}
		if (magnitude.y >= magnitude.z)
		{
			return direction.y >= 0.0f ? 2u : 3u;
		}
		return direction.z >= 0.0f ? 4u : 5u;
	}
}
