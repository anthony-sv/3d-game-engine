#include "Renderer/RenderTestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Asset/TextureAsset.h"
#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/RHI/TextureReadback.h"
#include "Strada/Renderer/InstanceMath.h"
#include "Strada/Renderer/SceneRenderer.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneRendering.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	constexpr uint32_t ImageSize = 128;

	// One mesh submission of a test frame.
	struct TestInstance
	{
		Ref<MeshSource> Mesh;
		Ref<MaterialAsset> Material;
		glm::mat4 Transform = glm::mat4(1.0f);
		uint32_t PickingId = 0;
		bool Selected = false;
		bool CastShadows = true;
	};

	// The lights of a test frame and the cascades of its directional light.
	struct TestLights
	{
		std::optional<DirectionalLightSubmission> Directional;
		std::vector<SpotLightSubmission> Spots;
		std::vector<PointLightSubmission> Points;
		uint32_t CascadeCount = 2;
	};

	Ref<MeshSource> GetMesh(BuiltInAsset mesh)
	{
		Ref<MeshSource> source = AssetManager::GetAsset<MeshSource>(GetBuiltInHandle(mesh));
		REQUIRE(source);
		return source;
	}

	// Unlit by any light: emissive, so the surfaces show their color without lights.
	Ref<MaterialAsset> MakeMaterial(glm::vec4 const& baseColor, MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque,
	                                bool doubleSided = false)
	{
		MaterialData data;
		data.BaseColor = baseColor;
		data.AlphaMode = alphaMode;
		data.DoubleSided = doubleSided;
		data.EmissiveColor = glm::vec3(baseColor);
		data.EmissiveIntensity = 1.0f;
		return CreateRef<MaterialAsset>(data);
	}

	glm::mat4 Place(glm::vec3 const& position, float yawDegrees = 0.0f, glm::vec3 const& scale = glm::vec3(1.0f))
	{
		glm::mat4 const rotation = glm::rotate(glm::mat4(1.0f), glm::radians(yawDegrees), glm::vec3(0.0f, 1.0f, 0.0f));
		return glm::scale(glm::translate(glm::mat4(1.0f), position) * rotation, scale);
	}

	SceneRendererCamera MakeCamera(glm::vec3 const& position, glm::vec3 const& target)
	{
		return Testing::MakeTestCamera(position, target, ImageSize, ImageSize);
	}

	Image Draw(SceneRenderer& renderer, SceneRendererCamera const& camera, std::span<TestInstance const> instances,
	           SceneRendererOverlays const& overlays = {}, TestLights const& lights = {})
	{
		SceneRendererSettings settings;
		// Images free of the post-processing that would blur the comparisons, and hard shadows.
		settings.Bloom = false;
		settings.FXAA = false;
		settings.AmbientOcclusion = false;
		settings.Dithering = false;
		settings.SoftShadows = false;
		settings.CascadeCount = lights.CascadeCount;
		renderer.BeginScene(camera, settings);
		if (lights.Directional)
		{
			renderer.SubmitDirectionalLight(*lights.Directional);
		}
		for (SpotLightSubmission const& spot : lights.Spots)
		{
			renderer.SubmitSpotLight(spot);
		}
		for (PointLightSubmission const& point : lights.Points)
		{
			renderer.SubmitPointLight(point);
		}
		for (TestInstance const& instance : instances)
		{
			Ref<MaterialAsset> const materials[] = {instance.Material};
			renderer.SubmitMesh(instance.Mesh, materials, instance.Transform, instance.CastShadows, instance.PickingId, instance.Selected);
		}
		renderer.SetOverlays(overlays);
		renderer.EndScene();
		Result<Image> image = ReadbackTexture(renderer.GetFinalImage());
		REQUIRE(image.IsOk());
		return std::move(image.GetValue());
	}

	std::optional<uint32_t> Pick(SceneRenderer& renderer, SceneRendererCamera const& camera, std::span<TestInstance const> instances,
	                             glm::uvec2 const& pixel)
	{
		renderer.RequestPick(pixel.x, pixel.y);
		(void)Draw(renderer, camera, instances);
		GraphicsDevice::GetDevice()->waitForIdle();
		return renderer.TakePickResult();
	}

	glm::vec2 Project(SceneRendererCamera const& camera, glm::vec3 const& point)
	{
		glm::vec4 const clip = camera.Projection * camera.View * glm::vec4(point, 1.0f);
		glm::vec2 const ndc = glm::vec2(clip) / clip.w;
		// D3D conventions: NDC +Y is up, pixel rows go down.
		return {(ndc.x * 0.5f + 0.5f) * static_cast<float>(ImageSize), (0.5f - ndc.y * 0.5f) * static_cast<float>(ImageSize)};
	}

	glm::uvec2 ProjectToPixel(SceneRendererCamera const& camera, glm::vec3 const& point)
	{
		glm::vec2 const pixel = Project(camera, point);
		return {static_cast<uint32_t>(pixel.x), static_cast<uint32_t>(pixel.y)};
	}

	glm::ivec3 PixelAt(Image const& image, glm::uvec2 const& pixel)
	{
		uint8_t const* data = image.GetPixel(pixel.x, pixel.y);
		return {data[0], data[1], data[2]};
	}

	uint32_t GetTriangles(Ref<MeshSource> const& mesh)
	{
		return mesh->GetSubmeshes()[0].IndexCount / 3;
	}

	bool HasSameBits(glm::mat4 const& a, glm::mat4 const& b)
	{
		return std::memcmp(&a, &b, sizeof(glm::mat4)) == 0;
	}
}

