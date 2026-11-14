#include "Physics/PhysicsTestUtilities.h"

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <vector>

using namespace Strada;

namespace
{
	constexpr float Step = 1.0f / 60.0f;
	UUID const GroundId(11);
	UUID const BoxId(12);
	UUID const BallId(13);
	UUID const HingeId(14);
	UUID const HeroId(15);
	UUID const SensorId(16);

	BodyDesc MakeBody(UUID entity, RigidBodyType type, glm::vec3 const& position, ColliderShape shape, bool trigger = false)
	{
		BodyDesc desc;
		desc.Entity = entity;
		desc.Type = type;
		desc.Position = position;
		ColliderDesc collider;
		collider.Shape = shape;
		collider.HalfExtents = glm::vec3(0.5f);
		collider.Radius = 0.5f;
		collider.IsTrigger = trigger;
		desc.Colliders.push_back(collider);
		return desc;
	}

	// A ground, a box, a ball rolling through a sensor into it, a box spun by a hinge's motor and a character falling onto
	// the box.
	Scope<PhysicsScene> CreateWorld()
	{
		Result<Scope<PhysicsScene>> created = PhysicsScene::Create({});
		REQUIRE(created.IsOk());
		Scope<PhysicsScene> world = created.TakeValue();
		BodyDesc ground = MakeBody(GroundId, RigidBodyType::Static, {0.0f, -0.5f, 0.0f}, ColliderShape::Box);
		ground.Colliders[0].HalfExtents = {10.0f, 0.5f, 10.0f};
		REQUIRE(world->AddBody(ground).IsOk());
		REQUIRE(world->AddBody(MakeBody(BoxId, RigidBodyType::Dynamic, {0.0f, 0.5f, 0.0f}, ColliderShape::Box)).IsOk());
		BodyDesc ball = MakeBody(BallId, RigidBodyType::Dynamic, {-4.0f, 0.5f, 0.0f}, ColliderShape::Sphere);
		ball.LinearVelocity = {6.0f, 0.0f, 0.0f};
		REQUIRE(world->AddBody(ball).IsOk());
		REQUIRE(world->AddBody(MakeBody(SensorId, RigidBodyType::Static, {-2.0f, 0.5f, 0.0f}, ColliderShape::Box, true)).IsOk());
		REQUIRE(world->AddBody(MakeBody(HingeId, RigidBodyType::Dynamic, {4.0f, 2.0f, 0.0f}, ColliderShape::Box)).IsOk());
		JointDesc hinge;
		hinge.Entity = HingeId;
		hinge.Type = JointType::Hinge;
		hinge.Point = {4.0f, 2.0f, 0.0f};
		hinge.Axis = {0.0f, 0.0f, 1.0f};
		hinge.MotorEnabled = true;
		hinge.MotorSpeed = 2.0f;
		REQUIRE(world->AddJoint(hinge).IsOk());
		CharacterDesc hero;
		hero.Entity = HeroId;
		hero.Position = {0.1f, 3.0f, 0.0f};
		hero.Radius = 0.3f;
		hero.Height = 1.6f;
		REQUIRE(world->AddCharacter(hero).IsOk());
		return world;
	}

	struct Stepped
	{
		std::vector<uint64_t> Hashes;
		std::vector<ContactEvent> Events;
	};

