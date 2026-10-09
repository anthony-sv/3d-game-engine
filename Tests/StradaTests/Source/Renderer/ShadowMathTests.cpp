#include "Strada/Math/Math.h"
#include "Strada/Renderer/ShadowMath.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

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
