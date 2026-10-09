#include "TestUtilities.h"

#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace Strada;

namespace
{
	Ref<Scene> MakeRichScene()
	{
		Ref<Scene> scene = CreateRef<Scene>("Showcase");
		scene->GetSettings().Gravity = {0.0f, -5.0f, 0.0f};

		Entity camera = scene->CreateEntity("Camera");
		camera.AddComponent<CameraComponent>().PerspectiveFOV = 70.0f;
		camera.GetComponent<TransformComponent>().Translation = {0.0f, 2.0f, 8.0f};

		Entity player = scene->CreateEntity("Player");
		player.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		player.AddComponent<CapsuleColliderComponent>();
		ScriptComponent& script = player.AddComponent<ScriptComponent>();
		script.ClassName = "Game.Player";
		script.Fields["Camera"] = ScriptFieldValue::FromEntity(camera.GetUUID());
		script.Fields["Speed"] = ScriptFieldValue::FromFloat(6.0f);

		Entity weapon = scene->CreateEntity("Weapon", player);
		weapon.AddComponent<MeshComponent>().Mesh = AssetHandle(UUID(1001));
		weapon.AddComponent<PointLightComponent>();

		Entity sun = scene->CreateEntity("Sun");
		sun.AddComponent<DirectionalLightComponent>().Intensity = 4.0f;
		sun.AddComponent<SkyLightComponent>();
		return scene;
	}

	Json DocumentWithEntities(Json entities)
	{
		Json document = Json::object();
		document["Strada"] = MakeFileHeader("Scene", 1);
		document["Entities"] = std::move(entities);
		return document;
	}

	Json EntityJson(std::string const& id, Json components = Json::object())
	{
		Json entity = Json::object();
		entity["ID"] = id;
		entity["Components"] = std::move(components);
		return entity;
	}

	Json WithParent(std::string const& parent)
	{
		Json components = Json::object();
		components["Relationship"] = Json::object({{"Parent", parent}, {"Children", Json::array()}});
		return components;
	}
}

TEST_CASE("SceneSerializer: round trip preserves names, settings, hierarchy and components")
{
	Ref<Scene> const scene = MakeRichScene();
	Json const document = SceneSerializer::Serialize(*scene);

	Result<Ref<Scene>> loaded = SceneSerializer::Deserialize(document, DeserializationContext{});
	REQUIRE_MESSAGE(loaded.IsOk(), loaded.GetError());
	Scene& copy = *loaded.GetValue();
	CHECK(copy.GetName() == "Showcase");
	CHECK(copy.GetSettings().Gravity == glm::vec3(0.0f, -5.0f, 0.0f));
	CHECK(copy.GetEntityCount() == 4);

	Entity player = copy.FindEntityByName("Player");
	REQUIRE(player);
	CHECK(player.GetComponent<RigidBodyComponent>().Type == RigidBodyType::Dynamic);
	REQUIRE(copy.GetChildren(player).size() == 1);
	CHECK(copy.GetChildren(player)[0].GetComponent<MeshComponent>().Mesh == AssetHandle(UUID(1001)));
	CHECK(player.GetComponent<ScriptComponent>().Fields.at("Camera").GetEntity() == copy.FindEntityByName("Camera").GetUUID());

	// Serializing the loaded scene yields the same document (deterministic output).
	CHECK(SceneSerializer::Serialize(copy) == document);
}

TEST_CASE("SceneSerializer: documents use the documented structure")
{
	Ref<Scene> const scene = MakeRichScene();
	Json const document = SceneSerializer::Serialize(*scene);
	CHECK(document["Strada"]["Type"] == "Scene");
	CHECK(document["Strada"]["Version"] == SceneSerializer::FormatVersion);
	CHECK(document["Scene"]["Name"] == "Showcase");
	REQUIRE(document["Entities"].size() == 4);

	// Hierarchy order: parents before children, roots in order.
	CHECK(document["Entities"][0]["Components"]["Tag"]["Tag"] == "Camera");
	CHECK(document["Entities"][1]["Components"]["Tag"]["Tag"] == "Player");
	CHECK(document["Entities"][2]["Components"]["Tag"]["Tag"] == "Weapon");
	CHECK(document["Entities"][3]["Components"]["Tag"]["Tag"] == "Sun");
	CHECK(document["Entities"][0]["ID"].is_string());
	CHECK_FALSE(document["Entities"][0]["Components"].contains("ID"));
}