	Stepped StepWorld(PhysicsScene& world, int steps)
	{
		Stepped stepped;
		for (int step = 0; step < steps; step++)
		{
			world.MoveCharacter(HeroId, {0.5f, 0.0f, 0.0f});
			world.Step(Step);
			stepped.Hashes.push_back(world.ComputeStateHash());
			std::vector<ContactEvent> const events = world.TakeContactEvents();
			stepped.Events.insert(stepped.Events.end(), events.begin(), events.end());
		}
		return stepped;
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

TEST_CASE("PhysicsScene: the state hash is the same for worlds built and stepped alike, and follows what moves")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> first = CreateWorld();
	Scope<PhysicsScene> second = CreateWorld();
	CHECK(first->ComputeStateHash() == second->ComputeStateHash());
	Stepped const firstSteps = StepWorld(*first, 120);
	Stepped const secondSteps = StepWorld(*second, 120);
	CHECK(firstSteps.Hashes == secondSteps.Hashes);
	CHECK(SameEvents(firstSteps.Events, secondSteps.Events));
	CHECK(firstSteps.Hashes.front() != firstSteps.Hashes.back());

	// Every part of the state counts: a pose, a velocity, a character's walk.
	uint64_t const before = first->ComputeStateHash();
	first->SetLinearVelocity(BoxId, {0.0f, 1.0f, 0.0f});
	CHECK(first->ComputeStateHash() != before);
	uint64_t const pushed = first->ComputeStateHash();
	first->MoveCharacter(HeroId, {1.0f, 0.0f, 0.0f});
	CHECK(first->ComputeStateHash() != pushed);
	uint64_t const walking = first->ComputeStateHash();
	first->SetBodyTransform(BallId, {1.0f, 5.0f, 1.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
	CHECK(first->ComputeStateHash() != walking);
}

TEST_CASE("PhysicsScene: a saved state restores bodies, joints, characters and contacts")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	StepWorld(*world, 20);
	PhysicsSnapshot const snapshot = world->SaveState();
	CHECK(snapshot.GetSize() > 0);
	uint64_t const saved = world->ComputeStateHash();
	std::optional<CharacterState> const hero = world->GetCharacterState(HeroId);
	REQUIRE(hero);

	Stepped const first = StepWorld(*world, 150);
	// The ball entered the sensor and hit the box, and the character landed, all after the snapshot.
	CHECK(Testing::HasEvent(first.Events, ContactEventType::TriggerEnter, BallId, SensorId));
	CHECK(Testing::HasEvent(first.Events, ContactEventType::CollisionEnter, BallId, BoxId));
	std::optional<CharacterState> const landed = world->GetCharacterState(HeroId);
	REQUIRE(landed);
	CHECK(landed->IsGrounded);

	world->SetCharacterVelocity(HeroId, {0.0f, 10.0f, 0.0f});
	REQUIRE(world->RestoreState(snapshot).IsOk());
	CHECK(world->ComputeStateHash() == saved);
	std::optional<CharacterState> const restored = world->GetCharacterState(HeroId);
	REQUIRE(restored);
	CHECK(restored->Position == hero->Position);
	CHECK(restored->Velocity == hero->Velocity);
	CHECK(restored->GroundEntity == hero->GroundEntity);

	// The same steps follow, with the same events: the contacts reported before the snapshot do not begin again.
	Stepped const second = StepWorld(*world, 150);
	CHECK(second.Hashes == first.Hashes);
	CHECK(SameEvents(second.Events, first.Events));
	std::optional<CharacterState> const again = world->GetCharacterState(HeroId);
	REQUIRE(again);
	CHECK(again->GroundEntity == landed->GroundEntity);
	CHECK(again->GroundEntity.IsValid());
}

TEST_CASE("PhysicsScene: a saved state is refused, changing nothing, once the world's structure changed")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	StepWorld(*world, 10);
	PhysicsSnapshot const snapshot = world->SaveState();
	uint64_t const version = world->GetStructureVersion();

	auto const refused = [&world, &snapshot]
	{
		uint64_t const before = world->ComputeStateHash();
		Result<void> const restored = world->RestoreState(snapshot);
		CHECK(restored.IsError());
		CHECK(world->ComputeStateHash() == before);
	};

	SUBCASE("a body added")
	{
		REQUIRE(world->AddBody(MakeBody(UUID(99), RigidBodyType::Dynamic, {2.0f, 4.0f, 2.0f}, ColliderShape::Sphere)).IsOk());
		CHECK(world->GetStructureVersion() != version);
		refused();
	}
	SUBCASE("a body removed and made again")
	{
		world->RemoveBody(BoxId);
		REQUIRE(world->AddBody(MakeBody(BoxId, RigidBodyType::Dynamic, {0.0f, 0.5f, 0.0f}, ColliderShape::Box)).IsOk());
		refused();
	}
	SUBCASE("a joint removed")
	{
		world->RemoveJoint(HingeId);
		refused();
	}
	SUBCASE("a character replaced")
	{
		CharacterDesc hero;
		hero.Entity = HeroId;
		hero.Position = {0.0f, 4.0f, 0.0f};
		REQUIRE(world->AddCharacter(hero).IsOk());
		refused();
	}
	SUBCASE("another world")
	{
		Scope<PhysicsScene> other = CreateWorld();
		CHECK(other->RestoreState(snapshot).IsError());
	}
	SUBCASE("changes of state alone")
	{
		// Moving, pushing and stepping change no structure: the snapshot puts them back.
		world->SetBodyTransform(BoxId, {3.0f, 3.0f, 3.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
		world->AddForce(BallId, {0.0f, 100.0f, 0.0f}, ForceMode::Impulse);
		StepWorld(*world, 5);
		CHECK(world->GetStructureVersion() == version);
		uint64_t const changed = world->ComputeStateHash();
		REQUIRE(world->RestoreState(snapshot).IsOk());
		CHECK(world->ComputeStateHash() != changed);
		CHECK(world->GetBodyTransform(BoxId)->Position.x < 1.0f);
	}
}
