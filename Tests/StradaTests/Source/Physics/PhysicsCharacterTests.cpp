#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"

#include "Strada/Core/Log.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <limits>
#include <optional>
#include <vector>

using namespace Strada;

namespace
{
	constexpr float Step = 1.0f / 60.0f;
	// The test characters: 1.8 m tall with a radius of 0.3 m, their position half their height above their feet.
	constexpr float HalfHeight = 0.9f;
	// How far characters keep from what they touch.
	constexpr float Padding = 0.02f;

	UUID const GroundId(301);
	UUID const HeroId(302);
	UUID const ObstacleId(303);
	UUID const BoxId(304);
	UUID const OtherId(305);

	Scope<PhysicsScene> CreateWorld(PhysicsWorldSettings const& settings = {})
	{
		Result<Scope<PhysicsScene>> world = PhysicsScene::Create(settings);
		REQUIRE(world.IsOk());
		return world.TakeValue();
	}

	BodyDesc MakeBox(UUID entity, RigidBodyType type, glm::vec3 const& position, glm::vec3 const& halfExtents,
	                 glm::quat const& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f), bool trigger = false)
	{
		BodyDesc desc;
		desc.Entity = entity;
		desc.Type = type;
		desc.Position = position;
		desc.Rotation = rotation;
		ColliderDesc collider;
		collider.Shape = ColliderShape::Box;
		collider.HalfExtents = halfExtents;
		collider.IsTrigger = trigger;
		desc.Colliders.push_back(collider);
		return desc;
	}

	// A 40 x 1 x 40 static floor whose top is at y = 0.
	BodyDesc Ground(uint32_t layer = 0)
	{
		BodyDesc ground = MakeBox(GroundId, RigidBodyType::Static, {0.0f, -0.5f, 0.0f}, {20.0f, 0.5f, 20.0f});
		ground.Layer = layer;
		return ground;
	}

	// A ramp rising along +X from (startX, 0) at an angle in degrees: a box whose top face is the slope.
	BodyDesc Ramp(float startX, float degrees)
	{
		float const angle = glm::radians(degrees);
		glm::vec3 const along(std::cos(angle), std::sin(angle), 0.0f);
		glm::vec3 const normal(-std::sin(angle), std::cos(angle), 0.0f);
		glm::vec3 const halfExtents(5.0f, 0.25f, 3.0f);
		glm::vec3 const topCenter = glm::vec3(startX, 0.0f, 0.0f) + along * halfExtents.x;
		return MakeBox(ObstacleId, RigidBodyType::Static, topCenter - normal * halfExtents.y, halfExtents,
		               glm::angleAxis(angle, glm::vec3(0.0f, 0.0f, 1.0f)));
	}

	CharacterDesc MakeCharacter(glm::vec3 const& feet, UUID entity = HeroId)
	{
		CharacterDesc desc;
		desc.Entity = entity;
		desc.Position = feet + glm::vec3(0.0f, HalfHeight, 0.0f);
		desc.Radius = 0.3f;
		desc.Height = 2.0f * HalfHeight;
		return desc;
	}

	CharacterState StateOf(PhysicsScene const& world, UUID entity = HeroId)
	{
		std::optional<CharacterState> const state = world.GetCharacterState(entity);
		REQUIRE(state);
		return *state;
	}

	glm::vec3 FeetOf(PhysicsScene const& world, UUID entity = HeroId)
	{
		return StateOf(world, entity).Position - glm::vec3(0.0f, HalfHeight, 0.0f);
	}

	glm::vec3 PositionOf(PhysicsScene const& world, UUID entity)
	{
		std::optional<BodyTransform> const transform = world.GetBodyTransform(entity);
		REQUIRE(transform);
		return transform->Position;
	}

	// Steps the world, walking the character at a velocity every step; returns the contact events.
	std::vector<ContactEvent> Walk(PhysicsScene& world, glm::vec3 const& velocity, int steps, UUID entity = HeroId)
	{
		std::vector<ContactEvent> events;
		for (int step = 0; step < steps; step++)
		{
			world.MoveCharacter(entity, velocity);
			world.Step(Step);
			std::vector<ContactEvent> const stepEvents = world.TakeContactEvents();
			events.insert(events.end(), stepEvents.begin(), stepEvents.end());
		}
		return events;
	}

	std::vector<ContactEvent> Wait(PhysicsScene& world, int steps)
	{
		return Walk(world, glm::vec3(0.0f), steps);
	}

	bool IsUp(glm::vec3 const& normal)
	{
		return glm::dot(normal, glm::vec3(0.0f, 1.0f, 0.0f)) > 0.999f;
	}
}

