#include "Physics/PhysicsTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Project/ProjectSettings.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

using namespace Strada;

namespace
{
	constexpr float Step = 1.0f / 60.0f;

	// What the test scenes are made of; made in a given order, with fixed UUIDs, so two scenes differ only in that order.
	struct EntityRecipe
	{
		uint64_t ID = 0;
		void (*Make)(Entity entity) = nullptr;
	};

	void MakeGround(Entity entity)
	{
		entity.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
		entity.AddComponent<BoxColliderComponent>().HalfExtents = {20.0f, 0.5f, 20.0f};
	}

	void MakeCrate(Entity entity, glm::vec3 const& position)
	{
		entity.GetComponent<TransformComponent>().Translation = position;
		entity.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		entity.AddComponent<BoxColliderComponent>().HalfExtents = {0.5f, 0.5f, 0.5f};
	}

	// A stack the ball knocks over, a motorized paddle on a hinge, a pendulum on a distance joint, characters with colliders
	// of their own below them landing on a crate and on a platform that turns very slowly, and a trigger the ball rolls
	// through.
	std::vector<EntityRecipe> const& GetRecipes()
	{
		static std::vector<EntityRecipe> const s_Recipes = {
			{100, MakeGround},
			{201,
		     [](Entity entity)
		     {
				 MakeCrate(entity, {0.0f, 0.5f, 0.0f});
			 }},
			{202,
		     [](Entity entity)
		     {
				 MakeCrate(entity, {0.05f, 1.5f, 0.0f});
			 }},
			{203,
		     [](Entity entity)
		     {
				 MakeCrate(entity, {-0.05f, 2.5f, 0.02f});
			 }},
			{204,
		     [](Entity entity)
		     {
				 MakeCrate(entity, {0.0f, 3.5f, -0.03f});
			 }},
			{300,
		     [](Entity entity)
		     {
				 entity.GetComponent<TransformComponent>().Translation = {-6.0f, 0.5f, 0.1f};
				 RigidBodyComponent& body = entity.AddComponent<RigidBodyComponent>();
				 body.Type = RigidBodyType::Dynamic;
				 body.Mass = 5.0f;
				 body.InitialLinearVelocity = {8.0f, 0.0f, 0.0f};
				 entity.AddComponent<SphereColliderComponent>().Radius = 0.5f;
			 }},
			{400,
		     [](Entity entity)
		     {
				 entity.GetComponent<TransformComponent>().Translation = {4.0f, 1.0f, 4.0f};
				 entity.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
				 entity.AddComponent<BoxColliderComponent>().HalfExtents = {1.5f, 0.1f, 0.3f};
				 JointComponent& joint = entity.AddComponent<JointComponent>();
				 joint.Type = JointType::Hinge;
				 joint.Axis = {0.0f, 1.0f, 0.0f};
				 joint.MotorEnabled = true;
				 joint.MotorSpeed = 120.0f;
			 }},
			{500,
		     [](Entity entity)
		     {
				 entity.GetComponent<TransformComponent>().Translation = {-4.0f, 3.0f, -4.0f};
				 entity.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
				 entity.AddComponent<SphereColliderComponent>().Radius = 0.3f;
				 JointComponent& joint = entity.AddComponent<JointComponent>();
				 joint.Type = JointType::Distance;
				 joint.ConnectedAnchor = {-2.0f, 5.0f, -4.0f};
			 }},
			{600,
		     [](Entity entity)
		     {
				 entity.GetComponent<TransformComponent>().Translation = {0.2f, 6.0f, 0.1f};
				 CharacterControllerComponent& character = entity.AddComponent<CharacterControllerComponent>();
				 character.Height = 1.6f;
				 character.Radius = 0.3f;
				 character.Offset = {0.0f, 0.8f, 0.0f};
			 }},
			{601,
		     [](Entity entity)
		     {
				 // Below the character (parented when every entity exists): a part of it, moved with it.
				 entity.GetComponent<TransformComponent>().Translation = {0.0f, 0.8f, 0.6f};
				 entity.AddComponent<SphereColliderComponent>().Radius = 0.2f;
			 }},
			{410,
		     [](Entity entity)
		     {
				 // A platform turned so slowly that a rider moves less per step than transform edits are told from noise.
				 entity.GetComponent<TransformComponent>().Translation = {8.0f, 1.0f, 0.0f};
				 entity.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
				 entity.AddComponent<BoxColliderComponent>().HalfExtents = {1.5f, 0.1f, 1.5f};
				 JointComponent& joint = entity.AddComponent<JointComponent>();
				 joint.Type = JointType::Hinge;
				 joint.Axis = {0.0f, 1.0f, 0.0f};
				 joint.MotorEnabled = true;
				 joint.MotorSpeed = 0.3f;
			 }},
			{610,
		     [](Entity entity)
		     {
				 entity.GetComponent<TransformComponent>().Translation = {8.6f, 1.5f, 0.3f};
				 CharacterControllerComponent& character = entity.AddComponent<CharacterControllerComponent>();
				 character.Height = 1.6f;
				 character.Radius = 0.3f;
				 character.Offset = {0.0f, 0.8f, 0.0f};
			 }},
			{611,
		     [](Entity entity)
		     {
				 entity.GetComponent<TransformComponent>().Translation = {0.0f, 0.8f, 0.6f};
				 entity.AddComponent<SphereColliderComponent>().Radius = 0.2f;
			 }},
			{700,
		     [](Entity entity)
		     {
				 entity.GetComponent<TransformComponent>().Translation = {-3.0f, 0.5f, 0.0f};
				 BoxColliderComponent& trigger = entity.AddComponent<BoxColliderComponent>();
				 trigger.HalfExtents = {0.5f, 1.0f, 2.0f};
				 trigger.IsTrigger = true;
			 }},
		};
		return s_Recipes;
	}

