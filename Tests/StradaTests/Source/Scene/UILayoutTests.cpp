#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/UILayout.h"

#include <doctest/doctest.h>

#include <limits>
#include <vector>

using namespace Strada;

namespace
{
	UIRectangle Rectangle(glm::vec2 min, glm::vec2 max)
	{
		return {min, max};
	}

	UIRectComponent Rect(glm::vec2 anchorMin, glm::vec2 anchorMax, glm::vec2 position, glm::vec2 size, glm::vec2 pivot)
	{
		UIRectComponent rect;
		rect.AnchorMin = anchorMin;
		rect.AnchorMax = anchorMax;
		rect.Position = position;
		rect.Size = size;
		rect.Pivot = pivot;
		return rect;
	}

	void CheckRectangle(UIRectangle const& actual, glm::vec2 min, glm::vec2 max)
	{
		CHECK(actual.Min.x == doctest::Approx(min.x));
		CHECK(actual.Min.y == doctest::Approx(min.y));
		CHECK(actual.Max.x == doctest::Approx(max.x));
		CHECK(actual.Max.y == doctest::Approx(max.y));
	}

	Entity AddElement(Scene& scene, char const* name, Entity parent, UIRectComponent const& rect)
	{
		Entity entity = scene.CreateEntity(name, parent);
		entity.AddComponent<UIRectComponent>() = rect;
		return entity;
	}

	UIElement const* FindElement(std::vector<UIElement> const& elements, Entity entity)
	{
		for (UIElement const& element : elements)
		{
			if (element.Handle == entity.GetHandle())
			{
				return &element;
			}
		}
		return nullptr;
	}
}

TEST_CASE("UILayout: canvases scale with the screen's height or keep pixels")
{
	UICanvasComponent canvas;
	canvas.ReferenceResolution = {1920.0f, 1080.0f};
	CHECK(UILayout::GetCanvasScale(canvas, {1920.0f, 1080.0f}) == doctest::Approx(1.0f));
	CHECK(UILayout::GetCanvasScale(canvas, {960.0f, 540.0f}) == doctest::Approx(0.5f));
	// Only the height counts: a wider screen shows more canvas units across.
	CHECK(UILayout::GetCanvasScale(canvas, {3000.0f, 2160.0f}) == doctest::Approx(2.0f));
	// Without a size there is nothing to fit.
	CHECK(UILayout::GetCanvasScale(canvas, {0.0f, 0.0f}) == doctest::Approx(1.0f));
	// References scripts set below a unit, or to values that are not finite, cannot blow the UI up.
	canvas.ReferenceResolution.y = 0.0f;
	CHECK(UILayout::GetCanvasScale(canvas, {800.0f, 600.0f}) == doctest::Approx(600.0f));
	canvas.ReferenceResolution.y = std::numeric_limits<float>::infinity();
	CHECK(UILayout::GetCanvasScale(canvas, {800.0f, 600.0f}) == doctest::Approx(600.0f));

	canvas.ScaleMode = UIScaleMode::ConstantPixelSize;
	canvas.ReferenceResolution = {1920.0f, 1080.0f};
	CHECK(UILayout::GetCanvasScale(canvas, {960.0f, 540.0f}) == doctest::Approx(1.0f));
	CHECK(UILayout::GetCanvasScale(canvas, {3840.0f, 2160.0f}) == doctest::Approx(1.0f));
}

