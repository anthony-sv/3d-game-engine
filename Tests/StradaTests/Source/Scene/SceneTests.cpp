#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <string>
#include <vector>

using namespace Strada;

namespace
{
	std::vector<std::string> Names(std::vector<Entity> const& entities)
	{
		std::vector<std::string> names;
		for (Entity const& entity : entities)
		{
			names.push_back(entity.GetName());
		}
		return names;
	}

	bool NearlyEqual(glm::vec3 const& a, glm::vec3 const& b, float epsilon = 1e-4f)
	{
		return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(epsilon)));
	}
}

TEST_CASE("Scene: new entities have core components and unique IDs")
{
	Scene scene("Test");
	CHECK(scene.GetName() == "Test");

	Entity const first = scene.CreateEntity("First");
	Entity const second = scene.CreateEntity();
	REQUIRE(first);
	CHECK(first.HasComponent<IDComponent, TagComponent, TransformComponent, RelationshipComponent>());
	CHECK(first.GetName() == "First");
	CHECK(second.GetName() == "Entity");
	CHECK(first.GetUUID() != second.GetUUID());
	CHECK(first.GetUUID().IsValid());
	CHECK(scene.GetEntityCount() == 2);
	CHECK(scene.HasEntity(first.GetUUID()));
	CHECK(scene.GetEntityByUUID(first.GetUUID()) == first);
	CHECK_FALSE(scene.GetEntityByUUID(UUID(12345)));

	Entity const named = scene.CreateEntityWithUUID(UUID(42), "");
	CHECK(named.GetUUID() == UUID(42));
	CHECK(named.GetName() == "Entity");
}

TEST_CASE("Scene: components can be added, queried and removed")
{
	Scene scene;
	Entity entity = scene.CreateEntity();
	CHECK_FALSE(entity.HasComponent<PointLightComponent>());
	CHECK(entity.TryGetComponent<PointLightComponent>() == nullptr);

	entity.AddComponent<PointLightComponent>().Range = 5.0f;
	CHECK(entity.HasComponent<PointLightComponent>());
	CHECK(entity.GetComponent<PointLightComponent>().Range == doctest::Approx(5.0f));
	entity.AddOrReplaceComponent<PointLightComponent>().Range = 7.0f;
	CHECK(entity.GetComponent<PointLightComponent>().Range == doctest::Approx(7.0f));

	int lights = 0;
	for ([[maybe_unused]] entt::entity const handle : scene.GetAllEntitiesWith<PointLightComponent>())
	{
		lights++;
	}
	CHECK(lights == 1);

	entity.RemoveComponent<PointLightComponent>();
	CHECK_FALSE(entity.HasComponent<PointLightComponent>());
}

TEST_CASE("Scene: destroying an entity destroys its descendants and invalidates handles")
{
	Scene scene;
	Entity const parent = scene.CreateEntity("Parent");
	Entity const child = scene.CreateEntity("Child", parent);
	Entity const grandchild = scene.CreateEntity("Grandchild", child);
	Entity const other = scene.CreateEntity("Other");

	scene.DestroyEntity(child);
	CHECK_FALSE(child.IsValid());
	CHECK_FALSE(grandchild.IsValid());
	CHECK(parent.IsValid());
	CHECK(scene.GetChildren(parent).empty());
	CHECK(scene.GetEntityCount() == 2);
	CHECK(Names(scene.GetRootEntities()) == std::vector<std::string>{"Parent", "Other"});

	// Destroying invalid or foreign entities is a no-op.
	scene.DestroyEntity(child);
	Scene otherScene;
	otherScene.DestroyEntity(other);
	CHECK(other.IsValid());
}

