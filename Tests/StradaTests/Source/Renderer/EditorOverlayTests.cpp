#include "Renderer/RenderTestUtilities.h"

#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/RHI/GraphicsDevice.h"

#include <doctest/doctest.h>

#include <optional>
#include <vector>

using namespace Strada;

namespace
{
	constexpr uint32_t ImageSize = 96;

	SceneRendererCamera MakeCamera(glm::vec3 const& position, glm::vec3 const& target)
	{
		return Testing::MakeTestCamera(position, target, ImageSize, ImageSize);
	}

	glm::uvec2 ProjectToPixel(SceneRendererCamera const& camera, glm::vec3 const& point)
	{
		glm::vec4 const clip = camera.Projection * camera.View * glm::vec4(point, 1.0f);
		glm::vec2 const ndc = glm::vec2(clip) / clip.w;
		// D3D conventions: NDC +Y is up, pixel rows go down.
		return glm::uvec2(static_cast<uint32_t>((ndc.x * 0.5f + 0.5f) * ImageSize),
		                  static_cast<uint32_t>((0.5f - ndc.y * 0.5f) * ImageSize));
	}

	glm::ivec3 Pixel(Image const& image, glm::uvec2 const& pixel)
	{
		uint8_t const* data = image.GetPixel(pixel.x, pixel.y);
		return {data[0], data[1], data[2]};
	}

	Ref<MaterialAsset> MakeMaterial(glm::vec4 const& baseColor, MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque)
	{
		MaterialData data;
		data.BaseColor = baseColor;
		data.AlphaMode = alphaMode;
		// Emissive so the surfaces are visible without lights.
		data.EmissiveColor = glm::vec3(baseColor);
		data.EmissiveIntensity = 1.0f;
		return CreateRef<MaterialAsset>(data);
	}

	struct MeshSubmission
	{
		BuiltInAsset Mesh = BuiltInAsset::CubeMesh;
		Ref<MaterialAsset> Material;
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Scale = glm::vec3(1.0f);
		uint32_t PickingId = 0;
		bool Selected = false;
	};

	struct LineSubmission
	{
		glm::vec3 From;
		glm::vec3 To;
		glm::vec4 Color;
		bool DepthTest = true;
	};

	struct Frame
	{
		std::vector<MeshSubmission> Meshes;
		std::vector<LineSubmission> Lines;
		SceneRendererOverlays Overlays;
	};

	Image Render(SceneRenderer& renderer, SceneRendererCamera const& camera, Frame const& frame)
	{
		SceneRendererSettings settings;
		// Keep the images free of post-processing that would blur the comparisons.
		settings.Bloom = false;
		settings.FXAA = false;
		settings.AmbientOcclusion = false;
		settings.Dithering = false;
		renderer.BeginScene(camera, settings);
		for (MeshSubmission const& mesh : frame.Meshes)
		{
			Ref<MeshSource> const source = AssetManager::GetAsset<MeshSource>(GetBuiltInHandle(mesh.Mesh));
			REQUIRE(source);
			Ref<MaterialAsset> const materials[] = {mesh.Material};
			glm::mat4 const transform = glm::scale(glm::translate(glm::mat4(1.0f), mesh.Position), mesh.Scale);
			renderer.SubmitMesh(source, materials, transform, false, mesh.PickingId, mesh.Selected);
		}
		for (LineSubmission const& line : frame.Lines)
		{
			renderer.SubmitLine(line.From, line.To, line.Color, line.DepthTest);
		}
		renderer.SetOverlays(frame.Overlays);
		renderer.EndScene();
		Result<Image> image = ReadbackTexture(renderer.GetFinalImage());
		REQUIRE(image.IsOk());
		return std::move(image.GetValue());
	}

	std::optional<uint32_t> Pick(SceneRenderer& renderer, SceneRendererCamera const& camera, Frame const& frame, glm::uvec2 const& pixel)
	{
		renderer.RequestPick(pixel.x, pixel.y);
		CHECK(renderer.IsPickPending());
		Render(renderer, camera, frame);
		GraphicsDevice::GetDevice()->waitForIdle();
		std::optional<uint32_t> const result = renderer.TakePickResult();
		// Results are consumed.
		CHECK_FALSE(renderer.TakePickResult().has_value());
		CHECK_FALSE(renderer.IsPickPending());
		return result;
	}
}