TEST_CASE("UILayout: rectangles follow their anchors, position, size and pivot")
{
	UIRectangle const parent = Rectangle({100.0f, 50.0f}, {500.0f, 250.0f});

	SUBCASE("point anchors place the pivot")
	{
		// Centered in the parent.
		CheckRectangle(
			UILayout::ComputeRectangle(Rect({0.5f, 0.5f}, {0.5f, 0.5f}, {0.0f, 0.0f}, {100.0f, 40.0f}, {0.5f, 0.5f}), parent, 1.0f),
			{250.0f, 130.0f}, {350.0f, 170.0f});
		// Top-left anchor and pivot with an offset: x right, y down.
		CheckRectangle(
			UILayout::ComputeRectangle(Rect({0.0f, 0.0f}, {0.0f, 0.0f}, {10.0f, 20.0f}, {100.0f, 40.0f}, {0.0f, 0.0f}), parent, 1.0f),
			{110.0f, 70.0f}, {210.0f, 110.0f});
		// Bottom-right anchor and pivot: an offset of -10 keeps a margin.
		CheckRectangle(
			UILayout::ComputeRectangle(Rect({1.0f, 1.0f}, {1.0f, 1.0f}, {-10.0f, -10.0f}, {100.0f, 40.0f}, {1.0f, 1.0f}), parent, 1.0f),
			{390.0f, 200.0f}, {490.0f, 240.0f});
	}

	SUBCASE("different anchors stretch with the parent")
	{
		// Fills the parent.
		CheckRectangle(UILayout::ComputeRectangle(Rect({0.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.5f, 0.5f}), parent, 1.0f),
		               {100.0f, 50.0f}, {500.0f, 250.0f});
		// A negative size leaves a margin around it.
		CheckRectangle(
			UILayout::ComputeRectangle(Rect({0.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f}, {-20.0f, -40.0f}, {0.5f, 0.5f}), parent, 1.0f),
			{110.0f, 70.0f}, {490.0f, 230.0f});
		// A bar along the top: stretched horizontally, 30 high, hanging from the top edge.
		CheckRectangle(
			UILayout::ComputeRectangle(Rect({0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 30.0f}, {0.5f, 0.0f}), parent, 1.0f),
			{100.0f, 50.0f}, {500.0f, 80.0f});
		// The left half.
		CheckRectangle(UILayout::ComputeRectangle(Rect({0.0f, 0.0f}, {0.5f, 1.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.5f, 0.5f}), parent, 1.0f),
		               {100.0f, 50.0f}, {300.0f, 250.0f});
	}

	SUBCASE("canvas units scale sizes and offsets but not the anchors")
	{
		CheckRectangle(
			UILayout::ComputeRectangle(Rect({0.0f, 0.0f}, {0.0f, 0.0f}, {10.0f, 20.0f}, {100.0f, 40.0f}, {0.0f, 0.0f}), parent, 0.5f),
			{105.0f, 60.0f}, {155.0f, 80.0f});
		CheckRectangle(
			UILayout::ComputeRectangle(Rect({0.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f}, {-20.0f, -40.0f}, {0.5f, 0.5f}), parent, 2.0f),
			{120.0f, 90.0f}, {480.0f, 210.0f});
	}

	SUBCASE("sizes below zero and values that overflow make empty rectangles")
	{
		UIRectangle const empty =
			UILayout::ComputeRectangle(Rect({0.5f, 0.5f}, {0.5f, 0.5f}, {0.0f, 0.0f}, {-50.0f, 10.0f}, {0.5f, 0.5f}), parent, 1.0f);
		CHECK(empty.GetSize().x == 0.0f);
		CHECK(empty.GetCenter().x == doctest::Approx(300.0f));
		CHECK_FALSE(empty.Contains(empty.Min));

		float const huge = std::numeric_limits<float>::max();
		UIRectangle const overflowing =
			UILayout::ComputeRectangle(Rect({0.0f, 0.0f}, {1.0f, 1.0f}, {huge, huge}, {huge, huge}, {0.5f, 0.5f}), parent, 4.0f);
		CHECK(overflowing.Min == parent.Min);
		CHECK(overflowing.Max == parent.Min);
	}

	SUBCASE("rectangles contain their top and left edges only")
	{
		UIRectangle const rectangle = Rectangle({10.0f, 10.0f}, {20.0f, 20.0f});
		CHECK(rectangle.Contains({10.0f, 10.0f}));
		CHECK(rectangle.Contains({19.9f, 19.9f}));
		CHECK_FALSE(rectangle.Contains({20.0f, 15.0f}));
		CHECK_FALSE(rectangle.Contains({15.0f, 20.0f}));
		CHECK_FALSE(rectangle.Contains({9.9f, 15.0f}));
	}
}

