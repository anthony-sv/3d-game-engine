#include "Strada/Math/Frustum.h"
#include "Strada/Math/Math.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

using namespace Strada;

namespace
{
	// Inside the clip volume (-w <= x, y <= w and 0 <= z <= w); margin is the smallest distance to its bounds in clip units.
	bool IsInClipVolume(glm::mat4 const& viewProjection, glm::vec3 const& point, float& margin)
	{
		glm::vec4 const clip = viewProjection * glm::vec4(point, 1.0f);
		std::array<float, 6> const bounds = {clip.w + clip.x, clip.w - clip.x, clip.w + clip.y, clip.w - clip.y, clip.z, clip.w - clip.z};
		margin = std::numeric_limits<float>::max();
		bool inside = true;
		for (float const bound : bounds)
		{
			margin = std::min(margin, std::abs(bound));
			inside = inside && bound >= 0.0f;
		}
		return inside;
	}

	AABB MakeBox(glm::vec3 const& center, float halfSize)
	{
		return AABB(center - glm::vec3(halfSize), center + glm::vec3(halfSize));
	}
}

TEST_CASE("Frustum: points are inside exactly when they project into the clip volume")
{
	glm::mat4 const view = glm::lookAt(glm::vec3(2.0f, 3.0f, 6.0f), glm::vec3(-1.0f, 0.5f, -2.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	float const fieldOfView = glm::radians(60.0f);
	std::array const projections = {
		Math::PerspectiveReversedZ(fieldOfView, 1.5f, 0.5f),
		Math::Perspective(fieldOfView, 1.5f, 0.5f, 15.0f),
		Math::OrthographicReversedZ(8.0f, 1.5f, -2.0f, 12.0f),
		Math::Orthographic(8.0f, 1.5f, 1.0f, 12.0f),
	};
	for (size_t index = 0; index < projections.size(); index++)
	{
		CAPTURE(index);
		glm::mat4 const viewProjection = projections[index] * view;
		Frustum const frustum(viewProjection);
		uint32_t inside = 0;
		uint32_t outside = 0;
		uint32_t mismatches = 0;
		for (int x = 0; x < 17; x++)
		{
			for (int y = 0; y < 17; y++)
			{
				for (int z = 0; z < 17; z++)
				{
					glm::vec3 const point = glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)) * 1.75f - 14.0f;
					float margin = 0.0f;
					bool const expected = IsInClipVolume(viewProjection, point, margin);
					// Rounding decides points on the planes either way.
					if (margin < 1e-3f)
					{
						continue;
					}
					(expected ? inside : outside)++;
					mismatches += frustum.Contains(point) == expected ? 0u : 1u;
					// A point is a box too.
					mismatches += frustum.Intersects(AABB(point, point)) == expected ? 0u : 1u;
				}
			}
		}
		CHECK(mismatches == 0);
		CHECK(inside > 20);
		CHECK(outside > 20);
	}
}

TEST_CASE("Frustum: boxes are rejected only when entirely outside one plane")
{
	// At the origin looking down -Z with a 90 degree field of view: the side planes are x = +-depth and y = +-depth.
	Frustum const frustum(Math::PerspectiveReversedZ(glm::radians(90.0f), 1.0f, 0.1f));

	CHECK(frustum.Intersects(MakeBox({0.0f, 0.0f, -5.0f}, 0.5f)));
	// Straddling each side plane.
	CHECK(frustum.Intersects(MakeBox({5.0f, 0.0f, -5.0f}, 0.5f)));
	CHECK(frustum.Intersects(MakeBox({-5.0f, 0.0f, -5.0f}, 0.5f)));
	CHECK(frustum.Intersects(MakeBox({0.0f, 5.0f, -5.0f}, 0.5f)));
	CHECK(frustum.Intersects(MakeBox({0.0f, -5.0f, -5.0f}, 0.5f)));
	// Around the camera, through the near plane.
	CHECK(frustum.Intersects(MakeBox({0.0f, 0.0f, 0.0f}, 0.5f)));
	// The far plane is at infinity.
	CHECK(frustum.Intersects(MakeBox({0.0f, 0.0f, -1.0e6f}, 1.0f)));
	CHECK(frustum.Contains({0.0f, 0.0f, -1.0e6f}));

	CHECK_FALSE(frustum.Intersects(MakeBox({7.0f, 0.0f, -5.0f}, 0.5f)));
	CHECK_FALSE(frustum.Intersects(MakeBox({-7.0f, 0.0f, -5.0f}, 0.5f)));
	CHECK_FALSE(frustum.Intersects(MakeBox({0.0f, 7.0f, -5.0f}, 0.5f)));
	CHECK_FALSE(frustum.Intersects(MakeBox({0.0f, -7.0f, -5.0f}, 0.5f)));
	// Behind the camera, and between it and the near plane.
	CHECK_FALSE(frustum.Intersects(MakeBox({0.0f, 0.0f, 5.0f}, 0.5f)));
	CHECK_FALSE(frustum.Intersects(MakeBox({0.0f, 0.0f, -0.05f}, 0.02f)));
	CHECK_FALSE(frustum.Intersects(AABB()));
}

