#pragma once

#include "Strada/Math/AABB.h"

#include <glm/glm.hpp>

#include <array>

namespace Strada
{
	// A convex volume for visibility tests: the part of space a projection maps into the clip volume (-w <= x, y <= w and
	// 0 <= z <= w, the engine's depth range), for perspective and orthographic projections with regular or reversed depth.
	// The far plane of an infinite projection keeps every point; the camera constructor can add a distance limit.
	class Frustum
	{
	public:
		explicit Frustum(glm::mat4 const& viewProjection);
		// Also limited to the points at most maxDistance in front of the view (along -Z in view space). A non-finite
		// maxDistance adds no limit.
		Frustum(glm::mat4 const& view, glm::mat4 const& projection, float maxDistance);

		// Conservative: false only when the box lies entirely outside one of the planes, so a few boxes near the frustum's
		// edges pass without touching it. Empty boxes never intersect.
		bool Intersects(AABB const& box) const;
		// Conservative in the same way. A negative radius never intersects.
		bool IntersectsSphere(glm::vec3 const& center, float radius) const;
		bool Contains(glm::vec3 const& point) const;

	private:
		// Each plane keeps the points p with dot(plane.xyz, p) + plane.w >= 0 (unnormalized: only signs count). Left,
		// right, bottom, top, z = 0 and z = w (near and far, in either order), then the distance limit.
		std::array<glm::vec4, 7> m_Planes;
	};
}