TEST_CASE("Instancing: the normal basis is the inverse transpose scaled by the determinant's magnitude, finite when singular")
{
	std::mt19937 random(20261115u);
	std::uniform_real_distribution<float> element(-2.0f, 2.0f);
	for (int round = 0; round < 200; round++)
	{
		glm::mat4 model(1.0f);
		for (int column = 0; column < 4; column++)
		{
			for (int row = 0; row < 3; row++)
			{
				model[column][row] = element(random);
			}
		}
		glm::mat3 const linear(model);
		float const determinant = glm::determinant(linear);
		if (std::abs(determinant) < 1e-3f)
		{
			continue;
		}
		glm::mat3 const expected = glm::transpose(glm::inverse(linear)) * std::abs(determinant);
		std::array<glm::vec4, 3> const basis = Instancing::ComputeNormalBasis(model);
		for (int column = 0; column < 3; column++)
		{
			CAPTURE(round);
			CAPTURE(column);
			CHECK(basis[column].w == 0.0f);
			for (int row = 0; row < 3; row++)
			{
				CHECK(basis[column][row] == doctest::Approx(expected[column][row]).epsilon(1e-4).scale(1.0));
			}
		}
	}

	// A mirror keeps the inverse transpose's directions: normals still point out of the mirrored surface.
	glm::mat4 const mirror = glm::scale(glm::mat4(1.0f), glm::vec3(-2.0f, 1.0f, 3.0f));
	std::array<glm::vec4, 3> const mirrored = Instancing::ComputeNormalBasis(mirror);
	glm::mat3 const mirrorInverseTranspose = glm::transpose(glm::inverse(glm::mat3(mirror)));
	for (int column = 0; column < 3; column++)
	{
		CHECK(glm::dot(glm::vec3(mirrored[column]), mirrorInverseTranspose[column]) > 0.0f);
	}

	// Singular scales give finite bases: a flattened axis keeps the normal of the plane it flattens to.
	for (glm::vec3 const& scale : {glm::vec3(1.0f, 0.0f, 1.0f), glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 2.0f)})
	{
		CAPTURE(scale.x);
		CAPTURE(scale.y);
		CAPTURE(scale.z);
		std::array<glm::vec4, 3> const basis = Instancing::ComputeNormalBasis(glm::scale(glm::mat4(1.0f), scale));
		for (glm::vec4 const& column : basis)
		{
			CHECK(std::isfinite(column.x));
			CHECK(std::isfinite(column.y));
			CHECK(std::isfinite(column.z));
		}
	}
	std::array<glm::vec4, 3> const flat = Instancing::ComputeNormalBasis(glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 1.0f)));
	CHECK(glm::vec3(flat[1]) == glm::vec3(0.0f, 1.0f, 0.0f));

	ShaderInterop::InstanceData const data = Instancing::MakeInstanceData(mirror, glm::mat4(2.0f), 7u | ShaderInterop::EntityIdSelectedBit);
	CHECK(HasSameBits(data.Model, mirror));
	CHECK(HasSameBits(data.PreviousModel, glm::mat4(2.0f)));
	CHECK(data.EntityId == (7u | ShaderInterop::EntityIdSelectedBit));
	CHECK(glm::vec3(data.NormalBasis[0]) == glm::vec3(mirrored[0]));
	CHECK(data.Padding == glm::vec3(0.0f));
}

TEST_CASE("Instancing: equal instances are one batch in the opaque and entity-ID passes, drawn where each one is")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageSize, ImageSize);
	renderer.SetEntityIdsEnabled(true);
	SceneRendererCamera const camera = MakeCamera({0.0f, 0.0f, 14.0f}, {0.0f, 0.0f, 0.0f});
	Ref<MeshSource> const cube = GetMesh(BuiltInAsset::CubeMesh);
	Ref<MaterialAsset> const material = MakeMaterial({0.3f, 0.6f, 0.9f, 1.0f});

	// 64 cubes of one mesh and material, each turned and scaled differently.
	std::vector<TestInstance> instances;
	for (uint32_t i = 0; i < 64; i++)
	{
		glm::vec3 const position(static_cast<float>(i % 8) * 1.5f - 5.25f, static_cast<float>(i / 8) * 1.5f - 5.25f, 0.0f);
		instances.push_back(
			{cube, material, Place(position, static_cast<float>(i) * 7.0f, glm::vec3(0.6f + 0.005f * static_cast<float>(i))), i + 1});
	}
	Image const batched = Draw(renderer, camera, instances);
	SceneRendererStatistics const& statistics = renderer.GetStatistics();
	CHECK(statistics.Batches == 2);
	CHECK(statistics.Instances == 128);
	// The two batches and the tonemap (no sky, FXAA, overlay or outline is drawn).
	CHECK(statistics.DrawCalls == 3);
	CHECK(statistics.Triangles == 2 * 64 * GetTriangles(cube));
	CHECK(statistics.Culled == 0);
	SceneRenderer::PassInstanceCounts const& passes = renderer.GetPassInstanceCounts();
	CHECK(passes.Opaque == 64);
	CHECK(passes.EntityIds == 64);
	CHECK(passes.Transparent == 0);
	CHECK(passes.ShadowViews.empty());

	// The instances as uploaded: one per submission, in submission order.
	std::span<ShaderInterop::InstanceData const> const uploaded = renderer.GetFrameInstances();
	REQUIRE(uploaded.size() == 64);
	for (uint32_t i = 0; i < 64; i++)
	{
		CAPTURE(i);
		CHECK(HasSameBits(uploaded[i].Model, instances[i].Transform));
		CHECK(HasSameBits(uploaded[i].PreviousModel, instances[i].Transform));
		CHECK(uploaded[i].EntityId == i + 1);
	}

	// A material of its own per cube (equal data) makes 64 batches of one, and the same image: each instance is drawn
	// with its own matrix and normal basis.
	for (TestInstance& instance : instances)
	{
		instance.Material = CreateRef<MaterialAsset>(material->GetData());
	}
	Image const separate = Draw(renderer, camera, instances);
	CHECK(renderer.GetStatistics().Batches == 128);
	CHECK(renderer.GetStatistics().Instances == 128);
	CHECK(Testing::CompareImages(batched, separate, 0).MaxDifference == 0);
	CHECK(Testing::CompareImages(batched, Draw(renderer, camera, {}), 8).OutlierFraction > 0.05);
}

TEST_CASE("Instancing: each mesh and material is a batch of its own, masked and double-sided variants included")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageSize, ImageSize);
	SceneRendererCamera const camera = MakeCamera({0.0f, 0.0f, 9.0f}, {0.0f, 0.0f, 0.0f});
	Ref<MeshSource> const cube = GetMesh(BuiltInAsset::CubeMesh);
	Ref<MeshSource> const sphere = GetMesh(BuiltInAsset::SphereMesh);

	SUBCASE("K materials are K batches, and another mesh with one of them is one more")
	{
		std::array<Ref<MaterialAsset>, 3> const materials = {MakeMaterial({0.9f, 0.2f, 0.2f, 1.0f}), MakeMaterial({0.2f, 0.9f, 0.2f, 1.0f}),
		                                                     MakeMaterial({0.2f, 0.2f, 0.9f, 1.0f})};
		std::vector<TestInstance> instances;
		for (uint32_t i = 0; i < 30; i++)
		{
			glm::vec3 const position(static_cast<float>(i % 6) - 2.5f, static_cast<float>(i / 6) - 2.0f, 0.0f);
			// Interleaved: no two neighbours in submission order share a material.
			instances.push_back({cube, materials[i % 3], Place(position, 0.0f, glm::vec3(0.4f))});
		}
		(void)Draw(renderer, camera, instances);
		CHECK(renderer.GetStatistics().Batches == 3);
		CHECK(renderer.GetStatistics().Instances == 30);
		CHECK(renderer.GetPassInstanceCounts().Opaque == 30);

		instances.push_back({sphere, materials[0], Place({3.5f, 0.0f, 0.0f}, 0.0f, glm::vec3(0.4f))});
		instances.push_back({sphere, materials[0], Place({-3.5f, 0.0f, 0.0f}, 0.0f, glm::vec3(0.4f))});
		(void)Draw(renderer, camera, instances);
		CHECK(renderer.GetStatistics().Batches == 4);
		CHECK(renderer.GetStatistics().Instances == 32);
		CHECK(renderer.GetStatistics().Triangles == 30 * GetTriangles(cube) + 2 * GetTriangles(sphere));
	}

	SUBCASE("masked and double-sided materials split from the opaque one, the cut-out discards and the back faces show")
	{
		Ref<MeshSource> const quad = GetMesh(BuiltInAsset::QuadMesh);
		glm::vec4 const color(0.9f, 0.6f, 0.2f, 1.0f);
		Ref<MaterialAsset> const opaque = MakeMaterial(color);
		// Fully cut out: its alpha is below the cutoff everywhere.
		Ref<MaterialAsset> const masked = MakeMaterial({color.r, color.g, color.b, 0.0f}, MaterialAlphaMode::Mask);
		Ref<MaterialAsset> const doubleSided = MakeMaterial(color, MaterialAlphaMode::Opaque, true);
		std::array<Ref<MaterialAsset>, 3> const variants = {opaque, masked, doubleSided};
		// The top row faces the camera, the bottom row faces away.
		std::vector<TestInstance> instances;
		for (uint32_t column = 0; column < 3; column++)
		{
			float const x = static_cast<float>(column) * 2.0f - 2.0f;
			instances.push_back({quad, variants[column], Place({x, 1.2f, 0.0f}, 0.0f, glm::vec3(1.5f))});
			instances.push_back({quad, variants[column], Place({x, -1.2f, 0.0f}, 180.0f, glm::vec3(1.5f))});
		}
		Image const image = Draw(renderer, camera, instances);
		CHECK(renderer.GetStatistics().Batches == 3);
		CHECK(renderer.GetStatistics().Instances == 6);

		auto const shows = [&image, &camera](glm::vec3 const& point)
		{
			glm::ivec3 const pixel = PixelAt(image, ProjectToPixel(camera, point));
			return pixel.r > 100 && pixel.g > 50;
		};
		CHECK(shows({-2.0f, 1.2f, 0.0f}));
		CHECK_FALSE(shows({0.0f, 1.2f, 0.0f}));
		CHECK(shows({2.0f, 1.2f, 0.0f}));
		CHECK_FALSE(shows({-2.0f, -1.2f, 0.0f}));
		CHECK_FALSE(shows({0.0f, -1.2f, 0.0f}));
		CHECK(shows({2.0f, -1.2f, 0.0f}));
	}
}