TEST_CASE("Scene: hierarchy ordering, reparenting and cycle prevention")
{
	Scene scene;
	Entity const a = scene.CreateEntity("A");
	Entity const b = scene.CreateEntity("B");
	Entity const c = scene.CreateEntity("C");

	REQUIRE(scene.SetParent(b, a).IsOk());
	REQUIRE(scene.SetParent(c, a).IsOk());
	CHECK(Names(scene.GetRootEntities()) == std::vector<std::string>{"A"});
	CHECK(Names(scene.GetChildren(a)) == std::vector<std::string>{"B", "C"});
	CHECK(scene.GetParent(b) == a);
	CHECK_FALSE(scene.GetParent(a));
	CHECK(scene.IsDescendantOf(c, a));
	CHECK_FALSE(scene.IsDescendantOf(a, c));

	CHECK(scene.SetParent(a, c).GetError() == "cannot parent 'A' to its descendant 'C'");
	CHECK(scene.SetParent(a, a).IsError());

	scene.SetSiblingIndex(c, 0);
	CHECK(Names(scene.GetChildren(a)) == std::vector<std::string>{"C", "B"});
	CHECK(scene.GetSiblingIndex(b) == 1);
	scene.SetSiblingIndex(c, 99);
	CHECK(Names(scene.GetChildren(a)) == std::vector<std::string>{"B", "C"});

	REQUIRE(scene.SetParent(b, Entity()).IsOk());
	CHECK(Names(scene.GetRootEntities()) == std::vector<std::string>{"A", "B"});

	Scene otherScene;
	Entity const foreign = otherScene.CreateEntity();
	CHECK(scene.SetParent(b, foreign).IsError());
}

TEST_CASE("Scene: world transforms compose through the hierarchy")
{
	Scene scene;
	Entity parent = scene.CreateEntity("Parent");
	Entity child = scene.CreateEntity("Child", parent);

	parent.GetComponent<TransformComponent>().Translation = {10.0f, 0.0f, 0.0f};
	parent.GetComponent<TransformComponent>().SetRotationEuler({0.0f, 90.0f, 0.0f});
	parent.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f);
	child.GetComponent<TransformComponent>().Translation = {1.0f, 0.0f, 0.0f};

	glm::vec3 const childWorld = glm::vec3(scene.GetWorldTransform(child)[3]);
	// Parent rotates +X onto -Z and doubles distances.
	CHECK(NearlyEqual(childWorld, {10.0f, 0.0f, -2.0f}));

	scene.SetWorldTransform(child, glm::translate(glm::mat4(1.0f), {0.0f, 5.0f, 0.0f}));
	CHECK(NearlyEqual(glm::vec3(scene.GetWorldTransform(child)[3]), {0.0f, 5.0f, 0.0f}));
}

TEST_CASE("Scene: reparenting keeps the world transform when requested")
{
	Scene scene;
	Entity parent = scene.CreateEntity("Parent");
	parent.GetComponent<TransformComponent>().Translation = {5.0f, 0.0f, 0.0f};
	Entity child = scene.CreateEntity("Child");
	child.GetComponent<TransformComponent>().Translation = {1.0f, 2.0f, 3.0f};

	REQUIRE(scene.SetParent(child, parent, true).IsOk());
	CHECK(NearlyEqual(glm::vec3(scene.GetWorldTransform(child)[3]), {1.0f, 2.0f, 3.0f}));
	CHECK(NearlyEqual(child.GetComponent<TransformComponent>().Translation, {-4.0f, 2.0f, 3.0f}));

	REQUIRE(scene.SetParent(child, Entity(), false).IsOk());
	CHECK(NearlyEqual(child.GetComponent<TransformComponent>().Translation, {-4.0f, 2.0f, 3.0f}));
}

TEST_CASE("Scene: duplication copies the subtree with new IDs and remaps internal references")
{
	Scene scene;
	Entity root = scene.CreateEntity("Root");
	Entity child = scene.CreateEntity("Child", root);
	Entity outside = scene.CreateEntity("Outside");
	scene.CreateEntity("After");

	ScriptComponent& script = root.AddComponent<ScriptComponent>();
	script.ClassName = "Game.Turret";
	script.Fields["Barrel"] = ScriptFieldValue::FromEntity(child.GetUUID());
	script.Fields["Target"] = ScriptFieldValue::FromEntity(outside.GetUUID());
	child.AddComponent<PointLightComponent>().Range = 3.0f;

	Entity const copy = scene.DuplicateEntity(root);
	REQUIRE(copy);
	CHECK(copy.GetUUID() != root.GetUUID());
	CHECK(copy.GetName() == "Root");
	CHECK(Names(scene.GetRootEntities()) == std::vector<std::string>{"Root", "Root", "Outside", "After"});

	std::vector<Entity> const copyChildren = scene.GetChildren(copy);
	REQUIRE(copyChildren.size() == 1);
	Entity const copiedChild = copyChildren[0];
	CHECK(copiedChild.GetUUID() != child.GetUUID());
	CHECK(copiedChild.GetComponent<PointLightComponent>().Range == doctest::Approx(3.0f));
	CHECK(scene.GetParent(copiedChild) == copy);

	ScriptComponent const& copiedScript = copy.GetComponent<ScriptComponent>();
	CHECK(copiedScript.Fields.at("Barrel").GetEntity() == copiedChild.GetUUID());
	CHECK(copiedScript.Fields.at("Target").GetEntity() == outside.GetUUID());
	CHECK(scene.GetEntityCount() == 6);
}