	// The test scene, its entities made in UUID order or the other way round.
	void BuildScene(Scene& scene, bool reversed)
	{
		std::vector<EntityRecipe> recipes = GetRecipes();
		if (reversed)
		{
			std::reverse(recipes.begin(), recipes.end());
		}
		for (EntityRecipe const& recipe : recipes)
		{
			recipe.Make(scene.CreateEntityWithUUID(UUID(recipe.ID), "Part"));
		}
		REQUIRE(scene.SetParent(scene.GetEntityByUUID(UUID(601)), scene.GetEntityByUUID(UUID(600)), false).IsOk());
		REQUIRE(scene.SetParent(scene.GetEntityByUUID(UUID(611)), scene.GetEntityByUUID(UUID(610)), false).IsOk());
	}

	SceneRuntimeSettings PhysicsOnly(uint64_t hashStep = 0)
	{
		SceneRuntimeSettings settings;
		settings.RunScripts = false;
		settings.PlayAudio = false;
		settings.PhysicsHashStep = hashStep;
		return settings;
	}

	// The scene's hash right after fixed step `step`, run at frames of frameTime seconds.
	uint64_t HashAtStep(float frameTime, uint64_t step)
	{
		Scene scene("Determinism");
		BuildScene(scene, false);
		scene.OnRuntimeStart(PhysicsOnly(step));
		REQUIRE(scene.GetPhysicsScene() != nullptr);
		for (uint64_t frame = 0; !scene.GetRecordedPhysicsHash() && frame < 100000; frame++)
		{
			scene.OnUpdateRuntime(Timestep(frameTime));
		}
		REQUIRE(scene.GetRecordedPhysicsHash().has_value());
		uint64_t const hash = *scene.GetRecordedPhysicsHash();
		scene.OnRuntimeStop();
		return hash;
	}

	// The hash after each of a scene's frames, each one fixed step long, while it runs the given frames.
	std::vector<uint64_t> RunHashes(Scene& scene, int frames, float frameTime = Step)
	{
		std::vector<uint64_t> hashes;
		for (int frame = 0; frame < frames; frame++)
		{
			scene.OnUpdateRuntime(Timestep(frameTime));
			hashes.push_back(scene.ComputePhysicsStateHash());
		}
		return hashes;
	}

	glm::vec3 GetPosition(Scene& scene, uint64_t id)
	{
		return scene.GetEntityByUUID(UUID(id)).GetComponent<TransformComponent>().Translation;
	}

	// What a scene did in some frames: its hash after each, its contact events, and where the ball, the character and its
	// part ended up.
	struct Played
	{
		std::vector<uint64_t> Hashes;
		std::vector<ContactEvent> Events;
		glm::vec3 Ball = glm::vec3(0.0f);
		glm::vec3 Character = glm::vec3(0.0f);
		glm::vec3 Part = glm::vec3(0.0f);
	};

	Played Play(Scene& scene, int frames, float frameTime)
	{
		Played played;
		for (int frame = 0; frame < frames; frame++)
		{
			scene.OnUpdateRuntime(Timestep(frameTime));
			played.Hashes.push_back(scene.ComputePhysicsStateHash());
			played.Events.insert(played.Events.end(), scene.GetContactEvents().begin(), scene.GetContactEvents().end());
		}
		played.Ball = GetPosition(scene, 300);
		played.Character = GetPosition(scene, 600);
		played.Part = glm::vec3(scene.GetWorldTransform(scene.GetEntityByUUID(UUID(601)))[3]);
		return played;
	}

