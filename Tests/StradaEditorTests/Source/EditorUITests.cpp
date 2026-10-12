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
#include <utility>
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
	CHECK(UI::FormatDisplayName("VSync") == "VSync");
	CHECK(UI::FormatDisplayName("FixedVSyncRate") == "Fixed VSync Rate");
	CHECK(UI::FormatDisplayName("Layer2Mask") == "Layer2 Mask");
	CHECK(UI::FormatDisplayName("Tag") == "Tag");
	// Script fields: private members drop their prefix.
	CHECK(UI::FormatDisplayName("m_JumpHeight") == "Jump Height");
	CHECK(UI::FormatDisplayName("s_Count") == "Count");
	CHECK(UI::FormatDisplayName("m_") == "m_");
	CHECK(UI::FormatDisplayName("m_lower") == "m_lower");
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
	// Labels are unique across the menus ("3D Text" in 3D Object, "Text" in UI).
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
		if (preset.NeedsCanvas)
		{
			// UI elements go into the scene's canvas (the Canvas preset comes first).
			CHECK(context.GetScene().GetParent(entity).HasComponent<UICanvasComponent>());
		}
		else
		{
			CHECK(entity.GetComponent<TransformComponent>().Translation == spawn);
		}
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

	// A character draws the capsule mesh its controller's default capsule matches.
	Result<UUID> const character = EntityPresets::Create(operations, find("Character"), UUID::Invalid(), spawn);
	REQUIRE(character.IsOk());
	Entity const characterEntity = context.GetScene().GetEntityByUUID(character.GetValue());
	CHECK(characterEntity.GetComponent<MeshComponent>().Mesh == GetBuiltInHandle(BuiltInAsset::CapsuleMesh));
	CharacterControllerComponent const& controller = characterEntity.GetComponent<CharacterControllerComponent>();
	CHECK(controller.Radius == 0.5f);
	CHECK(controller.Height == 2.0f);

	// Children start at their parent's origin; lights pointing down keep their rotation.
	Result<UUID> const spot = EntityPresets::Create(operations, find("Spot Light"), cube.GetValue(), spawn);
	REQUIRE(spot.IsOk());
	Entity const spotEntity = context.GetScene().GetEntityByUUID(spot.GetValue());
	CHECK(context.GetScene().GetParent(spotEntity) == cubeEntity);
	CHECK(spotEntity.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
	glm::vec3 const forward = spotEntity.GetComponent<TransformComponent>().Rotation * glm::vec3(0.0f, 0.0f, -1.0f);
	CHECK(forward.y == doctest::Approx(-1.0f).epsilon(1e-4));

	// Post-process volumes start global, with a 10 m box for when they are made local.
	Result<UUID> const volume = EntityPresets::Create(operations, find("Post Process Volume"), UUID::Invalid(), spawn);
	REQUIRE(volume.IsOk());
	Entity const volumeEntity = context.GetScene().GetEntityByUUID(volume.GetValue());
	REQUIRE(volumeEntity.HasComponent<PostProcessComponent>());
	CHECK(volumeEntity.GetComponent<PostProcessComponent>().Global);
	CHECK(volumeEntity.GetComponent<TransformComponent>().Scale == glm::vec3(10.0f));
}