namespace
{
	// The image mirrored left to right.
	Image MirrorImage(Image const& image)
	{
		Image mirrored(image.GetWidth(), image.GetHeight(), image.GetChannels());
		for (uint32_t y = 0; y < image.GetHeight(); y++)
		{
			for (uint32_t x = 0; x < image.GetWidth(); x++)
			{
				std::memcpy(mirrored.GetPixel(image.GetWidth() - 1 - x, y), image.GetPixel(x, y), image.GetChannels());
			}
		}
		return mirrored;
	}
}

TEST_CASE("Instancing: mirrored instances are batched apart and drawn as the mirror image of their unmirrored scene")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageSize, ImageSize);
	Ref<MeshSource> const cube = GetMesh(BuiltInAsset::CubeMesh);

	SUBCASE("a mirrored instance and an unmirrored one of a mesh and material are two batches")
	{
		Ref<MaterialAsset> const material = MakeMaterial({0.8f, 0.4f, 0.2f, 1.0f});
		SceneRendererCamera const camera = MakeCamera({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f});
		std::vector<TestInstance> const instances = {{cube, material, Place({-1.0f, 0.0f, 0.0f})},
		                                             {cube, material, Place({1.0f, 0.0f, 0.0f}, 0.0f, {-1.0f, 1.0f, 1.0f})},
		                                             {cube, material, Place({0.0f, 1.5f, 0.0f}, 0.0f, {1.0f, -1.0f, -1.0f})}};
		(void)Draw(renderer, camera, instances);
		// A turn by 180 degrees (two negative scales) keeps the winding: it joins the unmirrored instance.
		CHECK(renderer.GetStatistics().Batches == 2);
		CHECK(renderer.GetStatistics().Instances == 3);
	}

	SUBCASE("a scene under a mirroring parent draws the mirror image of the scene: outer faces, normals, tangents, shadows")
	{
		// Lit (not emissive), so that every face's shading shows its normal; a normal map whose normals lean along both
		// texture axes, so that its shading also shows the tangent frame's handedness.
		Image bumps(8, 8, 4);
		for (uint32_t y = 0; y < 8; y++)
		{
			for (uint32_t x = 0; x < 8; x++)
			{
				uint8_t* pixel = bumps.GetPixel(x, y);
				pixel[0] = x < 4 ? 210 : 45;
				pixel[1] = y < 4 ? 210 : 45;
				pixel[2] = 200;
				pixel[3] = 255;
			}
		}
		Result<Ref<TextureAsset>> bumpsTexture = TextureAsset::CreateFromImage(std::move(bumps));
		REQUIRE(bumpsTexture.IsOk());
		auto const makeLit = [](glm::vec4 const& baseColor, bool doubleSided)
		{
			MaterialData data;
			data.BaseColor = baseColor;
			data.Roughness = 0.6f;
			data.DoubleSided = doubleSided;
			return CreateRef<MaterialAsset>(data);
		};
		Ref<MaterialAsset> const stone = makeLit({0.6f, 0.6f, 0.58f, 1.0f}, false);
		Ref<MaterialAsset> const brick = makeLit({0.8f, 0.35f, 0.2f, 1.0f}, false);
		Ref<MaterialAsset> const leaf = makeLit({0.3f, 0.7f, 0.25f, 1.0f}, true);
		MaterialData bumped;
		bumped.BaseColor = {0.7f, 0.7f, 0.75f, 1.0f};
		bumped.Roughness = 0.4f;
		bumped.NormalTexture = AssetManager::AddMemoryAsset(bumpsTexture.GetValue(), "Bumps");
		Ref<MaterialAsset> const bumpy = CreateRef<MaterialAsset>(bumped);

		// The camera lies in the mirror's plane (x = 0) and looks along it, so the mirrored scene's image is the image
		// mirrored.
		SceneRendererCamera const camera = MakeCamera({0.0f, 2.6f, 4.2f}, {0.0f, 0.4f, 0.0f});
		std::vector<TestInstance> scene = {
			{GetMesh(BuiltInAsset::PlaneMesh), stone, Place({0.0f, 0.0f, 0.0f}, 0.0f, glm::vec3(6.0f))},
			{cube, brick, Place({-0.9f, 0.5f, 0.0f}, 25.0f, {0.8f, 1.0f, 0.6f})},
			{GetMesh(BuiltInAsset::ConeMesh), brick, Place({0.9f, 0.4f, 0.6f}, -40.0f, glm::vec3(0.7f))},
			{GetMesh(BuiltInAsset::QuadMesh), bumpy, Place({0.4f, 0.8f, -0.7f}, 15.0f, glm::vec3(1.4f))},
			{GetMesh(BuiltInAsset::QuadMesh), leaf, Place({-0.3f, 0.5f, 1.0f}, 140.0f, glm::vec3(0.8f))},
		};
		TestLights lights;
		lights.Directional = DirectionalLightSubmission{};
		lights.Directional->Direction = glm::normalize(glm::vec3(0.5f, -1.0f, -0.4f));
		lights.Directional->Intensity = 3.0f;
		lights.Directional->CastShadows = true;
		Image const image = Draw(renderer, camera, scene, {}, lights);

		glm::mat4 const mirror = glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f));
		for (TestInstance& instance : scene)
		{
			instance.Transform = mirror * instance.Transform;
		}
		lights.Directional->Direction.x = -lights.Directional->Direction.x;
		Image const mirrored = Draw(renderer, camera, scene, {}, lights);
		// Every instance is mirrored now, each one a batch of its own as before.
		CHECK(renderer.GetStatistics().Batches - renderer.GetStatistics().ShadowDrawCalls == 5);

		// Only rasterization and the shadow map's texel grid, which do not mirror, may differ: along edges.
		Testing::ImageDifference const difference = Testing::CompareImages(mirrored, MirrorImage(image), 24);
		CHECK(difference.MeanDifference < 1.0);
		CHECK(difference.OutlierFraction <= 0.01);
	}
}

