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

TEST_CASE("SceneRenderer: color grading, vignette, chromatic aberration and film grain change the image as set")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	// A wall filling the view: gray-green over most of it, red at the right (from x = 2, about 90% across), lit evenly by
	// emission.
	Scene scene("Grading");
	DisablePostProcessing(scene);
	AssetHandle const left = AddMaterial({0.0f, 0.0f, 0.0f, 1.0f}, {0.25f, 0.35f, 0.25f});
	AssetHandle const right = AddMaterial({0.0f, 0.0f, 0.0f, 1.0f}, {0.6f, 0.05f, 0.05f});
	Testing::AddTestMesh(scene, "Left", BuiltInAsset::CubeMesh, left, {-3.0f, 0.0f, 0.0f}, {10.0f, 10.0f, 0.1f});
	Testing::AddTestMesh(scene, "Right", BuiltInAsset::CubeMesh, right, {7.0f, 0.0f, 0.0f}, {10.0f, 10.0f, 0.1f});
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageWidth, ImageHeight);
	SceneRendererCamera const camera = Testing::MakeTestCamera({0.0f, 0.0f, 3.0f}, {0.0f, 0.0f, 0.0f}, ImageWidth, ImageHeight);
	SceneRendererSettings& settings = scene.GetSettings().Renderer;
	auto const pixel = [](Image const& image, uint32_t x, uint32_t y)
	{
		uint8_t const* data = image.GetPixel(x, y);
		return glm::ivec3(data[0], data[1], data[2]);
	};
	glm::uvec2 const leftPixel(ImageWidth / 4, ImageHeight / 2);
	glm::uvec2 const rightPixel(ImageWidth * 19 / 20, ImageHeight / 2);
	uint32_t const boundary = ProjectToPixel(camera, {2.0f, 0.0f, 0.0f}).x;

	Image const plain = Testing::RenderToImage(scene, camera, renderer);
	glm::ivec3 const red = pixel(plain, rightPixel.x, rightPixel.y);
	REQUIRE(red.r > red.g + 50);

	// Saturation 0 is grayscale.
	settings.Saturation = 0.0f;
	glm::ivec3 const gray = pixel(Testing::RenderToImage(scene, camera, renderer), rightPixel.x, rightPixel.y);
	CHECK(std::abs(gray.r - gray.g) <= 2);
	CHECK(std::abs(gray.g - gray.b) <= 2);
	settings.Saturation = 1.0f;

	// Warm temperature: more red than blue in the gray-green half.
	settings.Temperature = 1.0f;
	glm::ivec3 const warm = pixel(Testing::RenderToImage(scene, camera, renderer), leftPixel.x, leftPixel.y);
	glm::ivec3 const neutral = pixel(plain, leftPixel.x, leftPixel.y);
	CHECK(warm.r > neutral.r);
	CHECK(warm.b < neutral.b);
	settings.Temperature = 0.0f;

	// More contrast brightens what is above middle gray (the green half) and darkens what is below.
	settings.Contrast = 1.6f;
	CHECK(Brightness(Testing::RenderToImage(scene, camera, renderer), leftPixel.x, leftPixel.y) >
	      Brightness(plain, leftPixel.x, leftPixel.y) + 5.0f);
	settings.Contrast = 0.4f;
	CHECK(Brightness(Testing::RenderToImage(scene, camera, renderer), leftPixel.x, leftPixel.y) <
	      Brightness(plain, leftPixel.x, leftPixel.y) - 5.0f);
	settings.Contrast = 1.0f;

	// The color filter multiplies: no red light, no red.
	settings.ColorFilter = {0.0f, 1.0f, 1.0f};
	CHECK(pixel(Testing::RenderToImage(scene, camera, renderer), rightPixel.x, rightPixel.y).r < 5);
	settings.ColorFilter = glm::vec3(1.0f);

	// The vignette darkens the corners, not the middle.
	settings.Vignette = 1.0f;
	Image const vignetted = Testing::RenderToImage(scene, camera, renderer);
	CHECK(Brightness(vignetted, 2, 2) < Brightness(plain, 2, 2) * 0.5f);
	CHECK(Brightness(vignetted, ImageWidth / 2, ImageHeight / 2) ==
	      doctest::Approx(Brightness(plain, ImageWidth / 2, ImageHeight / 2)).epsilon(0.05));
	settings.Vignette = 0.0f;

	// Chromatic aberration separates the channels along the color edge towards the image's border, not in the middle.
	settings.ChromaticAberration = 1.0f;
	Image const fringed = Testing::RenderToImage(scene, camera, renderer);
	CHECK(pixel(fringed, ImageWidth / 2, ImageHeight / 2) == pixel(plain, ImageWidth / 2, ImageHeight / 2));
	int changed = 0;
	for (uint32_t x = boundary - 4; x < boundary + 4; x++)
	{
		changed += pixel(fringed, x, 2) != pixel(plain, x, 2) ? 1 : 0;
	}
	CHECK(changed > 0);
	settings.ChromaticAberration = 0.0f;

	// Film grain is noise that changes from frame to frame.
	settings.FilmGrain = 1.0f;
	Image const first = Testing::RenderToImage(scene, camera, renderer);
	Image const second = Testing::RenderToImage(scene, camera, renderer);
	int noisy = 0;
	int animated = 0;
	for (uint32_t x = 0; x < 40; x++)
	{
		noisy += pixel(first, x + 10, ImageHeight / 2) != pixel(plain, x + 10, ImageHeight / 2) ? 1 : 0;
		animated += pixel(first, x + 10, ImageHeight / 2) != pixel(second, x + 10, ImageHeight / 2) ? 1 : 0;
	}
	CHECK(noisy > 20);
	CHECK(animated > 20);
}

