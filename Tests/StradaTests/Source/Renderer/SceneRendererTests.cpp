#include "Core/ProfilerTestUtilities.h"
#include "Renderer/RenderTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Core/Image.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Math/Math.h"
#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/RHI/TextureReadback.h"
#include "Strada/Renderer/SceneRenderer.h"
#include "Strada/Renderer/TextureMips.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneRendering.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

TEST_CASE("TextureMips: chains go down to 1x1 one level at a time and average sRGB in linear space")
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

	// One level at a time, as the chain makes them (editor thumbnails shrink textures until they fit).
	Image const next = GenerateNextMipLevel(checker, true);
	CHECK(next.GetWidth() == 1);
	CHECK(next.GetHeight() == 1);
	CHECK(next.GetPixels() == srgb[1].GetPixels());
	CHECK(GenerateNextMipLevel(checker, false).GetPixels() == linear[1].GetPixels());
	Image const tall = GenerateNextMipLevel(Image(1, 6, 4), true);
	CHECK(tall.GetWidth() == 1);
	CHECK(tall.GetHeight() == 3);
	Image const single = GenerateNextMipLevel(Image(1, 1, 4), true);
	CHECK(single.GetWidth() == 1);
	CHECK(single.GetHeight() == 1);
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
	// Four opaque meshes and the glass are a batch each; the tonemap and FXAA are one draw each (no sky without an
	// environment).
	CHECK(renderer.GetStatistics().Batches - renderer.GetStatistics().ShadowDrawCalls == 5);
	CHECK(renderer.GetStatistics().DrawCalls - renderer.GetStatistics().ShadowDrawCalls == 7);
	CHECK(renderer.GetStatistics().ShadowMapViews == 4);
	CHECK(renderer.GetStatistics().ShadowDrawCalls > 0);
	Testing::CheckGoldenImage("SceneRenderer_Showcase", image);
}

TEST_CASE("SceneRenderer: meshes outside the view or beyond its distance are not drawn")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	Scene scene("Culling");
	AssetHandle const material = AddMaterial({0.5f, 0.5f, 0.5f, 1.0f}, 0.0f, 0.8f);
	Testing::AddTestMesh(scene, "Visible", BuiltInAsset::CubeMesh, material, {0.0f, 0.5f, 0.0f});
	Testing::AddTestMesh(scene, "Distant", BuiltInAsset::CubeMesh, material, {0.0f, 0.5f, -40.0f});
	Testing::AddTestMesh(scene, "Behind", BuiltInAsset::CubeMesh, material, {0.0f, 0.5f, 10.0f});
	Testing::AddTestMesh(scene, "Aside", BuiltInAsset::SphereMesh, material, {30.0f, 0.5f, 0.0f});
	Testing::AddTestMesh(scene, "Glass", BuiltInAsset::QuadMesh,
	                     AddMaterial({0.2f, 0.9f, 0.3f, 0.4f}, 0.0f, 0.2f, MaterialAlphaMode::Blend), {0.0f, 0.5f, 12.0f});
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageWidth, ImageHeight);
	SceneRendererCamera camera = MakeCamera({0.0f, 2.2f, 4.5f}, {0.0f, 0.4f, 0.0f});

	// Without lights the one mesh batch is of the main pass: the two cubes in view share their mesh and material, so they
	// are one batch of two instances. The tonemap and FXAA are the frame's other draws.
	(void)Testing::RenderToImage(scene, camera, renderer);
	CHECK(renderer.GetStatistics().Culled == 3);
	CHECK(renderer.GetStatistics().Batches == 1);
	CHECK(renderer.GetStatistics().DrawCalls == 3);
	CHECK(renderer.GetStatistics().Instances == 2);

	camera.MaxDistance = 20.0f;
	(void)Testing::RenderToImage(scene, camera, renderer);
	CHECK(renderer.GetStatistics().Culled == 4);
	CHECK(renderer.GetStatistics().Batches == 1);
	CHECK(renderer.GetStatistics().DrawCalls == 3);
	CHECK(renderer.GetStatistics().Instances == 1);
}

TEST_CASE("SceneRenderer: frames report their GPU time when the GPU can measure it")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	Scope<Scene> scene = MakeShowcaseScene();
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageWidth, ImageHeight);
	SceneRendererCamera const camera = MakeCamera({0.0f, 2.2f, 4.5f}, {0.0f, 0.4f, 0.0f});

	// No frame has finished before the first one.
	RenderScene(*scene, renderer, camera);
	CHECK_FALSE(renderer.GetStatistics().GpuMilliseconds.has_value());
	GraphicsDevice::WaitForIdle();
	// More frames than timers: a frame whose timer still waits for an earlier frame goes unmeasured.
	for (int frame = 0; frame < 8; frame++)
	{
		RenderScene(*scene, renderer, camera);
	}
	if (GraphicsDevice::GetAdapterInfo().SupportsTimerQueries)
	{
		REQUIRE(renderer.GetStatistics().GpuMilliseconds.has_value());
		CHECK(*renderer.GetStatistics().GpuMilliseconds > 0.0f);
	}
	else
	{
		CHECK_FALSE(renderer.GetStatistics().GpuMilliseconds.has_value());
	}
}