TEST_CASE("Instancing: blended items are never batched and are drawn back to front whatever their submission order")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageSize, ImageSize);
	renderer.SetEntityIdsEnabled(true);
	SceneRendererCamera const camera = MakeCamera({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f});
	Ref<MeshSource> const quad = GetMesh(BuiltInAsset::QuadMesh);

	// Four of one mesh and material: four draws of one instance in each of the transparent and entity-ID passes.
	Ref<MaterialAsset> const glass = MakeMaterial({0.2f, 0.8f, 0.4f, 0.5f}, MaterialAlphaMode::Blend);
	std::vector<TestInstance> equal;
	for (uint32_t i = 0; i < 4; i++)
	{
		equal.push_back({quad, glass, Place({static_cast<float>(i) - 1.5f, 0.0f, 0.0f}, 0.0f, glm::vec3(0.8f)), i + 1});
	}
	(void)Draw(renderer, camera, equal);
	CHECK(renderer.GetStatistics().Batches == 8);
	CHECK(renderer.GetStatistics().Instances == 8);
	CHECK(renderer.GetPassInstanceCounts().Transparent == 4);
	CHECK(renderer.GetPassInstanceCounts().EntityIds == 4);
	CHECK(renderer.GetPassInstanceCounts().Opaque == 0);

	// Five overlapping half-transparent quads, the nearest magenta: drawn back to front, the nearest weighs most.
	renderer.SetEntityIdsEnabled(false);
	std::array<glm::vec3, 5> const colors = {glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f),
	                                         glm::vec3(1.0f, 1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 1.0f)};
	std::vector<TestInstance> layers;
	for (uint32_t i = 0; i < colors.size(); i++)
	{
		layers.push_back({quad, MakeMaterial(glm::vec4(colors[i] * 0.6f, 0.5f), MaterialAlphaMode::Blend),
		                  Place({0.0f, 0.0f, static_cast<float>(i) * 0.25f}, 0.0f, glm::vec3(1.5f))});
	}
	Image const ordered = Draw(renderer, camera, layers);
	CHECK(renderer.GetStatistics().Batches == 5);
	CHECK(renderer.GetPassInstanceCounts().Transparent == 5);
	glm::ivec3 const center = PixelAt(ordered, ProjectToPixel(camera, {0.0f, 0.0f, 1.0f}));
	CHECK(center.b > center.g);
	CHECK(center.r > center.g);

	std::mt19937 random(5u);
	for (int shuffle = 0; shuffle < 4; shuffle++)
	{
		CAPTURE(shuffle);
		std::shuffle(layers.begin(), layers.end(), random);
		CHECK(Testing::CompareImages(Draw(renderer, camera, layers), ordered, 0).MaxDifference == 0);
	}
}

TEST_CASE("Instancing: picking returns each instance of a batch, and the outline surrounds the selected one alone")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageSize, ImageSize);
	renderer.SetEntityIdsEnabled(true);
	SceneRendererCamera const camera = MakeCamera({0.0f, 0.0f, 8.0f}, {0.0f, 0.0f, 0.0f});
	Ref<MeshSource> const cube = GetMesh(BuiltInAsset::CubeMesh);
	Ref<MaterialAsset> const material = MakeMaterial({0.2f, 0.3f, 0.9f, 1.0f});

	// A 3 x 3 grid of one mesh and material.
	std::vector<TestInstance> instances;
	std::vector<glm::vec3> centers;
	for (uint32_t i = 0; i < 9; i++)
	{
		glm::vec3 const position(static_cast<float>(i % 3) * 1.6f - 1.6f, static_cast<float>(i / 3) * 1.6f - 1.6f, 0.0f);
		centers.push_back(position);
		instances.push_back({cube, material, Place(position), 101 + i});
	}
	for (uint32_t i = 0; i < 9; i++)
	{
		CAPTURE(i);
		CHECK(Pick(renderer, camera, instances, ProjectToPixel(camera, centers[i] + glm::vec3(0.0f, 0.0f, 0.5f))) ==
		      std::optional<uint32_t>(101 + i));
	}
	CHECK(renderer.GetStatistics().Batches == 2);
	CHECK(renderer.GetStatistics().Instances == 18);
	// Between the cubes nothing is pickable.
	CHECK(Pick(renderer, camera, instances, ProjectToPixel(camera, {0.8f, 0.8f, 0.0f})) == std::optional<uint32_t>(0u));

	SceneRendererOverlays overlays;
	overlays.SelectionColor = {1.0f, 0.5f, 0.0f, 1.0f};
	overlays.SelectionOutlineWidth = 2;
	Image const unselected = Draw(renderer, camera, instances, overlays);
	instances[4].Selected = true;
	Image const selected = Draw(renderer, camera, instances, overlays);
	// The selection bit is the selected instance's alone, and does not split the batch.
	CHECK(renderer.GetStatistics().Batches == 2);
	std::span<ShaderInterop::InstanceData const> const uploaded = renderer.GetFrameInstances();
	REQUIRE(uploaded.size() == 9);
	for (uint32_t i = 0; i < 9; i++)
	{
		CAPTURE(i);
		CHECK(uploaded[i].EntityId == ((101 + i) | (i == 4 ? ShaderInterop::EntityIdSelectedBit : 0u)));
	}
	CHECK(Pick(renderer, camera, instances, ProjectToPixel(camera, centers[4] + glm::vec3(0.0f, 0.0f, 0.5f))) ==
	      std::optional<uint32_t>(105u));

	// Every pixel the outline changed lies around the selected cube: within the outline's width of its silhouette's box.
	glm::vec2 minimum(static_cast<float>(ImageSize));
	glm::vec2 maximum(0.0f);
	for (uint32_t corner = 0; corner < 8; corner++)
	{
		glm::vec3 const offset((corner & 1) ? 0.5f : -0.5f, (corner & 2) ? 0.5f : -0.5f, (corner & 4) ? 0.5f : -0.5f);
		glm::vec2 const pixel = Project(camera, centers[4] + offset);
		minimum = glm::min(minimum, pixel);
		maximum = glm::max(maximum, pixel);
	}
	float const margin = static_cast<float>(overlays.SelectionOutlineWidth) + 2.0f;
	size_t changed = 0;
	for (uint32_t y = 0; y < ImageSize; y++)
	{
		for (uint32_t x = 0; x < ImageSize; x++)
		{
			if (PixelAt(selected, {x, y}) == PixelAt(unselected, {x, y}))
			{
				continue;
			}
			changed++;
			CAPTURE(x);
			CAPTURE(y);
			glm::vec2 const center(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
			CHECK(center.x >= minimum.x - margin);
			CHECK(center.x <= maximum.x + margin);
			CHECK(center.y >= minimum.y - margin);
			CHECK(center.y <= maximum.y + margin);
		}
	}
	CHECK(changed > 20);
}

