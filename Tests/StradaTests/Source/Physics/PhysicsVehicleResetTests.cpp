#include "Physics/PhysicsTestUtilities.h"
#include "Physics/VehicleRideTestUtilities.h"

#include "Strada/Asset/Vehicle.h"
#include "Strada/Core/UUID.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Physics/PhysicsTypes.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

using namespace Strada;

// A vehicle's reset (Docs/Architecture.md §8, "The reset"): made again on its body with the values it runs with, carrying
// only its latest inputs, at rest where it stands. Each case compares a world whose vehicle is reset with a twin whose
// vehicle is removed, made again from its definition and stopped (both velocities set to zero, its latest inputs given
// again): their hashes are equal at the reset and at every step after it under equal inputs.
namespace
{
	constexpr float Step = Testing::VehicleRideStep;
	constexpr int RideOn = 300;

	UUID const GroundId(501);
	UUID const BikeId(510);
	UUID const OtherBikeId(520);

	Vehicle MakeManualBike()
	{
		Vehicle bike;
		bike.Transmission.Mode = VehicleTransmissionMode::Manual;
		bike.Transmission.SwitchTime = 0.1f;
		bike.Transmission.ClutchReleaseTime = 0.1f;
		bike.Lean.SpringDamping = 800.0f;
		return bike;
	}

	VehicleInputs Inputs(float throttle, float frontBrake = 0.0f, float rearBrake = 0.0f, float lean = 0.0f)
	{
		VehicleInputs inputs;
		inputs.Throttle = throttle;
		inputs.FrontBrake = frontBrake;
		inputs.RearBrake = rearBrake;
		inputs.Lean = lean;
		return inputs;
	}

	// Two worlds that ride one bike alike: Reset resets it, Fresh makes it again from its definition.
	struct Twins
	{
		VehicleDesc Source;
		Scope<PhysicsScene> Reset;
		Scope<PhysicsScene> Fresh;

		explicit Twins(Vehicle const& definition)
			: Source(MakeVehicleDesc(BikeId, definition)),
			  Reset(Testing::MakeVehicleRide(GroundId, BikeId, definition)),
			  Fresh(Testing::MakeVehicleRide(GroundId, BikeId, definition))
		{
		}

		void Ride(VehicleInputs const& inputs, int shift = 0)
		{
			for (PhysicsScene* world : {Reset.get(), Fresh.get()})
			{
				world->SetVehicleInputs(BikeId, inputs);
				if (shift != 0)
				{
					world->RequestVehicleShift(BikeId, shift);
				}
				world->Step(Step);
			}
		}

		bool Alike() const { return Reset->ComputeStateHash() == Fresh->ComputeStateHash(); }

		// The reset, and in the twin what it stands for.
		void ResetBoth()
		{
			uint64_t const structure = Reset->GetStructureVersion();
			REQUIRE(Reset->ResetVehicle(BikeId));
			CHECK(Reset->GetStructureVersion() == structure);
			VehicleInputs const latest = Testing::ReadVehicleState(*Fresh, BikeId).Inputs;
			Fresh->RemoveVehicle(BikeId);
			Testing::RequireVehicleOk(Fresh->AddVehicle(Source));
			Fresh->SetLinearVelocity(BikeId, glm::vec3(0.0f));
			Fresh->SetAngularVelocity(BikeId, glm::vec3(0.0f));
			Fresh->SetVehicleInputs(BikeId, latest);
		}

		// Rides both on alike for RideOn steps, the inputs a function of the step; counts the steps whose hashes agree.
		size_t RideOnAlike(std::function<VehicleInputs(int)> const& inputs, std::function<int(int)> const& shifts = {})
		{
			size_t alike = 0;
			for (int step = 0; step < RideOn; step++)
			{
				Ride(inputs(step), shifts ? shifts(step) : 0);
				alike += Alike() ? 1 : 0;
			}
			return alike;
		}
	};

