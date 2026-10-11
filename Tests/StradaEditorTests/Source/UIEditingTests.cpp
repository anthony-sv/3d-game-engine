#include "Editor/EditorOperations.h"
#include "Editor/EntityBounds.h"
#include "Editor/EntityPresets.h"
#include "Editor/Viewport/CanvasOverlay.h"

#include "Strada/Scene/Entity.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	// A preset of the UI menu.
	EntityPreset const& FindPreset(std::string_view label)
	{
		std::span<EntityPreset const> const presets = EntityPresets::GetAll();
		auto const it = std::find_if(presets.begin(), presets.end(),
		                             [label](EntityPreset const& preset)
		                             {
										 return preset.Category == "UI" && preset.Label == label;
									 });
		REQUIRE(it != presets.end());
		return *it;
	}

	Entity CreatePreset(EditorOperations& operations, std::string_view label, UUID parent, glm::vec3 const& position)
	{
		Result<UUID> created = EntityPresets::Create(operations, FindPreset(label), parent, position);
		REQUIRE(created.IsOk());
		Entity const entity = operations.GetContext().GetScene().GetEntityByUUID(created.GetValue());
		REQUIRE(entity.IsValid());
		return entity;
	}

	struct Bounds
	{
		glm::vec3 Min = glm::vec3(std::numeric_limits<float>::max());
		glm::vec3 Max = glm::vec3(std::numeric_limits<float>::lowest());
	};

	// The bounds of the lines of a color (each rectangle has its own).
	Bounds LineBounds(std::vector<OverlayLine> const& lines, glm::vec4 const& color, size_t& count)
	{
		Bounds bounds;
		count = 0;
		for (OverlayLine const& line : lines)
		{
			if (line.Color != color)
			{
				continue;
			}
			count++;
			bounds.Min = glm::min(bounds.Min, glm::min(line.From, line.To));
			bounds.Max = glm::max(bounds.Max, glm::max(line.From, line.To));
		}
		return bounds;
	}

	void CheckVector(glm::vec3 const& actual, glm::vec3 const& expected)
	{
		CHECK(actual.x == doctest::Approx(expected.x));
		CHECK(actual.y == doctest::Approx(expected.y));
		CHECK(actual.z == doctest::Approx(expected.z));
	}
}

TEST_CASE("EntityPresets: UI elements go into a canvas, made with them when there is none")
{
	EditorContext context;
	EditorOperations operations(context);
	Scene& scene = context.GetScene();
	glm::vec3 const spawn(1.0f, 2.0f, 3.0f);

	// Every UI preset but the canvas is a UI element.
	for (EntityPreset const& preset : EntityPresets::GetAll())
	{
		if (preset.Category == "UI")
		{
			CHECK(preset.NeedsCanvas == (preset.Label != "Canvas"));
		}
	}

	// Without a canvas, a new one at the spawn point holds the button, as one undo step.
	size_t const entityCount = scene.GetEntityCount();
	Entity const button = CreatePreset(operations, "Button", UUID::Invalid(), spawn);
	UUID const buttonId = button.GetUUID();
	Entity const canvas = scene.GetParent(button);
	REQUIRE(canvas.IsValid());
	UUID const canvasId = canvas.GetUUID();
	CHECK(canvas.GetName() == "Canvas");
	CHECK(canvas.HasComponent<UICanvasComponent>());
	CHECK_FALSE(scene.GetParent(canvas).IsValid());
	CHECK(canvas.GetComponent<TransformComponent>().Translation == spawn);
	CHECK(button.GetName() == "Button");
	CHECK(button.HasComponent<UIRectComponent>());
	CHECK(button.HasComponent<UIImageComponent>());
	CHECK(button.HasComponent<UIButtonComponent>());
	CHECK(button.GetComponent<UITextComponent>().Text == "Button");
	CHECK(scene.GetEntityCount() == entityCount + 2);
	REQUIRE(operations.Undo().IsOk());
	CHECK(scene.GetEntityCount() == entityCount);
	REQUIRE(operations.Redo().IsOk());
	CHECK(scene.GetEntityCount() == entityCount + 2);
	CHECK(scene.GetParent(scene.GetEntityByUUID(buttonId)).GetUUID() == canvasId);

	// At the root, the scene's first canvas takes new elements.
	Entity const text = CreatePreset(operations, "Text", UUID::Invalid(), spawn);
	CHECK(scene.GetParent(text).GetUUID() == canvasId);
	CHECK(text.GetComponent<UITextComponent>().Text == "Text");
	Entity const panel = CreatePreset(operations, "Panel", UUID::Invalid(), spawn);
	CHECK(scene.GetParent(panel).GetUUID() == canvasId);
	CHECK(panel.GetComponent<UIImageComponent>().CornerRadius > 0.0f);

	// Under a UI element, inside it.
	Entity const image = CreatePreset(operations, "Image", buttonId, spawn);
	CHECK(scene.GetParent(image).GetUUID() == buttonId);
	CHECK(image.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));

	// Under an entity outside canvases, in a new canvas there.
	EntityCreateInfo world;
	world.Name = "World";
	Result<UUID> const worldId = operations.CreateEntity(world);
	REQUIRE(worldId.IsOk());
	Entity const nested = CreatePreset(operations, "Image", worldId.GetValue(), spawn);
	Entity const nestedCanvas = scene.GetParent(nested);
	CHECK(nestedCanvas.HasComponent<UICanvasComponent>());
	CHECK(nestedCanvas.GetUUID() != canvasId);
	CHECK(scene.GetParent(nestedCanvas).GetUUID() == worldId.GetValue());
	CHECK(nestedCanvas.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));

	// The Canvas preset is a canvas of its own at the spawn point.
	Entity const second = CreatePreset(operations, "Canvas", UUID::Invalid(), spawn);
	CHECK(second.HasComponent<UICanvasComponent>());
	CHECK_FALSE(scene.GetParent(second).IsValid());
	CHECK(second.GetComponent<TransformComponent>().Translation == spawn);
}