TEST_CASE("Instancing: instances carry the scene's world and previous matrices, the same for every renderer of a frame")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	Scene scene("Motion");
	Entity parent = scene.CreateEntity("Parent");
	Entity child = scene.CreateEntity("Child", parent);
	child.AddComponent<MeshComponent>().Mesh = GetBuiltInHandle(BuiltInAsset::CubeMesh);
	child.GetComponent<TransformComponent>().Translation = {0.0f, 0.5f, 0.0f};
	TransformComponent& transform = parent.GetComponent<TransformComponent>();
	SceneRenderer first;
	SceneRenderer second;
	first.SetViewportSize(ImageSize, ImageSize);
	second.SetViewportSize(ImageSize, ImageSize);
	SceneRendererCamera const camera = MakeCamera({0.0f, 1.0f, 6.0f}, {0.0f, 0.0f, 0.0f});
	auto const instance = [](SceneRenderer const& renderer)
	{
		REQUIRE(renderer.GetFrameInstances().size() == 1);
		return renderer.GetFrameInstances()[0];
	};

	// A scene that never began a frame has no motion.
	RenderScene(scene, first, camera);
	ShaderInterop::InstanceData const still = instance(first);
	CHECK(HasSameBits(still.Model, scene.GetWorldTransform(child)));
	CHECK(HasSameBits(still.PreviousModel, still.Model));
	CHECK(still.EntityId == 0);

	// The parent moves in a frame: the child's instance has the matrix the frame began with as its previous one, in
	// both renderers that draw the frame.
	scene.BeginTransformFrame();
	transform.Translation = {1.0f, 0.0f, 0.0f};
	RenderScene(scene, first, camera);
	RenderScene(scene, second, camera);
	ShaderInterop::InstanceData const moved = instance(first);
	CHECK(HasSameBits(moved.Model, scene.GetWorldTransform(child)));
	CHECK(HasSameBits(moved.PreviousModel, still.Model));
	CHECK_FALSE(HasSameBits(moved.Model, still.Model));
	ShaderInterop::InstanceData const other = instance(second);
	CHECK(std::memcmp(&moved, &other, sizeof(moved)) == 0);

	// A write after the frame's first view reaches the next draw.
	transform.Translation = {2.0f, 0.0f, 0.0f};
	RenderScene(scene, first, camera);
	CHECK(HasSameBits(instance(first).Model, scene.GetWorldTransform(child)));
	CHECK(HasSameBits(instance(first).PreviousModel, still.Model));

	// A frame without a move: no motion.
	scene.BeginTransformFrame();
	glm::mat4 const settled = scene.GetWorldTransform(child);
	RenderScene(scene, first, camera);
	CHECK(HasSameBits(instance(first).Model, settled));
	CHECK(HasSameBits(instance(first).PreviousModel, settled));
}

TEST_CASE("Instancing: the instance buffers grow past their first capacity and every pass reads the instances beyond it")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageSize, ImageSize);
	renderer.SetEntityIdsEnabled(true);
	SceneRendererCamera const camera = MakeCamera({0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 0.0f});
	Ref<MeshSource> const cube = GetMesh(BuiltInAsset::CubeMesh);
	Ref<MaterialAsset> const material = MakeMaterial({0.9f, 0.5f, 0.1f, 1.0f});
	auto const makeFrame = [&cube, &material](uint32_t count)
	{
		// Small cubes (wider than a pixel) in a grid on the left; the one at index 1200, when there is one, large on the right.
		std::vector<TestInstance> frame;
		for (uint32_t i = 0; i < count; i++)
		{
			glm::vec3 const position(-4.4f + static_cast<float>(i % 30) * 0.13f, -3.2f + static_cast<float>(i / 30) * 0.13f, 0.0f);
			frame.push_back({cube, material, Place(position, 0.0f, glm::vec3(0.12f)), i + 1});
		}
		if (count > 1200)
		{
			frame[1200].Transform = Place({2.0f, 0.0f, 0.0f}, 0.0f, glm::vec3(1.5f));
		}
		return frame;
	};
	glm::uvec2 const right = ProjectToPixel(camera, {2.0f, 0.0f, 0.75f});

	// A spot light that reaches every cube, so that the shadow pass reads the grown buffers too (its binding set names
	// them).
	TestLights lights;
	SpotLightSubmission& spot = lights.Spots.emplace_back();
	spot.Position = {0.0f, 0.0f, 8.0f};
	spot.Direction = {0.0f, 0.0f, -1.0f};
	spot.Range = 30.0f;
	spot.OuterConeAngle = 60.0f;
	spot.CastShadows = true;

	// A small frame first: the buffers and the binding sets that name them at their first size.
	std::vector<TestInstance> const small = makeFrame(4);
	Image const before = Draw(renderer, camera, small, {}, lights);
	CHECK(PixelAt(before, right) == glm::ivec3(0));
	CHECK(renderer.GetPassInstanceCounts().ShadowViews == std::vector<uint32_t>{4});

	// 1,500 instances: more than the buffers first held, and an upload of more than 64 KiB.
	std::vector<TestInstance> const large = makeFrame(1500);
	Image const grown = Draw(renderer, camera, large, {}, lights);
	CHECK(renderer.GetStatistics().Batches == 3);
	CHECK(renderer.GetStatistics().ShadowDrawCalls == 1);
	CHECK(renderer.GetStatistics().Instances == 4500);
	CHECK(renderer.GetPassInstanceCounts().ShadowViews == std::vector<uint32_t>{1500});
	REQUIRE(renderer.GetFrameInstances().size() == 1500);
	// The instance beyond the first capacity is drawn where its own matrix puts it, and picked by its own ID.
	glm::ivec3 const color = PixelAt(grown, right);
	CHECK(color.r > 100);
	CHECK(color.r > color.b);
	CHECK(Pick(renderer, camera, large, right) == std::optional<uint32_t>(1201u));
	CHECK(Pick(renderer, camera, large, ProjectToPixel(camera, {-4.4f, -3.2f, 0.06f})) == std::optional<uint32_t>(1u));

	// Small frames draw and pick from the grown buffers.
	CHECK(Testing::CompareImages(Draw(renderer, camera, small, {}, lights), before, 0).MaxDifference == 0);
	CHECK(Pick(renderer, camera, small, ProjectToPixel(camera, {-4.4f + 0.13f, -3.2f, 0.06f})) == std::optional<uint32_t>(2u));
}