	// What a vehicle made where it stands holds before its first step: the engine at idle, neutral with the clutch closed
	// and no shift, its wheels still, straight, at their longest and without contact, nothing applied yet, and the chassis
	// at rest.
	void CheckAsMade(PhysicsScene const& world, VehicleDesc const& desc, VehicleInputs const& latest)
	{
		VehicleState const state = Testing::ReadVehicleState(world, BikeId);
		CHECK(state.EngineSpeed == desc.MinRPM);
		CHECK(state.Gear == 0);
		CHECK(state.ClutchFriction == 1.0f);
		CHECK_FALSE(state.IsShifting);
		CHECK(state.ForwardSpeed == 0.0f);
		CHECK(state.AppliedInputs == VehicleInputs());
		CHECK(state.Inputs == latest);
		for (size_t index = 0; index < VehicleWheelCount; index++)
		{
			CAPTURE(index);
			VehicleWheelState const& wheel = state.Wheels[index];
			CHECK(wheel.AngularVelocity == 0.0f);
			CHECK(wheel.RotationAngle == 0.0f);
			CHECK(wheel.SteerAngle == 0.0f);
			CHECK_FALSE(wheel.ContactEntity.IsValid());
			CHECK(wheel.SuspensionLength == desc.Wheels[index].SuspensionMaxLength);
			CHECK(wheel.LongitudinalSlip == 0.0f);
			CHECK(wheel.LateralSlip == 0.0f);
		}
		CHECK(world.GetLinearVelocity(BikeId) == glm::vec3(0.0f));
		CHECK(world.GetAngularVelocity(BikeId) == glm::vec3(0.0f));
	}

	VehicleInputs RideOnInputs(int step)
	{
		return Inputs(step < 200 ? 0.5f : 0.0f, step >= 200 ? 0.4f : 0.0f, 0.0f, step >= 60 && step < 120 ? 0.2f : 0.0f);
	}

	// A Manual gearbox's ride on from neutral: first gear at once, second after a second.
	int RideOnShifts(int step)
	{
		return step == 0 || step == 60 ? 1 : 0;
	}

	float HorizontalDistance(glm::vec3 const& from, glm::vec3 const& to)
	{
		return glm::length(glm::vec2(to.x - from.x, to.z - from.z));
	}
}

TEST_CASE("PhysicsScene: a vehicle reset from speed in a high gear is a new vehicle at rest where it stands, hashed as one made "
          "there, and rides on alike")
{
	Testing::PhysicsSystemScope physics;
	Twins twins(MakeManualBike());
	for (int step = 0; step < 240; step++)
	{
		twins.Ride(Inputs(0.6f), step == 0 || step == 60 || step == 120 ? 1 : 0);
	}
	VehicleState const riding = Testing::ReadVehicleState(*twins.Reset, BikeId);
	REQUIRE(riding.Gear == 3);
	REQUIRE(riding.ForwardSpeed > 10.0f);
	REQUIRE(riding.EngineSpeed > 2.0f * twins.Source.MinRPM);
	REQUIRE(twins.Alike());
	BodyTransform const before = Testing::ReadBodyTransform(*twins.Reset, BikeId);

	twins.ResetBoth();
	CheckAsMade(*twins.Reset, *twins.Reset->FindVehicleSettings(BikeId), Inputs(0.6f));
	BodyTransform const after = Testing::ReadBodyTransform(*twins.Reset, BikeId);
	CHECK(after.Position == before.Position);
	CHECK(after.Rotation == before.Rotation);
	CHECK(twins.Alike());
	CHECK(twins.RideOnAlike(RideOnInputs, RideOnShifts) == RideOn);
	CHECK(Testing::ReadVehicleState(*twins.Reset, BikeId).Gear == 2);
}

TEST_CASE("PhysicsScene: a vehicle reset after a fall past 55 degrees is a new vehicle where it lies, and rides on alike")
{
	Testing::PhysicsSystemScope physics;
	Twins twins(MakeManualBike());
	for (int step = 0; step < 90; step++)
	{
		twins.Ride(Inputs(0.6f), step == 0 ? 1 : 0);
	}
	// Thrown over to its right, the throttle closed: rolled about its front (-Z) until it lies past 55 degrees.
	int steps = 0;
	while (steps < 120 && std::abs(Testing::ReadVehicleState(*twins.Reset, BikeId).LeanAngle) < glm::radians(55.0f))
	{
		for (PhysicsScene* world : {twins.Reset.get(), twins.Fresh.get()})
		{
			world->SetAngularVelocity(BikeId, glm::vec3(0.0f, 0.0f, -4.0f));
		}
		twins.Ride(Inputs(0.0f));
		steps++;
	}
	REQUIRE(std::abs(Testing::ReadVehicleState(*twins.Reset, BikeId).LeanAngle) >= glm::radians(55.0f));
	REQUIRE(twins.Alike());

	twins.ResetBoth();
	CheckAsMade(*twins.Reset, *twins.Reset->FindVehicleSettings(BikeId), Inputs(0.0f));
	CHECK(twins.Alike());
	CHECK(twins.RideOnAlike(RideOnInputs, RideOnShifts) == RideOn);
}