TEST_CASE("PhysicsScene: characters stand and walk on the ground and stop at walls")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	REQUIRE(world->AddBody(Ground()).IsOk());
	REQUIRE(world->AddCharacter(MakeCharacter(glm::vec3(0.0f))).IsOk());
	CHECK(world->HasCharacter(HeroId));
	CHECK(world->GetCharacterCount() == 1);
	CHECK_FALSE(world->HasBody(HeroId));

	// Placed on the ground, it stands on it at once.
	CharacterState const placed = StateOf(*world);
	CHECK(placed.IsGrounded);
	CHECK(IsUp(placed.GroundNormal));
	CHECK(placed.GroundEntity == GroundId);
	CHECK(placed.Velocity == glm::vec3(0.0f));

	// Walking at 2 m/s for a second, on the ground the whole way.
	Walk(*world, {2.0f, 0.0f, 0.0f}, 60);
	glm::vec3 const walked = FeetOf(*world);
	CHECK(walked.x == doctest::Approx(2.0f).epsilon(0.02));
	CHECK(std::abs(walked.y) < 0.01f);
	CHECK(std::abs(walked.z) < 0.01f);
	CHECK(StateOf(*world).IsGrounded);

	// It stops when it is not moved, and stays where it is.
	Wait(*world, 30);
	CHECK(glm::length(FeetOf(*world) - walked) < 0.01f);
	CHECK(StateOf(*world).Velocity == glm::vec3(0.0f));

	// A wall whose face is at x = 3.5 stops it, its padding away.
	REQUIRE(world->AddBody(MakeBox(ObstacleId, RigidBodyType::Static, {4.0f, 2.0f, 0.0f}, {0.5f, 2.0f, 10.0f})).IsOk());
	Walk(*world, {2.0f, 0.0f, 0.0f}, 90);
	glm::vec3 const blocked = FeetOf(*world);
	CHECK(blocked.x == doctest::Approx(3.5f - 0.3f - Padding).epsilon(0.01));
	CHECK(StateOf(*world).IsGrounded);

	// Walking into it at an angle slides along it.
	Walk(*world, {2.0f, 0.0f, 2.0f}, 30);
	glm::vec3 const slid = FeetOf(*world);
	CHECK(slid.x == doctest::Approx(blocked.x).epsilon(0.01));
	CHECK(slid.z == doctest::Approx(1.0f).epsilon(0.05));
}