TEST_CASE("Instancing: each shadow view draws the casters that reach it, batched by mesh and material")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageSize, ImageSize);
	SceneRendererCamera const camera = MakeCamera({0.0f, 10.0f, 18.0f}, {0.0f, 0.0f, 0.0f});
	Ref<MeshSource> const cube = GetMesh(BuiltInAsset::CubeMesh);
	Ref<MeshSource> const sphere = GetMesh(BuiltInAsset::SphereMesh);
	Ref<MaterialAsset> const gray = MakeMaterial({0.5f, 0.5f, 0.5f, 1.0f});
	Ref<MaterialAsset> const red = MakeMaterial({0.8f, 0.2f, 0.2f, 1.0f});

	// Group A around x = -6 and group B around x = 6, eight cubes each, with a sphere in group A and two cubes there that
	// cast no shadow.
	std::vector<TestInstance> instances;
	for (uint32_t i = 0; i < 8; i++)
	{
		float const offset = static_cast<float>(i % 4) * 0.6f - 0.9f;
		float const depth = static_cast<float>(i / 4) * 0.6f - 0.3f;
		instances.push_back({cube, gray, Place({-6.0f + offset, 0.0f, depth}, 0.0f, glm::vec3(0.4f))});
		instances.push_back({cube, gray, Place({6.0f + offset, 0.0f, depth}, 0.0f, glm::vec3(0.4f))});
	}
	instances.push_back({sphere, red, Place({-6.0f, 1.0f, 1.2f}, 0.0f, glm::vec3(0.5f))});
	for (float const x : {-7.0f, -5.0f})
	{
		TestInstance& silent = instances.emplace_back(TestInstance{cube, gray, Place({x, 0.0f, -1.2f}, 0.0f, glm::vec3(0.4f))});
		silent.CastShadows = false;
	}
	uint32_t const inView = static_cast<uint32_t>(instances.size());

	// The camera's passes: one batch of the 18 gray cubes, one of the sphere.
	auto const checkCameraPasses = [&renderer, inView]()
	{
		SceneRendererStatistics const& statistics = renderer.GetStatistics();
		SceneRenderer::PassInstanceCounts const& passes = renderer.GetPassInstanceCounts();
		CHECK(passes.Opaque == inView);
		CHECK(statistics.Batches - statistics.ShadowDrawCalls == 2);
		uint32_t shadowInstances = 0;
		for (uint32_t const count : passes.ShadowViews)
		{
			shadowInstances += count;
		}
		// The definitions: every batch counts its instances once, and every draw of the frame is a batch or the tonemap.
		CHECK(statistics.Instances == passes.Opaque + shadowInstances);
		CHECK(statistics.DrawCalls == statistics.Batches + 1);
		CHECK(statistics.ShadowMapViews == passes.ShadowViews.size());
	};

	SUBCASE("a spot light over group A draws its nine casters in two batches")
	{
		TestLights lights;
		SpotLightSubmission& spot = lights.Spots.emplace_back();
		spot.Position = {-6.0f, 6.0f, 0.0f};
		spot.Direction = {0.0f, -1.0f, 0.0f};
		spot.Range = 8.0f;
		spot.OuterConeAngle = 50.0f;
		spot.CastShadows = true;
		(void)Draw(renderer, camera, instances, {}, lights);
		CHECK(renderer.GetPassInstanceCounts().ShadowViews == std::vector<uint32_t>{9});
		CHECK(renderer.GetStatistics().ShadowDrawCalls == 2);
		CHECK(renderer.GetStatistics().Instances == inView + 9);
		checkCameraPasses();
	}

	SUBCASE("a point light over group B draws its eight casters into each of its six faces, one batch each")
	{
		TestLights lights;
		PointLightSubmission& point = lights.Points.emplace_back();
		point.Position = {6.0f, 2.0f, 0.0f};
		point.Range = 5.0f;
		point.CastShadows = true;
		(void)Draw(renderer, camera, instances, {}, lights);
		CHECK(renderer.GetPassInstanceCounts().ShadowViews == std::vector<uint32_t>(6, 8u));
		CHECK(renderer.GetStatistics().ShadowDrawCalls == 6);
		checkCameraPasses();
	}

	SUBCASE("both lights: each view keeps to its own casters")
	{
		TestLights lights;
		SpotLightSubmission& spot = lights.Spots.emplace_back();
		spot.Position = {-6.0f, 6.0f, 0.0f};
		spot.Direction = {0.0f, -1.0f, 0.0f};
		spot.Range = 8.0f;
		spot.CastShadows = true;
		PointLightSubmission& point = lights.Points.emplace_back();
		point.Position = {6.0f, 2.0f, 0.0f};
		point.Range = 5.0f;
		point.CastShadows = true;
		(void)Draw(renderer, camera, instances, {}, lights);
		std::vector<uint32_t> expected = {9};
		expected.insert(expected.end(), 6, 8u);
		CHECK(renderer.GetPassInstanceCounts().ShadowViews == expected);
		CHECK(renderer.GetStatistics().ShadowDrawCalls == 2 + 6);
		checkCameraPasses();
	}

	SUBCASE("the cascades of a directional light draw what reaches them, at most one batch per mesh and material")
	{
		TestLights lights;
		DirectionalLightSubmission& sun = lights.Directional.emplace();
		sun.Direction = glm::normalize(glm::vec3(0.3f, -1.0f, -0.2f));
		sun.CastShadows = true;
		lights.CascadeCount = 3;
		(void)Draw(renderer, camera, instances, {}, lights);
		std::vector<uint32_t> const& views = renderer.GetPassInstanceCounts().ShadowViews;
		REQUIRE(views.size() == 3);
		// The casters are all in view, so the farthest cascade, which reaches the shadow distance, holds them all.
		CHECK(views.back() == 17);
		for (uint32_t const count : views)
		{
			CHECK(count <= 17);
		}
		CHECK(renderer.GetStatistics().ShadowDrawCalls <= 2 * 3);
		checkCameraPasses();
	}

	SUBCASE("a caster beyond the camera's distance is culled from its passes and still reaches the light")
	{
		// 26.7 meters from the camera, 8.5 from the spot light.
		instances.push_back({cube, gray, Place({-6.0f, 0.0f, -6.0f}, 0.0f, glm::vec3(0.4f))});
		SceneRendererCamera limited = camera;
		limited.MaxDistance = 25.0f;
		TestLights lights;
		SpotLightSubmission& spot = lights.Spots.emplace_back();
		spot.Position = {-6.0f, 6.0f, 0.0f};
		spot.Direction = {0.0f, -1.0f, 0.0f};
		spot.Range = 9.5f;
		spot.CastShadows = true;
		(void)Draw(renderer, limited, instances, {}, lights);
		CHECK(renderer.GetStatistics().Culled == 1);
		CHECK(renderer.GetPassInstanceCounts().ShadowViews == std::vector<uint32_t>{10});
		CHECK(renderer.GetStatistics().ShadowDrawCalls == 2);
		checkCameraPasses();
	}
}

