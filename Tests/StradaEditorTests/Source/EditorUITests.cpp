#include "Editor/ComponentInspection.h"
#include "Editor/EditorOperations.h"
#include "Editor/EntityPresets.h"
#include "Editor/HierarchyEditing.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/UI/FieldEditor.h"

#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Math/Math.h"
#include "Strada/Scene/Entity.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace Strada;

namespace
{
	UUID Create(EditorOperations& operations, std::string name, UUID parent = UUID::Invalid(), Json components = Json::object())
	{
		EntityCreateInfo info;
		info.Name = std::move(name);
		info.Parent = parent;
		info.Components = std::move(components);
		Result<UUID> entity = operations.CreateEntity(info);
		REQUIRE(entity.IsOk());
		return entity.GetValue();
	}

	std::vector<std::string_view> Names(std::vector<ComponentInfo const*> const& components)
	{
		std::vector<std::string_view> names;
		for (ComponentInfo const* component : components)
		{
			names.push_back(component->Name);
		}
		return names;
	}
}

TEST_CASE("UI: display names split PascalCase words and keep acronyms together")
{
	CHECK(UI::FormatDisplayName("PerspectiveFOV") == "Perspective FOV");
	CHECK(UI::FormatDisplayName("UVTiling") == "UV Tiling");
	CHECK(UI::FormatDisplayName("EV100") == "EV100");
	CHECK(UI::FormatDisplayName("FXAA") == "FXAA");
	CHECK(UI::FormatDisplayName("AmbientOcclusionRadius") == "Ambient Occlusion Radius");
	CHECK(UI::FormatDisplayName("PBRNeutral") == "PBR Neutral");
	CHECK(UI::FormatDisplayName("Layer2Mask") == "Layer2 Mask");
	CHECK(UI::FormatDisplayName("Tag") == "Tag");
	CHECK(UI::FormatDisplayName("").empty());
}

TEST_CASE("EditSession: one interaction is one undo step")
{
	EditorContext context;
	EditorOperations operations(context);
	UUID const entity = Create(operations, "Entity");
	size_t const steps = context.GetHistory().GetUndoCount();
	EditSession session(0x5445535400000000ull);

	auto const setX = [&](float x)
	{
		Json const patch = Json::object({{"Translation", {x, 0, 0}}});
		REQUIRE(operations.SetComponentFields(entity, "Transform", patch, session.GetMergeKey()).IsOk());
	};

	// A drag: several changes while an item stays active merge.
	session.Update(true, context.GetHistory());
	setX(1.0f);
	session.Update(true, context.GetHistory());
	setX(2.0f);
	CHECK(context.GetHistory().GetUndoCount() == steps + 1);

	// Releasing ends the interaction: the next change is a new step even though it reuses no state.
	session.Update(false, context.GetHistory());
	session.Update(true, context.GetHistory());
	setX(3.0f);
	CHECK(context.GetHistory().GetUndoCount() == steps + 2);

	uint64_t const key = session.GetMergeKey();
	CHECK(session.GetMergeKey() == key);
	session.Update(false, context.GetHistory());
	CHECK(session.GetMergeKey() != key);
}

TEST_CASE("FieldChange: patches apply the changed component to each object's own value")
{
	FieldChange change;
	change.Path = {"Translation"};
	change.Value = Json::array({5.0, 6.0, 7.0});
	change.Component = 1;
	Json const values = Json::object({{"Translation", {1.0, 2.0, 3.0}}, {"Scale", {1, 1, 1}}});
	CHECK(change.MakePatch(values) == Json::object({{"Translation", {1.0, 6.0, 3.0}}}));

	// Whole-value changes replace the value.
	change.Component.reset();
	CHECK(change.MakePatch(values) == Json::object({{"Translation", {5.0, 6.0, 7.0}}}));

	// Euler edits keep each object's other angles.
	FieldChange euler;
	euler.Path = {"Rotation"};
	euler.Component = 0;
	euler.EulerDegrees = glm::vec3(30.0f, 0.0f, 0.0f);
	glm::quat const yawed = Math::EulerDegreesToQuaternion({0.0f, 45.0f, 0.0f});
	Json const rotated = Json::object({{"Rotation", {yawed.x, yawed.y, yawed.z, yawed.w}}});
	Json const patch = euler.MakePatch(rotated);
	glm::quat const result = glm::quat::wxyz(patch["Rotation"][3].get<float>(), patch["Rotation"][0].get<float>(),
	                                         patch["Rotation"][1].get<float>(), patch["Rotation"][2].get<float>());
	glm::vec3 const degrees = Math::QuaternionToEulerDegrees(result);
	CHECK(degrees.x == doctest::Approx(30.0f).epsilon(1e-3));
	CHECK(degrees.y == doctest::Approx(45.0f).epsilon(1e-3));
	CHECK(degrees.z == doctest::Approx(0.0f).epsilon(1e-3));

	// Fields of nested structs patch only the nested field.
	FieldChange nested;
	nested.Path = {"Renderer", "Bloom"};
	nested.Value = false;
	Json const settings = Json::object({{"Renderer", {{"Bloom", true}, {"EV100", 1.0}}}});
	CHECK(nested.MakePatch(settings) == Json::object({{"Renderer", {{"Bloom", false}, {"EV100", 1.0}}}}));

	CHECK(FieldChange().MakePatch(values) == Json::object());
}

