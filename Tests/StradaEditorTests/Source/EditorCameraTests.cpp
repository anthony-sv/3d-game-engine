#include "Editor/EditorCamera.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace Strada;

namespace
{
	bool NearlyEqual(glm::vec3 const& a, glm::vec3 const& b, float epsilon = 1e-3f)
	{
		return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(epsilon)));
	}
}

TEST_CASE("EditorCamera: looks down -Z at zero yaw and pitch, with an orthonormal basis")
{
	EditorCamera camera;
	camera.SetView(glm::vec3(0.0f), 5.0f, 0.0f, 0.0f);
	CHECK(NearlyEqual(camera.GetForward(), {0.0f, 0.0f, -1.0f}));
	CHECK(NearlyEqual(camera.GetRight(), {1.0f, 0.0f, 0.0f}));
	CHECK(NearlyEqual(camera.GetUp(), {0.0f, 1.0f, 0.0f}));
	CHECK(NearlyEqual(camera.GetPosition(), {0.0f, 0.0f, 5.0f}));

	// The view matrix maps the focal point straight ahead.
	glm::vec4 const focal = camera.GetViewMatrix() * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
	CHECK(NearlyEqual(glm::vec3(focal), {0.0f, 0.0f, -5.0f}));

	camera.SetView(glm::vec3(1.0f, 2.0f, 3.0f), 2.0f, 90.0f, -30.0f);
	CHECK(glm::length(camera.GetPosition() - camera.GetFocalPoint()) == doctest::Approx(2.0f));
	CHECK(std::abs(glm::dot(camera.GetForward(), camera.GetRight())) < 1e-5f);
	CHECK(camera.GetForward().y < 0.0f);
}

TEST_CASE("EditorCamera: orbit keeps the focal point, pan keeps the direction, scroll zooms")
{
	EditorCamera camera;
	camera.SetView(glm::vec3(0.0f), 5.0f, 0.0f, 0.0f);

	EditorCameraInput orbit;
	orbit.Orbit = true;
	orbit.MouseDelta = {100.0f, 0.0f};
	camera.Update(orbit, 0.016f);
	CHECK(NearlyEqual(camera.GetFocalPoint(), glm::vec3(0.0f)));
	CHECK(camera.GetYaw() == doctest::Approx(-20.0f));
	CHECK(glm::length(camera.GetPosition()) == doctest::Approx(5.0f));

	glm::vec3 const forward = camera.GetForward();
	EditorCameraInput pan;
	pan.Pan = true;
	pan.MouseDelta = {50.0f, 0.0f};
	camera.Update(pan, 0.016f);
	CHECK(NearlyEqual(camera.GetForward(), forward));
	CHECK(glm::length(camera.GetFocalPoint()) > 0.0f);

	EditorCameraInput scroll;
	scroll.Scroll = 2.0f;
	camera.Update(scroll, 0.016f);
	CHECK(camera.GetDistance() == doctest::Approx(5.0f * 0.85f * 0.85f));

	// Pitch is limited so the view never flips over the poles.
	EditorCameraInput tilt;
	tilt.Orbit = true;
	tilt.MouseDelta = {0.0f, -10000.0f};
	camera.Update(tilt, 0.016f);
	CHECK(camera.GetPitch() == doctest::Approx(89.0f));
}

TEST_CASE("EditorCamera: looking rotates around the eye and flying moves it")
{
	EditorCamera camera;
	camera.SetView(glm::vec3(0.0f), 5.0f, 0.0f, 0.0f);
	glm::vec3 const eye = camera.GetPosition();

	EditorCameraInput look;
	look.Look = true;
	look.MouseDelta = {0.0f, 50.0f};
	camera.Update(look, 0.016f);
	CHECK(NearlyEqual(camera.GetPosition(), eye));

	EditorCameraInput fly;
	fly.Look = true;
	fly.Move = {0.0f, 0.0f, 1.0f};
	camera.Update(fly, 1.0f);
	glm::vec3 const moved = camera.GetPosition() - eye;
	CHECK(glm::dot(moved, camera.GetForward()) > 0.0f);
}

TEST_CASE("EditorCamera: focus frames the bounds")
{
	EditorCamera camera;
	camera.SetView(glm::vec3(0.0f), 5.0f, 0.0f, 0.0f);
	camera.Focus(AABB(glm::vec3(9.0f), glm::vec3(11.0f)));
	CHECK(NearlyEqual(camera.GetFocalPoint(), glm::vec3(10.0f)));
	CHECK(camera.GetDistance() > std::sqrt(3.0f));

	// Empty bounds change nothing.
	camera.Focus(AABB());
	CHECK(NearlyEqual(camera.GetFocalPoint(), glm::vec3(10.0f)));
}

TEST_CASE("EditorCamera: pixel rays match the projection and place drops on the ground")
{
	EditorCamera camera;
	camera.SetViewportSize(800, 600);
	camera.SetView(glm::vec3(0.0f, 0.5f, 0.0f), 7.0f, 30.0f, -20.0f);
	glm::vec2 const viewport(800.0f, 600.0f);

	CHECK(NearlyEqual(camera.GetRayDirection(viewport * 0.5f, viewport), camera.GetForward()));

	// A point along any pixel's ray projects back onto that pixel.
	glm::mat4 const viewProjection = camera.GetGizmoProjection() * camera.GetViewMatrix();
	for (glm::vec2 const pixel : {glm::vec2(0.0f, 0.0f), glm::vec2(800.0f, 600.0f), glm::vec2(123.0f, 456.0f), glm::vec2(700.0f, 50.0f)})
	{
		glm::vec3 const point = camera.GetPosition() + camera.GetRayDirection(pixel, viewport) * 10.0f;
		glm::vec4 const clip = viewProjection * glm::vec4(point, 1.0f);
		glm::vec2 const ndc = glm::vec2(clip) / clip.w;
		glm::vec2 const projected((ndc.x + 1.0f) * 0.5f * viewport.x, (1.0f - ndc.y) * 0.5f * viewport.y);
		CHECK(projected.x == doctest::Approx(pixel.x).epsilon(1e-3));
		CHECK(projected.y == doctest::Approx(pixel.y).epsilon(1e-3));
	}

	// Looking down at the ground: the drop lands on y = 0 under the cursor.
	glm::vec3 const ground = camera.GetPlacementPoint(viewport * 0.5f, viewport);
	CHECK(ground.y == doctest::Approx(0.0f).epsilon(1e-4));
	CHECK(glm::length(glm::cross(glm::normalize(ground - camera.GetPosition()), camera.GetForward())) < 1e-4f);

	// Above the horizon there is no ground: the drop lands at the focal distance along the ray.
	camera.SetView(glm::vec3(0.0f, 5.0f, 0.0f), 7.0f, 30.0f, 10.0f);
	REQUIRE(camera.GetPosition().y > 0.0f);
	glm::vec3 const air = camera.GetPlacementPoint(viewport * 0.5f, viewport);
	CHECK(glm::length(air - camera.GetPosition()) == doctest::Approx(7.0f));
	CHECK(NearlyEqual(air, camera.GetFocalPoint()));

	// Ground hits farther than the limit are not useful either.
	camera.SetView(glm::vec3(0.0f, 0.5f, 0.0f), 7.0f, 0.0f, -0.01f);
	CHECK(glm::length(camera.GetPlacementPoint(viewport * 0.5f, viewport) - camera.GetPosition()) == doctest::Approx(7.0f));
}