TEST_CASE("SceneRenderer: picking reports the nearest pickable surface under a pixel")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageSize, ImageSize);
	renderer.SetEntityIdsEnabled(true);
	SceneRendererCamera const camera = MakeCamera({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f});

	Frame frame;
	frame.Meshes.push_back({BuiltInAsset::CubeMesh, MakeMaterial({0.8f, 0.2f, 0.2f, 1.0f}), {-1.2f, 0.0f, 0.0f}, glm::vec3(1.0f), 7});
	frame.Meshes.push_back({BuiltInAsset::CubeMesh, MakeMaterial({0.2f, 0.8f, 0.2f, 1.0f}), {1.2f, 0.0f, 0.0f}, glm::vec3(1.0f), 9});
	// A transparent quad in front of the right cube is picked instead of it.
	frame.Meshes.push_back({BuiltInAsset::QuadMesh,
	                        MakeMaterial({0.2f, 0.2f, 0.8f, 0.3f}, MaterialAlphaMode::Blend),
	                        {1.2f, 0.0f, 1.0f},
	                        glm::vec3(0.6f),
	                        11});
	// A fully cut-out masked quad in front of the left cube is not pickable.
	frame.Meshes.push_back({BuiltInAsset::QuadMesh,
	                        MakeMaterial({1.0f, 1.0f, 1.0f, 0.0f}, MaterialAlphaMode::Mask),
	                        {-1.2f, 0.0f, 1.0f},
	                        glm::vec3(0.6f),
	                        13});
	// Not pickable, but it still hides the pickable cube behind it.
	frame.Meshes.push_back({BuiltInAsset::CubeMesh, MakeMaterial({0.5f, 0.5f, 0.5f, 1.0f}), {0.0f, -1.5f, 0.0f}, glm::vec3(0.8f), 0});
	frame.Meshes.push_back({BuiltInAsset::CubeMesh, MakeMaterial({0.5f, 0.5f, 0.5f, 1.0f}), {0.0f, -1.5f, -2.0f}, glm::vec3(1.0f), 21});

	CHECK(Pick(renderer, camera, frame, ProjectToPixel(camera, {-1.2f, 0.0f, 0.5f})) == std::optional<uint32_t>(7u));
	CHECK(Pick(renderer, camera, frame, ProjectToPixel(camera, {1.2f, 0.0f, 1.0f})) == std::optional<uint32_t>(11u));
	CHECK(Pick(renderer, camera, frame, ProjectToPixel(camera, {1.2f, 0.4f, 0.5f})) == std::optional<uint32_t>(9u));
	CHECK(Pick(renderer, camera, frame, ProjectToPixel(camera, {0.0f, -1.5f, 0.4f})) == std::optional<uint32_t>(0u));
	// Only the top edge of the cube behind is visible above the non-pickable one.
	CHECK(Pick(renderer, camera, frame, ProjectToPixel(camera, {0.0f, -1.05f, -1.5f})) == std::optional<uint32_t>(21u));
	CHECK(Pick(renderer, camera, frame, {2, 2}) == std::optional<uint32_t>(0u));
	// Outside the viewport.
	CHECK(Pick(renderer, camera, frame, {ImageSize + 5, 3}) == std::optional<uint32_t>(0u));

	// The selection bit does not leak into results.
	frame.Meshes[0].Selected = true;
	CHECK(Pick(renderer, camera, frame, ProjectToPixel(camera, {-1.2f, 0.0f, 0.5f})) == std::optional<uint32_t>(7u));

	// Without entity IDs, requests complete with 0.
	renderer.SetEntityIdsEnabled(false);
	CHECK(Pick(renderer, camera, frame, ProjectToPixel(camera, {-1.2f, 0.0f, 0.5f})) == std::optional<uint32_t>(0u));
	CHECK_FALSE(renderer.TakePickResult().has_value());
}