TEST_CASE("Frustum: spheres are rejected only when entirely outside one plane")
{
	glm::mat4 const projection = Math::PerspectiveReversedZ(glm::radians(90.0f), 1.0f, 0.1f);
	Frustum const frustum(projection);
	CHECK(frustum.IntersectsSphere({0.0f, 0.0f, -5.0f}, 1.0f));
	// (7, 0, -5) lies 2 / sqrt(2) outside the right plane x = -z.
	CHECK(frustum.IntersectsSphere({7.0f, 0.0f, -5.0f}, 1.5f));
	CHECK_FALSE(frustum.IntersectsSphere({7.0f, 0.0f, -5.0f}, 1.3f));
	// Behind the camera, reaching past the near plane or not.
	CHECK(frustum.IntersectsSphere({0.0f, 0.0f, 3.0f}, 5.0f));
	CHECK_FALSE(frustum.IntersectsSphere({0.0f, 0.0f, 3.0f}, 2.0f));
	// The far plane is at infinity.
	CHECK(frustum.IntersectsSphere({0.0f, 0.0f, -1.0e6f}, 0.0f));
	CHECK_FALSE(frustum.IntersectsSphere({0.0f, 0.0f, -5.0f}, -1.0f));
	CHECK_FALSE(frustum.IntersectsSphere({0.0f, 0.0f, -5.0f}, std::numeric_limits<float>::quiet_NaN()));

	Frustum const limited(glm::mat4(1.0f), projection, 10.0f);
	CHECK(limited.IntersectsSphere({0.0f, 0.0f, -11.0f}, 1.5f));
	CHECK_FALSE(limited.IntersectsSphere({0.0f, 0.0f, -12.0f}, 1.5f));
}

TEST_CASE("Frustum: a distance limit closes infinite projections")
{
	glm::mat4 const projection = Math::PerspectiveReversedZ(glm::radians(60.0f), 1.0f, 0.1f);
	// At (1, 2, 3) looking along +X.
	glm::mat4 const view = glm::lookAt(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(5.0f, 2.0f, 3.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	Frustum const limited(view, projection, 20.0f);
	CHECK(limited.Contains({20.5f, 2.0f, 3.0f}));
	CHECK_FALSE(limited.Contains({21.5f, 2.0f, 3.0f}));
	CHECK(limited.Intersects(AABB({20.0f, 1.0f, 2.0f}, {30.0f, 3.0f, 4.0f})));
	CHECK_FALSE(limited.Intersects(AABB({21.5f, 1.0f, 2.0f}, {30.0f, 3.0f, 4.0f})));

	// Non-finite distances leave the projection's frustum as it is.
	for (float const maxDistance : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
	{
		Frustum const open(view, projection, maxDistance);
		CHECK(open.Contains({1.0e5f, 2.0f, 3.0f}));
		CHECK_FALSE(open.Contains({0.0f, 2.0f, 3.0f}));
	}

	// An orthographic projection already ends at its far plane (50 here); the limit can only bring it closer. Its near
	// plane is 5 behind the camera.
	glm::mat4 const orthographic = Math::OrthographicReversedZ(10.0f, 1.0f, -5.0f, 50.0f);
	CHECK(Frustum(view, orthographic, 100.0f).Contains({45.0f, 2.0f, 3.0f}));
	CHECK_FALSE(Frustum(view, orthographic, 100.0f).Contains({55.0f, 2.0f, 3.0f}));
	CHECK_FALSE(Frustum(view, orthographic, 30.0f).Contains({45.0f, 2.0f, 3.0f}));
	CHECK(Frustum(view, orthographic, 30.0f).Contains({-2.0f, 2.0f, 3.0f}));
}