TEST_CASE("Instancing: batched shadows cast what one draw per caster casts")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageSize, ImageSize);
	SceneRendererCamera const camera = MakeCamera({0.0f, 7.0f, 8.0f}, {0.0f, 0.0f, 0.0f});
	Ref<MeshSource> const cube = GetMesh(BuiltInAsset::CubeMesh);
	Ref<MeshSource> const plane = GetMesh(BuiltInAsset::PlaneMesh);

	// Lit, not emissive: the floor shows the shadows of 16 turned cubes floating above it.
	MaterialData lit;
	lit.BaseColor = {0.7f, 0.7f, 0.7f, 1.0f};
	lit.Roughness = 0.9f;
	Ref<MaterialAsset> const floor = CreateRef<MaterialAsset>(lit);
	Ref<MaterialAsset> const shared = CreateRef<MaterialAsset>(lit);
	std::vector<TestInstance> instances = {{plane, floor, Place({0.0f, 0.0f, 0.0f}, 0.0f, glm::vec3(10.0f))}};
	for (uint32_t i = 0; i < 16; i++)
	{
		glm::vec3 const position(static_cast<float>(i % 4) * 1.3f - 1.95f, 1.0f + 0.1f * static_cast<float>(i % 3),
		                         static_cast<float>(i / 4) * 1.3f - 1.95f);
		instances.push_back({cube, shared, Place(position, static_cast<float>(i) * 11.0f, glm::vec3(0.5f, 0.3f, 0.7f))});
	}

	for (bool const directional : {true, false})
	{
		CAPTURE(directional);
		TestLights lights;
		if (directional)
		{
			DirectionalLightSubmission& sun = lights.Directional.emplace();
			sun.Direction = glm::normalize(glm::vec3(0.4f, -1.0f, -0.3f));
			sun.Intensity = 3.0f;
			sun.CastShadows = true;
		}
		else
		{
			SpotLightSubmission& spot = lights.Spots.emplace_back();
			spot.Position = {0.5f, 6.0f, 0.5f};
			spot.Direction = {0.0f, -1.0f, 0.0f};
			spot.Intensity = 200.0f;
			spot.Range = 12.0f;
			spot.OuterConeAngle = 55.0f;
			spot.CastShadows = true;
		}

		Image const batched = Draw(renderer, camera, instances, {}, lights);
		uint32_t const views = renderer.GetStatistics().ShadowMapViews;
		// The floor and the cubes: two batches per shadow view at most.
		CHECK(renderer.GetStatistics().ShadowDrawCalls <= 2 * views);

		// A material of its own per cube: one batch per cube in every view that it reaches, and the same image.
		std::vector<TestInstance> separate = instances;
		for (size_t i = 1; i < separate.size(); i++)
		{
			separate[i].Material = CreateRef<MaterialAsset>(lit);
		}
		Image const unbatched = Draw(renderer, camera, separate, {}, lights);
		CHECK(renderer.GetStatistics().ShadowDrawCalls > 2 * views);
		CHECK(Testing::CompareImages(batched, unbatched, 0).MaxDifference == 0);

		// Without shadows the floor is lit where the cubes shaded it: the shadows were drawn.
		std::vector<TestInstance> uncast = instances;
		for (size_t i = 1; i < uncast.size(); i++)
		{
			uncast[i].CastShadows = false;
		}
		CHECK(Testing::CompareImages(batched, Draw(renderer, camera, uncast, {}, lights), 16).OutlierFraction > 0.01);
	}
}

TEST_CASE("Instancing: the draw count holds every draw of every pass, the fullscreen ones and the quads' included")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageSize, ImageSize);
	SceneRendererCamera const camera = MakeCamera({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f});
	SceneRendererSettings settings;
	settings.Bloom = false;
	settings.AmbientOcclusion = false;
	settings.FXAA = false;
	auto const render = [&renderer, &camera, &settings](auto const& submit)
	{
		renderer.BeginScene(camera, settings);
		submit();
		renderer.EndScene();
		return renderer.GetStatistics();
	};
	auto const nothing = []() {};

	// An empty frame still tonemaps; FXAA is one more draw.
	SceneRendererStatistics statistics = render(nothing);
	CHECK(statistics.DrawCalls == 1);
	CHECK(statistics.Batches == 0);
	CHECK(statistics.Instances == 0);
	settings.FXAA = true;
	CHECK(render(nothing).DrawCalls == 2);

	// With entity IDs: a selected cube is a batch in the opaque and the entity-ID pass, the grid one draw, the lines two
	// (their layer and its composite), the outline one, and FXAA and the tonemap one each.
	renderer.SetEntityIdsEnabled(true);
	Ref<MeshSource> const cube = GetMesh(BuiltInAsset::CubeMesh);
	Ref<MaterialAsset> const material = MakeMaterial({0.4f, 0.4f, 0.8f, 1.0f});
	SceneRendererOverlays overlays;
	overlays.Grid.Enabled = true;
	auto const editorFrame = [&renderer, &cube, &material, &overlays]()
	{
		Ref<MaterialAsset> const materials[] = {material};
		renderer.SubmitMesh(cube, materials, glm::mat4(1.0f), true, 7, true);
		renderer.SubmitLine({-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f});
		renderer.SetOverlays(overlays);
	};
	statistics = render(editorFrame);
	CHECK(statistics.Batches == 2);
	CHECK(statistics.Instances == 2);
	CHECK(statistics.DrawCalls == 2 + 1 + 1 + 2 + 1 + 1);

	// A world sprite and a screen-space sprite: each a quad batch where it is drawn and one in the entity-ID pass.
	auto const withSprites = [&renderer, &editorFrame]()
	{
		editorFrame();
		SpriteSubmission world;
		world.Transform = glm::translate(glm::mat4(1.0f), {1.5f, 0.0f, 0.0f});
		world.PickingId = 8;
		renderer.SubmitSprite(world);
		SpriteSubmission screen;
		screen.ScreenSpace = true;
		screen.Transform = glm::scale(glm::translate(glm::mat4(1.0f), {20.0f, 20.0f, 0.0f}), {16.0f, 16.0f, 1.0f});
		screen.PickingId = 9;
		renderer.SubmitSprite(screen);
	};
	statistics = render(withSprites);
	CHECK(statistics.Quads == 2);
	CHECK(statistics.Batches == 2);
	CHECK(statistics.DrawCalls == 2 + 1 + 1 + 2 + 1 + 1 + 2 + 2);
}