#ifdef ST_ENABLE_PROFILING
TEST_CASE("SceneRenderer: while the profiler records, each pass's GPU time reaches the frame that rendered it")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	Testing::ProfilerScope const profiler;
	Scope<Scene> scene = MakeShowcaseScene();
	SceneRenderer renderer("Probe");
	CHECK(renderer.GetViewName() == "Probe");
	renderer.SetViewportSize(ImageWidth, ImageHeight);
	SceneRendererCamera const camera = MakeCamera({0.0f, 2.2f, 4.5f}, {0.0f, 0.4f, 0.0f});

	// Rendering before the recording times no pass.
	RenderScene(*scene, renderer, camera);
	REQUIRE(Profiler::Start().IsOk());
	for (int frame = 0; frame < 12; frame++)
	{
		Profiler::BeginFrame();
		RenderScene(*scene, renderer, camera);
		Profiler::EndFrame();
		if (frame % 4 == 3)
		{
			// The GPU finishes the frames; the next frame's EndScene reads their timers.
			GraphicsDevice::WaitForIdle();
		}
	}
	Profiler::Stop();
	// Frames rendered after the recording read the last timers and hand them to no recording.
	GraphicsDevice::WaitForIdle();
	RenderScene(*scene, renderer, camera);

	ProfilerCapture const& capture = Profiler::GetRecording();
	REQUIRE(capture.Frames.size() == 12);
	// Every frame's CPU phases of the renderer.
	for (std::string_view const phase :
	     {"Scene.UpdateTransforms", "Renderer.Submit", "Renderer.Cull", "Renderer.Prepare", "Renderer.Batch", "Renderer.Record"})
	{
		CAPTURE(phase);
		CHECK(Testing::FindZones(capture, phase).size() == 12);
	}
	size_t measuredFrames = 0;
	for (ProfilerFrame const& frame : capture.Frames)
	{
		if (frame.GpuPasses.empty())
		{
			continue;
		}
		measuredFrames++;
		std::vector<std::string> passes;
		for (ProfilerGpuPass const& pass : frame.GpuPasses)
		{
			CHECK(capture.Names[pass.View] == "Probe");
			CHECK(pass.DurationNanoseconds >= 0);
			passes.push_back(capture.Names[pass.Name]);
		}
		// The showcase's passes in the order they are recorded (its settings draw ambient occlusion and bloom).
		CHECK(passes == std::vector<std::string>{"Upload", "Shadows", "Opaque", "AmbientOcclusion", "Transparent", "Bloom", "Tonemap",
		                                         "Overlays", "FXAA", "ScreenSpace"});
	}
	if (GraphicsDevice::GetAdapterInfo().SupportsTimerQueries)
	{
		CHECK(measuredFrames > 0);
	}
	else
	{
		CHECK(measuredFrames == 0);
	}
}

