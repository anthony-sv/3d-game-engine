#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/BuiltInAssets.h"
#include "Strada/Asset/MaterialAsset.h"
#include "Strada/Asset/PrefabAsset.h"
#include "Strada/Math/Math.h"
#include "Strada/Project/ProjectSettings.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Scene/SceneSerializer.h"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>

using namespace Strada;

namespace
{
	constexpr float Step = 1.0f / 60.0f;

	Entity AddGround(Scene& scene)
	{
		Entity ground = scene.CreateEntity("Ground");
		ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
		ground.AddComponent<BoxColliderComponent>().HalfExtents = {10.0f, 0.5f, 10.0f};
		return ground;
	}

	Entity AddBall(Scene& scene, glm::vec3 const& position, Entity parent = {})
	{
		Entity ball = parent ? scene.CreateEntity("Ball", parent) : scene.CreateEntity("Ball");
		ball.GetComponent<TransformComponent>().Translation = position;
		ball.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		ball.AddComponent<SphereColliderComponent>().Radius = 0.5f;
		return ball;
	}

	std::vector<ContactEvent> Run(Scene& scene, float seconds)
	{
		std::vector<ContactEvent> events;
		for (float time = 0.0f; time < seconds; time += Step)
		{
			scene.OnUpdateRuntime(Timestep(Step));
			events.insert(events.end(), scene.GetContactEvents().begin(), scene.GetContactEvents().end());
		}
		return events;
	}

	glm::vec3 WorldPosition(Scene& scene, Entity entity)
	{
		return glm::vec3(scene.GetWorldTransform(entity)[3]);
	}
}

TEST_CASE("Scene: the runtime simulates physics and writes transforms back in parent space")
{
	Testing::PhysicsSystemScope physics;
	Scene scene("Physics");
	Entity const ground = AddGround(scene);
	Entity holder = scene.CreateEntity("Holder");
	holder.GetComponent<TransformComponent>().Translation = {0.0f, 2.0f, 0.0f};
	Entity const ball = AddBall(scene, {0.0f, 3.0f, 0.0f}, holder);
	// A RigidBody without colliders is not simulated (and does not fail).
	scene.CreateEntity("Empty").AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;

	scene.OnRuntimeStart();
	PhysicsScene* world = scene.GetPhysicsScene();
	REQUIRE(world != nullptr);
	CHECK(world->GetBodyCount() == 2);
	CHECK(world->HasBody(ground.GetUUID()));

	std::vector<ContactEvent> const events = Run(scene, 3.0f);
	CHECK(WorldPosition(scene, ball).y == doctest::Approx(0.5f).epsilon(0.05));
	// The parent did not move: the ball's local translation holds the difference.
	CHECK(ball.GetComponent<TransformComponent>().Translation.y == doctest::Approx(-1.5f).epsilon(0.05));
	CHECK(WorldPosition(scene, holder).y == doctest::Approx(2.0f));
	CHECK(Testing::HasEvent(events, ContactEventType::CollisionEnter, ground.GetUUID(), ball.GetUUID()));

	scene.OnRuntimeStop();
	CHECK(scene.GetPhysicsScene() == nullptr);
	CHECK(scene.GetContactEvents().empty());
}