TEST_CASE("EntityPresets: every preset creates its entity, placed at the spawn point or under a parent")
{
	EditorContext context;
	EditorOperations operations(context);
	std::set<std::string_view> labels;
	glm::vec3 const spawn(1.0f, 2.0f, 3.0f);
	for (EntityPreset const& preset : EntityPresets::GetAll())
	{
		CAPTURE(preset.Label);
		CHECK(labels.insert(preset.Label).second);
		Result<UUID> created = EntityPresets::Create(operations, preset, UUID::Invalid(), spawn);
		REQUIRE(created.IsOk());
		Entity const entity = context.GetScene().GetEntityByUUID(created.GetValue());
		CHECK(entity.GetName() == preset.EntityName);
		CHECK(entity.GetComponent<TransformComponent>().Translation == spawn);
	}
	// Categories are contiguous, so menus group them.
	std::span<EntityPreset const> const presets = EntityPresets::GetAll();
	std::set<std::string_view> finished;
	for (size_t i = 0; i < presets.size(); i++)
	{
		if (i > 0 && presets[i].Category != presets[i - 1].Category)
		{
			CHECK(finished.insert(presets[i - 1].Category).second);
		}
		CHECK_FALSE(finished.contains(presets[i].Category));
	}

	auto const find = [&](std::string_view label)
	{
		auto const it = std::find_if(presets.begin(), presets.end(),
		                             [label](EntityPreset const& preset)
		                             {
										 return preset.Label == label;
									 });
		REQUIRE(it != presets.end());
		return *it;
	};
	Result<UUID> const cube = EntityPresets::Create(operations, find("Cube"), UUID::Invalid(), spawn);
	REQUIRE(cube.IsOk());
	Entity const cubeEntity = context.GetScene().GetEntityByUUID(cube.GetValue());
	CHECK(cubeEntity.GetComponent<MeshComponent>().Mesh == GetBuiltInHandle(BuiltInAsset::CubeMesh));

	// Children start at their parent's origin; lights pointing down keep their rotation.
	Result<UUID> const spot = EntityPresets::Create(operations, find("Spot Light"), cube.GetValue(), spawn);
	REQUIRE(spot.IsOk());
	Entity const spotEntity = context.GetScene().GetEntityByUUID(spot.GetValue());
	CHECK(context.GetScene().GetParent(spotEntity) == cubeEntity);
	CHECK(spotEntity.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
	glm::vec3 const forward = spotEntity.GetComponent<TransformComponent>().Rotation * glm::vec3(0.0f, 0.0f, -1.0f);
	CHECK(forward.y == doctest::Approx(-1.0f).epsilon(1e-4));
}

TEST_CASE("HierarchyEditing: drops resolve to a parent and an anchor, never into an own subtree")
{
	EditorContext context;
	EditorOperations operations(context);
	UUID const a = Create(operations, "A");
	UUID const b = Create(operations, "B");
	UUID const c = Create(operations, "C");
	UUID const child = Create(operations, "Child", a);
	Scene& scene = context.GetScene();

	std::vector<UUID> const draggedC = {c};
	CHECK(HierarchyEditing::ResolveDrop(scene, draggedC, a, DropPosition::Before) == MoveTarget{UUID::Invalid(), a});
	CHECK(HierarchyEditing::ResolveDrop(scene, draggedC, a, DropPosition::After) == MoveTarget{UUID::Invalid(), b});
	CHECK(HierarchyEditing::ResolveDrop(scene, draggedC, a, DropPosition::Inside) == MoveTarget{a, UUID::Invalid()});
	CHECK(HierarchyEditing::ResolveDrop(scene, draggedC, child, DropPosition::After) == MoveTarget{a, UUID::Invalid()});
	// The anchor skips dragged entities.
	std::vector<UUID> const draggedBC = {b, c};
	CHECK(HierarchyEditing::ResolveDrop(scene, draggedBC, a, DropPosition::After) == MoveTarget{UUID::Invalid(), UUID::Invalid()});

	std::vector<UUID> const draggedA = {a};
	CHECK_FALSE(HierarchyEditing::ResolveDrop(scene, draggedA, a, DropPosition::Inside));
	CHECK_FALSE(HierarchyEditing::ResolveDrop(scene, draggedA, child, DropPosition::Inside));
	CHECK_FALSE(HierarchyEditing::ResolveDrop(scene, draggedA, child, DropPosition::Before));
	CHECK_FALSE(HierarchyEditing::ResolveDrop(scene, draggedA, UUID(999), DropPosition::Inside));
	std::vector<UUID> const unknown = {UUID(999)};
	CHECK_FALSE(HierarchyEditing::ResolveDrop(scene, unknown, a, DropPosition::Inside));
	CHECK_FALSE(HierarchyEditing::ResolveDrop(scene, {}, a, DropPosition::Inside));

	// Applying a resolved drop moves the entities there.
	std::optional<MoveTarget> const move = HierarchyEditing::ResolveDrop(scene, draggedC, a, DropPosition::Before);
	REQUIRE(move);
	REQUIRE(operations.MoveEntities(draggedC, move->Parent, move->InsertBefore).IsOk());
	CHECK(scene.GetRootEntityIDs() == std::vector<UUID>{c, a, b});
}

TEST_CASE("HierarchyEditing: ranges, name search and ancestors")
{
	std::vector<UUID> const displayed = {UUID(1), UUID(2), UUID(3), UUID(4)};
	CHECK(HierarchyEditing::GetRange(displayed, UUID(2), UUID(4)) == std::vector<UUID>{UUID(2), UUID(3), UUID(4)});
	CHECK(HierarchyEditing::GetRange(displayed, UUID(3), UUID(1)) == std::vector<UUID>{UUID(1), UUID(2), UUID(3)});
	CHECK(HierarchyEditing::GetRange(displayed, UUID(9), UUID(2)) == std::vector<UUID>{UUID(2)});

	EditorContext context;
	EditorOperations operations(context);
	UUID const level = Create(operations, "Level");
	UUID const lamp = Create(operations, "Desk Lamp", level);
	UUID const bulb = Create(operations, "Bulb", lamp);
	UUID const other = Create(operations, "Floor LAMP");
	Scene& scene = context.GetScene();
	CHECK(HierarchyEditing::FindByName(scene, "lamp") == std::vector<UUID>{lamp, other});
	CHECK(HierarchyEditing::FindByName(scene, "nothing").empty());
	CHECK(HierarchyEditing::GetAncestors(scene, bulb) == std::vector<UUID>{level, lamp});
	CHECK(HierarchyEditing::GetAncestors(scene, level).empty());
	CHECK(HierarchyEditing::GetAncestors(scene, UUID(12345)).empty());
}

TEST_CASE("ComponentInspection: shared and addable components, mixed fields and defaults")
{
	EditorContext context;
	EditorOperations operations(context);
	UUID const a = Create(operations, "A", UUID::Invalid(), Json::object({{"PointLight", {{"Range", 5.0}}}, {"Camera", Json::object()}}));
	UUID const b = Create(operations, "B", UUID::Invalid(), Json::object({{"PointLight", {{"Range", 8.0}}}}));
	Scene& scene = context.GetScene();
	std::vector<UUID> const both = {a, b};

	CHECK(Names(ComponentInspection::GetCommonComponents(scene, both)) == std::vector<std::string_view>{"Transform", "PointLight"});
	std::vector<UUID> const onlyA = {a};
	CHECK(Names(ComponentInspection::GetCommonComponents(scene, onlyA)) ==
	      std::vector<std::string_view>{"Transform", "Camera", "PointLight"});
	CHECK(ComponentInspection::GetCommonComponents(scene, {}).empty());

	std::vector<std::string_view> const addable = Names(ComponentInspection::GetAddableComponents(scene, both));
	// Camera is missing on B; PointLight is on both; core and internal components never appear.
	CHECK(std::find(addable.begin(), addable.end(), "Camera") != addable.end());
	CHECK(std::find(addable.begin(), addable.end(), "PointLight") == addable.end());
	CHECK(std::find(addable.begin(), addable.end(), "Transform") == addable.end());
	CHECK(std::find(addable.begin(), addable.end(), "Prefab") == addable.end());
	CHECK(ComponentInspection::GetEntitiesWithout(scene, both, ComponentRegistry::Get<CameraComponent>()) == std::vector<UUID>{b});

	ComponentInfo const& light = ComponentRegistry::Get<PointLightComponent>();
	std::vector<Json> const values = {light.Serialize(scene.GetRegistry(), scene.GetEntityByUUID(a).GetHandle()),
	                                  light.Serialize(scene.GetRegistry(), scene.GetEntityByUUID(b).GetHandle())};
	CHECK(ComponentInspection::FindMixedFields(values) == std::vector<std::string>{"Range"});
	CHECK(ComponentInspection::FindMixedFields(std::span<Json const>(values).first(1)).empty());

	Json const defaults = ComponentInspection::GetDefaultValues(light);
	CHECK(defaults["Range"] == 10.0f);
	CHECK(defaults.size() == light.Fields.size());
}
