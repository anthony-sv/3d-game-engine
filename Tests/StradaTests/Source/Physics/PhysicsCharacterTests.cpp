#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"

#include "Strada/Core/Log.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
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
	UUID const PartId(306);

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

	// How far a character strays from its spot on a kinematic platform driven back and forth along a direction (a second
	// each way, scripts' way: a target before each step), in how many steps it is not grounded, and how far the ground
	// displacement it reports is from the platform's.
	struct Ride
	{
		float WorstOffset = 0.0f;
		int StepsOffGround = 0;
		float WorstDisplacementError = 0.0f;
	};

	Ride RidePlatform(glm::vec3 const& direction, float speed)
	{
		Scope<PhysicsScene> world = CreateWorld();
		glm::vec3 platform(0.0f, 2.25f, 0.0f);
		REQUIRE(world->AddBody(MakeBox(ObstacleId, RigidBodyType::Kinematic, platform, {2.0f, 0.25f, 2.0f})).IsOk());
		REQUIRE(world->AddCharacter(MakeCharacter({0.0f, 2.5f, 0.0f})).IsOk());
		Wait(*world, 10);
		REQUIRE(StateOf(*world).GroundEntity == ObstacleId);
		glm::vec3 const spot = FeetOf(*world) - platform;
		Ride ride;
		for (int step = 0; step < 240; step++)
		{
			float const sign = (step / 60) % 2 == 0 ? 1.0f : -1.0f;
			glm::vec3 const move = direction * (sign * speed * Step);
			platform += move;
			world->MoveKinematic(ObstacleId, platform, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), Step);
			Wait(*world, 1);
			ride.WorstOffset = std::max(ride.WorstOffset, glm::length(FeetOf(*world) - platform - spot));
			ride.StepsOffGround += StateOf(*world).IsGrounded ? 0 : 1;
			ride.WorstDisplacementError = std::max(ride.WorstDisplacementError, glm::length(StateOf(*world).GroundDisplacement - move));
		}
		return ride;
	}

	// How many log entries since firstIndex contain the text.
	size_t CountLogged(uint64_t firstIndex, std::string_view text)
	{
		std::vector<LogEntry> const entries = Log::GetEntries(firstIndex);
		return static_cast<size_t>(std::count_if(entries.begin(), entries.end(),
		                                         [text](LogEntry const& entry)
		                                         {
													 return entry.Message.find(text) != std::string::npos;
												 }));
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

	// Walking at 2 m/s for a second, on the ground the whole way; the ground, which does not move, carries it nowhere.
	Walk(*world, {2.0f, 0.0f, 0.0f}, 60);
	glm::vec3 const walked = FeetOf(*world);
	CHECK(walked.x == doctest::Approx(2.0f).epsilon(0.02));
	CHECK(std::abs(walked.y) < 0.01f);
	CHECK(std::abs(walked.z) < 0.01f);
	CHECK(StateOf(*world).IsGrounded);
	CHECK(StateOf(*world).GroundDisplacement == glm::vec3(0.0f));

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

TEST_CASE("PhysicsScene: characters ride moving platforms and elevators without slipping or hovering")
{
	Testing::PhysicsSystemScope physics;

	// Along X at 3 m/s, turning back every second: the rider keeps its spot when the platform starts and turns.
	Ride const sideways = RidePlatform({1.0f, 0.0f, 0.0f}, 3.0f);
	CHECK(sideways.WorstOffset < 0.01f);
	CHECK(sideways.StepsOffGround == 0);
	// It reports how far the platform carried it in each step.
	CHECK(sideways.WorstDisplacementError < 1e-3f);

	// An elevator at 2 m/s, up then down: turning down does not throw the rider off, and it stays on the floor on the way
	// down instead of hovering above it.
	Ride const lift = RidePlatform({0.0f, 1.0f, 0.0f}, 2.0f);
	CHECK(lift.WorstOffset < 0.01f);
	CHECK(lift.StepsOffGround == 0);
	CHECK(lift.WorstDisplacementError < 1e-3f);

	// Down first, then up.
	Ride const drop = RidePlatform({0.0f, -1.0f, 0.0f}, 2.0f);
	CHECK(drop.WorstOffset < 0.01f);
	CHECK(drop.StepsOffGround == 0);
}

TEST_CASE("PhysicsScene: characters find their ground again when it is removed or replaced")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	REQUIRE(world->AddBody(Ground()).IsOk());
	BodyDesc const platform = MakeBox(ObstacleId, RigidBodyType::Static, {0.0f, 0.75f, 0.0f}, {1.0f, 0.25f, 1.0f});
	REQUIRE(world->AddBody(platform).IsOk());
	REQUIRE(world->AddCharacter(MakeCharacter({0.0f, 1.0f, 0.0f})).IsOk());
	REQUIRE(StateOf(*world).GroundEntity == ObstacleId);

	// Rebuilt under it (a changed collider), it still stands on it.
	REQUIRE(world->AddBody(platform).IsOk());
	CHECK(StateOf(*world).IsGrounded);
	CHECK(StateOf(*world).GroundEntity == ObstacleId);

	// Removed, it is in the air at once (not standing on what is gone until its next step), and falls to the ground.
	world->RemoveBody(ObstacleId);
	CHECK_FALSE(StateOf(*world).IsGrounded);
	CHECK_FALSE(StateOf(*world).GroundEntity.IsValid());
	Wait(*world, 60);
	CHECK(StateOf(*world).GroundEntity == GroundId);
	CHECK(std::abs(FeetOf(*world).y) < 0.01f);

	// A character standing on another one's head is in the air at once when that one is removed.
	REQUIRE(world->AddCharacter(MakeCharacter({0.0f, 2.2f, 0.0f}, OtherId)).IsOk());
	Wait(*world, 30);
	REQUIRE(StateOf(*world, OtherId).GroundEntity == HeroId);
	world->RemoveCharacter(HeroId);
	CHECK_FALSE(StateOf(*world, OtherId).IsGrounded);
	CHECK_FALSE(StateOf(*world, OtherId).GroundEntity.IsValid());
}