TEST_CASE("EditorOperations: an entity with children is created as one undo step")
{
	EditorContext context;
	EditorOperations operations(context);
	Scene& scene = context.GetScene();
	size_t const entityCount = scene.GetEntityCount();

	EntityCreateInfo root;
	root.Name = "Root";
	std::vector<EntityCreateInfo> children(2);
	children[0].Name = "First";
	// The parent each child names is replaced by the root.
	children[1].Name = "Second";
	children[1].Parent = UUID(12345);
	children[1].Components = Json::object({{"UIText", Json::object({{"Text", "Hello"}})}});
	Result<std::vector<UUID>> created = operations.CreateEntityWithChildren(root, children);
	REQUIRE(created.IsOk());
	REQUIRE(created.GetValue().size() == 3);
	Entity const rootEntity = scene.GetEntityByUUID(created.GetValue()[0]);
	CHECK(rootEntity.GetName() == "Root");
	std::vector<Entity> const rootChildren = scene.GetChildren(rootEntity);
	REQUIRE(rootChildren.size() == 2);
	CHECK(rootChildren[0].GetUUID() == created.GetValue()[1]);
	CHECK(rootChildren[1].GetComponent<UITextComponent>().Text == "Hello");
	REQUIRE(operations.Undo().IsOk());
	CHECK(scene.GetEntityCount() == entityCount);

	// A child that cannot be created leaves nothing behind.
	children[1].Components = Json::object({{"UIText", Json::object({{"FontSize", -1.0}})}});
	CHECK(operations.CreateEntityWithChildren(root, children).IsError());
	CHECK(scene.GetEntityCount() == entityCount);
}