TEST_CASE("Scene: copies are deep and independent")
{
	Scene source("Level");
	source.GetSettings().Physics.Gravity = {0.0f, -3.0f, 0.0f};
	Entity parent = source.CreateEntity("Parent");
	Entity child = source.CreateEntity("Child", parent);
	child.AddComponent<MeshComponent>().CastShadows = false;

	Ref<Scene> copy = Scene::Copy(source);
	CHECK(copy->GetName() == "Level");
	CHECK(copy->GetSettings().Physics.Gravity == glm::vec3(0.0f, -3.0f, 0.0f));
	CHECK(copy->GetEntityCount() == 2);

	Entity copiedChild = copy->GetEntityByUUID(child.GetUUID());
	REQUIRE(copiedChild);
	CHECK(copy->GetParent(copiedChild).GetUUID() == parent.GetUUID());
	CHECK_FALSE(copiedChild.GetComponent<MeshComponent>().CastShadows);

	copiedChild.GetComponent<TagComponent>().Tag = "Changed";
	copy->DestroyEntity(copy->GetEntityByUUID(parent.GetUUID()));
	CHECK(child.GetName() == "Child");
	CHECK(source.GetEntityCount() == 2);
}

TEST_CASE("Scene: name lookup and primary camera follow hierarchy order")
{
	Scene scene;
	Entity first = scene.CreateEntity("Camera");
	Entity second = scene.CreateEntity("Camera");
	CHECK(scene.FindEntityByName("Camera") == first);
	CHECK_FALSE(scene.FindEntityByName("Missing"));

	CHECK_FALSE(scene.GetPrimaryCameraEntity());
	second.AddComponent<CameraComponent>();
	first.AddComponent<CameraComponent>().Primary = false;
	CHECK(scene.GetPrimaryCameraEntity() == second);
	first.GetComponent<CameraComponent>().Primary = true;
	CHECK(scene.GetPrimaryCameraEntity() == first);
}

TEST_CASE("Scene: destruction is deferred to the end of the frame while running")
{
	Scene scene;
	Entity entity = scene.CreateEntity("Doomed");

	scene.OnRuntimeStart();
	CHECK(scene.IsRunning());
	scene.DestroyEntity(entity);
	scene.DestroyEntity(entity);
	CHECK(entity.IsValid());
	CHECK(scene.IsPendingDestruction(entity));

	scene.OnUpdateRuntime(1.0f / 60.0f);
	CHECK_FALSE(entity.IsValid());
	CHECK(scene.GetRuntimeFrame() == 1);

	Entity other = scene.CreateEntity("Other");
	scene.DestroyEntity(other);
	scene.OnRuntimeStop();
	CHECK_FALSE(other.IsValid());
	CHECK_FALSE(scene.IsRunning());
}

TEST_CASE("Scene: pausing stops runtime updates until stepped")
{
	Scene scene;
	scene.OnRuntimeStart();
	scene.SetPaused(true);
	scene.OnUpdateRuntime(0.5f);
	CHECK(scene.GetRuntimeFrame() == 0);

	scene.Step(2);
	scene.OnUpdateRuntime(0.5f);
	scene.OnUpdateRuntime(0.5f);
	scene.OnUpdateRuntime(0.5f);
	CHECK(scene.GetRuntimeFrame() == 2);
	CHECK(scene.GetRuntimeTime() == doctest::Approx(1.0));

	scene.SetPaused(false);
	scene.OnUpdateRuntime(0.5f);
	CHECK(scene.GetRuntimeFrame() == 3);
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: hierarchy traversal tolerates entities destroyed by the callback")
{
	Scene scene;
	Entity a = scene.CreateEntity("A");
	scene.CreateEntity("A1", a);
	scene.CreateEntity("B");

	std::vector<std::string> visited;
	scene.ForEachEntityInHierarchyOrder(
		[&](Entity entity)
		{
			visited.push_back(entity.GetName());
			if (entity.GetName() == "A")
			{
				scene.DestroyEntity(entity);
			}
		});
	CHECK(visited == std::vector<std::string>{"A", "B"});
	CHECK(scene.GetEntityCount() == 1);
}
