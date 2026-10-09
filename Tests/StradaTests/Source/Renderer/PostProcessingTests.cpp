#include "Renderer/RenderTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Math/Math.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace Strada;

namespace
{
	constexpr uint32_t ImageWidth = 320;
	constexpr uint32_t ImageHeight = 180;

	AssetHandle AddMaterial(glm::vec4 const& baseColor, glm::vec3 const& emissive = glm::vec3(0.0f))
	{
		MaterialData data;
		data.BaseColor = baseColor;
		data.Metallic = 0.0f;
		data.Roughness = 0.9f;
		data.EmissiveColor = emissive;
		data.EmissiveIntensity = 1.0f;
		return AssetManager::AddMemoryAsset(CreateRef<MaterialAsset>(data), "Post-processing test material");
	}

	float Brightness(Image const& image, uint32_t x, uint32_t y)
	{
		uint8_t const* pixel = image.GetPixel(x, y);
		return (static_cast<float>(pixel[0]) + static_cast<float>(pixel[1]) + static_cast<float>(pixel[2])) / 3.0f;
	}

	glm::uvec2 ProjectToPixel(SceneRendererCamera const& camera, glm::vec3 const& point)
	{
		glm::vec4 const clip = camera.Projection * camera.View * glm::vec4(point, 1.0f);
		glm::vec2 const ndc = glm::vec2(clip) / clip.w;
		REQUIRE(std::abs(ndc.x) < 1.0f);
		REQUIRE(std::abs(ndc.y) < 1.0f);
		return {static_cast<uint32_t>((ndc.x * 0.5f + 0.5f) * ImageWidth), static_cast<uint32_t>((0.5f - ndc.y * 0.5f) * ImageHeight)};
	}

	void DisablePostProcessing(Scene& scene)
	{
		SceneRendererSettings& settings = scene.GetSettings().Renderer;
		settings.AmbientOcclusion = false;
		settings.Bloom = false;
		settings.FXAA = false;
		settings.Dithering = false;
	}
}

TEST_CASE("SceneRenderer: ambient occlusion darkens creases in indirect light only")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	// Lit by uniform ambient light only: a floor and a wall standing on it.
	Scene scene("Occlusion");
	DisablePostProcessing(scene);
	AssetHandle const white = AddMaterial({0.8f, 0.8f, 0.8f, 1.0f});
	Testing::AddTestMesh(scene, "Floor", BuiltInAsset::PlaneMesh, white, {0.0f, 0.0f, 0.0f}, glm::vec3(10.0f));
	Testing::AddTestMesh(scene, "Wall", BuiltInAsset::CubeMesh, white, {0.0f, 1.0f, -0.5f}, {4.0f, 2.0f, 1.0f});
	Entity sky = scene.CreateEntity("Sky");
	sky.AddComponent<SkyLightComponent>().AmbientColor = glm::vec3(1.0f);

	SceneRenderer renderer;
	renderer.SetViewportSize(ImageWidth, ImageHeight);
	SceneRendererCamera const camera = Testing::MakeTestCamera({0.0f, 2.0f, 4.0f}, {0.0f, 0.3f, 0.0f}, ImageWidth, ImageHeight);
	glm::uvec2 const crease = ProjectToPixel(camera, {0.0f, 0.05f, 0.08f});
	glm::uvec2 const open = ProjectToPixel(camera, {2.5f, 0.0f, 1.5f});

	Image const without = Testing::RenderToImage(scene, camera, renderer);
	scene.GetSettings().Renderer.AmbientOcclusion = true;
	Image const with = Testing::RenderToImage(scene, camera, renderer);
	CHECK(Brightness(with, crease.x, crease.y) < Brightness(without, crease.x, crease.y) * 0.85f);
	CHECK(Brightness(with, open.x, open.y) == doctest::Approx(Brightness(without, open.x, open.y)).epsilon(0.03));

	// A larger radius and a higher intensity darken more.
	scene.GetSettings().Renderer.AmbientOcclusionRadius = 2.0f;
	scene.GetSettings().Renderer.AmbientOcclusionIntensity = 4.0f;
	Image const stronger = Testing::RenderToImage(scene, camera, renderer);
	CHECK(Brightness(stronger, crease.x, crease.y) < Brightness(with, crease.x, crease.y));

	// Direct light is not occluded: with a strong directional light the crease keeps most of its brightness.
	Entity sun = scene.CreateEntity("Sun");
	sun.GetComponent<TransformComponent>().SetRotationEuler({-60.0f, 0.0f, 0.0f});
	DirectionalLightComponent& light = sun.AddComponent<DirectionalLightComponent>();
	light.CastShadows = false;
	light.Intensity = 2.0f;
	sky.GetComponent<SkyLightComponent>().AmbientColor = glm::vec3(0.0f);
	scene.GetSettings().Renderer.AmbientOcclusion = false;
	Image const directOnly = Testing::RenderToImage(scene, camera, renderer);
	scene.GetSettings().Renderer.AmbientOcclusion = true;
	Image const directWithOcclusion = Testing::RenderToImage(scene, camera, renderer);
	CHECK(Brightness(directWithOcclusion, crease.x, crease.y) == doctest::Approx(Brightness(directOnly, crease.x, crease.y)).epsilon(0.02));
}

