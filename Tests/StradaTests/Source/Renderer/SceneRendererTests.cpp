#include "Renderer/RenderTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Math/Math.h"
#include "Strada/RHI/TextureReadback.h"
#include "Strada/Renderer/SceneRenderer.h"
#include "Strada/Renderer/TextureMips.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneRendering.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

using namespace Strada;

namespace
{
	constexpr uint32_t ImageWidth = 320;
	constexpr uint32_t ImageHeight = 180;

	SceneRendererCamera MakeCamera(glm::vec3 const& position, glm::vec3 const& target)
	{
		return Testing::MakeTestCamera(position, target, ImageWidth, ImageHeight);
	}

	AssetHandle AddMaterial(glm::vec4 const& baseColor, float metallic, float roughness,
	                        MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque)
	{
		MaterialData data;
		data.BaseColor = baseColor;
		data.Metallic = metallic;
		data.Roughness = roughness;
		data.AlphaMode = alphaMode;
		return AssetManager::AddMemoryAsset(CreateRef<MaterialAsset>(data), "Test material");
	}

	// A small lit scene: floor, metal and plastic spheres, a cube, a blended quad, three light types and ambient light.
	Scope<Scene> MakeShowcaseScene()
	{
		auto scene = CreateScope<Scene>("Showcase");
		Testing::AddTestMesh(*scene, "Floor", BuiltInAsset::PlaneMesh, AddMaterial({0.5f, 0.5f, 0.5f, 1.0f}, 0.0f, 0.8f),
		                     {0.0f, 0.0f, 0.0f}, glm::vec3(8.0f));
		Testing::AddTestMesh(*scene, "Plastic", BuiltInAsset::SphereMesh, AddMaterial({0.8f, 0.1f, 0.1f, 1.0f}, 0.0f, 0.35f),
		                     {-1.2f, 0.5f, 0.0f});
		Testing::AddTestMesh(*scene, "Gold", BuiltInAsset::SphereMesh, AddMaterial({1.0f, 0.77f, 0.34f, 1.0f}, 1.0f, 0.3f),
		                     {0.0f, 0.5f, -0.6f});
		Entity cube = Testing::AddTestMesh(*scene, "Cube", BuiltInAsset::CubeMesh, AddMaterial({0.2f, 0.4f, 0.9f, 1.0f}, 0.0f, 0.6f),
		                                   {1.3f, 0.5f, 0.2f});
		cube.GetComponent<TransformComponent>().SetRotationEuler({0.0f, 30.0f, 0.0f});
		Testing::AddTestMesh(*scene, "Glass", BuiltInAsset::QuadMesh,
		                     AddMaterial({0.2f, 0.9f, 0.3f, 0.4f}, 0.0f, 0.2f, MaterialAlphaMode::Blend), {0.0f, 0.6f, 1.0f},
		                     glm::vec3(0.8f));

		Entity sun = scene->CreateEntity("Sun");
		sun.GetComponent<TransformComponent>().SetRotationEuler({-50.0f, 35.0f, 0.0f});
		sun.AddComponent<DirectionalLightComponent>().Intensity = 3.0f;
		Entity lamp = scene->CreateEntity("Lamp");
		lamp.GetComponent<TransformComponent>().Translation = {-1.5f, 1.2f, 1.0f};
		PointLightComponent& point = lamp.AddComponent<PointLightComponent>();
		point.Color = {1.0f, 0.6f, 0.3f};
		point.Intensity = 6.0f;
		point.Range = 5.0f;
		Entity spot = scene->CreateEntity("Spot");
		spot.GetComponent<TransformComponent>().Translation = {1.5f, 3.0f, 1.0f};
		spot.GetComponent<TransformComponent>().SetRotationEuler({-80.0f, 0.0f, 0.0f});
		SpotLightComponent& spotLight = spot.AddComponent<SpotLightComponent>();
		spotLight.Color = {0.4f, 0.6f, 1.0f};
		spotLight.Intensity = 20.0f;
		spotLight.Range = 8.0f;
		Entity sky = scene->CreateEntity("Sky");
		SkyLightComponent& skyLight = sky.AddComponent<SkyLightComponent>();
		skyLight.AmbientColor = {0.35f, 0.45f, 0.6f};
		skyLight.Intensity = 0.6f;
		return scene;
	}

	glm::vec3 PixelColor(Image const& image, uint32_t x, uint32_t y)
	{
		uint8_t const* pixel = image.GetPixel(x, y);
		return {pixel[0], pixel[1], pixel[2]};
	}
}

