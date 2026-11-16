#include "Strada/Math/Math.h"
#include "Strada/Renderer/ShadowMath.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

using namespace Strada;

namespace
{
	glm::vec3 Project(glm::mat4 const& viewProjection, glm::vec3 const& point)
	{
		glm::vec4 const clip = viewProjection * glm::vec4(point, 1.0f);
		return glm::vec3(clip) / clip.w;
	}

	bool InsideClip(glm::vec3 const& ndc, float epsilon = 1e-4f)
	{
		return std::abs(ndc.x) <= 1.0f + epsilon && std::abs(ndc.y) <= 1.0f + epsilon && ndc.z >= -epsilon && ndc.z <= 1.0f + epsilon;
	}
}

TEST_CASE("Shadows: cascade splits are increasing and end at the far plane")
{
	for (float const lambda : {0.0f, 0.5f, 1.0f})
	{
		std::array<float, Shadows::MaxCascades> const splits = Shadows::ComputeCascadeSplits(0.1f, 100.0f, 4, lambda);
		CHECK(splits[0] > 0.1f);
		for (uint32_t i = 1; i < Shadows::MaxCascades; i++)
		{
			CHECK(splits[i] > splits[i - 1]);
		}
		CHECK(splits[3] == doctest::Approx(100.0f));
	}

	// Uniform and logarithmic extremes.
	CHECK(Shadows::ComputeCascadeSplits(0.0f, 100.0f, 4, 0.0f)[0] == doctest::Approx(25.0f).epsilon(0.01));
	CHECK(Shadows::ComputeCascadeSplits(1.0f, 10000.0f, 4, 1.0f)[1] == doctest::Approx(100.0f).epsilon(0.001));

	// Fewer cascades: the remaining entries repeat the far plane; out-of-range counts are clamped.
	std::array<float, Shadows::MaxCascades> const two = Shadows::ComputeCascadeSplits(0.1f, 50.0f, 2, 0.5f);
	CHECK(two[0] < 50.0f);
	CHECK(two[1] == doctest::Approx(50.0f));
	CHECK(two[3] == doctest::Approx(50.0f));
	CHECK(Shadows::ComputeCascadeSplits(0.1f, 50.0f, 0, 0.5f)[0] == doctest::Approx(50.0f));
	CHECK(Shadows::ComputeCascadeSplits(0.1f, 50.0f, 9, 0.5f)[3] == doctest::Approx(50.0f));
}

