#include "Renderer/FontTestUtilities.h"
#include "Renderer/RenderTestUtilities.h"

#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Asset/TextureAsset.h"
#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/Renderer/TextLayout.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <optional>
#include <vector>

using namespace Strada;

namespace
{
	constexpr uint32_t ImageSize = 96;

	SceneRendererCamera MakeCamera()
	{
		return Testing::MakeTestCamera({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, ImageSize, ImageSize);
	}

	glm::uvec2 ProjectToPixel(SceneRendererCamera const& camera, glm::vec3 const& point)
	{
		glm::vec4 const clip = camera.Projection * camera.View * glm::vec4(point, 1.0f);
		glm::vec2 const ndc = glm::vec2(clip) / clip.w;
		return glm::uvec2(static_cast<uint32_t>((ndc.x * 0.5f + 0.5f) * ImageSize),
		                  static_cast<uint32_t>((0.5f - ndc.y * 0.5f) * ImageSize));
	}

	glm::ivec3 Pixel(Image const& image, glm::uvec2 const& pixel)
	{
		uint8_t const* data = image.GetPixel(pixel.x, pixel.y);
		return {data[0], data[1], data[2]};
	}

	bool IsLit(Image const& image, uint32_t x, uint32_t y)
	{
		glm::ivec3 const color = Pixel(image, {x, y});
		return color.r + color.g + color.b > 30;
	}

	glm::mat4 Placement(glm::vec3 const& translation, glm::vec3 const& scale, float rotationDegrees = 0.0f)
	{
		glm::mat4 const rotation = glm::rotate(glm::mat4(1.0f), glm::radians(rotationDegrees), glm::vec3(0.0f, 0.0f, 1.0f));
		return glm::translate(glm::mat4(1.0f), translation) * rotation * glm::scale(glm::mat4(1.0f), scale);
	}

	struct Frame
	{
		std::vector<SpriteSubmission> Sprites;
		std::vector<TextSubmission> Texts;
		// An emissive cube (in front of the sprites when placed at z > 0).
		std::optional<glm::vec3> Cube;
	};

	void Submit(SceneRenderer& renderer, Frame const& frame)
	{
		SceneRendererSettings settings;
		settings.Bloom = false;
		settings.FXAA = false;
		settings.AmbientOcclusion = false;
		settings.Dithering = false;
		renderer.BeginScene(MakeCamera(), settings);
		if (frame.Cube)
		{
			MaterialData data;
			data.BaseColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
			data.EmissiveColor = glm::vec3(0.0f, 0.0f, 1.0f);
			data.EmissiveIntensity = 1.0f;
			Ref<MaterialAsset> const materials[] = {CreateRef<MaterialAsset>(data)};
			Ref<MeshSource> const cube = AssetManager::GetAsset<MeshSource>(GetBuiltInHandle(BuiltInAsset::CubeMesh));
			REQUIRE(cube);
			renderer.SubmitMesh(cube, materials, glm::scale(glm::translate(glm::mat4(1.0f), *frame.Cube), glm::vec3(0.5f)), false);
		}
		for (SpriteSubmission const& sprite : frame.Sprites)
		{
			renderer.SubmitSprite(sprite);
		}
		for (TextSubmission const& text : frame.Texts)
		{
			renderer.SubmitText(text);
		}
		renderer.EndScene();
	}

	Image Render(SceneRenderer& renderer, Frame const& frame)
	{
		Submit(renderer, frame);
		Result<Image> image = ReadbackTexture(renderer.GetFinalImage());
		REQUIRE(image.IsOk());
		return std::move(image.GetValue());
	}

	std::optional<uint32_t> Pick(SceneRenderer& renderer, Frame const& frame, glm::uvec2 const& pixel)
	{
		renderer.RequestPick(pixel.x, pixel.y);
		Submit(renderer, frame);
		GraphicsDevice::GetDevice()->waitForIdle();
		return renderer.TakePickResult();
	}

	// A 2x1 texture: red on the left, blue on the right.
	AssetHandle MakeTwoColorTexture()
	{
		Image image(2, 1, 4);
		uint8_t* left = image.GetPixel(0, 0);
		left[0] = 255;
		left[3] = 255;
		uint8_t* right = image.GetPixel(1, 0);
		right[2] = 255;
		right[3] = 255;
		Result<Ref<TextureAsset>> texture = TextureAsset::CreateFromImage(std::move(image));
		REQUIRE(texture.IsOk());
		return AssetManager::AddMemoryAsset(texture.GetValue(), "Two colors");
	}

	// Pixels covered by text, as a bounding box (empty optional when nothing is lit).
	std::optional<std::pair<glm::uvec2, glm::uvec2>> LitBounds(Image const& image)
	{
		std::optional<std::pair<glm::uvec2, glm::uvec2>> bounds;
		for (uint32_t y = 0; y < ImageSize; y++)
		{
			for (uint32_t x = 0; x < ImageSize; x++)
			{
				if (!IsLit(image, x, y))
				{
					continue;
				}
				if (!bounds)
				{
					bounds.emplace(glm::uvec2(x, y), glm::uvec2(x, y));
				}
				bounds->first = glm::min(bounds->first, glm::uvec2(x, y));
				bounds->second = glm::max(bounds->second, glm::uvec2(x, y));
			}
		}
		return bounds;
	}
}

TEST_CASE("SceneRenderer: world-space sprites are unlit, textured, blended and hidden behind closer surfaces")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageSize, ImageSize);
	SceneRendererCamera const camera = MakeCamera();

	Frame frame;
	SpriteSubmission sprite;
	sprite.Transform = Placement({0.0f, 0.0f, 0.0f}, {3.0f, 3.0f, 1.0f});
	sprite.Color = {1.0f, 0.0f, 0.0f, 1.0f};
	frame.Sprites.push_back(sprite);
	Image const opaque = Render(renderer, frame);
	glm::ivec3 const center = Pixel(opaque, ProjectToPixel(camera, {0.0f, 0.0f, 0.0f}));
	CHECK(center.r > 150);
	CHECK(center.g < 10);
	CHECK(center.b < 10);
	CHECK(Pixel(opaque, {2, 2}) == glm::ivec3(0));
	CHECK(renderer.GetStatistics().Quads == 1);

	// Half transparent over the black background.
	frame.Sprites[0].Color.a = 0.5f;
	glm::ivec3 const blended = Pixel(Render(renderer, frame), ProjectToPixel(camera, {0.0f, 0.0f, 0.0f}));
	CHECK(blended.r > 30);
	CHECK(blended.r < center.r);

	// Textured: red on the left half, blue on the right; the closer cube covers the sprite.
	frame.Sprites[0].Color = glm::vec4(1.0f);
	frame.Sprites[0].Texture = MakeTwoColorTexture();
	frame.Cube = glm::vec3(0.0f, 1.0f, 1.0f);
	Image const textured = Render(renderer, frame);
	// Probed at the texel centers: filtering (which wraps around for tiling) mixes the two colors near their edges.
	glm::ivec3 const left = Pixel(textured, ProjectToPixel(camera, {-0.75f, -0.5f, 0.0f}));
	glm::ivec3 const right = Pixel(textured, ProjectToPixel(camera, {0.75f, -0.5f, 0.0f}));
	CHECK(left.r > left.b + 100);
	CHECK(right.b > right.r + 100);
	glm::ivec3 const covered = Pixel(textured, ProjectToPixel(camera, {0.0f, 1.0f, 1.25f}));
	CHECK(covered.b > 150);
	CHECK(covered.r < 10);
}

