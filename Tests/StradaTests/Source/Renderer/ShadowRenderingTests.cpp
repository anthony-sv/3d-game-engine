#include "Renderer/RenderTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Math/Math.h"
#include "Strada/RHI/TextureReadback.h"
#include "Strada/Renderer/SceneRenderer.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneRendering.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <utility>

using namespace Strada;

namespace
{
	constexpr uint32_t ImageWidth = 320;
	constexpr uint32_t ImageHeight = 180;

	SceneRendererCamera MakeCamera(glm::vec3 const& position, glm::vec3 const& target)
	{
		return Testing::MakeTestCamera(position, target, ImageWidth, ImageHeight);
	}

	AssetHandle AddMaterial(glm::vec4 const& baseColor, MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque)
	{
		MaterialData data;
		data.BaseColor = baseColor;
		data.Metallic = 0.0f;
		data.Roughness = 0.9f;
		data.AlphaMode = alphaMode;
		return AssetManager::AddMemoryAsset(CreateRef<MaterialAsset>(data), "Shadow test material");
	}

	enum class ShadowLight
	{
		Directional,
		Spot,
		Point
	};

	// A floor with a cube floating above the origin, lit from straight above by one light; no ambient light.
	struct ShadowScene
	{
		Scope<Scene> SceneData = CreateScope<Scene>("Shadows");
		Entity Caster;
		Entity Light;
		SceneRendererCamera Camera = MakeCamera({0.0f, 3.0f, 5.0f}, {0.0f, 0.0f, 0.5f});

		explicit ShadowScene(ShadowLight light, MaterialAlphaMode casterMode = MaterialAlphaMode::Opaque, float casterAlpha = 1.0f)
		{
			Testing::AddTestMesh(*SceneData, "Floor", BuiltInAsset::PlaneMesh, AddMaterial({0.6f, 0.6f, 0.6f, 1.0f}), {0.0f, 0.0f, 0.0f},
			                     glm::vec3(10.0f));
			Caster = Testing::AddTestMesh(*SceneData, "Caster", BuiltInAsset::CubeMesh,
			                              AddMaterial({0.8f, 0.8f, 0.8f, casterAlpha}, casterMode), {0.0f, 1.5f, 0.0f}, glm::vec3(0.8f));
			Light = SceneData->CreateEntity("Light");
			TransformComponent& transform = Light.GetComponent<TransformComponent>();
			switch (light)
			{
				case ShadowLight::Directional:
					transform.SetRotationEuler({-90.0f, 0.0f, 0.0f});
					Light.AddComponent<DirectionalLightComponent>().CastShadows = true;
					break;
				case ShadowLight::Spot:
				{
					transform.Translation = {0.0f, 4.0f, 0.0f};
					transform.SetRotationEuler({-90.0f, 0.0f, 0.0f});
					SpotLightComponent& spot = Light.AddComponent<SpotLightComponent>();
					spot.Intensity = 60.0f;
					spot.Range = 10.0f;
					spot.OuterConeAngle = 45.0f;
					spot.CastShadows = true;
					break;
				}
				case ShadowLight::Point:
				{
					transform.Translation = {0.0f, 4.0f, 0.0f};
					PointLightComponent& point = Light.AddComponent<PointLightComponent>();
					point.Intensity = 60.0f;
					point.Range = 10.0f;
					point.CastShadows = true;
					break;
				}
			}
		}

		void SetCastShadows(bool castShadows)
		{
			if (Light.HasComponent<DirectionalLightComponent>())
			{
				Light.GetComponent<DirectionalLightComponent>().CastShadows = castShadows;
			}
			else if (Light.HasComponent<SpotLightComponent>())
			{
				Light.GetComponent<SpotLightComponent>().CastShadows = castShadows;
			}
			else
			{
				Light.GetComponent<PointLightComponent>().CastShadows = castShadows;
			}
		}

		// Average 8-bit brightness of the floor right under the caster.
		float SampleUnderCaster(SceneRenderer& renderer)
		{
			Image const image = Testing::RenderToImage(*SceneData, Camera, renderer);
			glm::vec4 const clip = Camera.Projection * Camera.View * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
			glm::vec2 const ndc = glm::vec2(clip) / clip.w;
			uint32_t const x = static_cast<uint32_t>((ndc.x * 0.5f + 0.5f) * ImageWidth);
			uint32_t const y = static_cast<uint32_t>((0.5f - ndc.y * 0.5f) * ImageHeight);
			uint8_t const* pixel = image.GetPixel(x, y);
			return (static_cast<float>(pixel[0]) + static_cast<float>(pixel[1]) + static_cast<float>(pixel[2])) / 3.0f;
		}
	};
}

TEST_CASE("SceneRenderer: every light type casts shadows")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageWidth, ImageHeight);

	for (auto const& [light, views] :
	     {std::pair{ShadowLight::Directional, 4u}, std::pair{ShadowLight::Spot, 1u}, std::pair{ShadowLight::Point, 6u}})
	{
		CAPTURE(static_cast<int>(light));
		ShadowScene scene(light);
		float const shadowed = scene.SampleUnderCaster(renderer);
		CHECK(renderer.GetStatistics().ShadowMapViews == views);
		CHECK(renderer.GetStatistics().ShadowDrawCalls > 0);
		scene.SetCastShadows(false);
		float const lit = scene.SampleUnderCaster(renderer);
		CHECK(renderer.GetStatistics().ShadowMapViews == 0);
		CHECK(lit > 60.0f);
		CHECK(shadowed < lit * 0.25f);
	}
}