TEST_CASE("CanvasOverlay: canvases show in their entity's plane, outlined, faint until something in them is selected")
{
	Scene scene("UI");
	// The scene view shows a canvas at its reference resolution, whatever the game view's size.
	scene.OnViewportResize(640, 480);
	Entity canvas = scene.CreateEntity("Canvas");
	canvas.AddComponent<UICanvasComponent>().ReferenceResolution = {1600.0f, 1200.0f};
	canvas.GetComponent<TransformComponent>().Translation = {1.0f, 2.0f, 3.0f};
	Entity panel = scene.CreateEntity("Panel", canvas);
	panel.AddComponent<UIRectComponent>().Size = {400.0f, 200.0f};
	panel.AddComponent<UIImageComponent>();
	// Entities without a UIRect share their parent's rectangle and get no outline of their own.
	Entity const group = scene.CreateEntity("Group", panel);
	Entity const outside = scene.CreateEntity("Outside");

	// The UI is laid out at the reference resolution, 1 cm per canvas unit, standing on the canvas entity (the middle of its
	// bottom edge there) with y up; faint while nothing in the canvas is selected.
	std::array<UUID, 1> const notUI = {outside.GetUUID()};
	std::optional<WorldCanvasPlacement> const faint = CanvasOverlay::PlaceCanvas(scene, canvas.GetUUID(), notUI);
	REQUIRE(faint);
	CHECK(faint->Size == glm::vec2(1600.0f, 1200.0f));
	CHECK(faint->Opacity == doctest::Approx(CanvasOverlay::UnselectedOpacity));
	CheckVector(glm::vec3(faint->Transform * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)), {-7.0f, 14.0f, 3.0f});
	CheckVector(glm::vec3(faint->Transform * glm::vec4(1600.0f, 1200.0f, 0.0f, 1.0f)), {9.0f, 2.0f, 3.0f});
	// Only canvases are placed.
	CHECK_FALSE(CanvasOverlay::PlaceCanvas(scene, panel.GetUUID(), notUI));
	CHECK_FALSE(CanvasOverlay::PlaceCanvas(scene, UUID(12345), notUI));

	// Every canvas is outlined, over the scene; nothing else is for a canvas that holds nothing selected.
	std::vector<OverlayLine> lines;
	CanvasOverlay::AppendCanvasOutlines(scene, notUI, lines);
	CHECK(lines.size() == 4);
	CHECK(std::none_of(lines.begin(), lines.end(),
	                   [](OverlayLine const& line)
	                   {
						   return line.DepthTest;
					   }));
	size_t count = 0;
	Bounds const canvasBounds = LineBounds(lines, CanvasOverlay::CanvasColor, count);
	CHECK(count == 4);
	CheckVector(canvasBounds.Min, {-7.0f, 2.0f, 3.0f});
	CheckVector(canvasBounds.Max, {9.0f, 14.0f, 3.0f});

	// A selected element brings its canvas forward and outlines its elements, the selected ones highlighted.
	std::array<UUID, 2> const selection = {panel.GetUUID(), group.GetUUID()};
	std::optional<WorldCanvasPlacement> const active = CanvasOverlay::PlaceCanvas(scene, canvas.GetUUID(), selection);
	REQUIRE(active);
	CHECK(active->Opacity == doctest::Approx(1.0f));
	lines.clear();
	CanvasOverlay::AppendCanvasOutlines(scene, selection, lines);
	CHECK(lines.size() == 8);
	LineBounds(lines, CanvasOverlay::CanvasColor, count);
	CHECK(count == 4);
	// The selected panel, 400 x 200 units in the middle.
	Bounds const panelBounds = LineBounds(lines, CanvasOverlay::SelectedColor, count);
	CHECK(count == 4);
	CheckVector(panelBounds.Min, {-1.0f, 7.0f, 3.0f});
	CheckVector(panelBounds.Max, {3.0f, 9.0f, 3.0f});

	// Selecting the canvas highlights its outline; the panel is drawn as an element. Constant pixel sizes lay out the same
	// way at the reference resolution; the canvas entity's rotation and scale turn and size the canvas.
	lines.clear();
	canvas.GetComponent<UICanvasComponent>().ScaleMode = UIScaleMode::ConstantPixelSize;
	canvas.GetComponent<TransformComponent>().Scale = {2.0f, 2.0f, 2.0f};
	std::array<UUID, 1> const canvasSelection = {canvas.GetUUID()};
	CanvasOverlay::AppendCanvasOutlines(scene, canvasSelection, lines);
	Bounds const selectedCanvas = LineBounds(lines, CanvasOverlay::SelectedColor, count);
	CHECK(count == 4);
	CheckVector(selectedCanvas.Min, {-15.0f, 2.0f, 3.0f});
	CheckVector(selectedCanvas.Max, {17.0f, 26.0f, 3.0f});
	LineBounds(lines, CanvasOverlay::ElementColor, count);
	CHECK(count == 4);
	std::optional<WorldCanvasPlacement> const scaled = CanvasOverlay::PlaceCanvas(scene, canvas.GetUUID(), canvasSelection);
	REQUIRE(scaled);
	CHECK(scaled->Opacity == doctest::Approx(1.0f));
	CheckVector(glm::vec3(scaled->Transform * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)), {-15.0f, 26.0f, 3.0f});

	// A canvas inside another is drawn with it, and references scripts set beyond the inspector's range are kept to it.
	Entity nested = scene.CreateEntity("Nested", panel);
	nested.AddComponent<UICanvasComponent>();
	std::array<UUID, 1> const nestedSelection = {nested.GetUUID()};
	CHECK_FALSE(CanvasOverlay::PlaceCanvas(scene, canvas.GetUUID(), notUI)->Opacity == doctest::Approx(1.0f));
	CHECK(CanvasOverlay::PlaceCanvas(scene, canvas.GetUUID(), nestedSelection)->Opacity == doctest::Approx(1.0f));
	lines.clear();
	CanvasOverlay::AppendCanvasOutlines(scene, notUI, lines);
	CHECK(lines.size() == 4);
	canvas.GetComponent<UICanvasComponent>().ReferenceResolution = {std::numeric_limits<float>::infinity(), 0.0f};
	std::optional<WorldCanvasPlacement> const clamped = CanvasOverlay::PlaceCanvas(scene, canvas.GetUUID(), notUI);
	REQUIRE(clamped);
	CHECK(clamped->Size == glm::vec2(1.0f, 1.0f));
}