TEST_CASE("PhysicsScene: characters climb steps and walkable slopes and slide down steeper ones")
{
	Testing::PhysicsSystemScope physics;

	// A 25 cm step is climbed (StepHeight is 30 cm), and walked down again on the far side.
	{
		Scope<PhysicsScene> world = CreateWorld();
		REQUIRE(world->AddBody(Ground()).IsOk());
		REQUIRE(world->AddBody(MakeBox(ObstacleId, RigidBodyType::Static, {2.0f, 0.125f, 0.0f}, {1.0f, 0.125f, 3.0f})).IsOk());
		REQUIRE(world->AddCharacter(MakeCharacter(glm::vec3(0.0f))).IsOk());
		Walk(*world, {2.0f, 0.0f, 0.0f}, 60);
		glm::vec3 const onStep = FeetOf(*world);
		CHECK(onStep.x == doctest::Approx(2.0f).epsilon(0.05));
		CHECK(onStep.y == doctest::Approx(0.25f).epsilon(0.05));
		CHECK(StateOf(*world).IsGrounded);
		CHECK(StateOf(*world).GroundEntity == ObstacleId);
		// Down the far side: the rounded bottom rolls over the edge (too steep to stand on for a moment), steadily down to
		// the floor.
		int offGround = 0;
		float height = onStep.y;
		for (int step = 0; step < 60; step++)
		{
			Walk(*world, {2.0f, 0.0f, 0.0f}, 1);
			CharacterState const state = StateOf(*world);
			offGround += state.IsGrounded ? 0 : 1;
			CHECK(state.Position.y - HalfHeight <= height + 1e-4f);
			height = state.Position.y - HalfHeight;
		}
		CHECK(offGround <= 3);
		CHECK(std::abs(FeetOf(*world).y) < 0.01f);
		CHECK(StateOf(*world).IsGrounded);
	}

	// A 50 cm step is not: the character stops in front of it.
	{
		Scope<PhysicsScene> world = CreateWorld();
		REQUIRE(world->AddBody(Ground()).IsOk());
		REQUIRE(world->AddBody(MakeBox(ObstacleId, RigidBodyType::Static, {2.0f, 0.25f, 0.0f}, {1.0f, 0.25f, 3.0f})).IsOk());
		REQUIRE(world->AddCharacter(MakeCharacter(glm::vec3(0.0f))).IsOk());
		Walk(*world, {2.0f, 0.0f, 0.0f}, 60);
		glm::vec3 const stopped = FeetOf(*world);
		CHECK(stopped.x == doctest::Approx(1.0f - 0.3f - Padding).epsilon(0.02));
		CHECK(std::abs(stopped.y) < 0.01f);

		// With a higher StepHeight, it climbs it.
		CharacterDesc climber = MakeCharacter(stopped);
		climber.StepHeight = 0.6f;
		REQUIRE(world->AddCharacter(climber).IsOk());
		Walk(*world, {2.0f, 0.0f, 0.0f}, 30);
		CHECK(FeetOf(*world).y == doctest::Approx(0.5f).epsilon(0.05));
	}

	// A 30 degree slope is walked up (MaxSlopeAngle is 45 degrees).
	{
		Scope<PhysicsScene> world = CreateWorld();
		REQUIRE(world->AddBody(Ground()).IsOk());
		REQUIRE(world->AddBody(Ramp(1.0f, 30.0f)).IsOk());
		REQUIRE(world->AddCharacter(MakeCharacter(glm::vec3(0.0f))).IsOk());
		Walk(*world, {2.0f, 0.0f, 0.0f}, 150);
		CharacterState const climbing = StateOf(*world);
		glm::vec3 const feet = climbing.Position - glm::vec3(0.0f, HalfHeight, 0.0f);
		CHECK(feet.y > 1.0f);
		CHECK(climbing.IsGrounded);
		CHECK(climbing.GroundEntity == ObstacleId);
		CHECK(climbing.GroundNormal.x == doctest::Approx(-0.5f).epsilon(0.02));
		CHECK(climbing.GroundNormal.y == doctest::Approx(std::sqrt(3.0f) * 0.5f).epsilon(0.02));
		// Standing on it, it does not slide down.
		Wait(*world, 60);
		CHECK(glm::length(FeetOf(*world) - feet) < 0.02f);
		// Running back down, it keeps to the slope instead of taking off.
		int offSlope = 0;
		for (int step = 0; step < 30; step++)
		{
			Walk(*world, {-4.0f, 0.0f, 0.0f}, 1);
			offSlope += StateOf(*world).IsGrounded ? 0 : 1;
		}
		CHECK(offSlope == 0);
		CHECK(FeetOf(*world).y < feet.y - 0.8f);
	}

	// A 60 degree slope is too steep: the character cannot walk up it, and slides down when it lands on it.
	{
		Scope<PhysicsScene> world = CreateWorld();
		REQUIRE(world->AddBody(Ground()).IsOk());
		REQUIRE(world->AddBody(Ramp(1.0f, 60.0f)).IsOk());
		REQUIRE(world->AddCharacter(MakeCharacter(glm::vec3(0.0f))).IsOk());
		Walk(*world, {2.0f, 0.0f, 0.0f}, 90);
		CHECK(FeetOf(*world).y < 0.35f);

		// Dropped onto the slope 2 m up it.
		float const slopeHeight = 2.0f * std::tan(glm::radians(60.0f));
		world->SetCharacterPosition(HeroId, glm::vec3(3.0f, slopeHeight + HalfHeight + 0.5f, 0.0f));
		bool slid = false;
		for (int step = 0; step < 60; step++)
		{
			Wait(*world, 1);
			CharacterState const state = StateOf(*world);
			if (state.GroundEntity == ObstacleId)
			{
				// On steep ground: not grounded, sliding down along the slope.
				CHECK_FALSE(state.IsGrounded);
				CHECK(state.GroundNormal.x < -0.8f);
				slid = slid || (state.Velocity.x < -0.5f && state.Velocity.y < -0.5f);
			}
		}
		CHECK(slid);
		Wait(*world, 120);
		CharacterState const bottom = StateOf(*world);
		CHECK(bottom.IsGrounded);
		CHECK(bottom.GroundEntity == GroundId);
		CHECK(bottom.Position.x < 1.0f);
	}
}

