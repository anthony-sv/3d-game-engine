#include "Renderer/RenderTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/Renderer/SceneRenderer.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneRendering.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

using namespace Strada;

TEST_CASE("SceneRenderer: Moto's dressed main scene draws its 10,000 props as instances, in at most 400 draws a frame")
{
	// The motorcycle sample's circuit is lined with about 10,000 props of a few meshes and two materials
	// (Projects/Moto/Tools/Props.py): instancing draws each mesh once per pass, so the whole frame, every pass counted, stays
	// within the 400 draws the slice's exit allows, where without instancing each prop would be a draw in every pass that
	// sees it (the statistics' Instances: thousands from the home view). The count is the frame's structure, not its time,
	// and is the same on every GPU.
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	Testing::TemporaryDirectory directory;
	std::filesystem::path const assets = directory.GetPath() / "Assets";
	std::error_code error;
	// The registry's refresh may write; the test reads a copy of the sample.
	std::filesystem::copy(FileSystem::PathFromUtf8(STRADA_SAMPLE_PROJECTS_DIR) / "Moto" / "Assets", assets,
	                      std::filesystem::copy_options::recursive, error);
	REQUIRE_MESSAGE(!error, error.message());
	Result<AssetRefreshResult> opened = AssetManager::OpenAssetDirectory(assets);
	REQUIRE_MESSAGE(opened.IsOk(), (opened ? std::string() : opened.GetError()));
	Result<Ref<Scene>> loaded =
		SceneSerializer::LoadFromFile(assets / "Scenes" / "Main.sscene", AssetManager::CreateDeserializationContext());
	REQUIRE_MESSAGE(loaded.IsOk(), (loaded ? std::string() : loaded.GetError()));
	Scene& scene = *loaded.GetValue();

	uint64_t meshes = 0;
	scene.GetRegistry().view<MeshComponent>().each(
		[&meshes](MeshComponent const& mesh)
		{
			meshes += mesh.Mesh.IsValid() ? 1 : 0;
		});
	REQUIRE(meshes > 10000);

	// 1080p's shape at a quarter of its pixels: the views are 1080p's, so the same props are seen and drawn, and a device
	// without a GPU (CI's lavapipe) shades a quarter as much.
	constexpr uint32_t Width = 960;
	constexpr uint32_t Height = 540;
	SUBCASE("from the scene's camera, at the home view over the start")
	{
		for (bool const entityIds : {false, true})
		{
			CAPTURE(entityIds);
			SceneRenderer renderer("Moto");
			renderer.SetViewportSize(Width, Height);
			renderer.SetEntityIdsEnabled(entityIds);
			REQUIRE(RenderSceneFromPrimaryCamera(scene, renderer));
			SceneRendererStatistics const& statistics = renderer.GetStatistics();
			CAPTURE(statistics.DrawCalls);
			CAPTURE(statistics.Batches);
			CAPTURE(statistics.Instances);
			CHECK(statistics.DrawCalls <= 400);
			// The grandstands, the fences and the boards of the main straight and the trees beyond are in view.
			CHECK(statistics.Instances >= 1000);
			GraphicsDevice::WaitForIdle();
		}
	}
	SUBCASE("from high above, every prop in view")
	{
		// The circuit's middle (Tools/GenerateAssets.py, circuit_center), seen from 2 km up, where the land the props stand on
		// fills the view.
		glm::vec3 const center(195.0f, 0.0f, 54.8f);
		SceneRendererCamera const camera = Testing::MakeTestCamera(center + glm::vec3(0.0f, 2000.0f, 900.0f), center, Width, Height);
		for (bool const entityIds : {false, true})
		{
			CAPTURE(entityIds);
			SceneRenderer renderer("Moto");
			renderer.SetViewportSize(Width, Height);
			renderer.SetEntityIdsEnabled(entityIds);
			RenderScene(scene, renderer, camera);
			SceneRendererStatistics const& statistics = renderer.GetStatistics();
			CAPTURE(statistics.DrawCalls);
			CAPTURE(statistics.Batches);
			CAPTURE(statistics.Instances);
			CHECK(statistics.DrawCalls <= 400);
			CHECK(renderer.GetFrameInstances().size() >= 10000);
			CHECK(statistics.Instances >= 10000);
			GraphicsDevice::WaitForIdle();
		}
	}
}