TEST_CASE("SceneRenderer: screen-space sprites are placed in viewport coordinates and layered")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageSize, ImageSize);

	Frame frame;
	SpriteSubmission green;
	green.ScreenSpace = true;
	green.Transform = Placement({0.25f, 0.25f, 0.0f}, {20.0f, 20.0f, 1.0f});
	green.Color = {0.0f, 1.0f, 0.0f, 1.0f};
	frame.Sprites.push_back(green);
	Image const image = Render(renderer, frame);
	// Drawn on the 8-bit image after tonemapping: exact colors.
	CHECK(Pixel(image, {24, 24}) == glm::ivec3(0, 255, 0));
	CHECK(Pixel(image, {24 + 8, 24 - 8}) == glm::ivec3(0, 255, 0));
	CHECK(Pixel(image, {24 + 12, 24}) == glm::ivec3(0));
	CHECK(Pixel(image, {72, 72}) == glm::ivec3(0));

	// A higher layer (translation z) draws on top whatever the submission order.
	SpriteSubmission red = green;
	red.Color = {1.0f, 0.0f, 0.0f, 1.0f};
	red.Transform = Placement({0.25f, 0.25f, 1.0f}, {10.0f, 10.0f, 1.0f});
	frame.Sprites.insert(frame.Sprites.begin(), red);
	Image const layered = Render(renderer, frame);
	CHECK(Pixel(layered, {24, 24}) == glm::ivec3(255, 0, 0));
	CHECK(Pixel(layered, {24 + 8, 24}) == glm::ivec3(0, 255, 0));

	// Rotation around Z turns a horizontal bar into a diagonal one.
	frame.Sprites.clear();
	SpriteSubmission bar = green;
	bar.Transform = Placement({0.5f, 0.5f, 0.0f}, {60.0f, 4.0f, 1.0f}, 45.0f);
	frame.Sprites.push_back(bar);
	Image const rotated = Render(renderer, frame);
	// +45 degrees around Z turns +X towards +Y, which is up on the screen.
	CHECK(Pixel(rotated, {48 + 15, 48 - 15}) == glm::ivec3(0, 255, 0));
	CHECK(Pixel(rotated, {48 + 15, 48 + 15}) == glm::ivec3(0));
	CHECK(Pixel(rotated, {48 + 20, 48}) == glm::ivec3(0));
}