TEST_CASE("UILayout: canvases lay out their descendants in hierarchy order")
{
	Scene scene("UI");
	glm::vec2 const viewport(800.0f, 600.0f);

	// Outside any canvas nothing is UI, not even a UIRect.
	Entity const world = scene.CreateEntity("World");
	AddElement(scene, "Stray", world, Rect({0.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.5f, 0.5f}));

	Entity canvas = scene.CreateEntity("Canvas");
	UICanvasComponent& canvasComponent = canvas.AddComponent<UICanvasComponent>();
	canvasComponent.ReferenceResolution = {1600.0f, 1200.0f};
	// A UIRect on the canvas itself is not used: the canvas covers the screen.
	canvas.AddComponent<UIRectComponent>().Size = {10.0f, 10.0f};

	Entity panel = AddElement(scene, "Panel", canvas, Rect({0.5f, 0.5f}, {0.5f, 0.5f}, {0.0f, 0.0f}, {800.0f, 600.0f}, {0.5f, 0.5f}));
	panel.GetComponent<UIRectComponent>().Opacity = 0.5f;
	panel.AddComponent<UIImageComponent>();
	// Without a UIRect an entity takes its parent's rectangle.
	Entity group = scene.CreateEntity("Group", panel);
	Entity button = AddElement(scene, "Button", group, Rect({0.0f, 1.0f}, {0.0f, 1.0f}, {20.0f, -20.0f}, {200.0f, 100.0f}, {0.0f, 1.0f}));
	button.AddComponent<UIImageComponent>();
	button.AddComponent<UIButtonComponent>();
	button.GetComponent<UIRectComponent>().Opacity = 0.5f;
	Entity label = AddElement(scene, "Label", button, Rect({0.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f}, {-20.0f, -20.0f}, {0.5f, 0.5f}));
	label.AddComponent<UITextComponent>().Text = "Play";
	Entity hidden = AddElement(scene, "Hidden", panel, Rect({0.5f, 0.5f}, {0.5f, 0.5f}, {0.0f, 0.0f}, {50.0f, 50.0f}, {0.5f, 0.5f}));
	hidden.GetComponent<UIRectComponent>().Visible = false;
	AddElement(scene, "Hidden child", hidden, Rect({0.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.5f, 0.5f}));
	// A canvas inside another starts again from the screen, with its own scale.
	Entity overlay = scene.CreateEntity("Overlay", panel);
	overlay.AddComponent<UICanvasComponent>().ScaleMode = UIScaleMode::ConstantPixelSize;
	Entity badge = AddElement(scene, "Badge", overlay, Rect({1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}, {64.0f, 32.0f}, {1.0f, 0.0f}));

	std::vector<UIElement> const elements = UILayout::LayoutScene(scene, viewport);
	std::vector<entt::entity> order;
	for (UIElement const& element : elements)
	{
		order.push_back(element.Handle);
	}
	CHECK(order == std::vector<entt::entity>{canvas.GetHandle(), panel.GetHandle(), group.GetHandle(), button.GetHandle(),
	                                         label.GetHandle(), overlay.GetHandle(), badge.GetHandle()});

	// 600 pixels for 1200 canvas units: half a pixel per unit.
	UIElement const* canvasElement = FindElement(elements, canvas);
	REQUIRE(canvasElement != nullptr);
	CheckRectangle(canvasElement->Rectangle, {0.0f, 0.0f}, {800.0f, 600.0f});
	CHECK(canvasElement->Scale == doctest::Approx(0.5f));
	CHECK_FALSE(canvasElement->HitTarget);

	UIElement const* panelElement = FindElement(elements, panel);
	REQUIRE(panelElement != nullptr);
	CheckRectangle(panelElement->Rectangle, {200.0f, 150.0f}, {600.0f, 450.0f});
	CHECK(panelElement->Opacity == doctest::Approx(0.5f));
	CHECK(panelElement->HitTarget);
	CHECK(panelElement->Button == entt::null);

	UIElement const* groupElement = FindElement(elements, group);
	REQUIRE(groupElement != nullptr);
	CHECK(groupElement->Rectangle == panelElement->Rectangle);
	CHECK_FALSE(groupElement->HitTarget);

	// Anchored at the panel's bottom-left corner, 10 pixels in from both edges.
	UIElement const* buttonElement = FindElement(elements, button);
	REQUIRE(buttonElement != nullptr);
	CheckRectangle(buttonElement->Rectangle, {210.0f, 390.0f}, {310.0f, 440.0f});
	CHECK(buttonElement->Opacity == doctest::Approx(0.25f));
	CHECK(buttonElement->Button == button.GetHandle());

	// Texts take no clicks; they belong to the button around them.
	UIElement const* labelElement = FindElement(elements, label);
	REQUIRE(labelElement != nullptr);
	CheckRectangle(labelElement->Rectangle, {215.0f, 395.0f}, {305.0f, 435.0f});
	CHECK_FALSE(labelElement->HitTarget);
	CHECK(labelElement->Button == button.GetHandle());

	CHECK(canvasElement->Canvas == canvas.GetHandle());
	CHECK(labelElement->Canvas == canvas.GetHandle());
	UIElement const* badgeElement = FindElement(elements, badge);
	REQUIRE(badgeElement != nullptr);
	CHECK(badgeElement->Canvas == overlay.GetHandle());
	CheckRectangle(badgeElement->Rectangle, {736.0f, 0.0f}, {800.0f, 32.0f});
	CHECK(badgeElement->Scale == doctest::Approx(1.0f));
	CHECK(badgeElement->Opacity == doctest::Approx(1.0f));

	// The layout follows the viewport's size.
	std::vector<UIElement> const larger = UILayout::LayoutScene(scene, viewport * 2.0f);
	UIElement const* largerButton = FindElement(larger, button);
	REQUIRE(largerButton != nullptr);
	CheckRectangle(largerButton->Rectangle, buttonElement->Rectangle.Min * 2.0f, buttonElement->Rectangle.Max * 2.0f);

	// Scenes without canvases have no UI.
	Scene empty("Empty");
	empty.CreateEntity("Entity");
	CHECK(UILayout::LayoutScene(empty, viewport).empty());
}