TEST_CASE("SceneRenderer: while the profiler records, each frame carries the renderer's statistics, and none while it is idle")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	Testing::ProfilerScope const profiler;
	Scope<Scene> scene = MakeShowcaseScene();
	SceneRenderer renderer("Probe");
	renderer.SetViewportSize(ImageWidth, ImageHeight);
	SceneRendererCamera const camera = MakeCamera({0.0f, 2.2f, 4.5f}, {0.0f, 0.4f, 0.0f});
	auto const findCount = [](ProfilerCapture const& capture, ProfilerCounterRecord const& record, std::string_view name)
	{
		for (ProfilerCounterValue const& value : record.Values)
		{
			if (capture.Names[value.Name] == name)
			{
				return std::optional<uint64_t>(value.Value);
			}
		}
		return std::optional<uint64_t>();
	};

	// An idle profiler's frames record nothing; a recorded frame holds one record per frame the renderer drew in it.
	Profiler::BeginFrame();
	RenderScene(*scene, renderer, camera);
	Profiler::EndFrame();
	REQUIRE(Profiler::Start().IsOk());
	std::vector<SceneRendererStatistics> drawn;
	for (int frame = 0; frame < 3; frame++)
	{
		Profiler::BeginFrame();
		RenderScene(*scene, renderer, camera);
		drawn.push_back(renderer.GetStatistics());
		if (frame == 2)
		{
			// A view that renders twice in a frame records twice.
			RenderScene(*scene, renderer, camera);
			drawn.push_back(renderer.GetStatistics());
		}
		Profiler::EndFrame();
	}
	Profiler::Stop();
	RenderScene(*scene, renderer, camera);

	ProfilerCapture const& capture = Profiler::GetRecording();
	REQUIRE(capture.Frames.size() == 3);
	std::vector<ProfilerCounterRecord const*> records;
	for (ProfilerFrame const& frame : capture.Frames)
	{
		CHECK(frame.Counters.size() == (frame.Index == capture.Frames.back().Index ? 2u : 1u));
		for (ProfilerCounterRecord const& record : frame.Counters)
		{
			records.push_back(&record);
		}
	}
	REQUIRE(records.size() == drawn.size());
	for (size_t index = 0; index < records.size(); index++)
	{
		CAPTURE(index);
		ProfilerCounterRecord const& record = *records[index];
		SceneRendererStatistics const& statistics = drawn[index];
		CHECK(capture.Names[record.View] == "Probe");
		CHECK(record.Values.size() == 11);
		CHECK(findCount(capture, record, "DrawCalls") == statistics.DrawCalls);
		CHECK(findCount(capture, record, "Batches") == statistics.Batches);
		CHECK(findCount(capture, record, "Instances") == statistics.Instances);
		CHECK(findCount(capture, record, "Triangles") == statistics.Triangles);
		CHECK(findCount(capture, record, "ShadowDrawCalls") == statistics.ShadowDrawCalls);
		CHECK(findCount(capture, record, "ShadowMapViews") == statistics.ShadowMapViews);
		CHECK(findCount(capture, record, "ShadowsDropped") == statistics.ShadowsDropped);
		CHECK(findCount(capture, record, "Lights") == statistics.Lights);
		CHECK(findCount(capture, record, "CulledLights") == statistics.CulledLights);
		CHECK(findCount(capture, record, "Culled") == statistics.Culled);
		CHECK(findCount(capture, record, "Quads") == statistics.Quads);
	}
	CHECK(drawn.front().DrawCalls > 0);

	// The medians name the counts by view.
	ProfilerMedians const medians = ComputeProfilerMedians(capture);
	auto const drawCalls = std::find_if(medians.Counters.begin(), medians.Counters.end(),
	                                    [](ProfilerCounterMedian const& counter)
	                                    {
											return counter.Name == "Probe/DrawCalls";
										});
	REQUIRE(drawCalls != medians.Counters.end());
	// Two frames rendered once and one twice, the same scene each time: the median frame rendered once.
	CHECK(drawCalls->Value == doctest::Approx(static_cast<double>(drawn.front().DrawCalls)));
}
#endif

TEST_CASE("SceneRenderer: the primary camera's far distance culls what lies beyond it")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	Scene scene("Far");
	AssetHandle const material = AddMaterial({0.5f, 0.5f, 0.5f, 1.0f}, 0.0f, 0.8f);
	// The camera stays at the origin, looking down -Z.
	Testing::AddTestMesh(scene, "Near", BuiltInAsset::CubeMesh, material, {0.0f, 0.0f, -5.0f});
	Testing::AddTestMesh(scene, "Far", BuiltInAsset::CubeMesh, material, {0.0f, 0.0f, -50.0f});
	CameraComponent& camera = scene.CreateEntity("Camera").AddComponent<CameraComponent>();
	camera.Primary = true;
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageWidth, ImageHeight);

	// The two cubes share their mesh and material: one batch, of two instances while both are in view. The tonemap and
	// FXAA are the frame's other draws.
	REQUIRE(RenderSceneFromPrimaryCamera(scene, renderer));
	CHECK(renderer.GetStatistics().Culled == 0);
	CHECK(renderer.GetStatistics().DrawCalls == 3);
	CHECK(renderer.GetStatistics().Instances == 2);

	camera.PerspectiveFar = 20.0f;
	REQUIRE(RenderSceneFromPrimaryCamera(scene, renderer));
	CHECK(renderer.GetStatistics().Culled == 1);
	CHECK(renderer.GetStatistics().DrawCalls == 3);
	CHECK(renderer.GetStatistics().Instances == 1);

	camera.Projection = ProjectionType::Orthographic;
	camera.OrthographicFar = 20.0f;
	REQUIRE(RenderSceneFromPrimaryCamera(scene, renderer));
	CHECK(renderer.GetStatistics().Culled == 1);
	CHECK(renderer.GetStatistics().DrawCalls == 3);
	CHECK(renderer.GetStatistics().Instances == 1);
}