TEST_CASE("SceneSerializer: files round trip on disk")
{
	Testing::TemporaryDirectory directory;
	std::filesystem::path const path = directory.GetPath() / "Scenes" / "Main.sscene";
	Ref<Scene> const scene = MakeRichScene();
	REQUIRE(SceneSerializer::SaveToFile(*scene, path).IsOk());

	Result<Ref<Scene>> loaded = SceneSerializer::LoadFromFile(path, DeserializationContext{});
	REQUIRE(loaded.IsOk());
	CHECK(SceneSerializer::Serialize(*loaded.GetValue()) == SceneSerializer::Serialize(*scene));

	Result<Ref<Scene>> missing = SceneSerializer::LoadFromFile(directory.GetPath() / "Missing.sscene", DeserializationContext{});
	CHECK(missing.IsError());

	REQUIRE(FileSystem::WriteTextFile(directory.GetPath() / "Broken.sscene", "{ not json").IsOk());
	Result<Ref<Scene>> broken = SceneSerializer::LoadFromFile(directory.GetPath() / "Broken.sscene", DeserializationContext{});
	REQUIRE(broken.IsError());
	CHECK(broken.GetError().find("Broken.sscene") != std::string::npos);
}

TEST_CASE("SceneSerializer: structural errors are rejected")
{
	CHECK(SceneSerializer::Deserialize(Json::object(), DeserializationContext{}).IsError());

	Json duplicate = DocumentWithEntities(Json::array({EntityJson("5"), EntityJson("5")}));
	CHECK(SceneSerializer::Deserialize(duplicate, DeserializationContext{}).GetError() == "Entities[1]: duplicate entity ID 5");

	Json missingId = DocumentWithEntities(Json::array({Json::object({{"Components", Json::object()}})}));
	CHECK(SceneSerializer::Deserialize(missingId, DeserializationContext{}).GetError() == "Entities[0] has no \"ID\"");

	Json zeroId = DocumentWithEntities(Json::array({EntityJson("0")}));
	CHECK(SceneSerializer::Deserialize(zeroId, DeserializationContext{}).IsError());

	Json badComponent =
		DocumentWithEntities(Json::array({EntityJson("7", Json::object({{"PointLight", Json::object({{"Range", "far"}})}}))}));
	CHECK(SceneSerializer::Deserialize(badComponent, DeserializationContext{}).GetError() ==
	      "entity 7: PointLight.Range: expected a number");

	Json newer = DocumentWithEntities(Json::array());
	newer["Strada"]["Version"] = 99;
	CHECK(SceneSerializer::Deserialize(newer, DeserializationContext{}).IsError());
}

TEST_CASE("SceneSerializer: unknown components follow the configured policy")
{
	Json document = DocumentWithEntities(Json::array({EntityJson("8", Json::object({{"Teleporter", Json::object()}}))}));
	CHECK(SceneSerializer::Deserialize(document, DeserializationContext{}).GetError() == "entity 8: unknown component 'Teleporter'");

	std::vector<std::string> warnings;
	DeserializationContext lenient;
	lenient.UnknownFields = UnknownFieldPolicy::Warn;
	lenient.Warnings = &warnings;
	Result<Ref<Scene>> loaded = SceneSerializer::Deserialize(document, lenient);
	REQUIRE(loaded.IsOk());
	CHECK(loaded.GetValue()->GetEntityCount() == 1);
	REQUIRE(warnings.size() == 1);
	CHECK(warnings[0] == "ignored unknown component 'Teleporter'");
}