TEST_CASE("PhysicsScene: characters fall with gravity, land, jump and stop under ceilings")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	REQUIRE(world->AddBody(Ground()).IsOk());
	REQUIRE(world->AddCharacter(MakeCharacter({0.0f, 5.0f, 0.0f})).IsOk());
	CharacterState const start = StateOf(*world);
	CHECK_FALSE(start.IsGrounded);
	CHECK(start.GroundNormal == glm::vec3(0.0f));
	CHECK_FALSE(start.GroundEntity.IsValid());

	// Half a second of free fall: 1.23 m at 4.9 m/s.
	Wait(*world, 30);
	CharacterState const falling = StateOf(*world);
	CHECK(falling.Position.y - HalfHeight == doctest::Approx(5.0f - 0.5f * 9.81f * 0.25f).epsilon(0.02));
	CHECK(falling.Velocity.y == doctest::Approx(-9.81f * 0.5f).epsilon(0.02));
	CHECK_FALSE(falling.IsGrounded);

	// Landing stops it.
	Wait(*world, 90);
	CharacterState const landed = StateOf(*world);
	CHECK(landed.IsGrounded);
	CHECK(std::abs(landed.Position.y - HalfHeight) < 0.01f);
	CHECK(landed.Velocity == glm::vec3(0.0f));

	// A jump: an upward velocity of its own, slowed down by gravity, back on the ground after about a second.
	world->SetCharacterVelocity(HeroId, {0.0f, 5.0f, 0.0f});
	Wait(*world, 15);
	CharacterState const jumping = StateOf(*world);
	CHECK_FALSE(jumping.IsGrounded);
	CHECK(jumping.Position.y - HalfHeight == doctest::Approx(5.0f * 0.25f - 0.5f * 9.81f * 0.0625f).epsilon(0.05));
	CHECK(jumping.Velocity.y == doctest::Approx(5.0f - 9.81f * 0.25f).epsilon(0.05));
	Wait(*world, 60);
	CHECK(StateOf(*world).IsGrounded);

	// Under a ceiling 2.2 m up, a jump stops at it and falls back at once.
	REQUIRE(world->AddBody(MakeBox(ObstacleId, RigidBodyType::Static, {0.0f, 2.7f, 0.0f}, {3.0f, 0.5f, 3.0f})).IsOk());
	world->SetCharacterVelocity(HeroId, {0.0f, 5.0f, 0.0f});
	Wait(*world, 6);
	CharacterState const bumped = StateOf(*world);
	CHECK(bumped.Velocity.y <= 0.0f);
	CHECK(bumped.Position.y + HalfHeight < 2.2f);
	Wait(*world, 30);
	CHECK(StateOf(*world).IsGrounded);

	// Without gravity, it floats where it is put; its own velocity carries it.
	CharacterDesc floating = MakeCharacter({5.0f, 3.0f, 0.0f}, OtherId);
	floating.GravityFactor = 0.0f;
	REQUIRE(world->AddCharacter(floating).IsOk());
	Wait(*world, 60);
	CHECK(FeetOf(*world, OtherId).y == doctest::Approx(3.0f).epsilon(1e-4));
	world->SetCharacterVelocity(OtherId, {1.0f, 0.0f, 0.0f});
	Wait(*world, 60);
	CHECK(FeetOf(*world, OtherId).x == doctest::Approx(6.0f).epsilon(0.01));
}