TEST_CASE("PhysicsScene: a vehicle reset in the middle of a shift drops it and the shift asked before the reset, makes the one asked "
          "after, and rides on alike, in a Manual and an Automatic gearbox")
{
	Testing::PhysicsSystemScope physics;
	SUBCASE("Manual")
	{
		Twins twins(MakeManualBike());
		for (int step = 0; step < 120; step++)
		{
			twins.Ride(Inputs(0.6f), step == 0 || step == 60 || step == 118 ? 1 : 0);
		}
		VehicleState const shifting = Testing::ReadVehicleState(*twins.Reset, BikeId);
		REQUIRE(shifting.IsShifting);
		REQUIRE(shifting.Gear == 3);
		// A shift asked before the reset is dropped with it; the twin's goes with its vehicle.
		for (PhysicsScene* world : {twins.Reset.get(), twins.Fresh.get()})
		{
			world->RequestVehicleShift(BikeId, 1);
		}
		twins.ResetBoth();
		CheckAsMade(*twins.Reset, *twins.Reset->FindVehicleSettings(BikeId), Inputs(0.6f));
		CHECK(twins.Alike());
		// One asked after it is made at the next step: first gear, from neutral.
		twins.Ride(Inputs(0.6f), 1);
		CHECK(Testing::ReadVehicleState(*twins.Reset, BikeId).Gear == 1);
		CHECK(twins.Alike());
		CHECK(twins.RideOnAlike(RideOnInputs) == RideOn);
	}
	SUBCASE("Automatic")
	{
		// Shift points the bike reaches at this throttle without spinning its rear tyre.
		Vehicle automatic;
		automatic.Transmission.ShiftUpRPM = 4000.0f;
		automatic.Transmission.ShiftDownRPM = 1500.0f;
		automatic.Lean.SpringDamping = 800.0f;
		Twins twins(automatic);
		int steps = 0;
		auto const shifting = [&twins]
		{
			VehicleState const state = Testing::ReadVehicleState(*twins.Reset, BikeId);
			return state.IsShifting && state.Gear >= 2;
		};
		while (steps < 900 && !shifting())
		{
			twins.Ride(Inputs(0.6f));
			steps++;
		}
		REQUIRE(shifting());
		twins.ResetBoth();
		CheckAsMade(*twins.Reset, *twins.Reset->FindVehicleSettings(BikeId), Inputs(0.6f));
		CHECK(twins.Alike());
		CHECK(twins.RideOnAlike(RideOnInputs) == RideOn);
	}
}

TEST_CASE("PhysicsScene: a snapshot taken before a vehicle's reset restores after it, and one taken after it restores, each "
          "reproducing the steps that followed it")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = Testing::MakeVehicleRide(GroundId, BikeId, MakeManualBike());
	auto const drive = [&world](int count)
	{
		world->SetVehicleInputs(BikeId, RideOnInputs(count));
		if (int const shift = RideOnShifts(count); shift != 0)
		{
			world->RequestVehicleShift(BikeId, shift);
		}
		world->Step(Step);
		return world->ComputeStateHash();
	};
	for (int step = 0; step < 120; step++)
	{
		drive(step);
	}
	// Before: the steps after the snapshot, which a reset then cuts short.
	PhysicsSnapshot const earlier = world->SaveState();
	uint64_t const earlierHash = world->ComputeStateHash();
	std::vector<uint64_t> fromEarlier;
	for (int step = 120; step < 150; step++)
	{
		fromEarlier.push_back(drive(step));
	}
	REQUIRE(world->ResetVehicle(BikeId));
	for (int step = 0; step < 20; step++)
	{
		drive(step);
	}
	Testing::RequireVehicleOk(world->RestoreState(earlier));
	CHECK(world->ComputeStateHash() == earlierHash);
	std::vector<uint64_t> replayed;
	for (int step = 120; step < 150; step++)
	{
		replayed.push_back(drive(step));
	}
	CHECK(replayed == fromEarlier);

	// After: taken right after a reset, before any step.
	REQUIRE(world->ResetVehicle(BikeId));
	PhysicsSnapshot const later = world->SaveState();
	uint64_t const laterHash = world->ComputeStateHash();
	std::vector<uint64_t> fromLater;
	for (int step = 0; step < 90; step++)
	{
		fromLater.push_back(drive(step));
	}
	Testing::RequireVehicleOk(world->RestoreState(later));
	CHECK(world->ComputeStateHash() == laterHash);
	CheckAsMade(*world, *world->FindVehicleSettings(BikeId), RideOnInputs(149));
	std::vector<uint64_t> again;
	for (int step = 0; step < 90; step++)
	{
		again.push_back(drive(step));
	}
	CHECK(again == fromLater);
}