TEST_CASE("Shadows: frustum slice corners lie at the requested view distances")
{
	glm::mat4 const view = glm::lookAtRH(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(1.0f, 2.0f, -10.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	glm::vec3 const forward(0.0f, 0.0f, -1.0f);
	glm::vec3 const position(1.0f, 2.0f, 3.0f);

	SUBCASE("Perspective with an infinite far plane")
	{
		glm::mat4 const projection = Math::PerspectiveReversedZ(glm::radians(60.0f), 2.0f, 0.1f);
		std::array<glm::vec3, 8> const corners = Shadows::ComputeFrustumSliceCorners(view, projection, 5.0f, 20.0f);
		for (uint32_t i = 0; i < 4; i++)
		{
			CHECK(glm::dot(corners[i] - position, forward) == doctest::Approx(5.0f));
			CHECK(glm::dot(corners[i + 4] - position, forward) == doctest::Approx(20.0f));
		}
		// Half-height at distance d is d * tan(30 degrees); width is twice that.
		CHECK(std::abs(corners[2].y - position.y) == doctest::Approx(5.0f * std::tan(glm::radians(30.0f))));
		CHECK(std::abs(corners[6].x - position.x) == doctest::Approx(40.0f * std::tan(glm::radians(30.0f))));
	}

	SUBCASE("Orthographic")
	{
		glm::mat4 const projection = Math::OrthographicReversedZ(10.0f, 1.0f, 0.1f, 100.0f);
		std::array<glm::vec3, 8> const corners = Shadows::ComputeFrustumSliceCorners(view, projection, 2.0f, 30.0f);
		for (uint32_t i = 0; i < 4; i++)
		{
			CHECK(glm::dot(corners[i] - position, forward) == doctest::Approx(2.0f));
			CHECK(glm::dot(corners[i + 4] - position, forward) == doctest::Approx(30.0f));
			CHECK(std::abs(corners[i].y - position.y) == doctest::Approx(5.0f));
		}
	}
}

TEST_CASE("Shadows: cascades contain their slice and every caster")
{
	glm::mat4 const view = glm::lookAtRH(glm::vec3(0.0f, 2.0f, 5.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	glm::mat4 const projection = Math::PerspectiveReversedZ(glm::radians(60.0f), 16.0f / 9.0f, 0.1f);
	std::array<glm::vec3, 8> const corners = Shadows::ComputeFrustumSliceCorners(view, projection, 0.1f, 15.0f);
	glm::vec3 const lightDirection = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f));
	// A caster high above the slice, towards the light.
	AABB const casters(glm::vec3(-1.0f, 40.0f, -1.0f), glm::vec3(1.0f, 42.0f, 1.0f));

	Shadows::CascadeProjection const cascade = Shadows::FitCascade(corners, lightDirection, casters, 2048);
	for (glm::vec3 const& corner : corners)
	{
		CHECK(InsideClip(Project(cascade.ViewProjection, corner)));
	}
	for (glm::vec3 const& corner : {casters.Min, casters.Max})
	{
		glm::vec3 const ndc = Project(cascade.ViewProjection, corner);
		CHECK(ndc.z <= 1.0f + 1e-4f);
		CHECK(ndc.z >= 0.0f);
	}
	CHECK(cascade.TexelSize == doctest::Approx(cascade.Width / 2048.0f));
	CHECK(cascade.DepthRange > cascade.Width);

	// Reversed Z: points closer to the light have larger depth.
	glm::vec3 const point(0.0f, 0.0f, 0.0f);
	CHECK(Project(cascade.ViewProjection, point - lightDirection).z > Project(cascade.ViewProjection, point).z);
}

TEST_CASE("Shadows: cascades are stable under small camera movements")
{
	glm::mat4 const projection = Math::PerspectiveReversedZ(glm::radians(60.0f), 1.5f, 0.1f);
	glm::vec3 const lightDirection = glm::normalize(glm::vec3(0.3f, -1.0f, 0.2f));
	uint32_t const mapSize = 1024;

	auto const fit = [&](glm::vec3 const& position)
	{
		glm::mat4 const view = glm::lookAtRH(position, position + glm::vec3(0.0f, -0.3f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		return Shadows::FitCascade(Shadows::ComputeFrustumSliceCorners(view, projection, 0.1f, 10.0f), lightDirection, AABB(), mapSize);
	};

	Shadows::CascadeProjection const a = fit(glm::vec3(0.0f, 2.0f, 0.0f));
	Shadows::CascadeProjection const b = fit(glm::vec3(0.0123f, 2.0f, 0.0371f));
	// Same scale, and the world origin moves by whole texels between the two projections.
	CHECK(a.Width == b.Width);
	glm::vec3 const offset =
		(Project(a.ViewProjection, glm::vec3(0.0f)) - Project(b.ViewProjection, glm::vec3(0.0f))) * 0.5f * static_cast<float>(mapSize);
	CHECK(std::abs(offset.x - std::round(offset.x)) < 1e-2f);
	CHECK(std::abs(offset.y - std::round(offset.y)) < 1e-2f);
}

TEST_CASE("Shadows: spot light projection covers the cone")
{
	glm::vec3 const position(2.0f, 5.0f, 1.0f);
	glm::vec3 const direction = glm::normalize(glm::vec3(0.2f, -1.0f, 0.1f));
	float const outer = glm::radians(30.0f);
	glm::mat4 const viewProjection = Shadows::ComputeSpotLightViewProjection(position, direction, outer, 12.0f);

	glm::vec3 const onAxis = Project(viewProjection, position + direction * 6.0f);
	CHECK(onAxis.x == doctest::Approx(0.0f).epsilon(1e-4));
	CHECK(onAxis.y == doctest::Approx(0.0f).epsilon(1e-4));
	// Reversed Z: near is 1, the range is 0.
	CHECK(Project(viewProjection, position + direction * Shadows::GetLocalShadowNearPlane(12.0f)).z == doctest::Approx(1.0f));
	CHECK(Project(viewProjection, position + direction * 12.0f).z == doctest::Approx(0.0f).epsilon(1e-4));

	// A point on the cone's edge is inside the map.
	glm::vec3 const side = glm::normalize(glm::cross(direction, glm::vec3(1.0f, 0.0f, 0.0f)));
	glm::vec3 const edge = position + (direction * std::cos(outer) + side * std::sin(outer)) * 8.0f;
	CHECK(InsideClip(Project(viewProjection, edge)));
}

TEST_CASE("Shadows: point light faces cover their axis with guard bands")
{
	glm::vec3 const position(-1.0f, 3.0f, 2.0f);
	uint32_t const mapSize = 512;
	std::array<glm::mat4, 6> const faces = Shadows::ComputePointLightViewProjections(position, 10.0f, mapSize, 4);
	glm::vec3 const axes[6] = {{1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
	                           {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f}};
	for (uint32_t face = 0; face < 6; face++)
	{
		CAPTURE(face);
		CHECK(Shadows::GetCubeFace(axes[face]) == face);
		glm::vec3 const center = Project(faces[face], position + axes[face] * 5.0f);
		CHECK(std::abs(center.x) < 1e-4f);
		CHECK(std::abs(center.y) < 1e-4f);

		// The 45-degree edge of the face lands exactly guardTexels inside the map.
		glm::vec3 const tangent = axes[(face + 2) % 6];
		glm::vec3 const edge = Project(faces[face], position + (axes[face] + tangent) * 3.0f);
		float const edgeTexels = (1.0f - std::max(std::abs(edge.x), std::abs(edge.y))) * 0.5f * static_cast<float>(mapSize);
		CHECK(edgeTexels == doctest::Approx(4.0f).epsilon(0.01));
	}
	// Ties resolve deterministically; the dominant axis wins otherwise.
	CHECK(Shadows::GetCubeFace(glm::vec3(0.2f, -0.9f, 0.5f)) == 3u);
	CHECK(Shadows::GetCubeFace(glm::vec3(0.1f, 0.2f, -0.3f)) == 5u);
}

TEST_CASE("Shadows: a box reaches a cascade by its center and extents exactly when its eight corners say so")
{
	// The test the eight corners gave: the projected footprint overlaps the map and the box is not beyond its far side.
	auto const byCorners = [](AABB const& bounds, glm::mat4 const& viewProjection)
	{
		glm::vec3 minimum(std::numeric_limits<float>::max());
		glm::vec3 maximum(std::numeric_limits<float>::lowest());
		for (uint32_t corner = 0; corner < 8; corner++)
		{
			glm::vec3 const point((corner & 1) ? bounds.Max.x : bounds.Min.x, (corner & 2) ? bounds.Max.y : bounds.Min.y,
			                      (corner & 4) ? bounds.Max.z : bounds.Min.z);
			glm::vec3 const clip = glm::vec3(viewProjection * glm::vec4(point, 1.0f));
			minimum = glm::min(minimum, clip);
			maximum = glm::max(maximum, clip);
		}
		return maximum.x >= -1.0f && minimum.x <= 1.0f && maximum.y >= -1.0f && minimum.y <= 1.0f && maximum.z >= 0.0f;
	};

	std::mt19937 random(20261116u);
	std::uniform_real_distribution<float> coordinate(-40.0f, 40.0f);
	std::uniform_real_distribution<float> size(0.01f, 12.0f);
	std::uniform_real_distribution<float> angle(-3.14f, 3.14f);
	size_t agreed = 0;
	size_t inside = 0;
	for (int round = 0; round < 50; round++)
	{
		// A cascade as FitCascade makes them: an orthographic projection of a light turned some way.
		glm::vec3 const direction =
			glm::normalize(glm::vec3(std::sin(angle(random)), -1.0f - std::abs(std::cos(angle(random))), std::cos(angle(random))));
		std::array<glm::vec3, 8> const corners = Shadows::ComputeFrustumSliceCorners(
			glm::lookAt(glm::vec3(0.0f, 2.0f, 8.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
			Math::PerspectiveReversedZ(glm::radians(50.0f), 1.6f, 0.1f), 0.1f, 15.0f + static_cast<float>(round));
		AABB casters({-30.0f, -5.0f, -30.0f}, {30.0f, 10.0f, 30.0f});
		glm::mat4 const viewProjection = Shadows::FitCascade(corners, direction, casters, 2048).ViewProjection;
		for (int box = 0; box < 200; box++)
		{
			glm::vec3 const center(coordinate(random), coordinate(random) * 0.25f, coordinate(random));
			glm::vec3 const extents(size(random), size(random), size(random));
			AABB const bounds(center - extents, center + extents);
			bool const expected = byCorners(bounds, viewProjection);
			bool const actual = Shadows::IntersectsCascade(bounds, viewProjection);
			// Both forms round differently: a box whose footprint touches an edge of the map within a rounding error
			// may fall either way.
			if (expected != actual)
			{
				// The footprint's edges in double precision: one of the tested ones lies on its bound.
				glm::dvec3 const clipCenter = glm::dmat4(viewProjection) * glm::dvec4(glm::dvec3(center), 1.0);
				glm::dvec3 const clipExtents =
					glm::dmat3(glm::abs(glm::dvec3(glm::dmat4(viewProjection)[0])), glm::abs(glm::dvec3(glm::dmat4(viewProjection)[1])),
				               glm::abs(glm::dvec3(glm::dmat4(viewProjection)[2]))) *
					glm::dvec3(extents);
				glm::dvec3 const low = clipCenter - clipExtents;
				glm::dvec3 const high = clipCenter + clipExtents;
				double const nearest = std::min(
					{std::abs(high.x + 1.0), std::abs(low.x - 1.0), std::abs(high.y + 1.0), std::abs(low.y - 1.0), std::abs(high.z)});
				CAPTURE(round);
				CAPTURE(box);
				CHECK(nearest < 1e-4);
				continue;
			}
			agreed++;
			inside += actual ? 1 : 0;
		}
	}
	// The boxes fall both ways, and the two forms agree on all but the few that touch an edge.
	CHECK(agreed >= 9990);
	CHECK(inside > 500);
	CHECK(inside < agreed - 500);

	// An empty box reaches no cascade.
	CHECK_FALSE(Shadows::IntersectsCascade(AABB(), glm::mat4(1.0f)));
	// A box between the light and the map (clip depth above 1) still casts; one wholly beyond the far side does not.
	CHECK(Shadows::IntersectsCascade(AABB({-0.1f, -0.1f, 1.5f}, {0.1f, 0.1f, 1.7f}), glm::mat4(1.0f)));
	CHECK_FALSE(Shadows::IntersectsCascade(AABB({-0.1f, -0.1f, -1.7f}, {0.1f, 0.1f, -1.5f}), glm::mat4(1.0f)));
	CHECK_FALSE(Shadows::IntersectsCascade(AABB({1.5f, -0.1f, 0.2f}, {1.7f, 0.1f, 0.4f}), glm::mat4(1.0f)));
}

TEST_CASE("Shadows: boxes moved into a light's space once reach each of its cascades as the cascade's own test says")
{
	std::mt19937 random(20261115u);
	std::uniform_real_distribution<float> coordinate(-40.0f, 40.0f);
	std::uniform_real_distribution<float> size(0.01f, 12.0f);
	std::uniform_real_distribution<float> angle(-3.14f, 3.14f);
	size_t agreed = 0;
	size_t inside = 0;
	for (int round = 0; round < 20; round++)
	{
		glm::vec3 const direction =
			glm::normalize(glm::vec3(std::sin(angle(random)), -1.0f - std::abs(std::cos(angle(random))), std::cos(angle(random))));
		glm::mat4 const view = glm::lookAt(glm::vec3(0.0f, 2.0f, 8.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		glm::mat4 const projection = Math::PerspectiveReversedZ(glm::radians(50.0f), 1.6f, 0.1f);
		AABB const casters({-30.0f, -5.0f, -30.0f}, {30.0f, 10.0f, 30.0f});
		// Four cascades of one light, as the renderer fits them: they share the light's view.
		std::array<float, Shadows::MaxCascades> const splits = Shadows::ComputeCascadeSplits(0.1f, 60.0f, Shadows::MaxCascades, 0.7f);
		std::vector<Shadows::CascadeProjection> cascades;
		float start = 0.1f;
		for (float const split : splits)
		{
			cascades.push_back(
				Shadows::FitCascade(Shadows::ComputeFrustumSliceCorners(view, projection, start, split), direction, casters, 2048));
			start = split;
		}
		for (Shadows::CascadeProjection const& cascade : cascades)
		{
			CHECK(cascade.LightView == cascades.front().LightView);
			glm::mat4 const product = cascade.Projection * cascade.LightView;
			CHECK(std::memcmp(&product, &cascade.ViewProjection, sizeof(glm::mat4)) == 0);
		}
		Shadows::LightSpaceTransform const lightSpace(cascades.front().LightView);
		for (int box = 0; box < 200; box++)
		{
			glm::vec3 const center(coordinate(random), coordinate(random) * 0.25f, coordinate(random));
			glm::vec3 const extents(size(random), size(random), size(random));
			AABB const bounds(center - extents, center + extents);
			Shadows::LightSpaceBox const moved = lightSpace.Apply(bounds);
			for (Shadows::CascadeProjection const& cascade : cascades)
			{
				bool const expected = Shadows::IntersectsCascade(bounds, cascade.ViewProjection);
				bool const actual = Shadows::LightSpaceCascadeTest(cascade.Projection).Intersects(moved);
				// The two forms round differently: a box whose footprint touches an edge of the map within a rounding
				// error may fall either way.
				if (expected != actual)
				{
					glm::dvec3 const clipCenter = glm::dmat4(cascade.ViewProjection) * glm::dvec4(glm::dvec3(center), 1.0);
					glm::dmat4 const exact(cascade.ViewProjection);
					glm::dvec3 const clipExtents =
						glm::dmat3(glm::abs(glm::dvec3(exact[0])), glm::abs(glm::dvec3(exact[1])), glm::abs(glm::dvec3(exact[2]))) *
						glm::dvec3(extents);
					glm::dvec3 const low = clipCenter - clipExtents;
					glm::dvec3 const high = clipCenter + clipExtents;
					double const nearest = std::min(
						{std::abs(high.x + 1.0), std::abs(low.x - 1.0), std::abs(high.y + 1.0), std::abs(low.y - 1.0), std::abs(high.z)});
					CAPTURE(round);
					CAPTURE(box);
					CHECK(nearest < 1e-4);
					continue;
				}
				agreed++;
				inside += actual ? 1 : 0;
			}
		}
	}
	// The boxes fall both ways, and the two forms agree on all but the few that touch an edge.
	CHECK(agreed >= 15990);
	CHECK(inside > 1000);
	CHECK(inside < agreed - 1000);
}
