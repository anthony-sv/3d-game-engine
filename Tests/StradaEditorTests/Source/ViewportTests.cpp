#include "Editor/EditorOperations.h"
#include "Editor/Viewport/PickingIdMap.h"
#include "Editor/Viewport/TransformEditing.h"
#include "Editor/Viewport/ViewportOverlays.h"

#include "Strada/Renderer/SceneRenderer.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

using namespace Strada;

namespace
{
	bool NearlyEqual(glm::vec3 const& a, glm::vec3 const& b, float epsilon = 1e-4f)
	{
		return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(epsilon)));
	}

	UUID CreateEntity(EditorOperations& operations, std::string name, UUID parent = UUID::Invalid(), Json components = Json::object())
	{
		EntityCreateInfo info;
		info.Name = std::move(name);
		info.Parent = parent;
		info.Components = std::move(components);
		Result<UUID> entity = operations.CreateEntity(info);
		REQUIRE(entity.IsOk());
		return entity.GetValue();
	}

	glm::vec3 GetTranslation(EditorContext& context, UUID id)
	{
		return context.GetScene().GetEntityByUUID(id).GetComponent<TransformComponent>().Translation;
	}

	Json TranslationPatch(glm::vec3 const& translation)
	{
		return Json::object({{"Translation", {translation.x, translation.y, translation.z}}});
	}
}

TEST_CASE("PickingIdMap: IDs are stable, unique and never reused")
{
	PickingIdMap map;
	UUID const a(101);
	UUID const b(202);
	CHECK(map.GetId(a) == 1);
	CHECK(map.GetId(b) == 2);
	CHECK(map.GetId(a) == 1);
	CHECK(map.GetEntity(1) == a);
	CHECK(map.GetEntity(2) == b);
	CHECK_FALSE(map.GetEntity(0).IsValid());
	CHECK_FALSE(map.GetEntity(3).IsValid());
	CHECK(map.GetId(UUID::Invalid()) == 0);
	CHECK(map.GetCount() == 2);

	map.Clear();
	CHECK(map.GetCount() == 0);
	CHECK_FALSE(map.GetEntity(1).IsValid());
	CHECK(map.GetId(b) == 1);
	CHECK(map.GetId(b) <= SceneRenderer::MaxPickingId);
}

TEST_CASE("TransformEditing: selection roots skip entities whose ancestor is selected")
{
	EditorContext context;
	EditorOperations operations(context);
	UUID const parent = CreateEntity(operations, "Parent");
	UUID const child = CreateEntity(operations, "Child", parent);
	UUID const grandchild = CreateEntity(operations, "Grandchild", child);
	UUID const other = CreateEntity(operations, "Other");

	Scene& scene = context.GetScene();
	std::vector<UUID> const selection = {grandchild, other, parent, UUID(424242), other};
	CHECK(TransformEditing::GetSelectionRoots(scene, selection) == std::vector<UUID>{other, parent});
	CHECK(TransformEditing::GetSelectionRoots(scene, std::vector<UUID>{child, grandchild}) == std::vector<UUID>{child});
	CHECK(TransformEditing::GetSelectionRoots(scene, {}).empty());
}

TEST_CASE("TransformEditing: local transforms are recovered under scaled and rotated parents")
{
	glm::mat4 const parent = glm::scale(
		glm::rotate(glm::translate(glm::mat4(1.0f), {10.0f, 0.0f, 0.0f}), glm::radians(90.0f), {0.0f, 1.0f, 0.0f}), glm::vec3(2.0f));
	glm::mat4 const world = glm::translate(glm::mat4(1.0f), {10.0f, 4.0f, -6.0f});
	Result<TransformComponent> const local = TransformEditing::ComputeLocalTransform(world, parent);
	REQUIRE(local.IsOk());
	CHECK(NearlyEqual(glm::vec3(parent * local.GetValue().GetTransform()[3]), {10.0f, 4.0f, -6.0f}));
	CHECK(NearlyEqual(local.GetValue().Scale, glm::vec3(0.5f)));

	glm::mat4 const degenerate = glm::scale(glm::mat4(1.0f), {0.0f, 1.0f, 1.0f});
	CHECK(TransformEditing::ComputeLocalTransform(degenerate, glm::mat4(1.0f)).IsError());
}