TEST_CASE("PhysicsScene: vehicles asleep reset in either order wake, leave the same world, ride on alike and fall asleep again, as "
          "one made there does")
{
	Testing::PhysicsSystemScope physics;
	// Two worlds of two bikes that may sleep, standing with no inputs until both sleep.
	auto const makeWorld = []
	{
		Scope<PhysicsScene> made = Testing::CreateVehicleWorld();
		Testing::RequireVehicleOk(made->AddBody(Testing::MakeVehicleGround(GroundId)));
		for (auto const& [entity, x] : {std::pair<UUID, float>{BikeId, -3.0f}, std::pair<UUID, float>{OtherBikeId, 3.0f}})
		{
			Testing::RequireVehicleOk(made->AddBody(Testing::MakeVehicleChassis(entity, glm::vec3(x, 0.1f, 0.0f), true)));
			Testing::RequireVehicleOk(made->AddVehicle(MakeVehicleDesc(entity, MakeManualBike())));
		}
		return made;
	};
	auto const sleepBoth = [](PhysicsScene& target)
	{
		for (int tick = 0; tick < 900 && !(target.IsSleeping(BikeId) && target.IsSleeping(OtherBikeId)); tick++)
		{
			target.Step(Step);
		}
		REQUIRE(target.IsSleeping(BikeId));
		REQUIRE(target.IsSleeping(OtherBikeId));
	};
	Scope<PhysicsScene> forward = makeWorld();
	Scope<PhysicsScene> backward = makeWorld();
	sleepBoth(*forward);
	sleepBoth(*backward);
	REQUIRE(forward->ComputeStateHash() == backward->ComputeStateHash());

	REQUIRE(forward->ResetVehicle(BikeId));
	REQUIRE(forward->ResetVehicle(OtherBikeId));
	REQUIRE(backward->ResetVehicle(OtherBikeId));
	REQUIRE(backward->ResetVehicle(BikeId));
	CHECK_FALSE(forward->IsSleeping(BikeId));
	CHECK_FALSE(forward->IsSleeping(OtherBikeId));
	CHECK(forward->ComputeStateHash() == backward->ComputeStateHash());
	size_t alike = 0;
	for (int step = 0; step < RideOn; step++)
	{
		forward->Step(Step);
		backward->Step(Step);
		alike += forward->ComputeStateHash() == backward->ComputeStateHash() ? 1 : 0;
	}
	CHECK(alike == RideOn);
	sleepBoth(*forward);
	sleepBoth(*backward);
	CHECK(forward->ComputeStateHash() == backward->ComputeStateHash());

	// One bike asleep, reset, against one made again there: alike from the reset on, and asleep again.
	Scope<PhysicsScene> reset = Testing::CreateVehicleWorld();
	Scope<PhysicsScene> fresh = Testing::CreateVehicleWorld();
	VehicleDesc const source = MakeVehicleDesc(BikeId, MakeManualBike());
	for (PhysicsScene* world : {reset.get(), fresh.get()})
	{
		Testing::RequireVehicleOk(world->AddBody(Testing::MakeVehicleGround(GroundId)));
		Testing::RequireVehicleOk(world->AddBody(Testing::MakeVehicleChassis(BikeId, glm::vec3(0.0f, 0.1f, 0.0f), true)));
		Testing::RequireVehicleOk(world->AddVehicle(source));
		for (int step = 0; step < 900 && !world->IsSleeping(BikeId); step++)
		{
			world->Step(Step);
		}
		REQUIRE(world->IsSleeping(BikeId));
	}
	REQUIRE(reset->ResetVehicle(BikeId));
	fresh->RemoveVehicle(BikeId);
	Testing::RequireVehicleOk(fresh->AddVehicle(source));
	fresh->SetLinearVelocity(BikeId, glm::vec3(0.0f));
	fresh->SetAngularVelocity(BikeId, glm::vec3(0.0f));
	CHECK_FALSE(reset->IsSleeping(BikeId));
	CHECK(reset->ComputeStateHash() == fresh->ComputeStateHash());
	alike = 0;
	int steps = 0;
	for (; steps < 900 && !reset->IsSleeping(BikeId); steps++)
	{
		reset->Step(Step);
		fresh->Step(Step);
		alike += reset->ComputeStateHash() == fresh->ComputeStateHash() ? 1 : 0;
	}
	CHECK(reset->IsSleeping(BikeId));
	CHECK(fresh->IsSleeping(BikeId));
	CHECK(alike == static_cast<size_t>(steps));
}