TEST_CASE("Instancing: a scene of instanced meshes, cut-outs, leaves, glass, a scaled and a mirrored group matches the golden image")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	constexpr uint32_t Width = 320;
	constexpr uint32_t Height = 180;
	auto const addMaterial = [](MaterialData const& data, char const* name)
	{
		return AssetManager::AddMemoryAsset(CreateRef<MaterialAsset>(data), name);
	};
	MaterialData stone;
	stone.BaseColor = {0.62f, 0.6f, 0.56f, 1.0f};
	stone.Roughness = 0.9f;
	MaterialData brick = stone;
	brick.BaseColor = {0.7f, 0.32f, 0.22f, 1.0f};
	// Without an environment a metal reflects nothing: the spheres are a glossy dielectric.
	MaterialData metal;
	metal.BaseColor = {0.85f, 0.7f, 0.3f, 1.0f};
	metal.Roughness = 0.35f;

	// A fence of wide bars: an alpha mask of four columns, two of them cut out, each several pixels wide in the image.
	Image bars(8, 8, 4);
	for (uint32_t y = 0; y < 8; y++)
	{
		for (uint32_t x = 0; x < 8; x++)
		{
			uint8_t* pixel = bars.GetPixel(x, y);
			pixel[0] = 230;
			pixel[1] = 230;
			pixel[2] = 230;
			pixel[3] = (x / 2) % 2 == 0 ? 255 : 0;
		}
	}
	Result<Ref<TextureAsset>> barsTexture = TextureAsset::CreateFromImage(std::move(bars));
	REQUIRE(barsTexture.IsOk());
	MaterialData fence;
	fence.BaseColor = {0.3f, 0.35f, 0.4f, 1.0f};
	fence.BaseColorTexture = AssetManager::AddMemoryAsset(barsTexture.GetValue(), "Fence bars");
	fence.AlphaMode = MaterialAlphaMode::Mask;
	MaterialData leaf;
	leaf.BaseColor = {0.25f, 0.55f, 0.2f, 1.0f};
	leaf.Roughness = 0.7f;
	leaf.DoubleSided = true;
	MaterialData glass;
	glass.BaseColor = {0.3f, 0.6f, 0.9f, 0.45f};
	glass.Roughness = 0.1f;
	glass.AlphaMode = MaterialAlphaMode::Blend;

	Scene scene("Instancing");
	Testing::AddTestMesh(scene, "Ground", BuiltInAsset::PlaneMesh, addMaterial(stone, "Ground"), {0.0f, 0.0f, 0.0f}, glm::vec3(16.0f));
	// A row of cubes of one mesh and material: one batch in each pass that draws them.
	AssetHandle const brickMaterial = addMaterial(brick, "Brick");
	for (int i = 0; i < 9; i++)
	{
		Entity block =
			Testing::AddTestMesh(scene, "Block", BuiltInAsset::CubeMesh, brickMaterial, {static_cast<float>(i) * 0.9f - 3.6f, 0.3f, -2.5f},
		                         {0.6f, 0.6f + 0.08f * static_cast<float>(i), 0.6f});
		block.GetComponent<TransformComponent>().SetRotationEuler({0.0f, static_cast<float>(i) * 10.0f, 0.0f});
	}
	// A turned and unevenly scaled group of spheres: their world matrices come from the cache through the parent, their
	// normals from the instance's normal basis.
	Entity group = scene.CreateEntity("Group");
	TransformComponent& groupTransform = group.GetComponent<TransformComponent>();
	groupTransform.Translation = {-2.0f, 0.0f, 0.5f};
	groupTransform.SetRotationEuler({0.0f, 35.0f, 0.0f});
	groupTransform.Scale = {1.3f, 0.8f, 1.1f};
	AssetHandle const metalMaterial = addMaterial(metal, "Metal");
	for (int i = 0; i < 6; i++)
	{
		Entity ball = scene.CreateEntity("Ball", group);
		MeshComponent& mesh = ball.AddComponent<MeshComponent>();
		mesh.Mesh = GetBuiltInHandle(BuiltInAsset::SphereMesh);
		mesh.Materials = {metalMaterial};
		TransformComponent& transform = ball.GetComponent<TransformComponent>();
		transform.Translation = {static_cast<float>(i % 3) * 0.9f - 0.9f, 0.45f + 0.25f * static_cast<float>(i / 3),
		                         static_cast<float>(i / 3) * 0.9f};
		transform.Scale = glm::vec3(0.7f);
	}
	// A mirrored group of leaning cones, a batch apart from unmirrored cones: drawn with the pipelines whose front faces are
	// clockwise, they show their outer faces.
	Entity mirrored = scene.CreateEntity("Mirrored");
	TransformComponent& mirroredTransform = mirrored.GetComponent<TransformComponent>();
	mirroredTransform.Translation = {-2.3f, 0.0f, 2.6f};
	mirroredTransform.SetRotationEuler({0.0f, -20.0f, 0.0f});
	mirroredTransform.Scale = {-1.0f, 1.0f, 1.0f};
	for (int i = 0; i < 3; i++)
	{
		Entity cone = scene.CreateEntity("Cone", mirrored);
		MeshComponent& mesh = cone.AddComponent<MeshComponent>();
		mesh.Mesh = GetBuiltInHandle(BuiltInAsset::ConeMesh);
		mesh.Materials = {brickMaterial};
		TransformComponent& transform = cone.GetComponent<TransformComponent>();
		transform.Translation = {static_cast<float>(i) * -0.7f, 0.35f, 0.0f};
		transform.SetRotationEuler({0.0f, 0.0f, -25.0f});
		transform.Scale = glm::vec3(0.5f);
	}
	// A fence of cut-out panels, leaves seen from both sides, and two glass panes.
	AssetHandle const fenceMaterial = addMaterial(fence, "Fence");
	for (int i = 0; i < 4; i++)
	{
		Entity panel = Testing::AddTestMesh(scene, "Fence", BuiltInAsset::QuadMesh, fenceMaterial,
		                                    {1.0f + static_cast<float>(i) * 1.1f, 0.55f, -0.8f}, {1.1f, 1.1f, 1.0f});
		panel.GetComponent<TransformComponent>().SetRotationEuler({0.0f, -20.0f, 0.0f});
	}
	AssetHandle const leafMaterial = addMaterial(leaf, "Leaf");
	for (int i = 0; i < 5; i++)
	{
		Entity blade = Testing::AddTestMesh(scene, "Leaf", BuiltInAsset::QuadMesh, leafMaterial,
		                                    {2.2f + static_cast<float>(i) * 0.5f, 0.45f, 1.4f}, {0.35f, 0.9f, 1.0f});
		blade.GetComponent<TransformComponent>().SetRotationEuler({0.0f, static_cast<float>(i) * 70.0f, 0.0f});
	}
	AssetHandle const glassMaterial = addMaterial(glass, "Glass");
	for (int i = 0; i < 2; i++)
	{
		Testing::AddTestMesh(scene, "Glass", BuiltInAsset::QuadMesh, glassMaterial,
		                     {-0.4f + static_cast<float>(i) * 0.7f, 0.6f, 1.6f + static_cast<float>(i) * 0.4f}, {1.0f, 1.2f, 1.0f});
	}
	Entity sun = scene.CreateEntity("Sun");
	sun.GetComponent<TransformComponent>().SetRotationEuler({-50.0f, -35.0f, 0.0f});
	DirectionalLightComponent& light = sun.AddComponent<DirectionalLightComponent>();
	light.Intensity = 3.0f;
	light.CastShadows = true;
	scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().AmbientColor = {0.2f, 0.24f, 0.3f};

	SceneRenderer renderer;
	renderer.SetViewportSize(Width, Height);
	SceneRendererCamera const camera = Testing::MakeTestCamera({0.4f, 3.4f, 6.2f}, {0.3f, 0.2f, -0.2f}, Width, Height);
	Image const image = Testing::RenderToImage(scene, camera, renderer);
	// The ground, the bricks, the spheres, the mirrored cones, the fence, the leaves: one camera batch each; the panes one
	// each.
	SceneRenderer::PassInstanceCounts const& passes = renderer.GetPassInstanceCounts();
	CHECK(passes.Opaque == 1 + 9 + 6 + 3 + 4 + 5);
	CHECK(passes.Transparent == 2);
	CHECK(renderer.GetStatistics().Batches - renderer.GetStatistics().ShadowDrawCalls == 6 + 2);
	CHECK(renderer.GetStatistics().ShadowMapViews == 4);
	CHECK(renderer.GetStatistics().ShadowDrawCalls <= 6 * 4);
	Testing::CheckGoldenImage("Instancing", image);
}