TEST_CASE("SceneRenderer: lights that cannot reach the view are culled")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	Scene scene("Lights");
	Testing::AddTestMesh(scene, "Floor", BuiltInAsset::PlaneMesh, AddMaterial({0.5f, 0.5f, 0.5f, 1.0f}, 0.0f, 0.8f), {0.0f, 0.0f, 0.0f},
	                     glm::vec3(8.0f));
	scene.CreateEntity("Sun").AddComponent<DirectionalLightComponent>().CastShadows = true;
	auto const addPointLight = [&scene](char const* name, glm::vec3 const& position)
	{
		Entity light = scene.CreateEntity(name);
		light.GetComponent<TransformComponent>().Translation = position;
		PointLightComponent& point = light.AddComponent<PointLightComponent>();
		point.Range = 5.0f;
		point.CastShadows = true;
	};
	auto const addSpotLight = [&scene](char const* name, float pitch)
	{
		Entity light = scene.CreateEntity(name);
		light.GetComponent<TransformComponent>().Translation = {0.0f, 5.0f, 0.0f};
		light.GetComponent<TransformComponent>().SetRotationEuler({pitch, 0.0f, 0.0f});
		SpotLightComponent& spot = light.AddComponent<SpotLightComponent>();
		spot.Range = 6.0f;
		spot.OuterConeAngle = 30.0f;
		spot.CastShadows = true;
	};
	addPointLight("Near", {0.0f, 1.0f, 0.0f});
	addPointLight("Aside", {40.0f, 1.0f, 0.0f});
	addSpotLight("Down", -90.0f);
	// Above the view and shining up: its range sphere reaches into the view, its cone does not.
	addSpotLight("Up", 90.0f);

	SceneRenderer renderer;
	renderer.SetViewportSize(ImageWidth, ImageHeight);
	(void)Testing::RenderToImage(scene, MakeCamera({0.0f, 2.2f, 4.5f}, {0.0f, 0.4f, 0.0f}), renderer);
	CHECK(renderer.GetStatistics().Lights == 3);
	CHECK(renderer.GetStatistics().CulledLights == 2);
	// Shadow maps only for the lights kept: the sun's cascades, the six faces of "Near" and the slice of "Down".
	CHECK(renderer.GetStatistics().ShadowMapViews == 4 + 6 + 1);
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

TEST_CASE("SceneRenderer: the first sky light in hierarchy order lights the scene, whatever the order the lights were made in")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	constexpr uint32_t Size = 64;
	Scene scene("Skies");
	MaterialData white;
	white.BaseColor = glm::vec4(1.0f);
	white.Roughness = 1.0f;
	Testing::AddTestMesh(scene, "Cube", BuiltInAsset::CubeMesh, AssetManager::AddMemoryAsset(CreateRef<MaterialAsset>(white), "White"),
	                     glm::vec3(0.0f));
	SceneRenderer renderer;
	renderer.SetViewportSize(Size, Size);
	SceneRendererCamera const camera = Testing::MakeTestCamera({0.0f, 0.5f, 3.0f}, glm::vec3(0.0f), Size, Size);
	auto const center = [&scene, &renderer, &camera]
	{
		Image const image = Testing::RenderToImage(scene, camera, renderer);
		uint8_t const* pixel = image.GetPixel(Size / 2, Size / 2);
		return glm::ivec3(pixel[0], pixel[1], pixel[2]);
	};

	// No sky light: no ambient light, and nothing else lights the cube.
	glm::ivec3 const dark = center();
	CHECK(dark.r + dark.g + dark.b < 30);

	// One sky light, wherever it is.
	Entity first = scene.CreateEntity("First root");
	Entity second = scene.CreateEntity("Second root");
	Entity red = scene.CreateEntity("Red sky", second);
	red.AddComponent<SkyLightComponent>().AmbientColor = {1.0f, 0.0f, 0.0f};
	glm::ivec3 const alone = center();
	CHECK(alone.r > alone.g + 40);
	CHECK(alone.r > alone.b + 40);

	// Three: the first in hierarchy order is neither the first made nor the last (the green one, below the first root).
	Entity green = scene.CreateEntity("Green sky", first);
	green.AddComponent<SkyLightComponent>().AmbientColor = {0.0f, 1.0f, 0.0f};
	Entity blue = scene.CreateEntity("Blue sky", second);
	blue.AddComponent<SkyLightComponent>().AmbientColor = {0.0f, 0.0f, 1.0f};
	glm::ivec3 const several = center();
	CHECK(several.g > several.r + 40);
	CHECK(several.g > several.b + 40);
}