TEST_CASE("SceneRenderer: text is drawn with the default font where its layout places it")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageSize, ImageSize);
	Scope<FontAtlas> const atlas = Testing::CreateDefaultFontAtlas();
	TextLayout const layout = LayoutText(*atlas, "HIH");
	float const fontSize = 32.0f;

	Frame frame;
	TextSubmission text;
	text.Text = "HIH";
	text.ScreenSpace = true;
	text.FontSize = fontSize;
	text.Color = {1.0f, 1.0f, 1.0f, 1.0f};
	text.Transform = Placement({0.25f, 0.25f, 0.0f}, glm::vec3(1.0f));
	frame.Texts.push_back(text);
	Image const left = Render(renderer, frame);
	CHECK(renderer.GetStatistics().Quads == layout.Glyphs.size());
	std::optional<std::pair<glm::uvec2, glm::uvec2>> const leftBounds = LitBounds(left);
	REQUIRE(leftBounds);
	// Left-aligned from the anchor (24, 24) downwards, within the layout's glyph quads.
	float const x0 = 24.0f;
	float const y0 = 24.0f;
	CHECK(static_cast<float>(leftBounds->first.x) >= x0 + layout.Glyphs.front().Min.x * fontSize - 1.0f);
	CHECK(static_cast<float>(leftBounds->second.x) <= x0 + layout.Glyphs.back().Max.x * fontSize + 1.0f);
	CHECK(static_cast<float>(leftBounds->first.y) >= y0 + layout.Glyphs.front().Min.y * fontSize - 1.0f);
	CHECK(static_cast<float>(leftBounds->second.y) <= y0 + layout.Glyphs.front().Max.y * fontSize + 1.0f);
	// The glyphs are solid inside: the middle of the 'I' stem is fully covered.
	TextGlyphQuad const& bar = layout.Glyphs[1];
	glm::vec2 const stem = glm::vec2(x0, y0) + 0.5f * (bar.Min + bar.Max) * fontSize;
	CHECK(Pixel(left, glm::uvec2(stem)) == glm::ivec3(255));

	// Right-aligned text ends at the anchor.
	frame.Texts[0].Layout.Alignment = TextAlignment::Right;
	frame.Texts[0].Transform = Placement({0.75f, 0.25f, 0.0f}, glm::vec3(1.0f));
	std::optional<std::pair<glm::uvec2, glm::uvec2>> const rightBounds = LitBounds(Render(renderer, frame));
	REQUIRE(rightBounds);
	CHECK(rightBounds->second.x <= 72u + 1u);
	CHECK(rightBounds->first.x >= 72u - static_cast<uint32_t>(layout.Max.x * fontSize) - 2u);

	// World-space text, one world unit per line, facing the camera.
	frame.Texts[0].ScreenSpace = false;
	frame.Texts[0].Layout.Alignment = TextAlignment::Center;
	frame.Texts[0].FontSize = 1.0f;
	frame.Texts[0].Transform = Placement({0.0f, 0.5f, 0.0f}, glm::vec3(1.0f));
	std::optional<std::pair<glm::uvec2, glm::uvec2>> const worldBounds = LitBounds(Render(renderer, frame));
	REQUIRE(worldBounds);
	SceneRendererCamera const camera = MakeCamera();
	glm::uvec2 const anchor = ProjectToPixel(camera, {0.0f, 0.5f, 0.0f});
	CHECK(worldBounds->first.x < anchor.x);
	CHECK(worldBounds->second.x > anchor.x);
	// The text hangs below its anchor (the top of the line).
	CHECK(worldBounds->first.y >= anchor.y);
}