TEST_CASE("Scene: bodies follow component changes, transform edits and destruction while running")
{
	Testing::PhysicsSystemScope physics;
	Testing::AssetManagerScope assets;
	Scene scene("Physics");
	Entity ball = AddBall(scene, {0.0f, 3.0f, 0.0f});
	Entity mover = scene.CreateEntity("Mover");
	mover.GetComponent<TransformComponent>().Translation = {5.0f, 1.0f, 0.0f};
	mover.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
	mover.AddComponent<BoxColliderComponent>();
	scene.OnRuntimeStart();
	PhysicsScene* world = scene.GetPhysicsScene();
	REQUIRE(world != nullptr);

	// A floor added while running is picked up before the next step.
	Entity const ground = AddGround(scene);
	Run(scene, 2.0f);
	CHECK(world->HasBody(ground.GetUUID()));
	CHECK(WorldPosition(scene, ball).y == doctest::Approx(0.5f).epsilon(0.05));

	// Kinematic bodies follow their transforms.
	mover.GetComponent<TransformComponent>().Translation.x = 6.0f;
	Run(scene, 0.1f);
	CHECK(world->GetBodyTransform(mover.GetUUID())->Position.x == doctest::Approx(6.0f).epsilon(0.001));

	// Moving a dynamic body's transform teleports it.
	ball.GetComponent<TransformComponent>().Translation = {-4.0f, 5.0f, 0.0f};
	scene.OnUpdateRuntime(Timestep(Step));
	glm::vec3 const teleported = world->GetBodyTransform(ball.GetUUID())->Position;
	CHECK(teleported.x == doctest::Approx(-4.0f).epsilon(0.001));
	CHECK(teleported.y > 4.9f);

	// Changing a collider rebuilds the body (it keeps falling where it was).
	SphereColliderComponent larger;
	larger.Radius = 2.0f;
	ball.AddOrReplaceComponent<SphereColliderComponent>(larger);
	Run(scene, 2.0f);
	CHECK(WorldPosition(scene, ball).y == doctest::Approx(2.0f).epsilon(0.05));

	// Removing the colliders removes the body; destroying an entity removes its body.
	mover.RemoveComponent<BoxColliderComponent>();
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK_FALSE(world->HasBody(mover.GetUUID()));
	UUID const groundId = ground.GetUUID();
	scene.DestroyEntity(ground);
	scene.OnUpdateRuntime(Timestep(Step));
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK_FALSE(world->HasBody(groundId));
	CHECK(world->GetBodyCount() == 1);

	// Mesh colliders use the entity's mesh when they have none of their own.
	Entity block = scene.CreateEntity("Block");
	block.GetComponent<TransformComponent>().Translation = {20.0f, 0.0f, 0.0f};
	block.AddComponent<MeshComponent>().Mesh = GetBuiltInHandle(BuiltInAsset::CubeMesh);
	block.AddComponent<MeshColliderComponent>();
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK(world->HasBody(block.GetUUID()));
	std::optional<RaycastHit> const hit = world->Raycast({20.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 100.0f);
	REQUIRE(hit);
	CHECK(hit->Entity == block.GetUUID());
	CHECK(hit->Point.y == doctest::Approx(0.5f).epsilon(0.01));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: project physics settings shape the runtime")
{
	Testing::PhysicsSystemScope physics;
	ProjectSettings project;
	project.Physics.FixedTimestep = 1.0f / 30.0f;
	project.Physics.Layers = {"Default", "Ghosts"};
	project.Physics.IgnoredCollisions = {{0, 1}};
	SceneRuntimeSettings const runtime = MakeSceneRuntimeSettings(project);
	CHECK(runtime.FixedTimestep == doctest::Approx(1.0f / 30.0f));
	CHECK(runtime.PhysicsLayerCount == 2);
	REQUIRE(runtime.IgnoredCollisions.size() == 1);
	CHECK(runtime.IgnoredCollisions[0] == glm::uvec2(0, 1));

	Scene scene("Layers");
	scene.GetSettings().Physics.Gravity = {0.0f, -20.0f, 0.0f};
	AddGround(scene);
	Entity ghost = AddBall(scene, {0.0f, 2.0f, 0.0f});
	ghost.GetComponent<RigidBodyComponent>().Layer = 1;
	scene.OnRuntimeStart(runtime);
	REQUIRE(scene.GetPhysicsScene() != nullptr);
	CHECK(scene.GetPhysicsScene()->GetGravity().y == doctest::Approx(-20.0f));
	Run(scene, 1.0f);
	// Its layer ignores the ground's.
	CHECK(WorldPosition(scene, ghost).y < -1.0f);

	// One frame of a 30 Hz step runs one step: frames shorter than a step accumulate.
	scene.OnRuntimeStop();
	scene.OnRuntimeStart(runtime);
	glm::vec3 const before = WorldPosition(scene, ghost);
	scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
	CHECK(WorldPosition(scene, ghost) == before);
	scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
	CHECK(WorldPosition(scene, ghost).y < before.y);
}

TEST_CASE("Scene: without the physics system the runtime runs without physics")
{
	REQUIRE_FALSE(PhysicsSystem::IsInitialized());
	Scene scene("No physics");
	Entity const ball = AddBall(scene, {0.0f, 3.0f, 0.0f});
	scene.OnRuntimeStart();
	CHECK(scene.GetPhysicsScene() == nullptr);
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK(WorldPosition(scene, ball).y == doctest::Approx(3.0f));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: prefabs instantiate at a world pose under their parent")
{
	Testing::AssetManagerScope assets;
	Scene source("Source");
	Entity crate = source.CreateEntity("Crate");
	crate.GetComponent<TransformComponent>().Scale = {1.0f, 2.0f, 1.0f};
	source.CreateEntity("Lid", crate);
	Result<Ref<PrefabAsset>> prefabAsset = PrefabAsset::Create(PrefabSerializer::Serialize(source, crate));
	REQUIRE(prefabAsset.IsOk());
	AssetHandle const prefab = AssetManager::AddMemoryAsset(prefabAsset.GetValue(), "Crate");

	Scene scene("Scene");
	Entity parent = scene.CreateEntity("Parent");
	TransformComponent& parentTransform = parent.GetComponent<TransformComponent>();
	parentTransform.Translation = {5.0f, 0.0f, 0.0f};
	parentTransform.Rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
	parentTransform.Scale = glm::vec3(3.0f);
	glm::quat const turn = glm::angleAxis(glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));

	Result<Entity> const instance = scene.InstantiatePrefab(prefab, {1.0f, 2.0f, 3.0f}, turn, parent);
	REQUIRE(instance.IsOk());
	Entity const root = instance.GetValue();
	CHECK(scene.GetParent(root) == parent);
	CHECK(scene.GetChildren(root).size() == 1);
	glm::vec3 position;
	glm::quat rotation;
	glm::vec3 scale;
	REQUIRE(Math::DecomposeTransform(scene.GetWorldTransform(root), position, rotation, scale));
	CHECK(glm::length(position - glm::vec3(1.0f, 2.0f, 3.0f)) < 1e-4f);
	CHECK(glm::abs(glm::dot(rotation, turn)) > 1.0f - 1e-5f);
	// The prefab's own scale stays (under the parent's).
	CHECK(glm::length(root.GetComponent<TransformComponent>().Scale - glm::vec3(1.0f, 2.0f, 1.0f)) < 1e-4f);

	CHECK(scene.InstantiatePrefab(GetBuiltInHandle(BuiltInAsset::CubeMesh), glm::vec3(0.0f), turn, Entity()).IsError());
	CHECK(scene.InstantiatePrefab(AssetHandle(UUID(12345)), glm::vec3(0.0f), turn, Entity()).IsError());
	CHECK(scene.GetEntityCount() == 3);
}

TEST_CASE("Scene: debug lines last their duration and clear when the runtime stops")
{
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Lines");
	scene.OnRuntimeStart();
	scene.DrawDebugLine(glm::vec3(0.0f), {1.0f, 0.0f, 0.0f}, glm::vec4(1.0f));
	scene.DrawDebugLine(glm::vec3(0.0f), {0.0f, 1.0f, 0.0f}, glm::vec4(1.0f), 0.25f);
	scene.DrawDebugLine(glm::vec3(0.0f), {0.0f, 0.0f, 1.0f}, glm::vec4(1.0f), -3.0f);
	CHECK(scene.GetDebugLines().size() == 3);
	// Lines without a duration were rendered once.
	scene.OnUpdateRuntime(Timestep(0.1f));
	REQUIRE(scene.GetDebugLines().size() == 1);
	CHECK(scene.GetDebugLines().front().To == glm::vec3(0.0f, 1.0f, 0.0f));

	// Frozen time keeps lines with a duration; one-frame lines still last one frame.
	scene.SetTimeScale(0.0f);
	scene.DrawDebugLine(glm::vec3(0.0f), {1.0f, 1.0f, 0.0f}, glm::vec4(1.0f));
	scene.OnUpdateRuntime(Timestep(0.1f));
	CHECK(scene.GetDebugLines().size() == 1);
	scene.SetTimeScale(1.0f);
	scene.OnUpdateRuntime(Timestep(0.2f));
	CHECK(scene.GetDebugLines().empty());

	for (size_t line = 0; line < Scene::MaxDebugLines + 10; line++)
	{
		scene.DrawDebugLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec4(1.0f), 5.0f);
	}
	CHECK(scene.GetDebugLines().size() == Scene::MaxDebugLines);
	CHECK(Testing::WasLogged(logStart, "more are dropped until some expire"));
	scene.OnRuntimeStop();
	CHECK(scene.GetDebugLines().empty());
	// Only running scenes draw them.
	scene.DrawDebugLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec4(1.0f), 5.0f);
	CHECK(scene.GetDebugLines().empty());
}

TEST_CASE("Scene: runtime assets are released when the runtime stops or the scene goes")
{
	Testing::AssetManagerScope assets;
	AssetHandle const first = AssetManager::AddMemoryAsset(CreateRef<MaterialAsset>(), "First");
	AssetHandle const second = AssetManager::AddMemoryAsset(CreateRef<MaterialAsset>(), "Second");
	AssetHandle const kept = AssetManager::AddMemoryAsset(CreateRef<MaterialAsset>(), "Kept");
	{
		Scene scene("Owner");
		scene.OnRuntimeStart();
		scene.AddRuntimeAsset(first);
		CHECK(scene.IsRuntimeAsset(first));
		CHECK_FALSE(scene.IsRuntimeAsset(kept));
		scene.OnRuntimeStop();
		CHECK_FALSE(AssetManager::IsValid(first));

		scene.OnRuntimeStart();
		scene.AddRuntimeAsset(second);
	}
	CHECK_FALSE(AssetManager::IsValid(second));
	CHECK(AssetManager::IsValid(kept));
}