	bool SameEvents(std::vector<ContactEvent> const& a, std::vector<ContactEvent> const& b)
	{
		return std::equal(a.begin(), a.end(), b.begin(), b.end(),
		                  [](ContactEvent const& first, ContactEvent const& second)
		                  {
							  return first.Type == second.Type && first.First == second.First && first.Second == second.Second;
						  });
	}
}

TEST_CASE("ScenePhysics: the fixed steps keep to the step settings at their new bounds")
{
	Testing::PhysicsSystemScope physics;
	ProjectSettings project;
	project.Physics.FixedTimestep = 0.0005f;
	project.Physics.MaxStepsPerFrame = 64;
	project.Physics.CollisionSteps = 4;
	REQUIRE(ValidateProjectSettings(project).IsOk());
	SceneRuntimeSettings runtime = MakeSceneRuntimeSettings(project);
	CHECK(runtime.MaxStepsPerFrame == 64);
	CHECK(runtime.CollisionSteps == 4);
	runtime.RunScripts = false;
	runtime.PlayAudio = false;

	Scene scene("Bounds");
	BuildScene(scene, false);
	scene.OnRuntimeStart(runtime);
	REQUIRE(scene.GetPhysicsScene() != nullptr);
	CHECK(scene.GetPhysicsScene()->GetCollisionSteps() == 4);
	CHECK(scene.GetFixedTimestep() == doctest::Approx(0.0005f));
	CHECK(scene.GetFixedStepCount() == 0);

	// 2 kHz steps: a 60 Hz frame runs 33 of them (with a third of one left over).
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK(scene.GetFixedStepCount() == 33);
	// A long frame runs 64 and drops the rest of its backlog but one step.
	scene.OnUpdateRuntime(Timestep(1.0f));
	CHECK(scene.GetFixedStepCount() == 33 + 64);
	scene.OnUpdateRuntime(Timestep(0.0f));
	CHECK(scene.GetFixedStepCount() <= 33 + 64 + 1);
	uint64_t const afterBacklog = scene.GetFixedStepCount();
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK(scene.GetFixedStepCount() - afterBacklog >= 33);
	CHECK(scene.GetFixedStepCount() - afterBacklog <= 34);
	// The simulation ran: the ball rolled.
	CHECK(GetPosition(scene, 300).x > -6.0f);
	scene.OnRuntimeStop();

	// One step a frame at most: a slow frame runs one, and the game slows down.
	runtime.FixedTimestep = Step;
	runtime.MaxStepsPerFrame = 1;
	runtime.CollisionSteps = 1;
	scene.OnRuntimeStart(runtime);
	CHECK(scene.GetPhysicsScene()->GetCollisionSteps() == 1);
	scene.OnUpdateRuntime(Timestep(0.5f));
	CHECK(scene.GetFixedStepCount() == 1);
	scene.OnUpdateRuntime(Timestep(0.5f));
	CHECK(scene.GetFixedStepCount() == 2);
	scene.OnRuntimeStop();
	CHECK(scene.GetFixedStepCount() == 2);

	// Restarting counts from zero; steps run without physics too.
	scene.OnRuntimeStart(runtime);
	CHECK(scene.GetFixedStepCount() == 0);
	scene.OnRuntimeStop();

	// The world keeps collision steps within Jolt's useful range.
	PhysicsWorldSettings world;
	world.CollisionSteps = 0;
	Result<Scope<PhysicsScene>> none = PhysicsScene::Create(world);
	REQUIRE(none.IsOk());
	CHECK(none.GetValue()->GetCollisionSteps() == 1);
	world.CollisionSteps = 1000;
	Result<Scope<PhysicsScene>> many = PhysicsScene::Create(world);
	REQUIRE(many.IsOk());
	CHECK(many.GetValue()->GetCollisionSteps() == MaxPhysicsCollisionSteps);
}