TEST_CASE("SceneRenderer: sprites and text are pickable, screen-space items on top")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageSize, ImageSize);
	renderer.SetEntityIdsEnabled(true);
	SceneRendererCamera const camera = MakeCamera();

	Frame frame;
	SpriteSubmission world;
	world.Transform = Placement({0.0f, 0.0f, 0.0f}, {3.0f, 3.0f, 1.0f});
	world.PickingId = 5;
	frame.Sprites.push_back(world);
	SpriteSubmission screen;
	screen.ScreenSpace = true;
	screen.Transform = Placement({0.5f, 0.5f, 0.0f}, {10.0f, 10.0f, 1.0f});
	screen.PickingId = 6;
	frame.Sprites.push_back(screen);
	TextSubmission text;
	text.Text = "I";
	text.ScreenSpace = true;
	text.FontSize = 64.0f;
	text.Transform = Placement({0.0f, 0.0f, 0.0f}, glm::vec3(1.0f));
	text.PickingId = 9;
	text.Selected = true;
	frame.Texts.push_back(text);

	Scope<FontAtlas> const atlas = Testing::CreateDefaultFontAtlas();
	TextGlyphQuad const bar = LayoutText(*atlas, "I").Glyphs.front();
	glm::uvec2 const stem = glm::uvec2(0.5f * (bar.Min + bar.Max) * text.FontSize);

	CHECK(Pick(renderer, frame, ProjectToPixel(camera, {-1.0f, 1.0f, 0.0f})) == std::optional<uint32_t>(5u));
	CHECK(Pick(renderer, frame, {48, 48}) == std::optional<uint32_t>(6u));
	// The selection bit does not leak into results.
	CHECK(Pick(renderer, frame, stem) == std::optional<uint32_t>(9u));
	// Between glyph outlines nothing is picked.
	CHECK(Pick(renderer, frame, {static_cast<uint32_t>(bar.Max.x * text.FontSize) + 2, 2}) == std::optional<uint32_t>(0u));
}

TEST_CASE("RenderScene: sprite and text components are drawn, a missing font falls back to the default one")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(ImageSize, ImageSize);
	renderer.SetEntityIdsEnabled(true);

	Scene scene("Text and sprites");
	Entity sprite = scene.CreateEntity("Sprite");
	SpriteRendererComponent& spriteComponent = sprite.AddComponent<SpriteRendererComponent>();
	spriteComponent.ScreenSpace = true;
	spriteComponent.Color = {0.0f, 0.0f, 1.0f, 1.0f};
	TransformComponent& spriteTransform = sprite.GetComponent<TransformComponent>();
	spriteTransform.Translation = {0.75f, 0.75f, 0.0f};
	spriteTransform.Scale = {16.0f, 16.0f, 1.0f};

	Entity label = scene.CreateEntity("Label");
	TextComponent& textComponent = label.AddComponent<TextComponent>();
	textComponent.Text = "HI";
	textComponent.ScreenSpace = true;
	textComponent.FontSize = 32.0f;
	// Not a registered asset: the default font is used.
	textComponent.Font = AssetHandle(UUID(424242));
	label.GetComponent<TransformComponent>().Translation = {0.1f, 0.1f, 0.0f};
	// Empty text draws nothing.
	scene.CreateEntity("Empty").AddComponent<TextComponent>();

	SceneRenderOptions options;
	UUID const spriteId = sprite.GetUUID();
	options.GetPickingId = [spriteId](UUID id)
	{
		return id == spriteId ? 3u : 0u;
	};
	renderer.RequestPick(72, 72);
	RenderScene(scene, renderer, MakeCamera(), options);
	GraphicsDevice::GetDevice()->waitForIdle();
	CHECK(renderer.TakePickResult() == std::optional<uint32_t>(3u));
	Result<Image> image = ReadbackTexture(renderer.GetFinalImage());
	REQUIRE(image.IsOk());
	CHECK(Pixel(image.GetValue(), {72, 72}) == glm::ivec3(0, 0, 255));
	CHECK(renderer.GetStatistics().Quads == 3);
	std::optional<std::pair<glm::uvec2, glm::uvec2>> const bounds = LitBounds(image.GetValue());
	REQUIRE(bounds);
	CHECK(bounds->first.x >= 9u);
	CHECK(bounds->first.y >= 9u);
}