TEST_CASE("SceneRenderer: post-process volumes change the rendered image around the camera")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	// A red wall lit evenly by emission, seen from 3 m.
	Scene scene("Volumes");
	DisablePostProcessing(scene);
	Testing::AddTestMesh(scene, "Wall", BuiltInAsset::CubeMesh, AddMaterial({0.0f, 0.0f, 0.0f, 1.0f}, {0.6f, 0.05f, 0.05f}),
	                     {0.0f, 0.0f, 0.0f}, {20.0f, 20.0f, 0.1f});
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageWidth, ImageHeight);
	glm::vec3 const target(0.0f, 0.0f, 0.0f);
	auto const centerPixel = [&](glm::vec3 const& cameraPosition)
	{
		Image const image =
			Testing::RenderToImage(scene, Testing::MakeTestCamera(cameraPosition, target, ImageWidth, ImageHeight), renderer);
		uint8_t const* data = image.GetPixel(ImageWidth / 2, ImageHeight / 2);
		return glm::ivec3(data[0], data[1], data[2]);
	};
	auto const isGray = [](glm::ivec3 const& color)
	{
		return std::abs(color.r - color.g) <= 2 && std::abs(color.g - color.b) <= 2;
	};

	glm::vec3 const inside(0.0f, 0.0f, 3.0f);
	glm::ivec3 const red = centerPixel(inside);
	REQUIRE(red.r > red.g + 50);

	// A global volume takes the color out everywhere.
	Entity volumeEntity = scene.CreateEntity("Volume");
	PostProcessComponent& volume = volumeEntity.AddComponent<PostProcessComponent>();
	volume.OverrideSaturation = true;
	volume.Saturation = 0.0f;
	CHECK(isGray(centerPixel(inside)));
	CHECK(isGray(centerPixel({0.0f, 0.0f, 8.0f})));

	// Made local, a 2 m box around the first camera position with a 2 m blend: gray inside, half the color halfway through
	// the blend, all of it beyond.
	volume.Global = false;
	volume.BlendDistance = 2.0f;
	volumeEntity.GetComponent<TransformComponent>().Translation = inside;
	volumeEntity.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f);
	CHECK(isGray(centerPixel(inside)));
	glm::ivec3 const halfway = centerPixel({0.0f, 0.0f, 5.0f});
	glm::ivec3 const outside = centerPixel({0.0f, 0.0f, 7.0f});
	CHECK(outside == red);
	CHECK(halfway.r - halfway.g > 10);
	CHECK(halfway.r - halfway.g < outside.r - outside.g - 10);

	// Without weight the volume does nothing; the scene's settings never changed.
	volume.Weight = 0.0f;
	CHECK(centerPixel(inside) == red);
	CHECK(scene.GetSettings().Renderer.Saturation == 1.0f);
}