TEST_CASE("PhysicsScene: characters ignore the bodies of their own parts")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	REQUIRE(world->AddBody(Ground()).IsOk());
	REQUIRE(world->AddCharacter(MakeCharacter(glm::vec3(0.0f))).IsOk());

	// A solid part inside the capsule (a held object): it neither pushes the character nor touches it.
	REQUIRE(world->AddBody(MakeBox(PartId, RigidBodyType::Static, {0.15f, 1.0f, 0.0f}, {0.1f, 0.3f, 0.3f})).IsOk());
	world->SetCharacterIgnoredEntities(HeroId, {PartId});
	std::vector<ContactEvent> events = Wait(*world, 60);
	CHECK(glm::length(FeetOf(*world)) < 1e-3f);
	CHECK(StateOf(*world).IsGrounded);
	CHECK_FALSE(Testing::HasEvent(events, ContactEventType::CollisionEnter, PartId, HeroId));

	// A dynamic part overlapping it (a carried crate) is not pushed out of it.
	BodyDesc crate = MakeBox(BoxId, RigidBodyType::Dynamic, {0.0f, 1.9f, 0.0f}, glm::vec3(0.2f));
	crate.GravityFactor = 0.0f;
	REQUIRE(world->AddBody(crate).IsOk());
	world->SetCharacterIgnoredEntities(HeroId, {PartId, BoxId});
	events = Wait(*world, 30);
	CHECK(glm::length(PositionOf(*world, BoxId) - glm::vec3(0.0f, 1.9f, 0.0f)) < 1e-4f);
	CHECK(glm::length(FeetOf(*world)) < 1e-3f);
	CHECK_FALSE(Testing::HasEvent(events, ContactEventType::CollisionEnter, BoxId, HeroId));

	// A solid part in front of it (a shield) does not block its way.
	REQUIRE(world->AddBody(MakeBox(ObstacleId, RigidBodyType::Static, {1.0f, 1.0f, 0.0f}, {0.1f, 1.0f, 2.0f})).IsOk());
	world->SetCharacterIgnoredEntities(HeroId, {PartId, BoxId, ObstacleId});
	Walk(*world, {2.0f, 0.0f, 0.0f}, 60);
	CHECK(FeetOf(*world).x == doctest::Approx(2.0f).epsilon(0.02));

	// Once it no longer ignores them, they block it again.
	world->SetCharacterIgnoredEntities(HeroId, {});
	Walk(*world, {-2.0f, 0.0f, 0.0f}, 60);
	CHECK(FeetOf(*world).x == doctest::Approx(1.1f + 0.3f + Padding).epsilon(0.01));

	// Calls for entities without a character do nothing.
	world->SetCharacterIgnoredEntities(OtherId, {PartId});
	Wait(*world, 1);
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
	glm::quat const rotation = glm::angleAxis(glm::radians(37.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	transform.Rotation = rotation;
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
	CHECK(CountLogged(logStart, "replaces its RigidBody and colliders") == 1);
	CHECK(StateOf(*world, id).IsGrounded);

	// Walking writes the position back; the entity keeps its rotation and scale exactly (the capsule does not turn). It
	// walks through the trigger, which notices it.
	std::vector<ContactEvent> events;
	for (int frame = 0; frame < 60; frame++)
	{
		world->MoveCharacter(id, {3.0f, 0.0f, 0.0f});
		std::vector<ContactEvent> const frameEvents = run(1);
		events.insert(events.end(), frameEvents.begin(), frameEvents.end());
	}
	CHECK(transform.Translation.x == doctest::Approx(3.0f).epsilon(0.02));
	CHECK(transform.Translation.y == doctest::Approx(1.0f).epsilon(0.01));
	CHECK(transform.Rotation == rotation);
	CHECK(transform.Scale == glm::vec3(1.0f));
	CHECK(Testing::HasEvent(events, ContactEventType::TriggerEnter, gate.GetUUID(), id));
	CHECK(Testing::HasEvent(events, ContactEventType::TriggerExit, gate.GetUUID(), id));

	// A smaller capsule whose center is half a meter above the entity: the entity ends up at the character's feet. Changing
	// the component changes the character, which says nothing more about the colliders it replaces.
	scene.GetRegistry().patch<CharacterControllerComponent>(hero.GetHandle(),
	                                                        [](CharacterControllerComponent& component)
	                                                        {
																component.Height = 1.0f;
																component.Offset = {0.0f, 0.5f, 0.0f};
															});
	run(60);
	CHECK(transform.Translation.y == doctest::Approx(0.0f).epsilon(0.01));
	CHECK(StateOf(*world, id).Position.y == doctest::Approx(0.5f).epsilon(0.01));
	CHECK(CountLogged(logStart, "replaces its RigidBody and colliders") == 1);

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

TEST_CASE("Scene: editing a character changes only the character, not the scene's bodies and joints")
{
	Testing::PhysicsSystemScope physics;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Characters and joints");
	Entity ground = scene.CreateEntity("Ground");
	ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
	ground.AddComponent<BoxColliderComponent>().HalfExtents = {20.0f, 0.5f, 20.0f};
	// A door hinged to the world at its left edge, limited to 30 degrees either way.
	Entity door = scene.CreateEntity("Door");
	TransformComponent& doorTransform = door.GetComponent<TransformComponent>();
	doorTransform.Translation = {10.0f, 1.1f, 0.0f};
	doorTransform.Scale = {2.0f, 2.0f, 0.1f};
	RigidBodyComponent& doorBody = door.AddComponent<RigidBodyComponent>();
	doorBody.Type = RigidBodyType::Dynamic;
	doorBody.Mass = 10.0f;
	doorBody.GravityFactor = 0.0f;
	doorBody.AllowSleep = false;
	door.AddComponent<BoxColliderComponent>();
	JointComponent& hinge = door.AddComponent<JointComponent>();
	hinge.Type = JointType::Hinge;
	hinge.Anchor = {-0.5f, 0.0f, 0.0f};
	hinge.Axis = {0.0f, 1.0f, 0.0f};
	hinge.LimitsEnabled = true;
	hinge.MinLimit = -30.0f;
	hinge.MaxLimit = 30.0f;
	// A character elsewhere, with a collider its controller replaces.
	Entity hero = scene.CreateEntity("Hero");
	hero.GetComponent<TransformComponent>().Translation = {0.0f, 1.0f, -10.0f};
	hero.AddComponent<CharacterControllerComponent>();
	hero.AddComponent<SphereColliderComponent>();
	UUID const doorId = door.GetUUID();
	UUID const heroId = hero.GetUUID();

	scene.OnRuntimeStart();
	PhysicsScene* world = scene.GetPhysicsScene();
	REQUIRE(world != nullptr);
	REQUIRE(world->HasJoint(doorId));
	auto const push = [&](int frames)
	{
		for (int frame = 0; frame < frames; frame++)
		{
			world->AddTorque(doorId, {0.0f, 200.0f, 0.0f}, ForceMode::Force);
			scene.OnUpdateRuntime(Timestep(Step));
		}
	};
	auto const doorAngle = [&]
	{
		std::optional<BodyTransform> const transform = world->GetBodyTransform(doorId);
		REQUIRE(transform);
		return glm::degrees(2.0f * std::atan2(transform->Rotation.y, transform->Rotation.w));
	};
	auto const setCharacter = [&](auto change)
	{
		scene.GetRegistry().patch<CharacterControllerComponent>(hero.GetHandle(), change);
	};

	// Pushed against its limit, the door rests there.
	push(120);
	CHECK(doorAngle() == doctest::Approx(30.0f).epsilon(0.03));

	// Scripts write a character's fields often, many times with the same value (a better jump every step, crouching,
	// ladders): the door stays at its limit. Remaking the joints where the bodies are now would move its limits along.
	for (int edit = 0; edit < 3; edit++)
	{
		setCharacter(
			[](CharacterControllerComponent& component)
			{
				component.GravityFactor = 1.0f;
			});
		push(60);
		CHECK(doorAngle() == doctest::Approx(30.0f).epsilon(0.03));
	}

	// The edits reach the character: a lower one (a 1 m sphere) rests lower. It said once that it replaces the hero's
	// collider, when it was made.
	setCharacter(
		[](CharacterControllerComponent& component)
		{
			component.Height = 1.0f;
		});
	push(60);
	CHECK(StateOf(*world, heroId).Position.y == doctest::Approx(0.5f).epsilon(0.01));
	CHECK(doorAngle() == doctest::Approx(30.0f).epsilon(0.03));
	CHECK(CountLogged(logStart, "replaces its RigidBody and colliders") == 1);
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: characters ignore the colliders of the entities below them")
{
	Testing::PhysicsSystemScope physics;
	Scene scene("Character parts");
	Entity ground = scene.CreateEntity("Ground");
	ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
	ground.AddComponent<BoxColliderComponent>().HalfExtents = {20.0f, 0.5f, 20.0f};
	Entity hero = scene.CreateEntity("Hero");
	TransformComponent& transform = hero.GetComponent<TransformComponent>();
	transform.Translation = {0.0f, 1.0f, 0.0f};
	hero.AddComponent<CharacterControllerComponent>();
	// A solid part inside its capsule (a held object) and a trigger around it (its reach).
	Entity held = scene.CreateEntity("Held", hero);
	held.GetComponent<TransformComponent>().Translation = {0.3f, 0.0f, 0.0f};
	held.AddComponent<BoxColliderComponent>().HalfExtents = {0.1f, 0.3f, 0.3f};
	Entity reach = scene.CreateEntity("Reach", hero);
	SphereColliderComponent& reachCollider = reach.AddComponent<SphereColliderComponent>();
	reachCollider.Radius = 1.5f;
	reachCollider.IsTrigger = true;
	UUID const id = hero.GetUUID();
	auto const walk = [&scene, id](glm::vec3 const& velocity, int frames)
	{
		std::vector<ContactEvent> events;
		for (int frame = 0; frame < frames; frame++)
		{
			scene.GetPhysicsScene()->MoveCharacter(id, velocity);
			scene.OnUpdateRuntime(Timestep(Step));
			events.insert(events.end(), scene.GetContactEvents().begin(), scene.GetContactEvents().end());
		}
		return events;
	};

	// Standing still, it stays where it was put: its parts neither push it away nor report touching it.
	scene.OnRuntimeStart();
	std::vector<ContactEvent> const events = walk(glm::vec3(0.0f), 120);
	CHECK(glm::length(transform.Translation - glm::vec3(0.0f, 1.0f, 0.0f)) < 1e-3f);
	CHECK(StateOf(*scene.GetPhysicsScene(), id).IsGrounded);
	CHECK_FALSE(Testing::HasEvent(events, ContactEventType::CollisionEnter, held.GetUUID(), id));
	CHECK_FALSE(Testing::HasEvent(events, ContactEventType::TriggerEnter, reach.GetUUID(), id));

	// A solid part in front of it (a shield, added while running) does not block its way; its parts go with it.
	Entity shield = scene.CreateEntity("Shield", hero);
	shield.GetComponent<TransformComponent>().Translation = {0.8f, 0.0f, 0.0f};
	shield.AddComponent<BoxColliderComponent>().HalfExtents = {0.1f, 0.8f, 0.8f};
	walk({2.0f, 0.0f, 0.0f}, 60);
	CHECK(transform.Translation.x == doctest::Approx(2.0f).epsilon(0.02));
	CHECK(scene.GetWorldTransform(shield)[3].x == doctest::Approx(2.8f).epsilon(0.02));

	// Taken away from it, the shield is in its way: it stops in front of it.
	REQUIRE(scene.SetParent(shield, Entity()).IsOk());
	walk({2.0f, 0.0f, 0.0f}, 60);
	float const shieldFace = scene.GetWorldTransform(shield)[3].x - 0.1f;
	CHECK(transform.Translation.x == doctest::Approx(shieldFace - 0.5f - Padding).epsilon(0.01));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: characters below a moving parent follow it and write their positions in its space")
{
	Testing::PhysicsSystemScope physics;
	Scene scene("Character rig");
	Entity ground = scene.CreateEntity("Ground");
	ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
	ground.AddComponent<BoxColliderComponent>().HalfExtents = {20.0f, 0.5f, 20.0f};
	// A rig turned a quarter turn to the left carries the character: the rig's +Z is the world's +X.
	Entity rig = scene.CreateEntity("Rig");
	TransformComponent& rigTransform = rig.GetComponent<TransformComponent>();
	rigTransform.Translation = {2.0f, 0.0f, 0.0f};
	rigTransform.Rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
	Entity hero = scene.CreateEntity("Hero", rig);
	TransformComponent& transform = hero.GetComponent<TransformComponent>();
	transform.Translation = {0.0f, 1.0f, 0.0f};
	hero.AddComponent<CharacterControllerComponent>();
	UUID const id = hero.GetUUID();
	auto const worldPosition = [&scene, &hero]
	{
		return glm::vec3(scene.GetWorldTransform(hero)[3]);
	};

	scene.OnRuntimeStart();
	PhysicsScene* world = scene.GetPhysicsScene();
	REQUIRE(world != nullptr);
	CHECK(glm::length(StateOf(*world, id).Position - glm::vec3(2.0f, 1.0f, 0.0f)) < 1e-4f);

	// Walking along the world's +X for a second: its translation in the rig's space follows, its rotation stays. Then it
	// stands, drawn where its character is.
	for (int frame = 0; frame < 60; frame++)
	{
		world->MoveCharacter(id, {2.0f, 0.0f, 0.0f});
		scene.OnUpdateRuntime(Timestep(Step));
	}
	scene.OnUpdateRuntime(Timestep(Step));
	glm::vec3 const walked = worldPosition();
	CHECK(walked.x == doctest::Approx(4.0f).epsilon(0.02));
	CHECK(glm::length(walked - StateOf(*world, id).Position) < 1e-3f);
	CHECK(transform.Translation.z == doctest::Approx(walked.x - 2.0f).epsilon(1e-3));
	CHECK(std::abs(transform.Translation.x) < 1e-3f);
	CHECK(transform.Translation.y == doctest::Approx(1.0f).epsilon(0.01));
	CHECK(transform.Rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

	// Jumping when the rig moves 5 m: the character goes with it, keeps rising, and lands where the rig took it.
	world->SetCharacterVelocity(id, {0.0f, 4.0f, 0.0f});
	scene.OnUpdateRuntime(Timestep(Step));
	rigTransform.Translation.z += 5.0f;
	scene.OnUpdateRuntime(Timestep(Step));
	CharacterState const carried = StateOf(*world, id);
	CHECK(carried.Position.z == doctest::Approx(5.0f).epsilon(1e-3));
	CHECK(carried.Position.x == doctest::Approx(walked.x).epsilon(1e-3));
	CHECK(carried.Velocity.y > 3.0f);
	CHECK_FALSE(carried.IsGrounded);
	for (int frame = 0; frame < 90; frame++)
	{
		scene.OnUpdateRuntime(Timestep(Step));
	}
	CHECK(StateOf(*world, id).IsGrounded);
	glm::vec3 const landed = worldPosition();
	CHECK(landed.x == doctest::Approx(walked.x).epsilon(1e-3));
	CHECK(landed.y == doctest::Approx(1.0f).epsilon(0.01));
	CHECK(landed.z == doctest::Approx(5.0f).epsilon(1e-3));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: characters are drawn between their last two steps, so they move smoothly at any frame rate")
{
	Testing::PhysicsSystemScope physics;
	Scene scene("Smooth");
	Entity ground = scene.CreateEntity("Ground");
	ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
	ground.AddComponent<BoxColliderComponent>().HalfExtents = {20.0f, 0.5f, 20.0f};
	Entity hero = scene.CreateEntity("Hero");
	TransformComponent& transform = hero.GetComponent<TransformComponent>();
	transform.Translation = {0.0f, 1.0f, 0.0f};
	hero.AddComponent<CharacterControllerComponent>();
	// A camera at eye height, as first-person games have.
	Entity head = scene.CreateEntity("Head", hero);
	head.GetComponent<TransformComponent>().Translation = {0.0f, 0.7f, 0.0f};
	UUID const id = hero.GetUUID();
	scene.OnRuntimeStart();
	PhysicsScene* world = scene.GetPhysicsScene();
	REQUIRE(world != nullptr);

	// 180 frames a second, three a step, walking at 3 m/s: once it is under way, the entity and the camera on it move on by
	// the same distance every frame, not a step's distance every third frame.
	float const frameTime = Step / 3.0f;
	float const perFrame = 3.0f * frameTime;
	std::vector<float> drawn;
	for (int frame = 0; frame < 90; frame++)
	{
		world->MoveCharacter(id, {3.0f, 0.0f, 0.0f});
		scene.OnUpdateRuntime(Timestep(frameTime));
		drawn.push_back(transform.Translation.x);
	}
	int unevenFrames = 0;
	for (size_t frame = 4; frame < drawn.size(); frame++)
	{
		unevenFrames += std::abs(drawn[frame] - drawn[frame - 1] - perFrame) > 0.02f * perFrame ? 1 : 0;
	}
	CHECK(unevenFrames == 0);
	CHECK(scene.GetWorldTransform(head)[3].x == doctest::Approx(transform.Translation.x));
	// The last frame finished a step: the entity is drawn a step behind its character.
	CHECK(transform.Translation.x == doctest::Approx(StateOf(*world, id).Position.x - 3.0f * Step).epsilon(1e-4));
	CHECK(transform.Translation.y == doctest::Approx(1.0f).epsilon(0.01));

	// Stopped, it comes to rest where its character is, without a jump.
	for (int frame = 0; frame < 6; frame++)
	{
		scene.OnUpdateRuntime(Timestep(frameTime));
		CHECK(transform.Translation.x - drawn.back() <= perFrame * 1.02f);
		drawn.push_back(transform.Translation.x);
	}
	CHECK(transform.Translation.x == doctest::Approx(StateOf(*world, id).Position.x).epsilon(1e-5));
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: characters riding platforms are drawn on them between steps")
{
	Testing::PhysicsSystemScope physics;
	// Platforms that scripts move at each step (in OnFixedUpdate) up and down like elevators, or to and fro, at 2 m/s, a
	// second each way; at 60 frames a second (a step each) and at 120 (a step every other frame).
	for (glm::vec3 const direction : {glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f)})
	{
		for (int const framesPerStep : {1, 2})
		{
			CAPTURE(direction.y);
			CAPTURE(framesPerStep);
			Scene scene("Platform");
			Entity platform = scene.CreateEntity("Platform");
			TransformComponent& platformTransform = platform.GetComponent<TransformComponent>();
			platformTransform.Translation = {0.0f, 2.25f, 0.0f};
			platform.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
			platform.AddComponent<BoxColliderComponent>().HalfExtents = {2.0f, 0.25f, 2.0f};
			Entity hero = scene.CreateEntity("Hero");
			TransformComponent& transform = hero.GetComponent<TransformComponent>();
			transform.Translation = {0.0f, 3.5f, 0.0f};
			hero.AddComponent<CharacterControllerComponent>();
			float const frameTime = Step / static_cast<float>(framesPerStep);
			scene.OnRuntimeStart();
			for (int frame = 0; frame < 10 * framesPerStep; frame++)
			{
				scene.OnUpdateRuntime(Timestep(frameTime));
			}
			REQUIRE(StateOf(*scene.GetPhysicsScene(), hero.GetUUID()).GroundEntity == platform.GetUUID());

			// Drawn, it keeps its spot on the drawn platform: the platform's motion is not drawn a step late.
			glm::vec3 const spot = transform.Translation - platformTransform.Translation;
			float worstOffset = 0.0f;
			for (int frame = 0; frame < 120 * framesPerStep; frame++)
			{
				// Moved before the frames that end with a step.
				if ((frame + 1) % framesPerStep == 0)
				{
					int const step = frame / framesPerStep;
					float const sign = (step / 60) % 2 == 0 ? 1.0f : -1.0f;
					platformTransform.Translation += direction * (sign * 2.0f * Step);
				}
				scene.OnUpdateRuntime(Timestep(frameTime));
				worstOffset = std::max(worstOffset, glm::length(transform.Translation - platformTransform.Translation - spot));
			}
			CHECK(worstOffset < 0.01f);
			scene.OnRuntimeStop();
		}
	}
}

TEST_CASE("Scene: characters changed or moved between steps")
{
	Testing::PhysicsSystemScope physics;
	Scene scene("Between steps");
	Entity ground = scene.CreateEntity("Ground");
	ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
	ground.AddComponent<BoxColliderComponent>().HalfExtents = {20.0f, 0.5f, 20.0f};
	Entity hero = scene.CreateEntity("Hero");
	TransformComponent& transform = hero.GetComponent<TransformComponent>();
	transform.Translation = {0.0f, 1.0f, 0.0f};
	hero.AddComponent<CharacterControllerComponent>();
	// A kinematic part it holds out in front of it: its body follows its entity at each step.
	Entity held = scene.CreateEntity("Held", hero);
	held.GetComponent<TransformComponent>().Translation = {0.6f, 0.0f, 0.0f};
	held.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
	held.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.1f);
	UUID const id = hero.GetUUID();
	float const frameTime = Step / 3.0f;
	scene.OnRuntimeStart();
	PhysicsScene* world = scene.GetPhysicsScene();
	REQUIRE(world != nullptr);
	auto const walk = [&](int frames)
	{
		for (int frame = 0; frame < frames; frame++)
		{
			world->MoveCharacter(id, {3.0f, 0.0f, 0.0f});
			scene.OnUpdateRuntime(Timestep(frameTime));
		}
	};

	// Walking at 180 frames a second. The steps start from where the last step left the character, not from where it was
	// drawn: the part it holds follows it there (its body reaches, during a step, where the character was before it).
	walk(30);
	for (int frame = 0; frame < 9; frame++)
	{
		walk(1);
		if (frame % 3 == 2)
		{
			std::optional<BodyTransform> const part = world->GetBodyTransform(held.GetUUID());
			REQUIRE(part);
			CHECK(part->Position.x == doctest::Approx(StateOf(*world, id).Position.x - 3.0f * Step + 0.6f).epsilon(1e-4));
		}
	}

	// Changed between steps (as scripts change fields in OnUpdate), the character stays where it is, not where it was drawn.
	scene.OnUpdateRuntime(Timestep(frameTime));
	glm::vec3 const before = StateOf(*world, id).Position;
	REQUIRE(transform.Translation.x < before.x - 0.01f);
	scene.GetRegistry().patch<CharacterControllerComponent>(hero.GetHandle(),
	                                                        [](CharacterControllerComponent& component)
	                                                        {
																component.GravityFactor = 1.0f;
															});
	scene.ApplyPhysicsChanges();
	CHECK(glm::length(StateOf(*world, id).Position - before) < 1e-5f);

	// Moved between steps (by a script or the editor), it stays where it was put, and the next step takes its character there.
	transform.Translation = {5.0f, 1.0f, 2.0f};
	scene.OnUpdateRuntime(Timestep(frameTime));
	CHECK(transform.Translation == glm::vec3(5.0f, 1.0f, 2.0f));
	scene.OnUpdateRuntime(Timestep(frameTime));
	CHECK(glm::length(StateOf(*world, id).Position - glm::vec3(5.0f, 1.0f, 2.0f)) < 0.01f);
	CHECK(glm::length(transform.Translation - glm::vec3(5.0f, 1.0f, 2.0f)) < 0.01f);
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: a character the physics refuses is left out until its component is fixed")
{
	Testing::PhysicsSystemScope physics;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Refused character");
	Entity ground = scene.CreateEntity("Ground");
	ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
	ground.AddComponent<BoxColliderComponent>().HalfExtents = {20.0f, 0.5f, 20.0f};
	Entity hero = scene.CreateEntity("Hero");
	TransformComponent& transform = hero.GetComponent<TransformComponent>();
	transform.Translation = {0.0f, 3.0f, 0.0f};
	hero.AddComponent<CharacterControllerComponent>();
	UUID const id = hero.GetUUID();
	auto const setHeight = [&scene, &hero](float height)
	{
		scene.GetRegistry().patch<CharacterControllerComponent>(hero.GetHandle(),
		                                                        [height](CharacterControllerComponent& component)
		                                                        {
																	component.Height = height;
																});
	};

	scene.OnRuntimeStart();
	PhysicsScene* world = scene.GetPhysicsScene();
	REQUIRE(world != nullptr);
	REQUIRE(world->HasCharacter(id));

	// Taller than the physics world allows (the field only asks for a positive height): refused with a warning, and the
	// entity has no character, so it stays where it is.
	setHeight(2.0e6f);
	scene.OnUpdateRuntime(Timestep(Step));
	CHECK(Testing::WasLogged(logStart, "the character is larger than the physics world allows"));
	CHECK_FALSE(world->HasCharacter(id));
	CHECK_FALSE(world->HasBody(id));
	glm::vec3 const left = transform.Translation;
	for (int frame = 0; frame < 30; frame++)
	{
		scene.OnUpdateRuntime(Timestep(Step));
	}
	CHECK(transform.Translation == left);

	// Fixed, it is a character again where its entity is, and falls from there.
	setHeight(2.0f);
	scene.OnUpdateRuntime(Timestep(Step));
	REQUIRE(world->HasCharacter(id));
	CHECK(StateOf(*world, id).Position.x == doctest::Approx(left.x));
	CHECK_FALSE(StateOf(*world, id).IsGrounded);
	for (int frame = 0; frame < 90; frame++)
	{
		scene.OnUpdateRuntime(Timestep(Step));
	}
	CHECK(StateOf(*world, id).IsGrounded);
	CHECK(transform.Translation.y == doctest::Approx(1.0f).epsilon(0.01));
	scene.OnRuntimeStop();
}