TEST_CASE("TransformEditing: world deltas move every selected root, children follow")
{
	EditorContext context;
	EditorOperations operations(context);
	UUID const parent = CreateEntity(operations, "Parent", UUID::Invalid(),
	                                 Json::object({{"Transform", Json::object({{"Translation", {1, 0, 0}}, {"Scale", {2, 2, 2}}})}}));
	UUID const child = CreateEntity(operations, "Child", parent, Json::object({{"Transform", TranslationPatch({0.0f, 1.0f, 0.0f})}}));
	UUID const other =
		CreateEntity(operations, "Other", UUID::Invalid(), Json::object({{"Transform", TranslationPatch({0.0f, 0.0f, 5.0f})}}));

	Scene& scene = context.GetScene();
	glm::mat4 const delta = glm::translate(glm::mat4(1.0f), {0.0f, 3.0f, 0.0f});
	std::vector<UUID> const roots = TransformEditing::GetSelectionRoots(scene, std::vector<UUID>{child, parent, other});
	Result<std::vector<ComponentEdit>> edits = TransformEditing::ApplyWorldDelta(scene, roots, delta);
	REQUIRE(edits.IsOk());
	REQUIRE(edits.GetValue().size() == 2);
	CHECK(edits.GetValue()[0].Component == "Transform");

	glm::vec3 const childWorldBefore = glm::vec3(scene.GetWorldTransform(scene.GetEntityByUUID(child))[3]);
	REQUIRE(operations.SetComponentFields(std::move(edits.GetValue())).IsOk());
	CHECK(NearlyEqual(GetTranslation(context, parent), {1.0f, 3.0f, 0.0f}));
	CHECK(NearlyEqual(GetTranslation(context, other), {0.0f, 3.0f, 5.0f}));
	// The child is moved by its parent, not twice.
	CHECK(NearlyEqual(GetTranslation(context, child), {0.0f, 1.0f, 0.0f}));
	CHECK(NearlyEqual(glm::vec3(scene.GetWorldTransform(scene.GetEntityByUUID(child))[3]), childWorldBefore + glm::vec3(0.0f, 3.0f, 0.0f)));
	// Scale is preserved.
	CHECK(NearlyEqual(scene.GetEntityByUUID(parent).GetComponent<TransformComponent>().Scale, glm::vec3(2.0f)));

	CHECK(TransformEditing::ApplyWorldDelta(scene, std::vector<UUID>{UUID(9)}, delta).IsError());
}