TEST_CASE("TextureMips: chains go down to 1x1 and average sRGB in linear space")
{
	CHECK(GetMipLevelCount(1, 1) == 1);
	CHECK(GetMipLevelCount(256, 64) == 9);
	CHECK(GetMipLevelCount(300, 5) == 9);

	Image checker(2, 2, 4);
	for (uint32_t y = 0; y < 2; y++)
	{
		for (uint32_t x = 0; x < 2; x++)
		{
			uint8_t const value = (x + y) % 2 == 0 ? 255 : 0;
			uint8_t* pixel = checker.GetPixel(x, y);
			pixel[0] = pixel[1] = pixel[2] = value;
			pixel[3] = value;
		}
	}
	std::vector<Image> const srgb = GenerateMipChain(checker, true);
	REQUIRE(srgb.size() == 2);
	// Linear mid-gray (0.5) is 188 in sRGB; a naive average would give 128.
	CHECK(srgb[1].GetPixel(0, 0)[0] == 188);
	CHECK(srgb[1].GetPixel(0, 0)[3] == 128);
	std::vector<Image> const linear = GenerateMipChain(checker, false);
	CHECK(linear[1].GetPixel(0, 0)[0] == 128);

	std::vector<Image> const odd = GenerateMipChain(Image(5, 3, 4), true);
	REQUIRE(odd.size() == 3);
	CHECK(odd[1].GetWidth() == 2);
	CHECK(odd[1].GetHeight() == 1);
	CHECK(odd[2].GetWidth() == 1);
}

TEST_CASE("SceneRenderer: renderer settings are serialized with the scene")
{
	Scene scene("Settings");
	scene.GetSettings().Renderer.EV100 = 2.5f;
	scene.GetSettings().Renderer.Tonemapper = TonemapOperator::AgX;
	scene.GetSettings().Renderer.Dithering = false;
	Json const document = SceneSerializer::Serialize(scene);
	CHECK(document["Scene"]["Settings"]["Renderer"]["Tonemapper"] == "AgX");

	Result<Ref<Scene>> loaded = SceneSerializer::Deserialize(document, DeserializationContext{});
	REQUIRE(loaded.IsOk());
	CHECK(loaded.GetValue()->GetSettings().Renderer == scene.GetSettings().Renderer);

	Json invalid = document;
	invalid["Scene"]["Settings"]["Renderer"]["Tonemapper"] = "Filmic";
	CHECK(SceneSerializer::Deserialize(invalid, DeserializationContext{}).IsError());
	CHECK(SceneRendererSettings().GetExposure() == doctest::Approx(1.0f / 1.2f));
}

TEST_CASE("SceneRenderer: lit scene matches the golden image")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	Scope<Scene> scene = MakeShowcaseScene();
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageWidth, ImageHeight);
	Image const image = Testing::RenderToImage(*scene, MakeCamera({0.0f, 2.2f, 4.5f}, {0.0f, 0.4f, 0.0f}), renderer);
	CHECK(renderer.GetStatistics().Lights == 3);
	CHECK(renderer.GetStatistics().DrawCalls - renderer.GetStatistics().ShadowDrawCalls == 5);
	CHECK(renderer.GetStatistics().ShadowMapViews == 4);
	CHECK(renderer.GetStatistics().ShadowDrawCalls > 0);
	Testing::CheckGoldenImage("SceneRenderer_Showcase", image);
}

TEST_CASE("SceneRenderer: tonemappers and exposure change the image")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	Scope<Scene> scene = MakeShowcaseScene();
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageWidth, ImageHeight);
	SceneRendererCamera const camera = MakeCamera({0.0f, 2.2f, 4.5f}, {0.0f, 0.4f, 0.0f});

	Image const aces = Testing::RenderToImage(*scene, camera, renderer);
	for (TonemapOperator const tonemapper :
	     {TonemapOperator::AgX, TonemapOperator::PBRNeutral, TonemapOperator::Reinhard, TonemapOperator::None})
	{
		CAPTURE(EnumToString(tonemapper));
		scene->GetSettings().Renderer.Tonemapper = tonemapper;
		CHECK(Testing::CompareImages(aces, Testing::RenderToImage(*scene, camera, renderer), 16).MeanDifference > 0.5);
	}

	// Two stops more EV darkens the red sphere.
	scene->GetSettings().Renderer.Tonemapper = TonemapOperator::ACES;
	scene->GetSettings().Renderer.EV100 = 2.0f;
	Image const darker = Testing::RenderToImage(*scene, camera, renderer);
	glm::vec3 const before = PixelColor(aces, ImageWidth / 2 - 50, ImageHeight / 2);
	glm::vec3 const after = PixelColor(darker, ImageWidth / 2 - 50, ImageHeight / 2);
	CHECK(after.r + after.g + after.b < before.r + before.g + before.b);
}

