#include "Renderer/RenderTestUtilities.h"

#include "Strada/Asset/TextureAsset.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/KeyCodes.h"
#include "Strada/Core/Timestep.h"
#include "Strada/RHI/GraphicsDevice.h"
#include "Strada/Scene/UILayout.h"

#include <doctest/doctest.h>

#include <optional>
#include <utility>

using namespace Strada;

namespace
{
	constexpr uint32_t Width = 160;
	constexpr uint32_t Height = 120;

	// Input is global: tests that feed it restore it.
	class InputScope
	{
	public:
		InputScope() { Input::Reset(); }
		~InputScope() { Input::Reset(); }

		InputScope(InputScope const&) = delete;
		InputScope& operator=(InputScope const&) = delete;
	};

	SceneRendererCamera MakeCamera(uint32_t width, uint32_t height)
	{
		return Testing::MakeTestCamera({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, width, height);
	}

	glm::ivec4 Pixel(Image const& image, uint32_t x, uint32_t y)
	{
		uint8_t const* data = image.GetPixel(x, y);
		return {data[0], data[1], data[2], data[3]};
	}

	glm::ivec3 Color(Image const& image, uint32_t x, uint32_t y)
	{
		return glm::ivec3(Pixel(image, x, y));
	}

	Image Render(Scene& scene, SceneRenderer& renderer, SceneRenderOptions const& options = {})
	{
		RenderScene(scene, renderer, MakeCamera(renderer.GetViewportWidth(), renderer.GetViewportHeight()), options);
		Result<Image> image = ReadbackTexture(renderer.GetFinalImage());
		REQUIRE(image.IsOk());
		return std::move(image.GetValue());
	}

	Entity AddCanvas(Scene& scene, UIScaleMode mode = UIScaleMode::ConstantPixelSize)
	{
		Entity canvas = scene.CreateEntity("Canvas");
		UICanvasComponent& component = canvas.AddComponent<UICanvasComponent>();
		component.ScaleMode = mode;
		component.ReferenceResolution = {static_cast<float>(Width), static_cast<float>(Height)};
		return canvas;
	}

	// An image whose top-left corner is at a pixel of its parent's rectangle.
	Entity AddImage(Scene& scene, char const* name, Entity parent, glm::vec2 topLeft, glm::vec2 size, glm::vec4 const& color)
	{
		Entity entity = scene.CreateEntity(name, parent);
		UIRectComponent& rect = entity.AddComponent<UIRectComponent>();
		rect.AnchorMin = rect.AnchorMax = {0.0f, 0.0f};
		rect.Pivot = {0.0f, 0.0f};
		rect.Position = topLeft;
		rect.Size = size;
		entity.AddComponent<UIImageComponent>().Color = color;
		return entity;
	}

	// Lit pixels (brighter than the dark background) as a bounding box.
	std::optional<std::pair<glm::uvec2, glm::uvec2>> LitBounds(Image const& image, glm::uvec2 min, glm::uvec2 max)
	{
		std::optional<std::pair<glm::uvec2, glm::uvec2>> bounds;
		for (uint32_t y = min.y; y < max.y; y++)
		{
			for (uint32_t x = min.x; x < max.x; x++)
			{
				glm::ivec3 const color = Color(image, x, y);
				if (color.r + color.g + color.b < 300)
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

TEST_CASE("RenderScene: UI images are drawn over the scene in hierarchy order, with rounded corners")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(Width, Height);
	Scene scene("UI");
	Entity const canvas = AddCanvas(scene);

	// A red panel, a green child inside it, and a blue sibling after it overlapping both.
	Entity panel = AddImage(scene, "Panel", canvas, {10.0f, 10.0f}, {80.0f, 60.0f}, {1.0f, 0.0f, 0.0f, 1.0f});
	AddImage(scene, "Child", panel, {10.0f, 10.0f}, {30.0f, 20.0f}, {0.0f, 1.0f, 0.0f, 1.0f});
	AddImage(scene, "Sibling", canvas, {30.0f, 25.0f}, {20.0f, 20.0f}, {0.0f, 0.0f, 1.0f, 1.0f});
	// A screen-space sprite under the UI.
	Entity sprite = scene.CreateEntity("Sprite");
	SpriteRendererComponent& spriteComponent = sprite.AddComponent<SpriteRendererComponent>();
	spriteComponent.ScreenSpace = true;
	spriteComponent.Color = {1.0f, 1.0f, 1.0f, 1.0f};
	TransformComponent& spriteTransform = sprite.GetComponent<TransformComponent>();
	spriteTransform.Translation = {20.0f / Width, 60.0f / Height, 100.0f};
	spriteTransform.Scale = {10.0f, 10.0f, 1.0f};

	Image const image = Render(scene, renderer);
	// Opaque UI colors are exact on the 8-bit image.
	CHECK(Color(image, 70, 60) == glm::ivec3(255, 0, 0));
	CHECK(Color(image, 25, 25) == glm::ivec3(0, 255, 0));
	CHECK(Color(image, 40, 35) == glm::ivec3(0, 0, 255));
	// The sprite is under the panel, which covers it whatever its layer.
	CHECK(Color(image, 20, 60) == glm::ivec3(255, 0, 0));
	CHECK(Color(image, 5, 5) == glm::ivec3(0));
	CHECK(Color(image, 95, 75) == glm::ivec3(0));
	// Edges on whole pixels are sharp.
	CHECK(Color(image, 10, 10) == glm::ivec3(255, 0, 0));
	CHECK(Color(image, 89, 69) == glm::ivec3(255, 0, 0));
	CHECK(Color(image, 90, 40) == glm::ivec3(0));
	CHECK(renderer.GetStatistics().Quads == 4);

	// Rounded corners: the corner pixel stays empty, the edge's middle and the center are covered.
	panel.GetComponent<UIImageComponent>().CornerRadius = 16.0f;
	Image const rounded = Render(scene, renderer);
	CHECK(Color(rounded, 10, 10) == glm::ivec3(0));
	CHECK(Color(rounded, 13, 13) == glm::ivec3(0));
	CHECK(Color(rounded, 70, 10) == glm::ivec3(255, 0, 0));
	CHECK(Color(rounded, 10, 45) == glm::ivec3(255, 0, 0));
	// Anti-aliased along the curve: some pixel between the corner and the inside is partly covered.
	bool partial = false;
	for (uint32_t i = 10; i < 20; i++)
	{
		int const red = Color(rounded, i, i).r;
		partial = partial || (red > 10 && red < 245);
	}
	CHECK(partial);
	// Radii beyond half the shorter side make a capsule.
	panel.GetComponent<UIImageComponent>().CornerRadius = 1000.0f;
	Image const capsule = Render(scene, renderer);
	CHECK(Color(capsule, 50, 40) == glm::ivec3(255, 0, 0));
	CHECK(Color(capsule, 11, 11) == glm::ivec3(0));

	// Hidden subtrees and fully transparent elements are not drawn; opacity fades the children too.
	panel.GetComponent<UIRectComponent>().Visible = false;
	Image const hidden = Render(scene, renderer);
	CHECK(Color(hidden, 25, 25) == glm::ivec3(0));
	CHECK(Color(hidden, 40, 35) == glm::ivec3(0, 0, 255));
	panel.GetComponent<UIRectComponent>().Visible = true;
	panel.GetComponent<UIImageComponent>().CornerRadius = 0.0f;
	panel.GetComponent<UIRectComponent>().Opacity = 0.5f;
	Image const faded = Render(scene, renderer);
	CHECK(Color(faded, 70, 60).r > 100);
	CHECK(Color(faded, 70, 60).r < 230);
	CHECK(Color(faded, 25, 25).g > 100);
	CHECK(Color(faded, 25, 25).g < 230);

	// Colors outside 0..1, which scripts can set, are clamped: an alpha above one covers like one.
	panel.GetComponent<UIRectComponent>().Opacity = 1.0f;
	panel.GetComponent<UIImageComponent>().Color = {0.5f, 0.5f, 0.5f, 2.0f};
	glm::ivec3 const gray = Color(Render(scene, renderer), 70, 60);
	CHECK(gray.r == gray.g);
	CHECK(gray.g == gray.b);
	CHECK(gray.r > 180);
	CHECK(gray.r < 195);

	// Editor scene views leave the UI out.
	SceneRenderOptions withoutUI;
	withoutUI.DrawUI = false;
	Image const scene3D = Render(scene, renderer, withoutUI);
	CHECK(Color(scene3D, 70, 60) == glm::ivec3(0));
	CHECK(Color(scene3D, 20, 60) == glm::ivec3(255));
}

TEST_CASE("RenderScene: UI layouts follow the viewport, buttons show their state and the UI is pickable")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(Width, Height);
	renderer.SetEntityIdsEnabled(true);
	Scene scene("UI");
	Entity const canvas = AddCanvas(scene, UIScaleMode::ScaleWithScreenHeight);

	// A button stretched over the bottom quarter of the screen, 10 canvas units from its edges.
	Entity button = scene.CreateEntity("Button", canvas);
	UIRectComponent& rect = button.AddComponent<UIRectComponent>();
	rect.AnchorMin = {0.0f, 0.75f};
	rect.AnchorMax = {1.0f, 1.0f};
	rect.Size = {-20.0f, -20.0f};
	button.AddComponent<UIImageComponent>();
	UIButtonComponent& buttonComponent = button.AddComponent<UIButtonComponent>();
	buttonComponent.NormalColor = {0.0f, 1.0f, 0.0f, 1.0f};
	buttonComponent.DisabledColor = {1.0f, 0.0f, 1.0f, 1.0f};

	Image const normal = Render(scene, renderer);
	CHECK(Color(normal, 80, 105) == glm::ivec3(0, 255, 0));
	CHECK(Color(normal, 12, 105) == glm::ivec3(0, 255, 0));
	CHECK(Color(normal, 8, 105) == glm::ivec3(0));
	CHECK(Color(normal, 80, 95) == glm::ivec3(0));

	button.GetComponent<UIButtonComponent>().Interactable = false;
	CHECK(Color(Render(scene, renderer), 80, 105) == glm::ivec3(255, 0, 255));
	button.GetComponent<UIButtonComponent>().Interactable = true;

	// While the scene runs, the mouse (in the viewport's pixels) hovers the button, then presses it.
	{
		InputScope input;
		button.GetComponent<UIButtonComponent>().HoveredColor = {0.0f, 0.0f, 1.0f, 1.0f};
		button.GetComponent<UIButtonComponent>().PressedColor = {1.0f, 1.0f, 0.0f, 1.0f};
		scene.OnViewportResize(Width, Height);
		scene.OnRuntimeStart();
		Input::SetMousePosition({80.0f, 105.0f});
		scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
		CHECK(Color(Render(scene, renderer), 80, 105) == glm::ivec3(0, 0, 255));
		Input::EndFrame();
		Input::SetMouseButtonState(MouseButton::Left, true);
		scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
		CHECK(Color(Render(scene, renderer), 80, 105) == glm::ivec3(255, 255, 0));
		// Away from the button, it is at rest again.
		Input::EndFrame();
		Input::SetMousePosition({80.0f, 50.0f});
		scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
		CHECK(Color(Render(scene, renderer), 80, 105) == glm::ivec3(0, 255, 0));
		scene.OnRuntimeStop();
	}

	// Twice the viewport: twice the margins (the canvas scales with the height).
	renderer.SetViewportSize(Width * 2, Height * 2);
	Image const larger = Render(scene, renderer);
	CHECK(Color(larger, 24, 210) == glm::ivec3(0, 255, 0));
	CHECK(Color(larger, 16, 210) == glm::ivec3(0));
	CHECK(Color(larger, 160, 190) == glm::ivec3(0));
	renderer.SetViewportSize(Width, Height);

	// Picking IDs: the UI is on top of everything.
	SceneRenderOptions options;
	UUID const buttonId = button.GetUUID();
	options.GetPickingId = [buttonId](UUID id)
	{
		return id == buttonId ? 7u : 0u;
	};
	renderer.RequestPick(80, 105);
	Render(scene, renderer, options);
	GraphicsDevice::GetDevice()->waitForIdle();
	CHECK(renderer.TakePickResult() == std::optional<uint32_t>(7u));
	renderer.RequestPick(80, 50);
	Render(scene, renderer, options);
	GraphicsDevice::GetDevice()->waitForIdle();
	CHECK(renderer.TakePickResult() == std::optional<uint32_t>(0u));
}

TEST_CASE("RenderScene: UI text is laid out in its rectangle, wrapped and aligned")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(Width, Height);
	Scene scene("UI");
	Entity const canvas = AddCanvas(scene);

	// A 100 x 60 rectangle at (30, 30).
	Entity label = scene.CreateEntity("Label", canvas);
	UIRectComponent& rect = label.AddComponent<UIRectComponent>();
	rect.AnchorMin = rect.AnchorMax = {0.0f, 0.0f};
	rect.Pivot = {0.0f, 0.0f};
	rect.Position = {30.0f, 30.0f};
	rect.Size = {100.0f, 60.0f};
	UITextComponent& text = label.AddComponent<UITextComponent>();
	text.Text = "HI";
	text.FontSize = 20.0f;
	text.Color = {1.0f, 1.0f, 1.0f, 1.0f};

	auto const bounds = [&]
	{
		Image const image = Render(scene, renderer);
		// Nothing outside the rectangle.
		CHECK_FALSE(LitBounds(image, {0, 0}, {Width, 29}));
		CHECK_FALSE(LitBounds(image, {0, 91}, {Width, Height}));
		std::optional<std::pair<glm::uvec2, glm::uvec2>> const lit = LitBounds(image, {0, 0}, {Width, Height});
		REQUIRE(lit);
		return *lit;
	};

	// Centered both ways by default.
	std::pair<glm::uvec2, glm::uvec2> const centered = bounds();
	float const middleX = 0.5f * static_cast<float>(centered.first.x + centered.second.x);
	float const middleY = 0.5f * static_cast<float>(centered.first.y + centered.second.y);
	CHECK(middleX == doctest::Approx(80.0f).epsilon(0.05));
	CHECK(middleY == doctest::Approx(60.0f).epsilon(0.08));

	// Top-left and bottom-right.
	label.GetComponent<UITextComponent>().Alignment = TextAlignment::Left;
	label.GetComponent<UITextComponent>().VerticalAlignment = TextVerticalAlignment::Top;
	std::pair<glm::uvec2, glm::uvec2> const topLeft = bounds();
	CHECK(topLeft.first.x >= 30u);
	CHECK(topLeft.first.x <= 34u);
	CHECK(topLeft.first.y >= 30u);
	CHECK(topLeft.first.y <= 37u);
	label.GetComponent<UITextComponent>().Alignment = TextAlignment::Right;
	label.GetComponent<UITextComponent>().VerticalAlignment = TextVerticalAlignment::Bottom;
	std::pair<glm::uvec2, glm::uvec2> const bottomRight = bounds();
	CHECK(bottomRight.second.x <= 130u);
	CHECK(bottomRight.second.x >= 124u);
	CHECK(bottomRight.second.y <= 90u);
	CHECK(bottomRight.second.y >= 80u);

	// Words that do not fit the width wrap onto more lines; without wrapping they run past the rectangle.
	label.GetComponent<UITextComponent>().Text = "HI HI HI HI";
	label.GetComponent<UITextComponent>().FontSize = 28.0f;
	label.GetComponent<UITextComponent>().Alignment = TextAlignment::Left;
	label.GetComponent<UITextComponent>().VerticalAlignment = TextVerticalAlignment::Top;
	std::pair<glm::uvec2, glm::uvec2> const wrapped = bounds();
	CHECK(wrapped.second.x <= 130u);
	CHECK(wrapped.second.y - wrapped.first.y > 28u);
	label.GetComponent<UITextComponent>().Wrap = false;
	Image const unwrapped = Render(scene, renderer);
	std::optional<std::pair<glm::uvec2, glm::uvec2>> const line = LitBounds(unwrapped, {0, 0}, {Width, Height});
	REQUIRE(line);
	CHECK(line->second.x > 130u);
	CHECK(line->second.y - line->first.y < 28u);
}

TEST_CASE("RenderScene: a screen-space UI matches its golden image")
{
	ST_REQUIRE_GPU();
	Testing::RenderTestScope scope(stGpuTestScope);
	SceneRenderer renderer;
	renderer.SetViewportSize(Width * 2, Height * 2);
	Scene scene("Menu");
	Entity const canvas = AddCanvas(scene, UIScaleMode::ScaleWithScreenHeight);

	// A rounded, translucent panel over a blue background, with a title, a textured icon and two buttons.
	Entity background = scene.CreateEntity("Background", canvas);
	background.AddComponent<UIImageComponent>().Color = {0.02f, 0.05f, 0.12f, 1.0f};
	Entity panel = AddImage(scene, "Panel", canvas, {20.0f, 10.0f}, {120.0f, 100.0f}, {0.2f, 0.22f, 0.3f, 0.85f});
	panel.GetComponent<UIImageComponent>().CornerRadius = 8.0f;
	Entity title = scene.CreateEntity("Title", panel);
	UIRectComponent& titleRect = title.AddComponent<UIRectComponent>();
	titleRect.AnchorMin = {0.0f, 0.0f};
	titleRect.AnchorMax = {1.0f, 0.0f};
	titleRect.Pivot = {0.5f, 0.0f};
	titleRect.Position = {0.0f, 6.0f};
	titleRect.Size = {0.0f, 20.0f};
	UITextComponent& titleText = title.AddComponent<UITextComponent>();
	titleText.Text = "Strada";
	titleText.FontSize = 16.0f;
	titleText.Color = {1.0f, 0.8f, 0.3f, 1.0f};

	Image checker(4, 4, 4);
	for (uint32_t y = 0; y < 4; y++)
	{
		for (uint32_t x = 0; x < 4; x++)
		{
			uint8_t* pixel = checker.GetPixel(x, y);
			uint8_t const value = (x + y) % 2 == 0 ? 255 : 60;
			pixel[0] = value;
			pixel[1] = value;
			pixel[2] = 255;
			pixel[3] = 255;
		}
	}
	Result<Ref<TextureAsset>> texture = TextureAsset::CreateFromImage(std::move(checker));
	REQUIRE(texture.IsOk());
	Entity icon = AddImage(scene, "Icon", panel, {8.0f, 32.0f}, {24.0f, 24.0f}, {1.0f, 1.0f, 1.0f, 1.0f});
	icon.GetComponent<UIImageComponent>().Texture = AssetManager::AddMemoryAsset(texture.GetValue(), "Checker");
	icon.GetComponent<UIImageComponent>().CornerRadius = 4.0f;

	auto const addButton = [&](char const* name, float top, bool interactable)
	{
		Entity button = AddImage(scene, name, panel, {40.0f, top}, {72.0f, 22.0f}, glm::vec4(1.0f));
		button.GetComponent<UIImageComponent>().CornerRadius = 11.0f;
		button.AddComponent<UIButtonComponent>().Interactable = interactable;
		UITextComponent& label = button.AddComponent<UITextComponent>();
		label.Text = name;
		label.FontSize = 12.0f;
	};
	addButton("Play", 32.0f, true);
	addButton("Quit", 62.0f, false);

	Image const image = Render(scene, renderer);
	Testing::CheckGoldenImage("SceneRenderer_UI", image);
}