TEST_CASE("PhysicsScene: a vehicle's reset keeps the inputs given before it for its next step, and an entity without a vehicle has "
          "none, which changes nothing")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = Testing::MakeVehicleRide(GroundId, BikeId, MakeManualBike());
	for (int step = 0; step < 60; step++)
	{
		world->SetVehicleInputs(BikeId, Inputs(0.6f));
		world->RequestVehicleShift(BikeId, step == 0 ? 1 : 0);
		world->Step(Step);
	}
	VehicleInputs const given = Inputs(0.5f, 0.0f, 0.25f, 0.25f);
	world->SetVehicleInputs(BikeId, given);
	REQUIRE(world->ResetVehicle(BikeId));
	CheckAsMade(*world, *world->FindVehicleSettings(BikeId), given);
	world->Step(Step);
	CHECK(Testing::ReadVehicleState(*world, BikeId).AppliedInputs == given);

	uint64_t const hash = world->ComputeStateHash();
	uint64_t const structure = world->GetStructureVersion();
	CHECK_FALSE(world->ResetVehicle(GroundId));
	CHECK_FALSE(world->ResetVehicle(UUID(999)));
	CHECK(world->ComputeStateHash() == hash);
	CHECK(world->GetStructureVersion() == structure);
}

TEST_CASE("PhysicsScene: a bike reset at 60 m/s on its brakes stays where it was reset, and one put back upright from its roll "
          "limit and reset stays where it was put")
{
	Testing::PhysicsSystemScope physics;
	SUBCASE("from 60 m/s, its wheels spinning")
	{
		Scope<PhysicsScene> world = Testing::CreateVehicleWorld();
		Testing::RequireVehicleOk(world->AddBody(Testing::MakeVehicleGround(GroundId)));
		BodyDesc chassis = Testing::MakeVehicleChassis(BikeId, glm::vec3(0.0f, 0.1f, 0.0f));
		chassis.LinearVelocity = glm::vec3(0.0f, 0.0f, -60.0f);
		Testing::RequireVehicleOk(world->AddBody(chassis));
		Testing::RequireVehicleOk(world->AddVehicle(MakeVehicleDesc(BikeId, MakeManualBike())));
		for (int step = 0; step < 60; step++)
		{
			world->Step(Step);
		}
		VehicleState const rolling = Testing::ReadVehicleState(*world, BikeId);
		REQUIRE(rolling.ForwardSpeed > 50.0f);
		REQUIRE(rolling.Wheels[1].AngularVelocity > 150.0f);
		// Both brakes held, as a race's countdown holds them, and reset.
		world->SetVehicleInputs(BikeId, Inputs(0.0f, 1.0f, 1.0f));
		REQUIRE(world->ResetVehicle(BikeId));
		glm::vec3 const place = Testing::ReadBodyTransform(*world, BikeId).Position;
		float farthest = 0.0f;
		for (int step = 0; step < 180; step++)
		{
			world->Step(Step);
			farthest = std::max(farthest, HorizontalDistance(place, Testing::ReadBodyTransform(*world, BikeId).Position));
		}
		CHECK_MESSAGE(farthest < 0.01f, "drifted " << farthest << " m");
	}
	SUBCASE("from its roll limit")
	{
		Scope<PhysicsScene> world = Testing::MakeVehicleRide(GroundId, BikeId, MakeManualBike());
		world->SetVehicleInputs(BikeId, Inputs(0.0f, 1.0f, 1.0f));
		for (int step = 0; step < 30; step++)
		{
			world->Step(Step);
		}
		// Laid over at its 60 degree limit about its front and left there a second, its constraint pressing on the limit.
		glm::vec3 const grid(0.0f, 0.1f, 0.0f);
		world->SetBodyTransform(BikeId, grid + glm::vec3(0.0f, 0.4f, 0.0f),
		                        glm::angleAxis(glm::radians(60.0f), glm::vec3(0.0f, 0.0f, 1.0f)));
		for (int step = 0; step < 60; step++)
		{
			world->Step(Step);
		}
		// Put back upright on the grid and reset, in the same step.
		world->SetBodyTransform(BikeId, grid, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
		REQUIRE(world->ResetVehicle(BikeId));
		float farthest = 0.0f;
		float turned = 0.0f;
		for (int step = 0; step < 180; step++)
		{
			world->Step(Step);
			BodyTransform const now = Testing::ReadBodyTransform(*world, BikeId);
			farthest = std::max(farthest, HorizontalDistance(grid, now.Position));
			turned = std::max(turned, glm::degrees(glm::angle(now.Rotation)));
		}
		CHECK_MESSAGE(farthest < 0.05f, "moved " << farthest << " m");
		CHECK_MESSAGE(turned < 1.0f, "turned " << turned << " degrees");
	}
}