TEST_CASE("SceneRenderer: shadows follow mesh, material and scene settings")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageWidth, ImageHeight);

	ShadowScene reference(ShadowLight::Directional);
	float const shadowed = reference.SampleUnderCaster(renderer);
	reference.SetCastShadows(false);
	float const lit = reference.SampleUnderCaster(renderer);
	REQUIRE(shadowed < lit * 0.25f);

	SUBCASE("Meshes that do not cast shadows")
	{
		ShadowScene scene(ShadowLight::Directional);
		scene.Caster.GetComponent<MeshComponent>().CastShadows = false;
		CHECK(scene.SampleUnderCaster(renderer) == doctest::Approx(lit).epsilon(0.05));
	}
	SUBCASE("Masked materials cut out of shadow maps")
	{
		ShadowScene scene(ShadowLight::Directional, MaterialAlphaMode::Mask, 0.0f);
		CHECK(scene.SampleUnderCaster(renderer) == doctest::Approx(lit).epsilon(0.05));
	}
	SUBCASE("Blended materials do not cast shadows")
	{
		ShadowScene scene(ShadowLight::Directional, MaterialAlphaMode::Blend, 0.5f);
		CHECK(scene.SampleUnderCaster(renderer) == doctest::Approx(lit).epsilon(0.05));
	}
	SUBCASE("Shadows disabled in the scene settings")
	{
		ShadowScene scene(ShadowLight::Directional);
		scene.SceneData->GetSettings().Renderer.Shadows = false;
		CHECK(scene.SampleUnderCaster(renderer) == doctest::Approx(lit).epsilon(0.05));
		CHECK(renderer.GetStatistics().ShadowMapViews == 0);
	}
	SUBCASE("Shadows end at the shadow distance")
	{
		ShadowScene scene(ShadowLight::Directional);
		scene.SceneData->GetSettings().Renderer.ShadowDistance = 1.0f;
		CHECK(scene.SampleUnderCaster(renderer) == doctest::Approx(lit).epsilon(0.05));
	}
	SUBCASE("One cascade with a fixed filter")
	{
		ShadowScene scene(ShadowLight::Directional);
		SceneRendererSettings& settings = scene.SceneData->GetSettings().Renderer;
		settings.CascadeCount = 1;
		settings.SoftShadows = false;
		settings.ShadowMapSize = 512;
		// A single small cascade needs a short shadow distance to resolve the caster.
		settings.ShadowDistance = 20.0f;
		CHECK(scene.SampleUnderCaster(renderer) < lit * 0.25f);
		CHECK(renderer.GetStatistics().ShadowMapViews == 1);
	}
}

TEST_CASE("SceneRenderer: local shadows beyond the slice budget are dropped")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	ShadowScene scene(ShadowLight::Point);
	// The scene's point light and three more fill the 24 slices; the fifth light renders without shadows.
	for (int i = 0; i < 4; i++)
	{
		Entity light = scene.SceneData->CreateEntity("Extra");
		light.GetComponent<TransformComponent>().Translation = {static_cast<float>(i) * 2.0f - 3.0f, 3.0f, -3.0f};
		light.AddComponent<PointLightComponent>().CastShadows = true;
	}
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageWidth, ImageHeight);
	(void)scene.SampleUnderCaster(renderer);
	CHECK(renderer.GetStatistics().ShadowMapViews == 24);
	CHECK(renderer.GetStatistics().ShadowsDropped == 1);
}

TEST_CASE("SceneRenderer: soft shadows match the golden image")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	Scene scene("SoftShadows");
	Testing::AddTestMesh(scene, "Floor", BuiltInAsset::PlaneMesh, AddMaterial({0.7f, 0.7f, 0.7f, 1.0f}), {0.0f, 0.0f, 0.0f},
	                     glm::vec3(12.0f));
	AssetHandle const material = AddMaterial({0.8f, 0.3f, 0.2f, 1.0f});
	Testing::AddTestMesh(scene, "Pillar", BuiltInAsset::CubeMesh, material, {-1.2f, 1.0f, 0.0f}, {0.3f, 2.0f, 0.3f});
	Testing::AddTestMesh(scene, "Sphere", BuiltInAsset::SphereMesh, material, {0.4f, 0.5f, 0.3f});
	Testing::AddTestMesh(scene, "Floating", BuiltInAsset::CubeMesh, material, {1.6f, 1.2f, -0.4f}, glm::vec3(0.6f));
	Entity sun = scene.CreateEntity("Sun");
	sun.GetComponent<TransformComponent>().SetRotationEuler({-40.0f, -30.0f, 0.0f});
	// A broad light source (an overcast sun) so the penumbrae are clearly visible.
	sun.AddComponent<DirectionalLightComponent>().LightSize = 12.0f;
	Entity sky = scene.CreateEntity("Sky");
	sky.AddComponent<SkyLightComponent>().AmbientColor = {0.15f, 0.2f, 0.3f};

	SceneRenderer renderer;
	renderer.SetViewportSize(ImageWidth, ImageHeight);
	SceneRendererCamera const camera = MakeCamera({0.0f, 3.5f, 6.0f}, {0.0f, 0.3f, 0.0f});
	Image const soft = Testing::RenderToImage(scene, camera, renderer);
	Testing::CheckGoldenImage("SceneRenderer_SoftShadows", soft);

	// The penumbra width follows the light size.
	sun.GetComponent<DirectionalLightComponent>().LightSize = 0.5f;
	CHECK(Testing::CompareImages(soft, Testing::RenderToImage(scene, camera, renderer), 16).MeanDifference > 0.15);
}
