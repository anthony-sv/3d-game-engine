#include "Physics/PhysicsTestUtilities.h"
#include "TestUtilities.h"

#include "Editor/EditorOperations.h"
#include "Editor/Panels/GameViewPanel.h"
#include "Editor/Viewport/PickingIdMap.h"
#include "Editor/Viewport/TransformEditing.h"
#include "Editor/Viewport/ViewportOverlays.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/MeshFactory.h"
#include "Strada/Renderer/SceneRenderer.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

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

	AABB GetBounds(std::vector<OverlayLine> const& lines)
	{
		AABB bounds;
		for (OverlayLine const& line : lines)
		{
			bounds.Expand(line.From);
			bounds.Expand(line.To);
		}
		return bounds;
	}

	// Two unit cubes 3 m apart (centers at x = -1.5 and 1.5): a mesh whose convex hull is one long box.
	Ref<MeshSource> CreateTwoCubes()
	{
		Ref<MeshSource> const cube = MeshFactory::CreateCube(AssetHandle());
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		for (float const offset : {-1.5f, 1.5f})
		{
			uint32_t const first = static_cast<uint32_t>(vertices.size());
			for (Vertex vertex : cube->GetVertices())
			{
				vertex.Position.x += offset;
				vertices.push_back(vertex);
			}
			for (uint32_t const index : cube->GetIndices())
			{
				indices.push_back(first + index);
			}
		}
		Submesh submesh;
		submesh.VertexCount = static_cast<uint32_t>(vertices.size());
		submesh.IndexCount = static_cast<uint32_t>(indices.size());
		Result<Ref<MeshSource>> mesh = MeshSource::Create(std::move(vertices), std::move(indices), {submesh}, {AssetHandle()});
		REQUIRE(mesh.IsOk());
		return mesh.GetValue();
	}

	// How many axes a line runs along.
	int CountAxes(OverlayLine const& line)
	{
		glm::vec3 const direction = glm::abs(line.To - line.From);
		return (direction.x > 1e-3f ? 1 : 0) + (direction.y > 1e-3f ? 1 : 0) + (direction.z > 1e-3f ? 1 : 0);
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

	// Perspective frustum: 4 lines from the apex plus the near and far rectangles and their 4 connections. Like every
	// shape, they are dimmed where surfaces hide them.
	UUID const camera = CreateEntity(operations, "Camera", UUID::Invalid(), Json::object({{"Camera", Json::object()}}));
	std::vector<OverlayLine> const frustum = shapes(camera);
	CHECK(frustum.size() == 16);
	float farthest = 0.0f;
	for (OverlayLine const& line : frustum)
	{
		farthest = std::max({farthest, -line.From.z, -line.To.z});
		CHECK(line.DepthTest);
		CHECK(line.Color == ViewportOverlays::CameraColor);
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

	// Only the kinds asked for.
	auto const kinds = [&](uint32_t which)
	{
		std::vector<OverlayLine> lines;
		ViewportOverlays::AppendEntityShapes(scene, scene.GetEntityByUUID(sensor), 16.0f / 9.0f, lines, which);
		return lines;
	};
	std::vector<OverlayLine> const colliders = kinds(ViewportOverlays::Shapes::Colliders);
	CHECK(!colliders.empty());
	CHECK(std::all_of(colliders.begin(), colliders.end(),
	                  [](OverlayLine const& line)
	                  {
						  return line.Color == ViewportOverlays::TriggerColor || line.Color == ViewportOverlays::ColliderColor;
					  }));
	std::vector<OverlayLine> const lights = kinds(ViewportOverlays::Shapes::Lights);
	CHECK(colliders.size() + lights.size() == combined.size());
	CHECK(kinds(ViewportOverlays::Shapes::Cameras).empty());

	// Directional light pointing down: a disc around its position, rays along the direction and an arrow down the middle,
	// drawn over everything (its position does not matter, and lights are often left on the ground).
	UUID const sun =
		CreateEntity(operations, "Sun", UUID::Invalid(),
	                 Json::object({{"DirectionalLight", Json::object()},
	                               {"Transform", Json::object({{"Translation", {0, 5, 0}}, {"RotationEuler", {-90, 0, 0}}})}}));
	std::vector<OverlayLine> const rays = shapes(sun);
	CHECK(rays.size() == ViewportOverlays::CircleSegments + ViewportOverlays::DirectionalLightRays + 5);
	size_t downward = 0;
	float lowest = 5.0f;
	for (OverlayLine const& line : rays)
	{
		CHECK(line.Color == ViewportOverlays::LightColor);
		CHECK_FALSE(line.DepthTest);
		lowest = std::min({lowest, line.From.y, line.To.y});
		glm::vec3 const direction = line.To - line.From;
		if (std::abs(direction.x) < 1e-4f && std::abs(direction.z) < 1e-4f && direction.y < 0.0f)
		{
			downward++;
		}
	}
	// The rays and the arrow point along the light.
	CHECK(downward == ViewportOverlays::DirectionalLightRays + 1);
	CHECK(lowest == doctest::Approx(5.0f - ViewportOverlays::DirectionalLightArrowLength));
	// Seen from a view, the shape grows with the distance, keeping its size on screen.
	std::vector<OverlayLine> farRays;
	ViewportOverlays::AppendEntityShapes(scene, scene.GetEntityByUUID(sun), 1.0f, farRays, ViewportOverlays::Shapes::Lights,
	                                     glm::vec3(0.0f, 5.0f, 20.0f));
	REQUIRE(farRays.size() == rays.size());
	float farLowest = 5.0f;
	for (OverlayLine const& line : farRays)
	{
		farLowest = std::min({farLowest, line.From.y, line.To.y});
	}
	CHECK(5.0f - farLowest ==
	      doctest::Approx(ViewportOverlays::DirectionalLightArrowLength * 20.0f * ViewportOverlays::DirectionalLightViewScale));

	// Spot light pointing down: the outer cone up to the range (rim, edges and the cap's arcs) and the inner cone's rim.
	UUID const spot =
		CreateEntity(operations, "Spot", UUID::Invalid(),
	                 Json::object({{"SpotLight", Json::object({{"Range", 4.0}, {"InnerConeAngle", 10.0}, {"OuterConeAngle", 30.0}})},
	                               {"Transform", Json::object({{"Translation", {0, 5, 0}}, {"RotationEuler", {-90, 0, 0}}})}}));
	std::vector<OverlayLine> const cone = shapes(spot);
	CHECK(cone.size() == 2 * ViewportOverlays::CircleSegments + 4 + 2 * ViewportOverlays::SpotCapSegments);
	glm::vec3 const apex(0.0f, 5.0f, 0.0f);
	float const outerRadius = 4.0f * std::sin(glm::radians(30.0f));
	float const innerRadius = 4.0f * std::sin(glm::radians(10.0f));
	size_t outerRim = 0;
	size_t innerRim = 0;
	for (OverlayLine const& line : cone)
	{
		// Nothing reaches beyond the range.
		CHECK(glm::length(line.From - apex) <= 4.0f + 1e-3f);
		CHECK(glm::length(line.To - apex) <= 4.0f + 1e-3f);
		float const radius = glm::length(glm::vec2(line.From.x, line.From.z));
		if (line.Color == ViewportOverlays::LightInnerColor)
		{
			innerRim++;
			CHECK(radius == doctest::Approx(innerRadius).epsilon(1e-3));
			CHECK(line.From.y == doctest::Approx(5.0f - 4.0f * std::cos(glm::radians(10.0f))).epsilon(1e-3));
		}
		else if (std::abs(line.From.y - (5.0f - 4.0f * std::cos(glm::radians(30.0f)))) < 1e-3f && std::abs(line.To.y - line.From.y) < 1e-3f)
		{
			outerRim++;
			CHECK(radius == doctest::Approx(outerRadius).epsilon(1e-3));
		}
	}
	CHECK(innerRim == ViewportOverlays::CircleSegments);
	CHECK(outerRim == ViewportOverlays::CircleSegments);
	// The cap's arcs reach straight down to the range.
	CHECK(std::any_of(cone.begin(), cone.end(),
	                  [](OverlayLine const& line)
	                  {
						  return NearlyEqual(line.To, glm::vec3(0.0f, 1.0f, 0.0f), 1e-3f) ||
		                         NearlyEqual(line.From, glm::vec3(0.0f, 1.0f, 0.0f), 1e-3f);
					  }));

	// Joints: the anchor's cross, the hinge axis and a line to the connected entity.
	UUID const door = CreateEntity(operations, "Door", UUID::Invalid(),
	                               Json::object({{"Joint", Json::object({{"Type", "Hinge"}, {"ConnectedEntity", lamp.ToString()}})}}));
	std::vector<OverlayLine> const hinge = shapes(door);
	CHECK(hinge.size() == 5);
	CHECK(std::all_of(hinge.begin(), hinge.end(),
	                  [](OverlayLine const& line)
	                  {
						  return line.Color == ViewportOverlays::JointColor;
					  }));
	CHECK(hinge.back().To == glm::vec3(0.0f, 2.0f, 0.0f));
	// Ball joints to the world have only the cross.
	REQUIRE(operations.SetComponentFields(door, "Joint", Json::object({{"Type", "Ball"}, {"ConnectedEntity", "0"}})).IsOk());
	CHECK(shapes(door).size() == 3);
}

TEST_CASE("GameViewPanel: the game's image keeps the chosen size or aspect ratio and is centered")
{
	// Free: the whole area, at its size.
	GameViewLayout const free = ComputeGameViewLayout(GameViewResolution::Free, {800.0f, 600.0f}, nullptr);
	CHECK(free.RenderSize == glm::uvec2(800, 600));
	CHECK(free.ImageOffset == glm::vec2(0.0f));

	// An aspect ratio: the largest rectangle that fits, letterboxed and rendered at its size.
	GameViewLayout const wide = ComputeGameViewLayout(GameViewResolution::Aspect16x9, {800.0f, 600.0f}, nullptr);
	CHECK(wide.RenderSize == glm::uvec2(800, 450));
	CHECK(wide.ImageOffset == glm::vec2(0.0f, 75.0f));
	GameViewLayout const narrow = ComputeGameViewLayout(GameViewResolution::Aspect4x3, {1600.0f, 600.0f}, nullptr);
	CHECK(narrow.RenderSize == glm::uvec2(800, 600));
	CHECK(narrow.ImageOffset == glm::vec2(400.0f, 0.0f));

	// Fixed sizes render at that size: shown pixel for pixel when they fit, scaled down when they do not.
	GameViewLayout const fits = ComputeGameViewLayout(GameViewResolution::HD, {1600.0f, 1000.0f}, nullptr);
	CHECK(fits.RenderSize == glm::uvec2(1280, 720));
	CHECK(fits.ImageSize == glm::vec2(1280.0f, 720.0f));
	CHECK(fits.ImageOffset == glm::vec2(160.0f, 140.0f));
	GameViewLayout const scaled = ComputeGameViewLayout(GameViewResolution::FullHD, {960.0f, 1000.0f}, nullptr);
	CHECK(scaled.RenderSize == glm::uvec2(1920, 1080));
	CHECK(scaled.ImageSize == glm::vec2(960.0f, 540.0f));
	// Without a project the project window size is the default window size.
	CHECK(ComputeGameViewLayout(GameViewResolution::ProjectWindow, {4000.0f, 4000.0f}, nullptr).RenderSize == glm::uvec2(1280, 720));

	// Degenerate areas still give a renderable size.
	CHECK(ComputeGameViewLayout(GameViewResolution::Free, {0.0f, -5.0f}, nullptr).RenderSize == glm::uvec2(1, 1));
	for (GameViewResolutionInfo const& info : GetGameViewResolutions())
	{
		GameViewLayout const tiny = ComputeGameViewLayout(info.Resolution, {0.0f, 0.0f}, nullptr);
		CHECK(tiny.RenderSize.x >= 1);
		CHECK(tiny.RenderSize.y >= 1);
		CHECK(tiny.ImageSize.x >= 1.0f);
	}
}

TEST_CASE("EditorContext: replacing the scene changes the scene version")
{
	EditorContext context;
	uint64_t const version = context.GetSceneVersion();
	EditorOperations operations(context);
	REQUIRE(operations.NewScene("Other").IsOk());
	CHECK(context.GetSceneVersion() != version);
}

TEST_CASE("ViewportOverlays: the overlays draw shapes for the selection or every entity, per kind")
{
	EditorContext context;
	EditorOperations operations(context);
	Scene& scene = context.GetScene();
	UUID const camera = CreateEntity(operations, "Camera", UUID::Invalid(), Json::object({{"Camera", Json::object()}}));
	UUID const lamp = CreateEntity(operations, "Lamp", UUID::Invalid(), Json::object({{"PointLight", Json::object()}}));
	UUID const crate = CreateEntity(operations, "Crate", UUID::Invalid(), Json::object({{"BoxCollider", Json::object()}}));
	UUID const hinge = CreateEntity(operations, "Hinge", UUID::Invalid(), Json::object({{"Joint", Json::object({{"Type", "Hinge"}})}}));
	auto const lineCount = [&](UUID id, uint32_t kinds)
	{
		std::vector<OverlayLine> lines;
		ViewportOverlays::AppendEntityShapes(scene, scene.GetEntityByUUID(id), 1.0f, lines, kinds);
		return lines.size();
	};
	size_t const cameraLines = lineCount(camera, ViewportOverlays::Shapes::Cameras);
	size_t const lampLines = lineCount(lamp, ViewportOverlays::Shapes::Lights);
	size_t const crateLines = lineCount(crate, ViewportOverlays::Shapes::Colliders);
	size_t const hingeLines = lineCount(hinge, ViewportOverlays::Shapes::Joints);
	REQUIRE(cameraLines > 0);
	REQUIRE(lampLines > 0);
	REQUIRE(crateLines > 0);
	REQUIRE(hingeLines > 0);
	CHECK(lineCount(crate, 0) == 0);

	auto const draw = [&](SceneViewOverlays const& overlays, std::vector<UUID> const& selection)
	{
		std::vector<OverlayLine> lines;
		ViewportOverlays::AppendSceneShapes(scene, selection, overlays, 1.0f, glm::vec3(0.0f, 0.0f, 10.0f), lines);
		return lines.size();
	};

	// By default every kind is drawn for the selected entities only.
	SceneViewOverlays overlays;
	CHECK(ViewportOverlays::GetShapeKinds(overlays, true) == ViewportOverlays::Shapes::All);
	CHECK(ViewportOverlays::GetShapeKinds(overlays, false) == 0);
	CHECK(draw(overlays, {}) == 0);
	CHECK(draw(overlays, {lamp}) == lampLines);
	CHECK(draw(overlays, {camera, lamp, crate, hinge}) == cameraLines + lampLines + crateLines + hingeLines);

	// Kinds turned off are not drawn even when selected; kinds for all entities are drawn for unselected ones too.
	overlays.Lights.Enabled = false;
	overlays.Colliders.AllEntities = true;
	CHECK(ViewportOverlays::GetShapeKinds(overlays, true) ==
	      (ViewportOverlays::Shapes::Cameras | ViewportOverlays::Shapes::Colliders | ViewportOverlays::Shapes::Joints));
	CHECK(ViewportOverlays::GetShapeKinds(overlays, false) == ViewportOverlays::Shapes::Colliders);
	CHECK(draw(overlays, {lamp}) == crateLines);
	CHECK(draw(overlays, {camera}) == cameraLines + crateLines);
	// A selected entity's shapes are drawn once.
	CHECK(draw(overlays, {crate}) == crateLines);

	// Every kind for every entity, whatever is selected; turned off, the choice of entities does not matter.
	overlays.Lights = {true, true};
	overlays.Cameras.AllEntities = true;
	overlays.Joints.AllEntities = true;
	CHECK(draw(overlays, {}) == cameraLines + lampLines + crateLines + hingeLines);
	overlays.Cameras.Enabled = false;
	overlays.Lights.Enabled = false;
	overlays.Colliders.Enabled = false;
	overlays.Joints.Enabled = false;
	CHECK(draw(overlays, {camera, lamp, crate, hinge}) == 0);
	// Unknown and deleted entities in the selection are skipped.
	overlays = {};
	CHECK(draw(overlays, {UUID(12345), lamp}) == lampLines);
}

TEST_CASE("ViewportOverlays: mesh colliders show the convex hull or the triangles physics collides with")
{
	Testing::AssetManagerScope assets;
	Testing::PhysicsSystemScope physics;
	EditorContext context;
	context.SetAssetReferenceResolver(
		[](std::string_view reference)
		{
			return AssetManager::ResolveReference(reference);
		});
	EditorOperations operations(context);
	Scene& scene = context.GetScene();
	auto const shapes = [&](UUID id, ViewportOverlays::MeshColliderOutlineCache* cache = nullptr)
	{
		std::vector<OverlayLine> lines;
		ViewportOverlays::AppendEntityShapes(scene, scene.GetEntityByUUID(id), 1.0f, lines, ViewportOverlays::Shapes::Colliders,
		                                     std::nullopt, cache);
		return lines;
	};

	// A cube's mesh collider, of the entity's mesh: twelve edges either way (flat faces show no diagonals), moved and scaled
	// with the entity and dimmed where surfaces hide them.
	UUID const crate = CreateEntity(operations, "Crate", UUID::Invalid(),
	                                Json::object({{"Mesh", Json::object({{"Mesh", "builtin://Cube"}})},
	                                              {"MeshCollider", Json::object()},
	                                              {"Transform", Json::object({{"Translation", {5, 0, 0}}, {"Scale", {2, 2, 2}}})}}));
	std::vector<OverlayLine> const hull = shapes(crate);
	CHECK(hull.size() == 12);
	for (OverlayLine const& line : hull)
	{
		CHECK(line.DepthTest);
		CHECK(line.Color == ViewportOverlays::ColliderColor);
		CHECK(CountAxes(line) == 1);
	}
	CHECK(NearlyEqual(GetBounds(hull).Min, {4.0f, -1.0f, -1.0f}, 1e-3f));
	CHECK(NearlyEqual(GetBounds(hull).Max, {6.0f, 1.0f, 1.0f}, 1e-3f));
	REQUIRE(operations.SetComponentFields(crate, "MeshCollider", Json::object({{"Convex", false}, {"IsTrigger", true}})).IsOk());
	std::vector<OverlayLine> const triangles = shapes(crate);
	CHECK(triangles.size() == 12);
	for (OverlayLine const& line : triangles)
	{
		CHECK(line.Color == ViewportOverlays::TriggerColor);
		CHECK(CountAxes(line) == 1);
	}

	// A mesh that is not convex: its triangles as a triangle mesh, one box around both cubes as a convex hull, and the hull
	// too for dynamic bodies, which cannot collide with triangle meshes.
	AssetHandle const twoCubes = AssetManager::AddMemoryAsset(CreateTwoCubes(), "TwoCubes");
	UUID const pair =
		CreateEntity(operations, "Pair", UUID::Invalid(),
	                 Json::object({{"MeshCollider", Json::object({{"Mesh", AssetManager::GetReference(twoCubes)}, {"Convex", false}})}}));
	std::vector<OverlayLine> const pairTriangles = shapes(pair);
	CHECK(pairTriangles.size() == 24);
	CHECK(NearlyEqual(GetBounds(pairTriangles).Min, {-2.0f, -0.5f, -0.5f}, 1e-3f));
	CHECK(NearlyEqual(GetBounds(pairTriangles).Max, {2.0f, 0.5f, 0.5f}, 1e-3f));
	REQUIRE(operations.SetComponentFields(pair, "MeshCollider", Json::object({{"Convex", true}})).IsOk());
	std::vector<OverlayLine> const pairHull = shapes(pair);
	CHECK(pairHull.size() == 12);
	CHECK(NearlyEqual(GetBounds(pairHull).Min, {-2.0f, -0.5f, -0.5f}, 1e-3f));
	CHECK(NearlyEqual(GetBounds(pairHull).Max, {2.0f, 0.5f, 0.5f}, 1e-3f));
	// Its edges run across the gap between the cubes.
	CHECK(std::count_if(pairHull.begin(), pairHull.end(),
	                    [](OverlayLine const& line)
	                    {
							return std::abs(std::abs(line.To.x - line.From.x) - 4.0f) < 1e-3f;
						}) == 4);
	REQUIRE(operations.SetComponentFields(pair, "MeshCollider", Json::object({{"Convex", false}})).IsOk());
	REQUIRE(operations.AddComponent(pair, "RigidBody", Json::object({{"Type", "Dynamic"}})).IsOk());
	CHECK(shapes(pair).size() == 12);
	REQUIRE(operations.SetComponentFields(pair, "RigidBody", Json::object({{"Type", "Kinematic"}})).IsOk());
	CHECK(shapes(pair).size() == 24);

	// Without a mesh there is nothing to draw, as there is nothing to collide with.
	UUID const empty = CreateEntity(operations, "Empty", UUID::Invalid(), Json::object({{"MeshCollider", Json::object()}}));
	CHECK(shapes(empty).empty());

	// The cache builds each outline once and forgets those of meshes that are gone.
	ViewportOverlays::MeshColliderOutlineCache cache;
	CHECK(shapes(pair, &cache).size() == 24);
	CHECK(shapes(crate, &cache).size() == 12);
	CHECK(shapes(pair, &cache).size() == 24);
	CHECK(cache.GetSize() == 2);
	REQUIRE(operations.SetComponentFields(pair, "MeshCollider", Json::object({{"Convex", true}})).IsOk());
	CHECK(shapes(pair, &cache).size() == 12);
	CHECK(cache.GetSize() == 3);
	AssetManager::RemoveMemoryAsset(twoCubes);
	cache.RemoveReleased();
	CHECK(cache.GetSize() == 1);
	CHECK(shapes(pair, &cache).empty());
}
