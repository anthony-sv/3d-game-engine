#pragma once

#include "Physics/PhysicsTestUtilities.h"

#include "Strada/Asset/Vehicle.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Physics/PhysicsTypes.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <optional>
#include <string>

// What the cases of the engine's damping in neutral and of a vehicle's reset share (PhysicsVehicleNeutralTests.cpp,
// PhysicsVehicleResetTests.cpp): a flat ground, a bike's chassis and a world to ride it in, as PhysicsVehicleTests.cpp makes
// them.
namespace Strada::Testing
{
	inline constexpr float VehicleRideStep = 1.0f / 60.0f;

	inline Scope<PhysicsScene> CreateVehicleWorld(PhysicsWorldSettings const& settings = {})
	{
		Result<Scope<PhysicsScene>> world = PhysicsScene::Create(settings);
		REQUIRE(world.IsOk());
		return world.TakeValue();
	}

	inline void RequireVehicleOk(Result<void> const& result)
	{
		REQUIRE_MESSAGE(result.IsOk(), (result ? std::string() : result.GetError()));
	}

	// A flat static ground whose top is at y = 0.
	inline BodyDesc MakeVehicleGround(UUID entity)
	{
		BodyDesc desc;
		desc.Entity = entity;
		desc.Type = RigidBodyType::Static;
		desc.Position = glm::vec3(0.0f, -0.5f, 0.0f);
		ColliderDesc collider;
		collider.HalfExtents = glm::vec3(400.0f, 0.5f, 400.0f);
		desc.Colliders.push_back(collider);
		return desc;
	}

	// A 250 kg chassis box from 0.25 m to 1.25 m above its origin, 0.5 m wide and 1.2 m long, whose roll inertia about its
	// front (26 kg m²) is above the default lean damping times the step, which Jolt's lean spring needs (Docs/Architecture.md
	// §8, "Vehicles").
	inline BodyDesc MakeVehicleChassis(UUID entity, glm::vec3 const& position, bool allowSleep = false)
	{
		BodyDesc desc;
		desc.Entity = entity;
		desc.Type = RigidBodyType::Dynamic;
		desc.Position = position;
		desc.Mass = 250.0f;
		desc.LinearDamping = 0.0f;
		desc.AllowSleep = allowSleep;
		ColliderDesc collider;
		collider.HalfExtents = glm::vec3(0.25f, 0.5f, 0.6f);
		collider.Offset = glm::vec3(0.0f, 0.75f, 0.0f);
		desc.Colliders.push_back(collider);
		return desc;
	}

	// A world with the ground and one bike of the definition on it, at the position given.
	inline Scope<PhysicsScene> MakeVehicleRide(UUID ground, UUID bike, Vehicle const& definition,
	                                           glm::vec3 const& position = glm::vec3(0.0f, 0.1f, 0.0f))
	{
		Scope<PhysicsScene> world = CreateVehicleWorld();
		RequireVehicleOk(world->AddBody(MakeVehicleGround(ground)));
		RequireVehicleOk(world->AddBody(MakeVehicleChassis(bike, position)));
		RequireVehicleOk(world->AddVehicle(MakeVehicleDesc(bike, definition)));
		return world;
	}

	inline VehicleState ReadVehicleState(PhysicsScene const& world, UUID entity)
	{
		std::optional<VehicleState> const state = world.GetVehicleState(entity);
		REQUIRE(state);
		return *state;
	}

	inline BodyTransform ReadBodyTransform(PhysicsScene const& world, UUID entity)
	{
		std::optional<BodyTransform> const transform = world.GetBodyTransform(entity);
		REQUIRE(transform);
		return *transform;
	}
}