TEST_CASE("EditorOperations: batched component edits are one all-or-nothing undo step")
{
	EditorContext context;
	EditorOperations operations(context);
	UUID const a = CreateEntity(operations, "A");
	UUID const b = CreateEntity(operations, "B");
	size_t const stepsBefore = context.GetHistory().GetUndoCount();

	std::vector<ComponentEdit> edits = {{a, "Transform", TranslationPatch({1.0f, 0.0f, 0.0f})},
	                                    {b, "Transform", TranslationPatch({0.0f, 2.0f, 0.0f})}};
	REQUIRE(operations.SetComponentFields(edits).IsOk());
	CHECK(context.GetHistory().GetUndoCount() == stepsBefore + 1);
	CHECK(context.GetHistory().GetUndoDescription() == "Edit 2 Components");
	CHECK(GetTranslation(context, a) == glm::vec3(1.0f, 0.0f, 0.0f));
	CHECK(GetTranslation(context, b) == glm::vec3(0.0f, 2.0f, 0.0f));

	REQUIRE(operations.Undo().IsOk());
	CHECK(GetTranslation(context, a) == glm::vec3(0.0f));
	CHECK(GetTranslation(context, b) == glm::vec3(0.0f));
	REQUIRE(operations.Redo().IsOk());
	CHECK(GetTranslation(context, b) == glm::vec3(0.0f, 2.0f, 0.0f));

	// A failing edit (missing component, invalid value, unknown entity) leaves everything unchanged.
	Json const before = SceneSerializer::Serialize(context.GetScene());
	size_t const steps = context.GetHistory().GetUndoCount();
	CHECK(operations.SetComponentFields({{a, "Transform", TranslationPatch({9.0f, 9.0f, 9.0f})}, {b, "PointLight", Json::object()}})
	          .IsError());
	CHECK(operations
	          .SetComponentFields(
				  {{a, "Transform", TranslationPatch({9.0f, 9.0f, 9.0f})}, {b, "Transform", Json::object({{"Translation", "up"}})}})
	          .IsError());
	CHECK(operations.SetComponentFields({{UUID(77), "Transform", TranslationPatch({9.0f, 9.0f, 9.0f})}}).IsError());
	CHECK(operations.SetComponentFields({{a, "ID", Json::object()}}).IsError());
	CHECK(operations.SetComponentFields(std::vector<ComponentEdit>{}).IsError());
	CHECK(SceneSerializer::Serialize(context.GetScene()) == before);
	CHECK(context.GetHistory().GetUndoCount() == steps);

	// Editing a value to what it already is records nothing.
	CHECK(operations.SetComponentFields({{a, "Transform", TranslationPatch({1.0f, 0.0f, 0.0f})}}).IsOk());
	CHECK(context.GetHistory().GetUndoCount() == steps);
}

TEST_CASE("EditorOperations: batched edits with the same key and targets merge")
{
	EditorContext context;
	EditorOperations operations(context);
	UUID const a = CreateEntity(operations, "A");
	UUID const b = CreateEntity(operations, "B");
	size_t const steps = context.GetHistory().GetUndoCount();

	constexpr uint64_t DragKey = 1234;
	for (int i = 1; i <= 5; i++)
	{
		float const offset = static_cast<float>(i);
		REQUIRE(operations
		            .SetComponentFields({{a, "Transform", TranslationPatch({offset, 0.0f, 0.0f})},
		                                 {b, "Transform", TranslationPatch({0.0f, offset, 0.0f})}},
		                                DragKey)
		            .IsOk());
	}
	CHECK(context.GetHistory().GetUndoCount() == steps + 1);
	CHECK(GetTranslation(context, a) == glm::vec3(5.0f, 0.0f, 0.0f));

	// Other targets with the same key start a new step; so does a broken merge.
	REQUIRE(operations.SetComponentFields({{a, "Transform", TranslationPatch({6.0f, 0.0f, 0.0f})}}, DragKey).IsOk());
	CHECK(context.GetHistory().GetUndoCount() == steps + 2);
	context.GetHistory().BreakMerge();
	REQUIRE(operations.SetComponentFields({{a, "Transform", TranslationPatch({7.0f, 0.0f, 0.0f})}}, DragKey).IsOk());
	CHECK(context.GetHistory().GetUndoCount() == steps + 3);

	REQUIRE(operations.Undo().IsOk());
	REQUIRE(operations.Undo().IsOk());
	REQUIRE(operations.Undo().IsOk());
	CHECK(GetTranslation(context, a) == glm::vec3(0.0f));
	CHECK(GetTranslation(context, b) == glm::vec3(0.0f));
}