TEST_CASE("PhysicsScene: characters push dynamic bodies, up to their push force")
{
	Testing::PhysicsSystemScope physics;
	auto const pushed = [](float pushForce, float boxMass)
	{
		Scope<PhysicsScene> world = CreateWorld();
		REQUIRE(world->AddBody(Ground()).IsOk());
		BodyDesc box = MakeBox(BoxId, RigidBodyType::Dynamic, {2.0f, 0.25f, 0.0f}, glm::vec3(0.25f));
		box.Mass = boxMass;
		REQUIRE(world->AddBody(box).IsOk());
		CharacterDesc hero = MakeCharacter(glm::vec3(0.0f));
		hero.PushForce = pushForce;
		REQUIRE(world->AddCharacter(hero).IsOk());
		// Let the box settle, then walk into it for two seconds.
		Wait(*world, 30);
		Walk(*world, {2.0f, 0.0f, 0.0f}, 120);
		return PositionOf(*world, BoxId).x - 2.0f;
	};
	// A 1 kg box is pushed along.
	CHECK(pushed(200.0f, 1.0f) > 2.0f);
	// Without a push force, or against a heavy crate that friction holds, nothing moves.
	CHECK(pushed(0.0f, 1.0f) < 0.05f);
	CHECK(pushed(200.0f, 1000.0f) < 0.05f);
}

