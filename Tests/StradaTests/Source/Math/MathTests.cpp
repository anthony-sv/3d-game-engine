#include "Strada/Math/Math.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

using namespace Strada;

namespace
{
	bool NearlyEqual(glm::vec3 const& a, glm::vec3 const& b, float epsilon = 1e-4f)
	{
		return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(epsilon)));
	}

	bool NearlyEqual(glm::mat4 const& a, glm::mat4 const& b, float epsilon = 1e-4f)
	{
		for (int column = 0; column < 4; column++)
		{
			for (int row = 0; row < 4; row++)
			{
				if (std::abs(a[column][row] - b[column][row]) > epsilon)
				{
					return false;
				}
			}
		}
		return true;
	}

	// q and -q describe the same rotation.
	bool SameRotation(glm::quat const& a, glm::quat const& b, float epsilon = 1e-4f)
	{
		return std::abs(std::abs(glm::dot(a, b)) - 1.0f) < epsilon;
	}

	glm::vec4 Project(glm::mat4 const& projection, glm::vec3 const& viewPosition)
	{
		glm::vec4 const clip = projection * glm::vec4(viewPosition, 1.0f);
		return clip / clip.w;
	}
}

TEST_CASE("Math: compose and decompose round trip")
{
	glm::vec3 const translation(1.5f, -2.0f, 3.25f);
	glm::quat const rotation = Math::EulerDegreesToQuaternion({30.0f, -45.0f, 10.0f});
	glm::vec3 const scale(2.0f, 0.5f, 3.0f);

	glm::mat4 const transform = Math::ComposeTransform(translation, rotation, scale);
	glm::mat4 const expected = glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
	CHECK(NearlyEqual(transform, expected));

	glm::vec3 outTranslation;
	glm::quat outRotation;
	glm::vec3 outScale;
	REQUIRE(Math::DecomposeTransform(transform, outTranslation, outRotation, outScale));
	CHECK(NearlyEqual(outTranslation, translation));
	CHECK(NearlyEqual(outScale, scale));
	CHECK(SameRotation(outRotation, rotation));
}

TEST_CASE("Math: mirrored transforms decompose into a negative X scale that recomposes exactly")
{
	glm::mat4 const transform =
		Math::ComposeTransform({0.0f, 1.0f, 0.0f}, Math::EulerDegreesToQuaternion({0.0f, 90.0f, 0.0f}), {1.0f, -2.0f, 1.0f});

	glm::vec3 translation;
	glm::quat rotation;
	glm::vec3 scale;
	REQUIRE(Math::DecomposeTransform(transform, translation, rotation, scale));
	CHECK(scale.x < 0.0f);
	CHECK(NearlyEqual(Math::ComposeTransform(translation, rotation, scale), transform));
}

TEST_CASE("Math: degenerate transforms are rejected")
{
	glm::vec3 translation(7.0f);
	glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
	glm::vec3 scale(7.0f);
	glm::mat4 const degenerate = Math::ComposeTransform({1.0f, 2.0f, 3.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), {0.0f, 1.0f, 1.0f});
	CHECK_FALSE(Math::DecomposeTransform(degenerate, translation, rotation, scale));
	CHECK(translation == glm::vec3(7.0f));
}

TEST_CASE("Math: Euler degree conversion round trips away from gimbal lock")
{
	for (glm::vec3 const degrees :
	     {glm::vec3(0.0f), glm::vec3(10.0f, 20.0f, 30.0f), glm::vec3(-45.0f, 60.0f, -120.0f), glm::vec3(80.0f, 0.0f, 5.0f)})
	{
		CAPTURE(degrees.x);
		CAPTURE(degrees.y);
		CAPTURE(degrees.z);
		glm::quat const rotation = Math::EulerDegreesToQuaternion(degrees);
		CHECK(SameRotation(Math::EulerDegreesToQuaternion(Math::QuaternionToEulerDegrees(rotation)), rotation));
	}
}

TEST_CASE("Math: reversed-Z perspective maps the near plane to 1 and far distances towards 0")
{
	glm::mat4 const projection = Math::PerspectiveReversedZ(glm::radians(60.0f), 16.0f / 9.0f, 0.1f);

	CHECK(Project(projection, {0.0f, 0.0f, -0.1f}).z == doctest::Approx(1.0f));
	CHECK(Project(projection, {0.0f, 0.0f, -1.0f}).z == doctest::Approx(0.1f));
	CHECK(Project(projection, {0.0f, 0.0f, -100000.0f}).z == doctest::Approx(0.0f).epsilon(1e-5));

	// D3D clip conventions: a point above the view axis lands at positive NDC Y.
	CHECK(Project(projection, {0.0f, 1.0f, -5.0f}).y > 0.0f);
	// The top of the frustum at distance d is at y = d * tan(fov / 2).
	float const top = 5.0f * std::tan(glm::radians(30.0f));
	CHECK(Project(projection, {0.0f, top, -5.0f}).y == doctest::Approx(1.0f));
}

TEST_CASE("Math: reversed-Z orthographic maps near to 1 and far to 0")
{
	glm::mat4 const projection = Math::OrthographicReversedZ(10.0f, 2.0f, 0.5f, 100.0f);
	CHECK(Project(projection, {0.0f, 0.0f, -0.5f}).z == doctest::Approx(1.0f));
	CHECK(Project(projection, {0.0f, 0.0f, -100.0f}).z == doctest::Approx(0.0f));
	CHECK(Project(projection, {10.0f, 5.0f, -10.0f}).x == doctest::Approx(1.0f));
	CHECK(Project(projection, {10.0f, 5.0f, -10.0f}).y == doctest::Approx(1.0f));
}

TEST_CASE("Math: conventional projections use depth 0..1")
{
	glm::mat4 const perspective = Math::Perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
	CHECK(Project(perspective, {0.0f, 0.0f, -0.1f}).z == doctest::Approx(0.0f).epsilon(1e-5));
	CHECK(Project(perspective, {0.0f, 0.0f, -100.0f}).z == doctest::Approx(1.0f));

	glm::mat4 const orthographic = Math::Orthographic(4.0f, 1.0f, 0.0f, 10.0f);
	CHECK(Project(orthographic, {0.0f, 0.0f, 0.0f}).z == doctest::Approx(0.0f));
	CHECK(Project(orthographic, {0.0f, 0.0f, -10.0f}).z == doctest::Approx(1.0f));
}

TEST_CASE("Math: the sRGB transfer function matches reference values and round trips")
{
	CHECK(Math::SrgbToLinear(0.0f) == 0.0f);
	CHECK(Math::SrgbToLinear(1.0f) == doctest::Approx(1.0f));
	CHECK(Math::SrgbToLinear(0.5f) == doctest::Approx(0.214041f).epsilon(1e-5));
	CHECK(Math::LinearToSrgb(0.214041f) == doctest::Approx(0.5f).epsilon(1e-5));
	// The linear segment near black.
	CHECK(Math::SrgbToLinear(0.02f) == doctest::Approx(0.02f / 12.92f));
	CHECK(Math::LinearToSrgb(0.001f) == doctest::Approx(0.01292f));
	for (int i = 0; i <= 100; i++)
	{
		float const value = static_cast<float>(i) / 100.0f;
		CHECK(Math::LinearToSrgb(Math::SrgbToLinear(value)) == doctest::Approx(value).epsilon(1e-5));
	}
}