TEST_CASE("UILayout: a click goes to the nearest button of the topmost element that takes clicks")
{
	auto const element = [](glm::vec2 min, glm::vec2 max, bool hitTarget, entt::entity button)
	{
		UIElement result;
		result.Rectangle = {min, max};
		result.HitTarget = hitTarget;
		result.Button = button;
		return result;
	};
	entt::registry registry;
	entt::entity const play = registry.create();
	entt::entity const quit = registry.create();

	std::vector<UIElement> const elements = {
		// A background panel, then the play button with an icon inside it and a label over it.
		element({0.0f, 0.0f}, {100.0f, 100.0f}, true, entt::null),
		element({10.0f, 10.0f}, {50.0f, 30.0f}, true, play),
		element({12.0f, 12.0f}, {20.0f, 20.0f}, true, play),
		element({10.0f, 10.0f}, {50.0f, 30.0f}, false, play),
		// The quit button, half covered by an image that is not part of it.
		element({10.0f, 40.0f}, {50.0f, 60.0f}, true, quit),
		element({30.0f, 40.0f}, {60.0f, 60.0f}, true, entt::null),
	};
	CHECK(UILayout::FindButtonAt(elements, {40.0f, 20.0f}) == play);
	CHECK(UILayout::FindButtonAt(elements, {15.0f, 15.0f}) == play);
	CHECK(UILayout::FindButtonAt(elements, {15.0f, 50.0f}) == quit);
	CHECK(UILayout::FindButtonAt(elements, {40.0f, 50.0f}) == entt::null);
	CHECK(UILayout::FindButtonAt(elements, {80.0f, 80.0f}) == entt::null);
	CHECK(UILayout::FindButtonAt(elements, {200.0f, 200.0f}) == entt::null);
	CHECK(UILayout::FindButtonAt({}, {15.0f, 15.0f}) == entt::null);
}