TEST_CASE("ScenePhysics: steps run without physics and are counted")
{
	REQUIRE_FALSE(PhysicsSystem::IsInitialized());
	Scene scene("No physics");
	scene.OnRuntimeStart(PhysicsOnly(3));
	CHECK(scene.GetPhysicsScene() == nullptr);
	uint64_t const empty = scene.ComputePhysicsStateHash();
	scene.OnUpdateRuntime(Timestep(Step * 2.0f));
	CHECK(scene.GetFixedStepCount() == 2);
	CHECK_FALSE(scene.GetRecordedPhysicsHash().has_value());
	CHECK(scene.ComputePhysicsStateHash() != empty);
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK(scene.GetRecordedPhysicsHash() == scene.ComputePhysicsStateHash());
	CHECK(scene.SavePhysicsState().IsError());
	scene.OnRuntimeStop();
}

TEST_CASE("ScenePhysics: Jolt is called in the same order whatever order the entities were made in")
{
	Testing::PhysicsSystemScope physics;
	Scene forward("Forward");
	Scene backward("Backward");
	BuildScene(forward, false);
	BuildScene(backward, true);
	forward.OnRuntimeStart(PhysicsOnly());
	backward.OnRuntimeStart(PhysicsOnly());
	CHECK(forward.ComputePhysicsStateHash() == backward.ComputePhysicsStateHash());
	std::vector<uint64_t> const forwardHashes = RunHashes(forward, 90);
	std::vector<uint64_t> const backwardHashes = RunHashes(backward, 90);
	CHECK(forwardHashes == backwardHashes);
	CHECK(SameEvents(forward.GetContactEvents(), backward.GetContactEvents()));

	// Bodies made, rebuilt and removed while running, in opposite orders in the two scenes.
	auto const change = [](Scene& scene, bool reversed)
	{
		std::vector<uint64_t> ids = {801, 802, 803, 804};
		if (reversed)
		{
			std::reverse(ids.begin(), ids.end());
		}
		for (uint64_t const id : ids)
		{
			MakeCrate(scene.CreateEntityWithUUID(UUID(id), "Dropped"), {static_cast<float>(id - 800) * 0.4f - 1.0f, 5.0f, 2.0f});
		}
		std::vector<uint64_t> rebuilt = {202, 204};
		if (reversed)
		{
			std::reverse(rebuilt.begin(), rebuilt.end());
		}
		for (uint64_t const id : rebuilt)
		{
			scene.GetRegistry().patch<RigidBodyComponent>(scene.GetEntityByUUID(UUID(id)).GetHandle(),
			                                              [](RigidBodyComponent& body)
			                                              {
															  body.Mass = 3.0f;
														  });
		}
		scene.DestroyEntity(scene.GetEntityByUUID(UUID(203)));
	};
	change(forward, false);
	change(backward, true);
	CHECK(RunHashes(forward, 120) == RunHashes(backward, 120));
	CHECK(forward.GetPhysicsScene()->GetBodyCount() == backward.GetPhysicsScene()->GetBodyCount());
	forward.OnRuntimeStop();
	backward.OnRuntimeStop();
}

TEST_CASE("ScenePhysics: one scene gives the same state hashes at 30, 60 and 144 frames per second")
{
	Testing::PhysicsSystemScope physics;
	for (uint64_t const step : {1u, 45u, 150u, 300u, 600u})
	{
		CAPTURE(step);
		uint64_t const at60 = HashAtStep(1.0f / 60.0f, step);
		CHECK(HashAtStep(1.0f / 30.0f, step) == at60);
		CHECK(HashAtStep(1.0f / 144.0f, step) == at60);
	}
	// The hash tells steps apart, and the simulation moves.
	CHECK(HashAtStep(Step, 150) != HashAtStep(Step, 151));
}

TEST_CASE("ScenePhysics: a snapshot restores the simulation, which then runs as it did")
{
	Testing::PhysicsSystemScope physics;
	Scene scene("Snapshot");
	BuildScene(scene, false);
	scene.OnRuntimeStart(PhysicsOnly());
	// Frames that leave part of a step over: the accumulator is part of the snapshot.
	constexpr float FrameTime = 1.0f / 144.0f;
	RunHashes(scene, 100, FrameTime);
	Result<ScenePhysicsSnapshot> saved = scene.SavePhysicsState();
	REQUIRE_MESSAGE(saved.IsOk(), (saved ? std::string() : saved.GetError()));
	ScenePhysicsSnapshot const& snapshot = saved.GetValue();
	CHECK(snapshot.GetStep() == scene.GetFixedStepCount());
	CHECK(snapshot.GetPhysicsSize() > 0);
	uint64_t const savedHash = scene.ComputePhysicsStateHash();

	Played const first = Play(scene, 300, FrameTime);
	CHECK_FALSE(first.Events.empty());

	// Moved and pushed since: restoring puts it all back.
	scene.GetEntityByUUID(UUID(202)).GetComponent<TransformComponent>().Translation = {9.0f, 9.0f, 9.0f};
	scene.GetPhysicsScene()->SetLinearVelocity(UUID(300), {0.0f, 20.0f, 0.0f});
	REQUIRE(scene.RestorePhysicsState(snapshot).IsOk());
	CHECK(scene.GetFixedStepCount() == snapshot.GetStep());
	CHECK(scene.ComputePhysicsStateHash() == savedHash);
	Played const second = Play(scene, 300, FrameTime);
	CHECK(second.Hashes == first.Hashes);
	CHECK(second.Events.size() == first.Events.size());
	CHECK(SameEvents(second.Events, first.Events));
	CHECK(second.Ball == first.Ball);
	CHECK(second.Character == first.Character);
	CHECK(second.Part == first.Part);

	// A snapshot can be gone back to again.
	REQUIRE(scene.RestorePhysicsState(snapshot).IsOk());
	CHECK(scene.ComputePhysicsStateHash() == savedHash);
	scene.OnRuntimeStop();
}

