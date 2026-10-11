#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/UILayout.h"

#include <doctest/doctest.h>

#include <algorithm>
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
	canvas.ScaleMode = UIScaleMode::ScaleWithScreenHeight;
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

TEST_CASE("UILayout: canvases that scale with the screen's size fit their reference resolution on any screen")
{
	// The default: a 1920 x 1080 design.
	UICanvasComponent canvas;
	CHECK(canvas.ScaleMode == UIScaleMode::ScaleWithScreenSize);
	canvas.ReferenceResolution = {1920.0f, 1080.0f};
	// The canvas's size in units on a screen: the screen's size over the scale.
	auto const canvasSize = [&canvas](glm::vec2 screen)
	{
		return screen / UILayout::GetCanvasScale(canvas, screen);
	};
	auto const checkSize = [](glm::vec2 actual, glm::vec2 expected)
	{
		CHECK(actual.x == doctest::Approx(expected.x));
		CHECK(actual.y == doctest::Approx(expected.y));
	};

	// The reference's own shape, at any size.
	checkSize(canvasSize({1280.0f, 720.0f}), {1920.0f, 1080.0f});
	checkSize(canvasSize({3840.0f, 2160.0f}), {1920.0f, 1080.0f});
	// Wider screens (21:9): the height fits and the canvas grows wider, as with ScaleWithScreenHeight.
	checkSize(canvasSize({2560.0f, 1080.0f}), {2560.0f, 1080.0f});
	// Narrower screens (4:3, 16:10, portrait): the width fits and the canvas grows taller, where ScaleWithScreenHeight cuts
	// the sides off.
	checkSize(canvasSize({1440.0f, 1080.0f}), {1920.0f, 1440.0f});
	checkSize(canvasSize({1728.0f, 1080.0f}), {1920.0f, 1200.0f});
	checkSize(canvasSize({1080.0f, 1920.0f}), {1920.0f, 3413.333f});
	canvas.ScaleMode = UIScaleMode::ScaleWithScreenHeight;
	checkSize(canvasSize({1440.0f, 1080.0f}), {1440.0f, 1080.0f});
	checkSize(canvasSize({1080.0f, 1920.0f}), {607.5f, 1080.0f});
	canvas.ScaleMode = UIScaleMode::ScaleWithScreenSize;

	// So elements anchored to opposite corners never overlap on a narrow screen when they do not at the reference: two
	// 900-unit-wide bars in the top corners of a 4:3 screen.
	Scene scene("HUD");
	Entity hud = scene.CreateEntity("HUD");
	hud.AddComponent<UICanvasComponent>() = canvas;
	Entity left = AddElement(scene, "Score", hud, Rect({0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {900.0f, 100.0f}, {0.0f, 0.0f}));
	Entity right = AddElement(scene, "Health", hud, Rect({1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}, {900.0f, 100.0f}, {1.0f, 0.0f}));
	std::vector<UIElement> const elements = UILayout::LayoutScene(scene, {1440.0f, 1080.0f});
	UIElement const* leftElement = FindElement(elements, left);
	UIElement const* rightElement = FindElement(elements, right);
	REQUIRE(leftElement != nullptr);
	REQUIRE(rightElement != nullptr);
	CheckRectangle(leftElement->Rectangle, {0.0f, 0.0f}, {675.0f, 75.0f});
	CheckRectangle(rightElement->Rectangle, {765.0f, 0.0f}, {1440.0f, 75.0f});
	CHECK(leftElement->Rectangle.Max.x <= rightElement->Rectangle.Min.x);
	hud.GetComponent<UICanvasComponent>().ScaleMode = UIScaleMode::ScaleWithScreenHeight;
	std::vector<UIElement> const cut = UILayout::LayoutScene(scene, {1440.0f, 1080.0f});
	REQUIRE(FindElement(cut, left) != nullptr);
	REQUIRE(FindElement(cut, right) != nullptr);
	CHECK(FindElement(cut, left)->Rectangle.Max.x > FindElement(cut, right)->Rectangle.Min.x);

	// Screens without a size, and references below a unit or not finite, as for the height.
	CHECK(UILayout::GetCanvasScale(canvas, {0.0f, 720.0f}) == doctest::Approx(1.0f));
	CHECK(UILayout::GetCanvasScale(canvas, {1280.0f, 0.0f}) == doctest::Approx(1.0f));
	CHECK(UILayout::GetCanvasScale(canvas, {std::numeric_limits<float>::infinity(), 720.0f}) == doctest::Approx(1.0f));
	canvas.ReferenceResolution = {0.0f, std::numeric_limits<float>::quiet_NaN()};
	CHECK(UILayout::GetCanvasScale(canvas, {800.0f, 600.0f}) == doctest::Approx(600.0f));
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
	CHECK(UILayout::GetCanvases(empty).empty());
}

TEST_CASE("UILayout: canvases are found in hierarchy order among many other entities")
{
	Scene scene("Level");
	glm::vec2 const viewport(640.0f, 480.0f);
	// A canvas with a panel, made at a point of the level.
	auto const addCanvas = [&scene](char const* name, Entity parent)
	{
		Entity canvas = parent ? scene.CreateEntity(name, parent) : scene.CreateEntity(name);
		canvas.AddComponent<UICanvasComponent>();
		Entity panel = AddElement(scene, "Panel", canvas, Rect({0.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f}, {-20.0f, -20.0f}, {0.5f, 0.5f}));
		panel.AddComponent<UIImageComponent>();
		return canvas;
	};

	// A level of 1500 entities: wide rows of roots with deep chains below some of them, and canvases at the start, inside a
	// chain, inside another canvas, and at the end. A UIRect outside canvases is not UI.
	Entity const first = addCanvas("First", Entity());
	Entity chainEnd;
	for (int root = 0; root < 50; root++)
	{
		Entity parent = scene.CreateEntity("Root");
		for (int depth = 0; depth < 29; depth++)
		{
			parent = scene.CreateEntity("Child", parent);
		}
		if (root == 20)
		{
			chainEnd = parent;
			parent.AddComponent<UIRectComponent>();
		}
	}
	Entity const deep = addCanvas("Deep", chainEnd);
	Entity const nested = addCanvas("Nested", deep);
	Entity const last = addCanvas("Last", Entity());
	// A canvas made before the others but moved to the front of the roots comes first.
	Entity const moved = addCanvas("Moved", Entity());
	scene.SetSiblingIndex(moved, 0);

	CHECK(UILayout::GetCanvases(scene) ==
	      std::vector<entt::entity>{moved.GetHandle(), first.GetHandle(), deep.GetHandle(), last.GetHandle()});

	// The same elements, in the same order, as a walk over the whole hierarchy finds below canvases.
	std::vector<entt::entity> expected;
	scene.ForEachEntityInHierarchyOrder(
		[&scene, &expected](Entity entity)
		{
			for (Entity ancestor = entity; ancestor; ancestor = scene.GetParent(ancestor))
			{
				if (ancestor.HasComponent<UICanvasComponent>())
				{
					expected.push_back(entity.GetHandle());
					return;
				}
			}
		});
	std::vector<entt::entity> order;
	for (UIElement const& element : UILayout::LayoutScene(scene, viewport))
	{
		order.push_back(element.Handle);
	}
	CHECK(order.size() == 10);
	CHECK(order == expected);
	// The nested canvas is laid out with its parent's, starting again from the screen.
	auto const nestedElement = std::find(order.begin(), order.end(), nested.GetHandle());
	REQUIRE(nestedElement != order.end());
	CHECK(std::find(order.begin(), order.end(), deep.GetHandle()) < nestedElement);
}

TEST_CASE("UILayout: the topmost element that takes clicks is under the pointer")
{
	auto const element = [](glm::vec2 min, glm::vec2 max, bool hitTarget)
	{
		UIElement result;
		result.Rectangle = {min, max};
		result.HitTarget = hitTarget;
		return result;
	};
	// A panel, a text over it (no hit target), and an icon over the panel's corner.
	std::vector<UIElement> const elements = {
		element({0.0f, 0.0f}, {100.0f, 100.0f}, true),
		element({0.0f, 0.0f}, {100.0f, 50.0f}, false),
		element({80.0f, 80.0f}, {120.0f, 120.0f}, true),
	};
	CHECK(UILayout::FindHitTargetAt(elements, {10.0f, 10.0f}) == &elements[0]);
	CHECK(UILayout::FindHitTargetAt(elements, {90.0f, 90.0f}) == &elements[2]);
	CHECK(UILayout::FindHitTargetAt(elements, {110.0f, 110.0f}) == &elements[2]);
	CHECK(UILayout::FindHitTargetAt(elements, {110.0f, 10.0f}) == nullptr);
	CHECK(UILayout::FindHitTargetAt({}, {10.0f, 10.0f}) == nullptr);
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