TEST_CASE("PhysicsScene: triggers, raycasts and bodies notice characters")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	REQUIRE(world->AddBody(Ground()).IsOk());
	REQUIRE(world->AddCharacter(MakeCharacter(glm::vec3(0.0f))).IsOk());

	// A trigger across its way: entered and left, never in its way.
	REQUIRE(world
	            ->AddBody(MakeBox(ObstacleId, RigidBodyType::Static, {2.0f, 1.0f, 0.0f}, {0.25f, 1.0f, 3.0f},
	                              glm::quat(1.0f, 0.0f, 0.0f, 0.0f), true))
	            .IsOk());
	std::vector<ContactEvent> const events = Walk(*world, {2.0f, 0.0f, 0.0f}, 90);
	CHECK(FeetOf(*world).x == doctest::Approx(3.0f).epsilon(0.02));
	CHECK(Testing::HasEvent(events, ContactEventType::TriggerEnter, ObstacleId, HeroId));
	CHECK(Testing::HasEvent(events, ContactEventType::TriggerExit, ObstacleId, HeroId));
	CHECK_FALSE(Testing::HasEvent(events, ContactEventType::CollisionEnter, ObstacleId, HeroId));
	// It touches the ground the whole time: no contact with it ends.
	CHECK_FALSE(Testing::HasEvent(events, ContactEventType::CollisionExit, GroundId, HeroId));

	// Raycasts hit it; rays starting inside it pass out of it.
	std::optional<RaycastHit> const hit = world->Raycast({3.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 100.0f);
	REQUIRE(hit);
	CHECK(hit->Entity == HeroId);
	CHECK(hit->Point.y == doctest::Approx(2.0f * HalfHeight).epsilon(0.03));
	std::optional<RaycastHit> const fromInside = world->Raycast({3.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 100.0f);
	REQUIRE(fromInside);
	CHECK(fromInside->Entity == GroundId);

	// A ball dropped on it lands on its head, which reports the collision.
	BodyDesc ball = MakeBox(BoxId, RigidBodyType::Dynamic, {3.0f, 4.0f, 0.0f}, glm::vec3(0.1f));
	REQUIRE(world->AddBody(ball).IsOk());
	std::vector<ContactEvent> const landing = Wait(*world, 60);
	CHECK(Testing::HasEvent(landing, ContactEventType::CollisionEnter, BoxId, HeroId));
	CHECK(PositionOf(*world, BoxId).y > 2.0f * HalfHeight);

	// Another character blocks it.
	REQUIRE(world->AddCharacter(MakeCharacter({5.0f, 0.0f, 0.0f}, OtherId)).IsOk());
	Walk(*world, {2.0f, 0.0f, 0.0f}, 90);
	CHECK(FeetOf(*world).x < 5.0f - 0.55f);
	CHECK(FeetOf(*world, OtherId).x == doctest::Approx(5.0f).epsilon(0.01));
}

TEST_CASE("PhysicsScene: characters change in place, are removed, and refuse what they cannot simulate")
{
	Testing::PhysicsSystemScope physics;
	PhysicsWorldSettings settings;
	settings.LayerCount = 2;
	settings.IgnoredCollisions = {{0, 1}};
	Scope<PhysicsScene> world = CreateWorld(settings);
	REQUIRE(world->AddBody(Ground()).IsOk());
	REQUIRE(world->AddCharacter(MakeCharacter(glm::vec3(0.0f))).IsOk());
	CHECK(Testing::HasEvent(Wait(*world, 10), ContactEventType::CollisionEnter, GroundId, HeroId));

	// A changed description changes the character where it is put, keeping its velocity: a 1 m sphere-like capsule.
	world->SetCharacterVelocity(HeroId, {1.0f, 0.0f, 0.0f});
	CharacterDesc smaller = MakeCharacter({0.0f, 0.0f, 0.0f});
	smaller.Height = 0.6f;
	smaller.Position = glm::vec3(0.0f, 0.3f, 0.0f);
	REQUIRE(world->AddCharacter(smaller).IsOk());
	CHECK(world->GetCharacterCount() == 1);
	CHECK(StateOf(*world).Velocity == glm::vec3(1.0f, 0.0f, 0.0f));
	CHECK(StateOf(*world).IsGrounded);
	CHECK(StateOf(*world).Position.y == doctest::Approx(0.3f).epsilon(1e-4));

	// On an ignored layer it falls through the ground.
	smaller.Layer = 1;
	REQUIRE(world->AddCharacter(smaller).IsOk());
	Wait(*world, 30);
	CHECK(StateOf(*world).Position.y < 0.0f);
	smaller.Layer = 0;
	REQUIRE(world->AddCharacter(smaller).IsOk());
	CHECK(StateOf(*world).IsGrounded);

	// Moving it keeps its velocity and finds what it stands on there.
	world->SetCharacterPosition(HeroId, {0.0f, 10.0f, 0.0f});
	CHECK_FALSE(StateOf(*world).IsGrounded);
	CHECK(StateOf(*world).Position == glm::vec3(0.0f, 10.0f, 0.0f));
	world->SetCharacterPosition(HeroId, {0.0f, 0.3f, 0.0f});
	CHECK(StateOf(*world).IsGrounded);

	// Values that are not finite are ignored; huge ones are clamped.
	float const nan = std::numeric_limits<float>::quiet_NaN();
	world->MoveCharacter(HeroId, {nan, 0.0f, 0.0f});
	world->SetCharacterVelocity(HeroId, {0.0f, nan, 0.0f});
	world->SetCharacterPosition(HeroId, {nan, 0.0f, 0.0f});
	CHECK(StateOf(*world).Position == glm::vec3(0.0f, 0.3f, 0.0f));
	world->SetCharacterVelocity(HeroId, {0.0f, 1.0e30f, 0.0f});
	CHECK(glm::length(StateOf(*world).Velocity) == doctest::Approx(MaxCharacterSpeed));
	world->MoveCharacter(HeroId, {1.0e30f, 0.0f, 0.0f});
	Wait(*world, 1);
	CHECK(std::isfinite(StateOf(*world).Position.x));
	CHECK(std::isfinite(StateOf(*world).Position.y));
	CharacterDesc extreme = MakeCharacter(glm::vec3(1.0e30f));
	extreme.GravityFactor = 1.0e30f;
	extreme.StepHeight = 1.0e30f;
	extreme.Mass = -5.0f;
	extreme.PushForce = 1.0e30f;
	extreme.MaxSlopeAngle = 0.0f;
	REQUIRE(world->AddCharacter(extreme).IsOk());
	CHECK(StateOf(*world).Position.x == doctest::Approx(MaxPhysicsCoordinate));
	Wait(*world, 5);
	CHECK(std::isfinite(StateOf(*world).Position.y));

	// Refused descriptions leave the entity without a character.
	CharacterDesc broken = MakeCharacter(glm::vec3(0.0f));
	broken.Radius = nan;
	CHECK(world->AddCharacter(broken).IsError());
	CHECK_FALSE(world->HasCharacter(HeroId));
	CharacterDesc huge = MakeCharacter(glm::vec3(0.0f));
	huge.Height = 2.0f * MaxPhysicsExtent;
	CHECK(world->AddCharacter(huge).IsError());
	CHECK_FALSE(world->GetCharacterState(HeroId).has_value());

	// An entity has a body or a character, and removing a character ends its contacts.
	REQUIRE(world->AddCharacter(MakeCharacter(glm::vec3(0.0f))).IsOk());
	Wait(*world, 10);
	world->TakeContactEvents();
	REQUIRE(world->AddBody(MakeBox(HeroId, RigidBodyType::Dynamic, {0.0f, 1.0f, 0.0f}, glm::vec3(0.5f))).IsOk());
	CHECK_FALSE(world->HasCharacter(HeroId));
	CHECK(Testing::HasEvent(world->TakeContactEvents(), ContactEventType::CollisionExit, GroundId, HeroId));
	REQUIRE(world->AddCharacter(MakeCharacter(glm::vec3(0.0f))).IsOk());
	CHECK_FALSE(world->HasBody(HeroId));
	Wait(*world, 10);
	world->TakeContactEvents();
	world->RemoveCharacter(HeroId);
	CHECK(world->GetCharacterCount() == 0);
	CHECK(Testing::HasEvent(world->TakeContactEvents(), ContactEventType::CollisionExit, GroundId, HeroId));
	world->RemoveCharacter(HeroId);

	// Calls on entities without a character do nothing.
	world->MoveCharacter(OtherId, {1.0f, 0.0f, 0.0f});
	world->SetCharacterVelocity(OtherId, {1.0f, 0.0f, 0.0f});
	world->SetCharacterPosition(OtherId, {1.0f, 0.0f, 0.0f});
	CHECK_FALSE(world->GetCharacterState(OtherId).has_value());
	Wait(*world, 1);
}

TEST_CASE("Scene: character controllers move their entities while running and follow edits")
{
	Testing::PhysicsSystemScope physics;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Characters");
	Entity ground = scene.CreateEntity("Ground");
	ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
	ground.AddComponent<BoxColliderComponent>().HalfExtents = {20.0f, 0.5f, 20.0f};
	Entity hero = scene.CreateEntity("Hero");
	TransformComponent& transform = hero.GetComponent<TransformComponent>();
	transform.Translation = {0.0f, 1.0f, 0.0f};
	transform.Rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
	hero.AddComponent<CharacterControllerComponent>();
	// Its colliders are left out: the character replaces them.
	hero.AddComponent<SphereColliderComponent>();
	Entity gate = scene.CreateEntity("Gate");
	gate.GetComponent<TransformComponent>().Translation = {1.0f, 1.0f, 0.0f};
	gate.AddComponent<BoxColliderComponent>().IsTrigger = true;
	UUID const id = hero.GetUUID();
	auto const run = [&scene](int frames)
	{
		std::vector<ContactEvent> events;
		for (int frame = 0; frame < frames; frame++)
		{
			scene.OnUpdateRuntime(Timestep(Step));
			events.insert(events.end(), scene.GetContactEvents().begin(), scene.GetContactEvents().end());
		}
		return events;
	};

	scene.OnRuntimeStart();
	PhysicsScene* world = scene.GetPhysicsScene();
	REQUIRE(world != nullptr);
	CHECK(world->HasCharacter(id));
	CHECK_FALSE(world->HasBody(id));
	CHECK(Testing::WasLogged(logStart, "replaces its RigidBody and colliders"));
	CHECK(StateOf(*world, id).IsGrounded);

	// Walking writes the position back; the entity keeps its rotation. It walks through the trigger, which notices it.
	std::vector<ContactEvent> events;
	for (int frame = 0; frame < 60; frame++)
	{
		world->MoveCharacter(id, {3.0f, 0.0f, 0.0f});
		std::vector<ContactEvent> const frameEvents = run(1);
		events.insert(events.end(), frameEvents.begin(), frameEvents.end());
	}
	CHECK(transform.Translation.x == doctest::Approx(3.0f).epsilon(0.02));
	CHECK(transform.Translation.y == doctest::Approx(1.0f).epsilon(0.01));
	CHECK(std::abs(glm::dot(transform.Rotation, glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f)))) > 0.9999f);
	CHECK(Testing::HasEvent(events, ContactEventType::TriggerEnter, gate.GetUUID(), id));
	CHECK(Testing::HasEvent(events, ContactEventType::TriggerExit, gate.GetUUID(), id));

	// A smaller capsule whose center is half a meter above the entity: the entity ends up at the character's feet.
	scene.GetRegistry().patch<CharacterControllerComponent>(hero.GetHandle(),
	                                                        [](CharacterControllerComponent& component)
	                                                        {
																component.Height = 1.0f;
																component.Offset = {0.0f, 0.5f, 0.0f};
															});
	run(60);
	CHECK(transform.Translation.y == doctest::Approx(0.0f).epsilon(0.01));
	CHECK(StateOf(*world, id).Position.y == doctest::Approx(0.5f).epsilon(0.01));

	// Moved by an edit (or a script): the character goes there and falls from there.
	transform.Translation = {-4.0f, 3.0f, 2.0f};
	run(1);
	CHECK(StateOf(*world, id).Position.x == doctest::Approx(-4.0f).epsilon(1e-3));
	CHECK_FALSE(StateOf(*world, id).IsGrounded);
	run(90);
	CHECK(transform.Translation == glm::vec3(-4.0f, transform.Translation.y, 2.0f));
	CHECK(std::abs(transform.Translation.y) < 0.01f);

	// Scaled up twice: a capsule twice the size, its feet still at the entity.
	transform.Scale = glm::vec3(2.0f);
	run(30);
	CHECK(StateOf(*world, id).Position.y == doctest::Approx(1.0f).epsilon(0.01));
	CHECK(std::abs(transform.Translation.y) < 0.01f);

	// Teleporting places the entity and stops the character.
	world->SetCharacterVelocity(id, {0.0f, 3.0f, 0.0f});
	scene.TeleportCharacter(hero, {6.0f, 0.0f, -1.0f});
	CHECK(transform.Translation == glm::vec3(6.0f, 0.0f, -1.0f));
	CHECK(StateOf(*world, id).Velocity == glm::vec3(0.0f));
	CHECK(StateOf(*world, id).IsGrounded);

	// Without the component, the entity has no character (and gets its collider's body back).
	hero.RemoveComponent<CharacterControllerComponent>();
	run(1);
	CHECK_FALSE(world->HasCharacter(id));
	CHECK(world->HasBody(id));
	hero.RemoveComponent<SphereColliderComponent>();
	hero.AddComponent<CharacterControllerComponent>();
	run(1);
	CHECK(world->HasCharacter(id));
	CHECK_FALSE(world->HasBody(id));

	// Destroyed while running: the character goes with the entity.
	scene.DestroyEntity(hero);
	run(2);
	CHECK(world->GetCharacterCount() == 0);
	scene.OnRuntimeStop();
	CHECK(scene.GetPhysicsScene() == nullptr);
}
