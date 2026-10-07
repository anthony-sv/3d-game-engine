#include "Strada/Scene/SceneCamera.h"

#include <doctest/doctest.h>

using namespace Strada;

namespace
{
	float ProjectDepth(glm::mat4 const& projection, float viewZ)
	{
		glm::vec4 const clip = projection * glm::vec4(0.0f, 0.0f, viewZ, 1.0f);
		return clip.z / clip.w;
	}
}

TEST_CASE("SceneCamera: aspect ratio follows the viewport unless fixed")
{
	CameraComponent camera;
	CHECK(GetCameraAspectRatio(camera, 2.0f) == doctest::Approx(2.0f));
	camera.FixedAspectRatio = true;
	camera.AspectRatio = 1.5f;
	CHECK(GetCameraAspectRatio(camera, 2.0f) == doctest::Approx(1.5f));

	// Degenerate viewports do not produce invalid projections.
	camera.FixedAspectRatio = false;
	glm::mat4 const projection = ComputeCameraProjection(camera, 0.0f);
	CHECK(std::isfinite(projection[0][0]));
}

TEST_CASE("SceneCamera: rendering projections are reversed-Z, gizmo projections are not")
{
	CameraComponent camera;
	camera.PerspectiveNear = 0.5f;
	CHECK(ProjectDepth(ComputeCameraProjection(camera, 1.0f), -0.5f) == doctest::Approx(1.0f));
	CHECK(ProjectDepth(ComputeCameraGizmoProjection(camera, 1.0f), -0.5f) == doctest::Approx(0.0f).epsilon(1e-5));

	camera.Projection = ProjectionType::Orthographic;
	camera.OrthographicNear = 1.0f;
	camera.OrthographicFar = 11.0f;
	CHECK(ProjectDepth(ComputeCameraProjection(camera, 1.0f), -1.0f) == doctest::Approx(1.0f));
	CHECK(ProjectDepth(ComputeCameraProjection(camera, 1.0f), -11.0f) == doctest::Approx(0.0f));
	CHECK(ProjectDepth(ComputeCameraGizmoProjection(camera, 1.0f), -11.0f) == doctest::Approx(1.0f));
}

TEST_CASE("SceneCamera: field of view and size are clamped to usable ranges")
{
	CameraComponent camera;
	camera.PerspectiveFOV = 0.0f;
	glm::mat4 const narrow = ComputeCameraProjection(camera, 1.0f);
	CHECK(std::isfinite(narrow[1][1]));

	camera.Projection = ProjectionType::Orthographic;
	camera.OrthographicSize = 0.0f;
	glm::mat4 const tiny = ComputeCameraProjection(camera, 1.0f);
	CHECK(std::isfinite(tiny[1][1]));
}