TEST_CASE("SceneRenderer: the selection outline surrounds selected meshes")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageSize, ImageSize);
	renderer.SetEntityIdsEnabled(true);
	SceneRendererCamera const camera = MakeCamera({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f});

	Frame frame;
	frame.Meshes.push_back({BuiltInAsset::CubeMesh, MakeMaterial({0.2f, 0.2f, 0.8f, 1.0f}), {0.0f, 0.0f, 0.0f}, glm::vec3(1.0f), 3});
	frame.Overlays.SelectionColor = {1.0f, 0.5f, 0.0f, 1.0f};
	frame.Overlays.SelectionOutlineWidth = 2;
	Image const unselected = Render(renderer, camera, frame);
	frame.Meshes[0].Selected = true;
	Image const selected = Render(renderer, camera, frame);

	glm::uvec2 const center = ProjectToPixel(camera, {0.0f, 0.0f, 0.5f});
	CHECK(Pixel(selected, center) == Pixel(unselected, center));
	glm::uvec2 const corner(2, 2);
	CHECK(Pixel(selected, corner) == Pixel(unselected, corner));

	// The outline is drawn over the black background only (not over the cube): every changed pixel has the orange hue
	// (scaled down at the anti-aliased corners).
	size_t changed = 0;
	for (uint32_t y = 0; y < ImageSize; y++)
	{
		for (uint32_t x = 0; x < ImageSize; x++)
		{
			glm::ivec3 const before = Pixel(unselected, {x, y});
			glm::ivec3 const after = Pixel(selected, {x, y});
			if (before == after)
			{
				continue;
			}
			changed++;
			CHECK(before == glm::ivec3(0));
			CHECK(after.r >= after.g);
			CHECK(after.g >= after.b);
		}
	}
	CHECK(changed > 4 * ImageSize / 10);

	// Right of the silhouette on the center row the outline is solid.
	glm::uvec2 const edge = ProjectToPixel(camera, {0.5f, 0.0f, 0.5f});
	bool solid = false;
	for (uint32_t x = edge.x; x <= edge.x + 3; x++)
	{
		glm::ivec3 const color = Pixel(selected, {x, center.y});
		solid = solid || (color.r > 240 && color.g > 110 && color.g < 150 && color.b < 20);
	}
	CHECK(solid);

	// Without entity IDs there is no outline.
	renderer.SetEntityIdsEnabled(false);
	CHECK(Testing::CompareImages(Render(renderer, camera, frame), unselected, 0).MaxDifference == 0);
}

TEST_CASE("SceneRenderer: the grid lies on the ground and hides behind surfaces")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageSize, ImageSize);
	// Looking 10.6 degrees down with a 50 degree field of view: the top rows see the sky.
	SceneRendererCamera const camera = MakeCamera({0.0f, 2.0f, 8.0f}, {0.0f, 0.5f, 0.0f});

	Frame frame;
	Image const plain = Render(renderer, camera, frame);
	frame.Overlays.Grid.Enabled = true;
	Image const grid = Render(renderer, camera, frame);

	// The axes cross at the origin; with the default colors the crossing is reddish or bluish.
	glm::ivec3 const origin = Pixel(grid, ProjectToPixel(camera, {0.0f, 0.0f, 0.0f}));
	CHECK(origin.r + origin.b > 150);
	// The top rows look above the horizon: the sky is untouched.
	for (uint32_t x = 0; x < ImageSize; x += 7)
	{
		CHECK(Pixel(grid, {x, 1}) == Pixel(plain, {x, 1}));
	}
	CHECK(Testing::CompareImages(grid, plain, 8).OutlierFraction > 0.02);

	// A cube standing on the origin hides the grid behind its front face.
	frame.Meshes.push_back({BuiltInAsset::CubeMesh, MakeMaterial({0.6f, 0.6f, 0.6f, 1.0f}), {0.0f, 0.5f, 0.0f}, glm::vec3(1.0f), 0});
	Image const covered = Render(renderer, camera, frame);
	frame.Overlays.Grid.Enabled = false;
	Image const coveredPlain = Render(renderer, camera, frame);
	glm::uvec2 const front = ProjectToPixel(camera, {0.0f, 0.5f, 0.5f});
	CHECK(Pixel(covered, front) == Pixel(coveredPlain, front));
}

TEST_CASE("SceneRenderer: debug lines respect depth testing")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageSize, ImageSize);
	SceneRendererCamera const camera = MakeCamera({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f});

	Frame frame;
	frame.Meshes.push_back({BuiltInAsset::CubeMesh, MakeMaterial({0.3f, 0.3f, 0.3f, 1.0f}), {0.0f, 0.0f, 0.0f}, glm::vec3(1.0f), 0});
	// Through the middle of the cube, from left to right.
	frame.Lines.push_back({{-3.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, true});
	Image const tested = Render(renderer, camera, frame);
	frame.Lines[0].DepthTest = false;
	Image const onTop = Render(renderer, camera, frame);

	glm::uvec2 const center = ProjectToPixel(camera, {0.0f, 0.0f, 0.0f});
	glm::uvec2 const outside = ProjectToPixel(camera, {2.0f, 0.0f, 0.0f});
	auto const isGreen = [](glm::ivec3 const& color)
	{
		return color.g > 200 && color.r < 80 && color.b < 80;
	};
	// Lines are one pixel wide; check the pixel row and its neighbor in case of rasterization rounding.
	auto const lineVisible = [&](Image const& image, glm::uvec2 const& pixel)
	{
		return isGreen(Pixel(image, pixel)) || isGreen(Pixel(image, {pixel.x, pixel.y - 1})) ||
		       isGreen(Pixel(image, {pixel.x, pixel.y + 1}));
	};
	CHECK(lineVisible(tested, outside));
	CHECK_FALSE(lineVisible(tested, center));
	CHECK(lineVisible(onTop, outside));
	CHECK(lineVisible(onTop, center));
}