TEST_CASE("SceneSerializer: inconsistent hierarchies are repaired with warnings")
{
	std::vector<std::string> warnings;
	DeserializationContext context;
	context.Warnings = &warnings;

	// 1 -> missing parent; 2 <-> 3 form a cycle; 4 is a child of 5 but 5 does not list it.
	Json document =
		DocumentWithEntities(Json::array({EntityJson("1", WithParent("999")), EntityJson("2", WithParent("3")),
	                                      EntityJson("3", WithParent("2")), EntityJson("5"), EntityJson("4", WithParent("5"))}));
	Result<Ref<Scene>> loaded = SceneSerializer::Deserialize(document, context);
	REQUIRE_MESSAGE(loaded.IsOk(), loaded.GetError());
	Scene& scene = *loaded.GetValue();

	CHECK_FALSE(scene.GetParent(scene.GetEntityByUUID(UUID(1))));
	Entity const four = scene.GetEntityByUUID(UUID(4));
	CHECK(scene.GetParent(four).GetUUID() == UUID(5));
	REQUIRE(scene.GetChildren(scene.GetEntityByUUID(UUID(5))).size() == 1);

	// The cycle is broken: at least one of the pair became a root, and no entity is its own ancestor.
	Entity const two = scene.GetEntityByUUID(UUID(2));
	Entity const three = scene.GetEntityByUUID(UUID(3));
	CHECK_FALSE((scene.IsDescendantOf(two, three) && scene.IsDescendantOf(three, two)));
	CHECK(warnings.size() >= 2);

	// Every entity is reachable exactly once from the roots.
	size_t visited = 0;
	scene.ForEachEntityInHierarchyOrder(
		[&visited](Entity)
		{
			visited++;
		});
	CHECK(visited == scene.GetEntityCount());
}

namespace
{
	Json SerializeSubtree(Scene const& scene, Entity root)
	{
		Json entities = Json::array();
		for (entt::entity const handle : SceneSerializer::CollectHierarchy(scene, {root.GetUUID()}))
		{
			entities.push_back(SceneSerializer::SerializeEntity(scene, handle));
		}
		return entities;
	}
}

TEST_CASE("SceneSerializer: entity subtrees are restored with their original IDs, links and sibling order")
{
	Ref<Scene> const scene = MakeRichScene();
	Entity player = scene->FindEntityByName("Player");
	Entity shield = scene->CreateEntity("Shield", player);
	shield.GetComponent<TransformComponent>().SetRotationEuler({10.0f, 20.0f, 30.0f});
	Json const before = SceneSerializer::Serialize(*scene);

	SUBCASE("root entity")
	{
		Json const subtree = SerializeSubtree(*scene, player);
		REQUIRE(subtree.size() == 3);
		size_t const index = scene->GetSiblingIndex(player);
		scene->DestroyEntity(player);
		REQUIRE(scene->GetEntityCount() == 2);

		Result<Entity> restored = SceneSerializer::DeserializeEntityHierarchy(*scene, subtree, Entity(), index, DeserializationContext{});
		REQUIRE_MESSAGE(restored.IsOk(), restored.GetError());
		CHECK(restored.GetValue().GetName() == "Player");
		CHECK(SceneSerializer::Serialize(*scene) == before);
	}

	SUBCASE("child entity")
	{
		Entity weapon = scene->FindEntityByName("Weapon");
		Json const subtree = SerializeSubtree(*scene, weapon);
		scene->DestroyEntity(weapon);
		REQUIRE(scene->GetChildren(player).size() == 1);

		Result<Entity> restored = SceneSerializer::DeserializeEntityHierarchy(*scene, subtree, player, 0, DeserializationContext{});
		REQUIRE(restored.IsOk());
		CHECK(scene->GetParent(restored.GetValue()) == player);
		CHECK(SceneSerializer::Serialize(*scene) == before);
	}

	SUBCASE("sibling index past the end appends")
	{
		Json const subtree = SerializeSubtree(*scene, shield);
		scene->DestroyEntity(shield);
		Result<Entity> restored = SceneSerializer::DeserializeEntityHierarchy(*scene, subtree, player, 1000, DeserializationContext{});
		REQUIRE(restored.IsOk());
		CHECK(scene->GetSiblingIndex(restored.GetValue()) == 1);
		CHECK(SceneSerializer::Serialize(*scene) == before);
	}
}