TEST_CASE("CanvasOverlay: focusing a canvas or its UI frames where the scene view draws it")
{
	Scene scene("UI");
	// A 16 x 12 meter canvas standing on (1, 2, 3), with a 4 x 2 meter panel in its middle.
	Entity canvas = scene.CreateEntity("Canvas");
	canvas.AddComponent<UICanvasComponent>().ReferenceResolution = {1600.0f, 1200.0f};
	canvas.GetComponent<TransformComponent>().Translation = {1.0f, 2.0f, 3.0f};
	Entity panel = scene.CreateEntity("Panel", canvas);
	panel.AddComponent<UIRectComponent>().Size = {400.0f, 200.0f};
	Entity const group = scene.CreateEntity("Group", panel);
	Entity hidden = scene.CreateEntity("Hidden", panel);
	hidden.AddComponent<UIRectComponent>().Visible = false;
	Entity nested = scene.CreateEntity("Nested", panel);
	nested.AddComponent<UICanvasComponent>();
	Entity outside = scene.CreateEntity("Outside");
	outside.GetComponent<TransformComponent>().Translation = {20.0f, 0.0f, -5.0f};

	auto const checkBounds = [&scene](std::vector<UUID> const& entities, glm::vec3 const& min, glm::vec3 const& max)
	{
		AABB const bounds = ComputeEntityBounds(scene, entities);
		REQUIRE(bounds.IsValid());
		CheckVector(bounds.Min, min);
		CheckVector(bounds.Max, max);
	};
	// The canvas, and the canvases nested in it (laid out over the same screen), cover the whole plane.
	checkBounds({canvas.GetUUID()}, {-7.0f, 2.0f, 3.0f}, {9.0f, 14.0f, 3.0f});
	checkBounds({nested.GetUUID()}, {-7.0f, 2.0f, 3.0f}, {9.0f, 14.0f, 3.0f});
	// Elements, and entities without a UIRect below them, cover their rectangles; hidden ones, which are not laid out, the
	// canvas.
	checkBounds({panel.GetUUID()}, {-1.0f, 7.0f, 3.0f}, {3.0f, 9.0f, 3.0f});
	checkBounds({group.GetUUID()}, {-1.0f, 7.0f, 3.0f}, {3.0f, 9.0f, 3.0f});
	checkBounds({hidden.GetUUID()}, {-7.0f, 2.0f, 3.0f}, {9.0f, 14.0f, 3.0f});
	// Entities outside canvases count as their origins, and several entities as the box around them all.
	CHECK_FALSE(CanvasOverlay::GetUIBounds(scene, outside.GetUUID()));
	CHECK_FALSE(CanvasOverlay::GetUIBounds(scene, UUID(12345)));
	checkBounds({panel.GetUUID(), outside.GetUUID(), UUID(12345)}, {-1.0f, 0.0f, -5.0f}, {20.0f, 9.0f, 3.0f});
	CHECK_FALSE(ComputeEntityBounds(scene, std::vector<UUID>{UUID(12345)}).IsValid());

	// The canvas entity's rotation turns the plane: a quarter turn around Y faces it along +X.
	canvas.GetComponent<TransformComponent>().Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	checkBounds({panel.GetUUID()}, {1.0f, 7.0f, 1.0f}, {1.0f, 9.0f, 5.0f});
}

TEST_CASE("CanvasOverlay: UI elements are the rectangles below a canvas")
{
	Scene scene("UI");
	Entity canvas = scene.CreateEntity("Canvas");
	canvas.AddComponent<UICanvasComponent>();
	Entity panel = scene.CreateEntity("Panel", canvas);
	panel.AddComponent<UIRectComponent>();
	Entity const group = scene.CreateEntity("Group", panel);
	Entity label = scene.CreateEntity("Label", group);
	label.AddComponent<UIRectComponent>();
	Entity stray = scene.CreateEntity("Stray");
	stray.AddComponent<UIRectComponent>();

	CHECK(CanvasOverlay::IsUIElement(scene, panel.GetUUID()));
	CHECK(CanvasOverlay::IsUIElement(scene, label.GetUUID()));
	// The canvas's Transform places its outline, and entities without a UIRect may hold more than UI.
	CHECK_FALSE(CanvasOverlay::IsUIElement(scene, canvas.GetUUID()));
	CHECK_FALSE(CanvasOverlay::IsUIElement(scene, group.GetUUID()));
	// Outside a canvas a UIRect is not UI.
	CHECK_FALSE(CanvasOverlay::IsUIElement(scene, stray.GetUUID()));
	CHECK_FALSE(CanvasOverlay::IsUIElement(scene, UUID::Invalid()));
	CHECK_FALSE(CanvasOverlay::IsUIElement(scene, UUID(12345)));
}
