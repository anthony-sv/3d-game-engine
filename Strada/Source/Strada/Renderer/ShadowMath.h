#pragma once

#include "Strada/Math/AABB.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>

// Shadow projection math shared by the scene renderer and its tests. All projections use reversed Z (closest to the
// light maps to 1) and D3D clip conventions, like the camera projections.
namespace Strada::Shadows
{
	inline constexpr uint32_t MaxCascades = 4;

	// View distances at which each cascade ends (entries past cascadeCount repeat farPlane). lambda blends the uniform
	// (0) and logarithmic (1) split schemes.
	std::array<float, MaxCascades> ComputeCascadeSplits(float nearPlane, float farPlane, uint32_t cascadeCount, float lambda);

	// World-space corners of the camera frustum between two view distances: four at nearDistance, then four at
	// farDistance. Works for perspective projections with an infinite far plane and for orthographic projections.
	std::array<glm::vec3, 8> ComputeFrustumSliceCorners(glm::mat4 const& view, glm::mat4 const& projection, float nearDistance,
	                                                    float farDistance);

	struct CascadeProjection
	{
		glm::mat4 ViewProjection = glm::mat4(1.0f);
		// World-space size of one shadow-map texel.
		float TexelSize = 0.0f;
		// World-space width of the cascade and length of its depth range (light to the farthest receiver).
		float Width = 0.0f;
		float DepthRange = 0.0f;
	};

	// Orthographic projection that covers the bounding sphere of a view slice. The sphere's radius depends only on the
	// slice shape and its center is snapped to whole texels, so shadows do not shimmer as the camera moves or turns.
	// The depth range extends towards the light to include every caster in casterBounds.
	CascadeProjection FitCascade(std::span<glm::vec3 const, 8> sliceCorners, glm::vec3 const& lightDirection, AABB const& casterBounds,
	                             uint32_t mapSize);

	// lightDirection: direction the light travels. outerConeAngle: half-angle in radians.
	glm::mat4 ComputeSpotLightViewProjection(glm::vec3 const& position, glm::vec3 const& lightDirection, float outerConeAngle, float range);

	// One projection per cube face (+X, -X, +Y, -Y, +Z, -Z). Each face's field of view is widened so that guardTexels of
	// filtering around its edges still sample the same face.
	std::array<glm::mat4, 6> ComputePointLightViewProjections(glm::vec3 const& position, float range, uint32_t mapSize,
	                                                          uint32_t guardTexels);

	// Whether world-space boxes can cast into a cascade's orthographic view: a box's footprint in the view's clip space
	// overlaps the map in x and y, and it does not lie entirely beyond the farthest receiver (clip depth below 0). Boxes
	// between the light and the map are always inside, since FitCascade extends the depth range to every caster. A box's
	// center and extents are taken into clip space once, as AABB::Transform does, which for the affine viewProjection of
	// an orthographic view bounds the same footprint as its eight corners. An invalid (empty) box reaches no view. Made
	// once per view, then asked for every caster.
	class CascadeTest
	{
	public:
		explicit CascadeTest(glm::mat4 const& viewProjection)
			: m_ViewProjection(viewProjection)
		{
			glm::mat3 const linear(viewProjection);
			for (int column = 0; column < 3; column++)
			{
				m_Absolute[column] = glm::abs(linear[column]);
			}
		}

		bool Intersects(AABB const& bounds) const
		{
			if (!bounds.IsValid())
			{
				return false;
			}
			glm::vec3 const center = glm::vec3(m_ViewProjection * glm::vec4(bounds.GetCenter(), 1.0f));
			glm::vec3 const extents = m_Absolute * bounds.GetExtents();
			glm::vec3 const minimum = center - extents;
			glm::vec3 const maximum = center + extents;
			return maximum.x >= -1.0f && minimum.x <= 1.0f && maximum.y >= -1.0f && minimum.y <= 1.0f && maximum.z >= 0.0f;
		}

	private:
		glm::mat4 m_ViewProjection;
		glm::mat3 m_Absolute;
	};

	// CascadeTest(viewProjection).Intersects(bounds), for a single box.
	bool IntersectsCascade(AABB const& bounds, glm::mat4 const& viewProjection);

	// Index of the cube face (in the order above) whose axis dominates direction.
	uint32_t GetCubeFace(glm::vec3 const& direction);

	// Near plane distance of local light shadow projections.
	float GetLocalShadowNearPlane(float range);
}
