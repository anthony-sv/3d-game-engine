#include "Strada/Math/AABB.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

using namespace Strada;

TEST_CASE("AABB: empty boxes grow to contain points and boxes")
{
	AABB box;
	CHECK_FALSE(box.IsValid());
	box.Expand(AABB());
	CHECK_FALSE(box.IsValid());

	box.Expand(glm::vec3(1.0f, 2.0f, 3.0f));
	CHECK(box.IsValid());
	CHECK(box.Min == glm::vec3(1.0f, 2.0f, 3.0f));
	CHECK(box.Max == glm::vec3(1.0f, 2.0f, 3.0f));

	box.Expand(glm::vec3(-1.0f, 4.0f, 0.0f));
	CHECK(box.Min == glm::vec3(-1.0f, 2.0f, 0.0f));
	CHECK(box.Max == glm::vec3(1.0f, 4.0f, 3.0f));
	CHECK(box.GetCenter() == glm::vec3(0.0f, 3.0f, 1.5f));
	CHECK(box.GetSize() == glm::vec3(2.0f, 2.0f, 3.0f));
	CHECK(box.GetExtents() == glm::vec3(1.0f, 1.0f, 1.5f));
	CHECK(box.Contains(glm::vec3(0.0f, 3.0f, 1.0f)));
	CHECK_FALSE(box.Contains(glm::vec3(0.0f, 5.0f, 1.0f)));

	box.Expand(AABB(glm::vec3(-5.0f), glm::vec3(-4.0f)));
	CHECK(box.Min == glm::vec3(-5.0f, -5.0f, -5.0f));
}

TEST_CASE("AABB: transforms produce the bounds of the transformed corners")
{
	AABB const box(glm::vec3(-1.0f, -1.0f, -1.0f), glm::vec3(1.0f, 1.0f, 1.0f));

	AABB const moved = box.Transform(glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)));
	CHECK(moved.Min == glm::vec3(9.0f, -1.0f, -1.0f));
	CHECK(moved.Max == glm::vec3(11.0f, 1.0f, 1.0f));

	// A 45 degree rotation around Y widens the X/Z extents to sqrt(2).
	AABB const rotated = box.Transform(glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
	CHECK(rotated.Max.x == doctest::Approx(std::sqrt(2.0f)));
	CHECK(rotated.Max.y == doctest::Approx(1.0f));
	CHECK(rotated.Min.z == doctest::Approx(-std::sqrt(2.0f)));

	AABB const mirrored = box.Transform(glm::scale(glm::mat4(1.0f), glm::vec3(-2.0f, 1.0f, 1.0f)));
	CHECK(mirrored.Min.x == doctest::Approx(-2.0f));
	CHECK(mirrored.Max.x == doctest::Approx(2.0f));

	CHECK_FALSE(AABB().Transform(glm::mat4(1.0f)).IsValid());
}
