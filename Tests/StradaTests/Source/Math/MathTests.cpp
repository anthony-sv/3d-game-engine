#include "Strada/Math/Math.h"

#include <doctest/doctest.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>

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

	// Basis vectors whose length overflows, or is not a number, have no direction that can be computed either.
	glm::mat4 overflowing(1.0f);
	overflowing[0] = glm::vec4(3.0e38f, 3.0e38f, 0.0f, 0.0f);
	CHECK_FALSE(Math::DecomposeTransform(overflowing, translation, rotation, scale));
	glm::mat4 notANumber(1.0f);
	notANumber[2] = glm::vec4(std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f, 0.0f);
	CHECK_FALSE(Math::DecomposeTransform(notANumber, translation, rotation, scale));
	CHECK(translation == glm::vec3(7.0f));
}

TEST_CASE("Math: quaternions of any finite size normalize to their direction")
{
	glm::quat const quarterTurn = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
	glm::quat const identity(1.0f, 0.0f, 0.0f, 0.0f);

	// Squaring components near the float range overflows (glm::normalize returns zero for them), and squaring tiny ones
	// underflows (glm::normalize returns the identity).
	for (float const size : {1.0f, 2.0f, 1.0e30f, 1.0e-30f, -1.0e30f})
	{
		CAPTURE(size);
		glm::quat const scaled = quarterTurn * size;
		CHECK(SameRotation(Math::NormalizeRotation(scaled), quarterTurn));
		CHECK(glm::length(Math::NormalizeRotation(scaled)) == doctest::Approx(1.0f));
		CHECK(Math::QuaternionLength(scaled) / std::abs(size) == doctest::Approx(1.0f));
	}
	float const largest = std::numeric_limits<float>::max();
	glm::quat const huge = Math::NormalizeRotation(glm::quat::wxyz(largest, largest, largest, -largest));
	CHECK(huge.w == doctest::Approx(0.5f));
	CHECK(huge.z == doctest::Approx(-0.5f));
	CHECK(Math::QuaternionLength(glm::quat::wxyz(largest, largest, 0.0f, 0.0f)) == std::numeric_limits<float>::infinity());

	// Zero has no direction: it is the identity.
	CHECK(Math::NormalizeRotation(glm::quat::wxyz(0.0f, 0.0f, 0.0f, 0.0f)) == identity);
	CHECK(Math::QuaternionLength(glm::quat::wxyz(0.0f, 0.0f, 0.0f, 0.0f)) == 0.0f);

	// Ordinary rotations normalize exactly as glm does: Euler angles near 90 degrees of yaw (computed with asin) move by
	// 0.02 degrees when a component changes by an ulp.
	glm::vec3 const yawed = Math::QuaternionToEulerDegrees(Math::EulerDegreesToQuaternion({0.0f, 90.0f, 0.0f}));
	CHECK(yawed.y == doctest::Approx(90.0f).epsilon(1e-5));

	// Transforms and Euler angles of such rotations are the ones of their direction.
	glm::quat const rotation = Math::EulerDegreesToQuaternion({30.0f, -45.0f, 10.0f});
	CHECK(NearlyEqual(Math::ComposeTransform(glm::vec3(0.0f), rotation * 1.0e30f, glm::vec3(1.0f)), glm::mat4_cast(rotation)));
	CHECK(NearlyEqual(Math::QuaternionToEulerDegrees(rotation * 1.0e30f), {30.0f, -45.0f, 10.0f}));
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