TEST_CASE("EntityPresets: the create menus offer Core, 3D Object, 2D Object, Rendering, Effects, Physics, Audio and UI")
{
	using Labels = std::vector<std::string_view>;
	std::span<std::string_view const> const categories = EntityPresets::GetCategories();
	CHECK(std::vector<std::string_view>(categories.begin(), categories.end()) ==
	      Labels{"Core", "3D Object", "2D Object", "Rendering", "Effects", "Physics", "Audio", "UI"});

	std::vector<EntityPresetMenuSection> const menu = EntityPresets::GetMenu();
	REQUIRE(menu.size() == categories.size());
	auto const labels = [](EntityPresetMenuSection const& section)
	{
		Labels result;
		for (EntityPreset const* preset : section.Presets)
		{
			result.push_back(preset->Label);
		}
		return result;
	};
	// Core comes first, directly in the menu; every other category is a submenu.
	CHECK(menu[0].Category == EntityPresets::CoreCategory);
	CHECK_FALSE(menu[0].Submenu);
	CHECK(labels(menu[0]) == Labels{"Create Empty"});
	CHECK(labels(menu[1]) == Labels{"Cube", "Sphere", "Plane", "Cylinder", "Capsule", "Cone", "Quad", "3D Text"});
	CHECK(labels(menu[2]) == Labels{"Sprite"});
	CHECK(labels(menu[3]) == Labels{"Camera", "Directional Light", "Point Light", "Spot Light", "Sky Light"});
	CHECK(labels(menu[4]) == Labels{"Particle System", "Fire", "Smoke", "Sparks", "Post Process Volume"});
	CHECK(labels(menu[5]) == Labels{"Character"});
	CHECK(labels(menu[6]) == Labels{"Audio Source", "Audio Listener"});
	CHECK(labels(menu[7]) == Labels{"Canvas", "Panel", "Image", "Text", "Button"});
	// Every preset is in the menus once, in the section of its category.
	size_t count = 0;
	for (size_t i = 0; i < menu.size(); i++)
	{
		CAPTURE(menu[i].Category);
		CHECK(menu[i].Category == categories[i]);
		CHECK(menu[i].Submenu == (i != 0));
		for (EntityPreset const* preset : menu[i].Presets)
		{
			CHECK(preset->Category == menu[i].Category);
		}
		count += menu[i].Presets.size();
	}
	CHECK(count == EntityPresets::GetAll().size());

	// What the new entries create.
	EditorContext context;
	EditorOperations operations(context);
	Result<UUID> const empty = EntityPresets::Create(operations, *menu[0].Presets[0], UUID::Invalid(), glm::vec3(0.0f));
	REQUIRE(empty.IsOk());
	Entity const emptyEntity = context.GetScene().GetEntityByUUID(empty.GetValue());
	CHECK(emptyEntity.GetName() == "Entity");
	CHECK_FALSE(emptyEntity.HasComponent<MeshComponent>());
	Result<UUID> const listener = EntityPresets::Create(operations, *menu[6].Presets[1], UUID::Invalid(), glm::vec3(0.0f));
	REQUIRE(listener.IsOk());
	Entity const listenerEntity = context.GetScene().GetEntityByUUID(listener.GetValue());
	CHECK(listenerEntity.GetName() == "Audio Listener");
	CHECK(listenerEntity.HasComponent<AudioListenerComponent>());
	CHECK(listenerEntity.GetComponent<AudioListenerComponent>().Active);
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

TEST_CASE("ComponentInspection: the Add Component menu groups components by category and finds them by name")
{
	EditorContext context;
	EditorOperations operations(context);
	UUID const entity = Create(operations, "Lamp", UUID::Invalid(), Json::object({{"PointLight", Json::object()}}));
	std::vector<UUID> const selection = {entity};
	std::vector<ComponentInfo const*> const addable = ComponentInspection::GetAddableComponents(context.GetScene(), selection);

	// Categories in menu order without the empty Core one (nothing core can be added), components by display name; every
	// addable component is listed exactly once. The expected groups come from the registry, so components added later only
	// need their category.
	std::vector<ComponentInspection::ComponentGroup> const groups = ComponentInspection::GroupByCategory(addable);
	std::vector<std::pair<std::string_view, std::vector<std::string>>> expected;
	for (std::string_view const category : ComponentCategory::All)
	{
		std::vector<std::string> names;
		for (ComponentInfo const* component : addable)
		{
			if (component->Category == category)
			{
				names.push_back(UI::FormatDisplayName(component->Name));
			}
		}
		std::sort(names.begin(), names.end());
		if (!names.empty())
		{
			expected.emplace_back(category, std::move(names));
		}
	}
	REQUIRE(groups.size() == expected.size());
	for (size_t i = 0; i < groups.size(); i++)
	{
		CAPTURE(groups[i].Category);
		CHECK(groups[i].Category == expected[i].first);
		std::vector<std::string> names;
		for (ComponentInfo const* component : groups[i].Components)
		{
			names.push_back(UI::FormatDisplayName(component->Name));
		}
		CHECK(names == expected[i].second);
	}
	REQUIRE_FALSE(groups.empty());
	CHECK(groups.front().Category == ComponentCategory::Rendering);
	auto const findGroup = [&groups](std::string_view category) -> std::vector<std::string_view>
	{
		for (ComponentInspection::ComponentGroup const& group : groups)
		{
			if (group.Category == category)
			{
				return Names(group.Components);
			}
		}
		return {};
	};
	auto const contains = [](std::vector<std::string_view> const& names, std::string_view name)
	{
		return std::find(names.begin(), names.end(), name) != names.end();
	};
	// The lamp has its point light already; the others are where users look for them.
	CHECK_FALSE(contains(findGroup(ComponentCategory::Rendering), "PointLight"));
	CHECK(contains(findGroup(ComponentCategory::Rendering), "Mesh"));
	CHECK(contains(findGroup(ComponentCategory::Rendering), "Text"));
	CHECK(contains(findGroup(ComponentCategory::Physics), "BoxCollider"));
	CHECK(contains(findGroup(ComponentCategory::Scripting), "Script"));
	CHECK(contains(findGroup(ComponentCategory::UI), "UIButton"));
	CHECK(ComponentInspection::GroupByCategory({}).empty());

	// The menu searches what can be added: the lamp's point light is not offered.
	std::vector<std::string_view> const lights = Names(ComponentInspection::SearchComponents(addable, "light"));
	CHECK(contains(lights, "DirectionalLight"));
	CHECK_FALSE(contains(lights, "PointLight"));
	CHECK(ComponentInspection::SearchComponents(addable, "").size() == addable.size());

	// Names starting with the text come first, then names with a word starting with it, then the others; case, spaces
	// around the text and the spelling of the name do not matter. These searches run over a fixed set of components, so
	// components added later cannot change their matches.
	std::vector<ComponentInfo const*> searched;
	for (std::string_view const name :
	     {"AudioListener", "AudioSource", "BoxCollider", "Camera", "CapsuleCollider", "DirectionalLight", "Mesh", "MeshCollider",
	      "ParticleSystem", "RigidBody", "Script", "SkyLight", "SphereCollider", "SpotLight", "SpriteRenderer", "Text"})
	{
		ComponentInfo const* component = ComponentRegistry::Find(name);
		REQUIRE(component != nullptr);
		searched.push_back(component);
	}
	CHECK(Names(ComponentInspection::SearchComponents(searched, "light")) ==
	      std::vector<std::string_view>{"DirectionalLight", "SkyLight", "SpotLight"});
	CHECK(Names(ComponentInspection::SearchComponents(searched, "  COLL ")) ==
	      std::vector<std::string_view>{"BoxCollider", "CapsuleCollider", "MeshCollider", "SphereCollider"});
	CHECK(Names(ComponentInspection::SearchComponents(searched, "sphere col")) == std::vector<std::string_view>{"SphereCollider"});
	CHECK(Names(ComponentInspection::SearchComponents(searched, "spherecol")) == std::vector<std::string_view>{"SphereCollider"});
	CHECK(Names(ComponentInspection::SearchComponents(searched, "li")) ==
	      std::vector<std::string_view>{"AudioListener", "DirectionalLight", "SkyLight", "SpotLight", "BoxCollider", "CapsuleCollider",
	                                    "MeshCollider", "SphereCollider"});
	CHECK(Names(ComponentInspection::SearchComponents(searched, "s")) ==
	      std::vector<std::string_view>{"Script", "SkyLight", "SphereCollider", "SpotLight", "SpriteRenderer", "AudioSource",
	                                    "ParticleSystem", "AudioListener", "CapsuleCollider", "Mesh", "MeshCollider"});
	CHECK(Names(ComponentInspection::SearchComponents(searched, "urce")) == std::vector<std::string_view>{"AudioSource"});
	CHECK(ComponentInspection::SearchComponents(searched, "teleporter").empty());
	CHECK(ComponentInspection::SearchComponents(searched, "").size() == searched.size());
	CHECK(ComponentInspection::SearchComponents({}, "light").empty());
}