TEST_CASE("SceneRenderer: ambient occlusion works with orthographic cameras")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	Scene scene("OrthographicOcclusion");
	DisablePostProcessing(scene);
	scene.GetSettings().Renderer.AmbientOcclusion = true;
	AssetHandle const white = AddMaterial({0.8f, 0.8f, 0.8f, 1.0f});
	Testing::AddTestMesh(scene, "Floor", BuiltInAsset::PlaneMesh, white, {0.0f, 0.0f, 0.0f}, glm::vec3(10.0f));
	Testing::AddTestMesh(scene, "Box", BuiltInAsset::CubeMesh, white, {0.0f, 0.5f, 0.0f});
	scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().AmbientColor = glm::vec3(1.0f);

	SceneRenderer renderer;
	renderer.SetViewportSize(ImageWidth, ImageHeight);
	SceneRendererCamera camera;
	camera.Position = {3.0f, 3.0f, 3.0f};
	camera.View = glm::lookAt(camera.Position, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	camera.Projection = Math::OrthographicReversedZ(4.0f, static_cast<float>(ImageWidth) / ImageHeight, 0.1f, 20.0f);
	Image const with = Testing::RenderToImage(scene, camera, renderer);
	scene.GetSettings().Renderer.AmbientOcclusion = false;
	Image const without = Testing::RenderToImage(scene, camera, renderer);
	CHECK(Testing::CompareImages(with, without, 4).MeanDifference > 0.2);
}

TEST_CASE("SceneRenderer: bloom spreads bright light")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	Scene scene("Bloom");
	DisablePostProcessing(scene);
	Testing::AddTestMesh(scene, "Glow", BuiltInAsset::SphereMesh, AddMaterial({0.0f, 0.0f, 0.0f, 1.0f}, glm::vec3(80.0f, 60.0f, 40.0f)),
	                     {0.0f, 0.0f, 0.0f}, glm::vec3(0.3f));

	SceneRenderer renderer;
	renderer.SetViewportSize(ImageWidth, ImageHeight);
	SceneRendererCamera const camera = Testing::MakeTestCamera({0.0f, 0.0f, 4.0f}, {0.0f, 0.0f, 0.0f}, ImageWidth, ImageHeight);
	uint32_t const x = ImageWidth / 2 + 30;
	uint32_t const y = ImageHeight / 2;

	Image const without = Testing::RenderToImage(scene, camera, renderer);
	CHECK(Brightness(without, x, y) < 1.0f);
	scene.GetSettings().Renderer.Bloom = true;
	Image const with = Testing::RenderToImage(scene, camera, renderer);
	CHECK(Brightness(with, x, y) >= Brightness(without, x, y) + 2.0f);
	scene.GetSettings().Renderer.BloomIntensity = 0.0f;
	CHECK(Testing::CompareImages(Testing::RenderToImage(scene, camera, renderer), without, 1).MaxDifference == 0);
}

TEST_CASE("SceneRenderer: FXAA smooths aliased edges")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	Scene scene("Fxaa");
	DisablePostProcessing(scene);
	Entity box = Testing::AddTestMesh(scene, "Box", BuiltInAsset::CubeMesh, AddMaterial({1.0f, 1.0f, 1.0f, 1.0f}, glm::vec3(20.0f)),
	                                  {0.0f, 0.0f, 0.0f});
	box.GetComponent<TransformComponent>().SetRotationEuler({0.0f, 0.0f, 17.0f});

	SceneRenderer renderer;
	renderer.SetViewportSize(ImageWidth, ImageHeight);
	SceneRendererCamera const camera = Testing::MakeTestCamera({0.0f, 0.0f, 4.0f}, {0.0f, 0.0f, 0.0f}, ImageWidth, ImageHeight);
	// The faces saturate to white on black, so intermediate values only appear along the edges.
	auto const countEdgePixels = [](Image const& image)
	{
		uint32_t count = 0;
		for (uint32_t y = 0; y < image.GetHeight(); y++)
		{
			for (uint32_t x = 0; x < image.GetWidth(); x++)
			{
				float const value = Brightness(image, x, y);
				count += value > 20.0f && value < 200.0f ? 1u : 0u;
			}
		}
		return count;
	};

	uint32_t const aliased = countEdgePixels(Testing::RenderToImage(scene, camera, renderer));
	scene.GetSettings().Renderer.FXAA = true;
	uint32_t const smoothed = countEdgePixels(Testing::RenderToImage(scene, camera, renderer));
	CHECK(smoothed > aliased * 2);
}

TEST_CASE("SceneRenderer: post-processing handles tiny and resized viewports")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	Scene scene("Tiny");
	Testing::AddTestMesh(scene, "Box", BuiltInAsset::CubeMesh, AssetHandle(), {0.0f, 0.0f, 0.0f});
	scene.CreateEntity("Sun").AddComponent<DirectionalLightComponent>();

	SceneRenderer renderer;
	for (glm::uvec2 const size : {glm::uvec2(1, 1), glm::uvec2(3, 2), glm::uvec2(17, 9), glm::uvec2(64, 64)})
	{
		CAPTURE(size.x);
		CAPTURE(size.y);
		renderer.SetViewportSize(size.x, size.y);
		SceneRendererCamera const camera = Testing::MakeTestCamera({0.0f, 1.0f, 4.0f}, {0.0f, 0.0f, 0.0f}, size.x, size.y);
		Image const image = Testing::RenderToImage(scene, camera, renderer);
		CHECK(image.GetWidth() == size.x);
		CHECK(image.GetHeight() == size.y);
	}
}