TEST_CASE("ScenePhysics: a snapshot is refused, changing nothing, once bodies, joints or characters changed")
{
	Testing::PhysicsSystemScope physics;
	Scene scene("Refused");
	BuildScene(scene, false);
	CHECK(scene.SavePhysicsState().IsError());
	scene.OnRuntimeStart(PhysicsOnly());
	RunHashes(scene, 30);
	Result<ScenePhysicsSnapshot> saved = scene.SavePhysicsState();
	REQUIRE(saved.IsOk());
	ScenePhysicsSnapshot const snapshot = saved.TakeValue();

	auto const refused = [&scene, &snapshot]
	{
		// Changes not applied yet are applied by a restore, as by the next frame: what it must not change comes after.
		scene.ApplyPhysicsChanges();
		uint64_t const before = scene.ComputePhysicsStateHash();
		glm::vec3 const ball = GetPosition(scene, 300);
		Result<void> const restored = scene.RestorePhysicsState(snapshot);
		CHECK(restored.IsError());
		CHECK(scene.ComputePhysicsStateHash() == before);
		CHECK(GetPosition(scene, 300) == ball);
	};

	SUBCASE("a body added")
	{
		MakeCrate(scene.CreateEntity("Late"), {3.0f, 3.0f, 3.0f});
		refused();
	}
	SUBCASE("a body rebuilt")
	{
		scene.GetRegistry().patch<SphereColliderComponent>(scene.GetEntityByUUID(UUID(300)).GetHandle(),
		                                                   [](SphereColliderComponent& sphere)
		                                                   {
															   sphere.Radius = 0.6f;
														   });
		refused();
	}
	SUBCASE("a joint removed")
	{
		scene.GetEntityByUUID(UUID(400)).RemoveComponent<JointComponent>();
		refused();
	}
	SUBCASE("a character removed")
	{
		scene.GetEntityByUUID(UUID(600)).RemoveComponent<CharacterControllerComponent>();
		refused();
	}
	SUBCASE("another run")
	{
		scene.OnRuntimeStop();
		scene.OnRuntimeStart(PhysicsOnly());
		refused();
	}
	scene.OnRuntimeStop();
	CHECK(scene.RestorePhysicsState(snapshot).IsError());
}

TEST_CASE("ScenePhysics: entities made while a scene runs get the same UUIDs on every run")
{
	Scene scene("Spawner");
	BuildScene(scene, false);
	Ref<Scene> const copy = Scene::Copy(scene);
	Scene other("Other");
	other.CreateEntity("Alone");

	auto const spawn = [](Scene& running)
	{
		running.OnRuntimeStart(PhysicsOnly());
		std::vector<UUID> ids;
		for (int i = 0; i < 4; i++)
		{
			ids.push_back(running.CreateEntity("Spawned").GetUUID());
		}
		running.OnRuntimeStop();
		return ids;
	};
	std::vector<UUID> const first = spawn(scene);
	CHECK(spawn(*copy) == first);
	// The sequence starts from the scene's entities: a scene with others makes other IDs.
	CHECK(spawn(other) != first);
	for (UUID const id : first)
	{
		CHECK(id.IsValid());
	}
	std::vector<UUID> sorted = first;
	std::sort(sorted.begin(), sorted.end());
	CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

	// While editing, new entities get random IDs.
	CHECK(scene.CreateEntity("Edited").GetUUID() != copy->CreateEntity("Edited").GetUUID());
}