TEST_CASE("SceneSerializer: restoring an entity subtree fails without side effects")
{
	Ref<Scene> const scene = MakeRichScene();
	Entity const player = scene->FindEntityByName("Player");
	Json const subtree = SerializeSubtree(*scene, player);
	Json const before = SceneSerializer::Serialize(*scene);

	// The IDs are still in use.
	Result<Entity> duplicate = SceneSerializer::DeserializeEntityHierarchy(*scene, subtree, Entity(), 0, DeserializationContext{});
	REQUIRE(duplicate.IsError());
	CHECK(duplicate.GetError() == fmt::format("an entity with ID {} already exists", player.GetUUID()));

	Json broken = Json::array({EntityJson("71"), EntityJson("72", Json::object({{"PointLight", Json::object({{"Range", true}})}}))});
	Result<Entity> invalid = SceneSerializer::DeserializeEntityHierarchy(*scene, broken, Entity(), 0, DeserializationContext{});
	REQUIRE(invalid.IsError());
	CHECK(invalid.GetError() == "entity 72: PointLight.Range: expected a number");

	CHECK(SceneSerializer::DeserializeEntityHierarchy(*scene, Json::array(), Entity(), 0, DeserializationContext{}).IsError());
	CHECK(SceneSerializer::DeserializeEntityHierarchy(*scene, Json::object(), Entity(), 0, DeserializationContext{}).IsError());

	Scene other;
	Entity const foreignParent = other.CreateEntity("Foreign");
	Json const single = Json::array({EntityJson("73")});
	CHECK(SceneSerializer::DeserializeEntityHierarchy(*scene, single, foreignParent, 0, DeserializationContext{}).IsError());

	CHECK(SceneSerializer::Serialize(*scene) == before);
}

TEST_CASE("SceneSerializer: restored descendants with parents outside the subtree attach to its root")
{
	Scene scene;
	// 82 claims a parent that is not part of the subtree; 81 is the root even though it names a parent.
	Json const subtree =
		Json::array({EntityJson("81", WithParent("999")), EntityJson("82", WithParent("555")), EntityJson("83", WithParent("81"))});
	Result<Entity> restored = SceneSerializer::DeserializeEntityHierarchy(scene, subtree, Entity(), 0, DeserializationContext{});
	REQUIRE_MESSAGE(restored.IsOk(), restored.GetError());

	Entity const root = restored.GetValue();
	CHECK(root.GetUUID() == UUID(81));
	CHECK_FALSE(scene.GetParent(root));
	std::vector<Entity> const children = scene.GetChildren(root);
	REQUIRE(children.size() == 2);
	CHECK(children[0].GetUUID() == UUID(83));
	CHECK(children[1].GetUUID() == UUID(82));
	CHECK(scene.GetRootEntities().size() == 1);
}

TEST_CASE("SceneSerializer: scene settings update partially and follow the unknown-key policy")
{
	SceneSettings settings;
	REQUIRE(SceneSerializer::DeserializeSettings(Json::object(), settings, DeserializationContext{}).IsOk());
	CHECK(settings.Gravity == SceneSettings().Gravity);

	Json const gravity = Json::parse(R"({ "Physics": { "Gravity": [0, -3, 0] } })");
	REQUIRE(SceneSerializer::DeserializeSettings(gravity, settings, DeserializationContext{}).IsOk());
	CHECK(settings.Gravity == glm::vec3(0.0f, -3.0f, 0.0f));
	CHECK(SceneSerializer::SerializeSettings(settings)["Physics"] == gravity["Physics"]);
	CHECK(SceneSerializer::SerializeSettings(settings).contains("Renderer"));

	Json const typo = Json::parse(R"({ "Physics": { "Gravity": [0, -1, 0], "Gravty": [0, 1, 0] } })");
	Result<void> const rejected = SceneSerializer::DeserializeSettings(typo, settings, DeserializationContext{});
	REQUIRE(rejected.IsError());
	CHECK(rejected.GetError() == "unknown scene setting 'Physics.Gravty'");
	CHECK(settings.Gravity == glm::vec3(0.0f, -3.0f, 0.0f));

	Json const invalid = Json::parse(R"({ "Physics": { "Gravity": [0, "down", 0] } })");
	CHECK(SceneSerializer::DeserializeSettings(invalid, settings, DeserializationContext{}).GetError() ==
	      "Scene.Settings.Physics.Gravity: expected an array of 3 finite numbers");
	CHECK(SceneSerializer::DeserializeSettings(Json::array(), settings, DeserializationContext{}).IsError());

	std::vector<std::string> warnings;
	DeserializationContext lenient;
	lenient.UnknownFields = UnknownFieldPolicy::Warn;
	lenient.Warnings = &warnings;
	Json const newer = Json::parse(R"({ "Weather": { "Rain": 1 }, "Physics": { "Gravity": [0, -9, 0] } })");
	REQUIRE(SceneSerializer::DeserializeSettings(newer, settings, lenient).IsOk());
	CHECK(settings.Gravity == glm::vec3(0.0f, -9.0f, 0.0f));
	REQUIRE(warnings.size() == 1);
	CHECK(warnings[0] == "ignored unknown scene setting 'Weather'");
}