TEST_CASE("SceneRenderer: material edits and resizes take effect")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	Scene scene("Material");
	AssetHandle const material = AddMaterial({1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 1.0f);
	Testing::AddTestMesh(scene, "Wall", BuiltInAsset::QuadMesh, material, {0.0f, 0.0f, 0.0f}, glm::vec3(4.0f));
	Entity light = scene.CreateEntity("Light");
	light.AddComponent<DirectionalLightComponent>().Intensity = 2.0f;

	SceneRenderer renderer;
	renderer.SetViewportSize(64, 64);
	SceneRendererCamera camera = MakeCamera({0.0f, 0.0f, 2.0f}, {0.0f, 0.0f, 0.0f});
	camera.Projection = Math::PerspectiveReversedZ(glm::radians(50.0f), 1.0f, 0.1f);

	Image const white = Testing::RenderToImage(scene, camera, renderer);
	glm::vec3 const center = PixelColor(white, 32, 32);
	CHECK(center.r > 100.0f);
	CHECK(std::abs(center.r - center.b) < 2.0f);

	Ref<MaterialAsset> const asset = AssetManager::GetAsset<MaterialAsset>(material);
	MaterialData data = asset->GetData();
	data.BaseColor = {1.0f, 0.0f, 0.0f, 1.0f};
	asset->SetData(data);
	Image const red = Testing::RenderToImage(scene, camera, renderer);
	glm::vec3 const redCenter = PixelColor(red, 32, 32);
	CHECK(redCenter.r > 100.0f);
	CHECK(redCenter.b < 10.0f);

	// The quad faces +Z: seen from behind, back-face culling removes it and the background shows.
	SceneRendererCamera behind = camera;
	behind.View = glm::lookAt(glm::vec3(0.0f, 0.0f, -2.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	behind.Position = {0.0f, 0.0f, -2.0f};
	glm::vec3 const back = PixelColor(Testing::RenderToImage(scene, behind, renderer), 32, 32);
	CHECK(back.r + back.g + back.b < 3.0f);

	renderer.SetViewportSize(100, 40);
	camera.Projection = Math::PerspectiveReversedZ(glm::radians(50.0f), 2.5f, 0.1f);
	Image const resized = Testing::RenderToImage(scene, camera, renderer);
	CHECK(resized.GetWidth() == 100);
	CHECK(resized.GetHeight() == 40);
}

TEST_CASE("SceneRenderer: image-based lighting matches the golden image")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	Scene scene("IBL");
	float const roughness[] = {0.05f, 0.35f, 0.75f};
	for (int row = 0; row < 2; row++)
	{
		for (int column = 0; column < 3; column++)
		{
			AssetHandle const material = AddMaterial(row == 0 ? glm::vec4(0.95f, 0.64f, 0.54f, 1.0f) : glm::vec4(0.1f, 0.3f, 0.8f, 1.0f),
			                                         row == 0 ? 1.0f : 0.0f, roughness[column]);
			Testing::AddTestMesh(scene, "Sphere", BuiltInAsset::SphereMesh, material,
			                     {static_cast<float>(column - 1) * 1.2f, row == 0 ? 1.3f : 0.0f, 0.0f});
		}
	}
	Entity sky = scene.CreateEntity("Sky");
	SkyLightComponent& skyLight = sky.AddComponent<SkyLightComponent>();
	skyLight.Environment = GetBuiltInHandle(BuiltInAsset::DefaultSky);

	SceneRenderer renderer;
	renderer.SetViewportSize(ImageWidth, ImageHeight);
	SceneRendererCamera const camera = MakeCamera({0.0f, 0.65f, 4.2f}, {0.0f, 0.65f, 0.0f});
	Image const image = Testing::RenderToImage(scene, camera, renderer);
	Testing::CheckGoldenImage("SceneRenderer_IBL", image);

	// The environment intensity scales both the lighting and the sky; hiding the sky shows the ambient color.
	skyLight.Intensity = 2.0f;
	CHECK(Testing::CompareImages(image, Testing::RenderToImage(scene, camera, renderer), 16).MeanDifference > 5.0);
	skyLight.DrawSkybox = false;
	skyLight.AmbientColor = glm::vec3(0.0f);
	glm::vec3 const corner = PixelColor(Testing::RenderToImage(scene, camera, renderer), 2, 2);
	CHECK(corner.r + corner.g + corner.b < 3.0f);
}