TEST_CASE("ViewportOverlays: shapes of cameras, lights and colliders")
{
	EditorContext context;
	EditorOperations operations(context);
	Scene& scene = context.GetScene();
	auto const shapes = [&](UUID id)
	{
		std::vector<OverlayLine> lines;
		ViewportOverlays::AppendEntityShapes(scene, scene.GetEntityByUUID(id), 16.0f / 9.0f, lines);
		return lines;
	};

	// Nothing to draw for plain entities.
	CHECK(shapes(CreateEntity(operations, "Empty")).empty());

	// Perspective frustum: 4 lines from the apex plus the near and far rectangles and their 4 connections.
	UUID const camera = CreateEntity(operations, "Camera", UUID::Invalid(), Json::object({{"Camera", Json::object()}}));
	std::vector<OverlayLine> const frustum = shapes(camera);
	CHECK(frustum.size() == 16);
	float farthest = 0.0f;
	for (OverlayLine const& line : frustum)
	{
		farthest = std::max({farthest, -line.From.z, -line.To.z});
		CHECK_FALSE(line.DepthTest);
	}
	CHECK(farthest == doctest::Approx(0.1f + ViewportOverlays::CameraFrustumLength));
	REQUIRE(operations.SetComponentFields(camera, "Camera", Json::object({{"Projection", "Orthographic"}})).IsOk());
	CHECK(shapes(camera).size() == 12);

	// Point light: three circles of the light's range around its position.
	UUID const lamp =
		CreateEntity(operations, "Lamp", UUID::Invalid(),
	                 Json::object({{"PointLight", Json::object({{"Range", 4.0}})}, {"Transform", TranslationPatch({0.0f, 2.0f, 0.0f})}}));
	std::vector<OverlayLine> const circles = shapes(lamp);
	CHECK(circles.size() == 3 * ViewportOverlays::CircleSegments);
	for (OverlayLine const& line : circles)
	{
		CHECK(glm::length(line.From - glm::vec3(0.0f, 2.0f, 0.0f)) == doctest::Approx(4.0f));
	}

	// Box collider: 12 edges, scaled by the entity and offset.
	UUID const crate = CreateEntity(operations, "Crate", UUID::Invalid(),
	                                Json::object({{"BoxCollider", Json::object({{"HalfExtents", {1, 1, 1}}, {"Offset", {0, 1, 0}}})},
	                                              {"Transform", Json::object({{"Translation", {5, 0, 0}}, {"Scale", {2, 2, 2}}})}}));
	std::vector<OverlayLine> const box = shapes(crate);
	REQUIRE(box.size() == 12);
	AABB bounds;
	for (OverlayLine const& line : box)
	{
		bounds.Expand(line.From);
		bounds.Expand(line.To);
		CHECK(line.DepthTest);
		CHECK(line.Color == ViewportOverlays::ColliderColor);
	}
	CHECK(NearlyEqual(bounds.Min, {3.0f, 0.0f, -2.0f}));
	CHECK(NearlyEqual(bounds.Max, {7.0f, 4.0f, 2.0f}));

	// Triggers use their own color; several shapes on one entity add up.
	UUID const sensor = CreateEntity(operations, "Sensor", UUID::Invalid(),
	                                 Json::object({{"SphereCollider", Json::object({{"IsTrigger", true}})},
	                                               {"CapsuleCollider", Json::object()},
	                                               {"SpotLight", Json::object()},
	                                               {"DirectionalLight", Json::object()}}));
	std::vector<OverlayLine> const combined = shapes(sensor);
	CHECK(std::count_if(combined.begin(), combined.end(),
	                    [](OverlayLine const& line)
	                    {
							return line.Color == ViewportOverlays::TriggerColor;
						}) == 3 * ViewportOverlays::CircleSegments);
	CHECK(combined.size() > 3 * ViewportOverlays::CircleSegments);
}

TEST_CASE("EditorContext: replacing the scene changes the scene version")
{
	EditorContext context;
	uint64_t const version = context.GetSceneVersion();
	EditorOperations operations(context);
	REQUIRE(operations.NewScene("Other").IsOk());
	CHECK(context.GetSceneVersion() != version);
}