TEST_CASE("Prefab: instances get fresh IDs, remapped references and prefab links")
{
	Ref<Scene> const source = MakeRichScene();
	Entity player = source->FindEntityByName("Player");
	ScriptComponent& script = player.GetComponent<ScriptComponent>();
	Entity const weapon = source->GetChildren(player)[0];
	script.Fields["Weapon"] = ScriptFieldValue::FromEntity(weapon.GetUUID());

	Json const prefab = PrefabSerializer::Serialize(*source, player);
	CHECK(prefab["Strada"]["Type"] == "Prefab");
	REQUIRE(prefab["Entities"].size() == 2);
	CHECK(prefab["Entities"][0]["Components"]["Relationship"]["Parent"] == "0");

	Scene scene;
	Entity const anchor = scene.CreateEntity("Anchor");
	AssetHandle const prefabHandle(UUID(5000));

	Result<Entity> first = PrefabSerializer::Instantiate(scene, prefab, prefabHandle, anchor, DeserializationContext{});
	Result<Entity> second = PrefabSerializer::Instantiate(scene, prefab, prefabHandle, Entity(), DeserializationContext{});
	REQUIRE_MESSAGE(first.IsOk(), first.GetError());
	REQUIRE(second.IsOk());

	Entity const instance = first.GetValue();
	CHECK(instance.GetUUID() != player.GetUUID());
	CHECK(instance.GetUUID() != second.GetValue().GetUUID());
	CHECK(scene.GetParent(instance) == anchor);
	CHECK_FALSE(scene.GetParent(second.GetValue()));
	CHECK(scene.GetEntityCount() == 5);

	PrefabComponent const& link = instance.GetComponent<PrefabComponent>();
	CHECK(link.Prefab == prefabHandle);
	CHECK(link.SourceEntity == player.GetUUID());

	std::vector<Entity> const children = scene.GetChildren(instance);
	REQUIRE(children.size() == 1);
	CHECK(children[0].GetComponent<PrefabComponent>().SourceEntity == weapon.GetUUID());

	// Internal references follow the instance; external ones are kept.
	ScriptComponent const& instanceScript = instance.GetComponent<ScriptComponent>();
	CHECK(instanceScript.Fields.at("Weapon").GetEntity() == children[0].GetUUID());
	CHECK(instanceScript.Fields.at("Camera").GetEntity() == source->FindEntityByName("Camera").GetUUID());
}

TEST_CASE("Prefab: failed instantiation creates nothing")
{
	Json prefab = Json::object();
	prefab["Strada"] = MakeFileHeader("Prefab", 1);
	prefab["Entities"] = Json::array({EntityJson("1"), EntityJson("2", Json::object({{"PointLight", Json::object({{"Range", true}})}}))});

	Scene scene;
	Result<Entity> const result = PrefabSerializer::Instantiate(scene, prefab, AssetHandle(), Entity(), DeserializationContext{});
	REQUIRE(result.IsError());
	CHECK(result.GetError() == "prefab entity 2: PointLight.Range: expected a number");
	CHECK(scene.GetEntityCount() == 0);
	CHECK(scene.GetRootEntities().empty());

	prefab["Entities"] = Json::array();
	CHECK(PrefabSerializer::Instantiate(scene, prefab, AssetHandle(), Entity(), DeserializationContext{}).IsError());
	prefab["Strada"]["Type"] = "Scene";
	CHECK(PrefabSerializer::Instantiate(scene, prefab, AssetHandle(), Entity(), DeserializationContext{}).IsError());
}
