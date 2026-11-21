#include "Physics/PhysicsTestUtilities.h"
#include "Physics/VehicleRideTestUtilities.h"

#include "Strada/Asset/Vehicle.h"
#include "Strada/Core/UUID.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Physics/PhysicsTypes.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace Strada;

// The engine in neutral (Docs/Architecture.md §8, "The engine in neutral"): while the engine runs free of the wheels (in
// neutral, or with a Manual gearbox's clutch held open below idle outside a shift) its damping is AngularDamping plus
// NeutralDamping times how far the throttle is closed, and AngularDamping alone otherwise.
namespace
{
	constexpr float Step = Testing::VehicleRideStep;
	constexpr float MinRPM = 1700.0f;
	constexpr float MaxRPM = 16000.0f;
	// Moto's engine damping, and the neutral damping that brings its engine from the red line to idle in about a second.
	constexpr float EngineDamping = 0.005f;
	constexpr float NeutralDamping = 2.245f;

	UUID const GroundId(401);
	UUID const BikeId(410);

	// Moto's light crank, which revs freely in neutral, and a new definition's, which the rides below take off on without
	// spinning the rear tyre.
	constexpr float LightCrank = 0.05f;
	constexpr float NewCrank = 0.5f;

	// A superbike's engine speeds (idle at 1,700 rpm, the red line at 16,000) on a new definition's bike, with Moto's engine
	// damping and the neutral damping given.
	Vehicle MakeEngineBike(VehicleTransmissionMode mode, float neutralDamping, float engineDamping = EngineDamping,
	                       float inertia = LightCrank)
	{
		Vehicle bike;
		bike.Engine.MinRPM = MinRPM;
		bike.Engine.MaxRPM = MaxRPM;
		bike.Engine.Inertia = inertia;
		bike.Engine.AngularDamping = engineDamping;
		bike.Engine.NeutralDamping = neutralDamping;
		bike.Transmission.Mode = mode;
		bike.Transmission.SwitchTime = 0.05f;
		bike.Transmission.ClutchReleaseTime = 0.1f;
		bike.Lean.SpringDamping = 800.0f;
		return bike;
	}

	VehicleInputs Inputs(float throttle, float frontBrake = 0.0f, float rearBrake = 0.0f)
	{
		VehicleInputs inputs;
		inputs.Throttle = throttle;
		inputs.FrontBrake = frontBrake;
		inputs.RearBrake = rearBrake;
		return inputs;
	}

	// Full throttle in neutral with both brakes held until the engine reaches its red line.
	void RevToTheRedLine(PhysicsScene& world)
	{
		world.SetVehicleInputs(BikeId, Inputs(1.0f, 1.0f, 1.0f));
		for (int step = 0; step < 900 && Testing::ReadVehicleState(world, BikeId).EngineSpeed < MaxRPM; step++)
		{
			world.Step(Step);
		}
		VehicleState const revved = Testing::ReadVehicleState(world, BikeId);
		REQUIRE(revved.Gear == 0);
		REQUIRE(revved.EngineSpeed == MaxRPM);
	}

	// What one collision step of the engine with nothing coupled to it and the throttle closed leaves of its speed, as
	// Jolt's VehicleEngine::ApplyDamping computes it with the damping the step sets.
	float DampedOnce(float rpm, float damping)
	{
		return std::max(rpm * std::max(0.0f, 1.0f - damping * Step), MinRPM);
	}

	// The rear wheel turns a Manual gearbox's gear at least this share above idle: far from the clutch's hold below idle.
	bool TurnsWellAboveIdle(Vehicle const& definition, VehicleState const& state)
	{
		if (state.Gear < 1)
		{
			return false;
		}
		float const ratio = definition.Transmission.GearRatios[static_cast<size_t>(state.Gear - 1)] * definition.Transmission.FinalDrive;
		float const rpm = std::abs(state.Wheels[1].AngularVelocity) * ratio * 60.0f / glm::two_pi<float>();
		return rpm > 1.2f * MinRPM;
	}
}

TEST_CASE("PhysicsScene: a revved engine in neutral falls to idle in about a second at its neutral damping, never rising or "
          "hunting, and keeps its revs without it")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = Testing::MakeVehicleRide(GroundId, BikeId, MakeEngineBike(VehicleTransmissionMode::Manual, NeutralDamping));
	RevToTheRedLine(*world);
	// With the sum c = 2.25 a second and 60 collision steps a second the speed falls by 1 - c / 60 at each step:
	// ln(1.01 x 1,700 / 16,000) / ln(1 - 2.25 / 60) = 58.4, so 1.01 times idle is reached at the 59th step, 0.98 s.
	world->SetVehicleInputs(BikeId, Inputs(0.0f, 1.0f, 1.0f));
	float previous = MaxRPM;
	int reached = -1;
	int rises = 0;
	for (int step = 1; step <= 60 * 3; step++)
	{
		world->Step(Step);
		float const rpm = Testing::ReadVehicleState(*world, BikeId).EngineSpeed;
		rises += rpm > previous ? 1 : 0;
		if (reached < 0 && rpm <= 1.01f * MinRPM)
		{
			reached = step;
		}
		if (reached >= 0)
		{
			// Held at idle from there on: Jolt clamps the speed, with no governor to hunt about it.
			CHECK(rpm >= MinRPM);
			CHECK(rpm <= 1.01f * MinRPM);
		}
		previous = rpm;
	}
	CHECK(rises == 0);
	CHECK(previous == MinRPM);
	REQUIRE(reached > 0);
	float const seconds = static_cast<float>(reached) * Step;
	CHECK_MESSAGE(seconds >= 0.90f, seconds << " s to idle");
	CHECK_MESSAGE(seconds <= 1.05f, seconds << " s to idle");

	// Without it the engine keeps its revs: 80 rpm a second at its own damping alone.
	Scope<PhysicsScene> asBuilt = Testing::MakeVehicleRide(GroundId, BikeId, MakeEngineBike(VehicleTransmissionMode::Manual, 0.0f));
	RevToTheRedLine(*asBuilt);
	asBuilt->SetVehicleInputs(BikeId, Inputs(0.0f, 1.0f, 1.0f));
	for (int step = 0; step < 60; step++)
	{
		asBuilt->Step(Step);
	}
	CHECK(Testing::ReadVehicleState(*asBuilt, BikeId).EngineSpeed > 15900.0f);
}

TEST_CASE("PhysicsScene: a ride whose closed and part throttles are all in gear gives the same hashes with the neutral damping "
          "at 0 and set, in a Manual and an Automatic gearbox")
{
	Testing::PhysicsSystemScope physics;
	for (VehicleTransmissionMode const mode : {VehicleTransmissionMode::Manual, VehicleTransmissionMode::Automatic})
	{
		CAPTURE(static_cast<int>(mode));
		Vehicle const plain = MakeEngineBike(mode, 0.0f, EngineDamping, NewCrank);
		Scope<PhysicsScene> built = Testing::MakeVehicleRide(GroundId, BikeId, plain);
		Scope<PhysicsScene> damped =
			Testing::MakeVehicleRide(GroundId, BikeId, MakeEngineBike(mode, NeutralDamping, EngineDamping, NewCrank));
		// Out of neutral at full throttle for a step (an Automatic gearbox is in neutral as it begins, and takes first gear in
		// its update), then a part throttle in gear, a smaller one, a closed throttle on the front brake, and a part throttle
		// again; a Manual gearbox goes into first at the first step, up to second at the 90th and down to first at the 330th.
		auto const inputs = [](int step)
		{
			if (step == 0)
			{
				return Inputs(1.0f);
			}
			if (step < 180 || step >= 300)
			{
				return Inputs(0.6f);
			}
			return step < 240 ? Inputs(0.2f) : Inputs(0.0f, 0.3f);
		};
		size_t equal = 0;
		size_t closed = 0;
		int highest = 0;
		for (int step = 0; step < 360; step++)
		{
			VehicleInputs const given = inputs(step);
			if (given.Throttle < 1.0f)
			{
				// What the damping reads: the gear Jolt's gearbox is in as the step begins (the shifts at a throttle below 1
				// move between two gears, never through neutral), and with the throttle closed, for a Manual gearbox, a rear
				// wheel far above the clutch's hold below idle.
				VehicleState const before = Testing::ReadVehicleState(*built, BikeId);
				REQUIRE(before.Gear != 0);
				if (mode == VehicleTransmissionMode::Manual && given.Throttle <= 0.0f)
				{
					REQUIRE(TurnsWellAboveIdle(plain, before));
				}
				closed++;
			}
			for (PhysicsScene* world : {built.get(), damped.get()})
			{
				world->SetVehicleInputs(BikeId, given);
				if (mode == VehicleTransmissionMode::Manual && (step == 0 || step == 90 || step == 330))
				{
					world->RequestVehicleShift(BikeId, step == 330 ? -1 : 1);
				}
				world->Step(Step);
			}
			equal += built->ComputeStateHash() == damped->ComputeStateHash() ? 1 : 0;
			highest = std::max(highest, Testing::ReadVehicleState(*built, BikeId).Gear);
		}
		CHECK(equal == 360);
		CHECK(closed == 359);
		// The ride did ride, at speed: the Manual gearbox up to second, the Automatic one in first (below its shift-up
		// speed at that throttle).
		CHECK(highest == (mode == VehicleTransmissionMode::Manual ? 2 : 1));
		CHECK(Testing::ReadVehicleState(*built, BikeId).ForwardSpeed > 10.0f);
	}
}

TEST_CASE("PhysicsScene: in neutral at a part throttle the engine is damped by its own damping plus the neutral damping times how "
          "far the throttle is closed, in a Manual gearbox and in an Automatic one's step out of neutral")
{
	Testing::PhysicsSystemScope physics;
	// 0.25 + 2 x (1 - 0.5) and 1.25 + 0 x (1 - 0.5) are both 1.25, exactly in binary.
	Scope<PhysicsScene> split = Testing::MakeVehicleRide(GroundId, BikeId, MakeEngineBike(VehicleTransmissionMode::Manual, 2.0f, 0.25f));
	Scope<PhysicsScene> whole = Testing::MakeVehicleRide(GroundId, BikeId, MakeEngineBike(VehicleTransmissionMode::Manual, 0.0f, 1.25f));
	// The same until the Automatic step below, which it takes at 0.25 alone.
	Scope<PhysicsScene> control = Testing::MakeVehicleRide(GroundId, BikeId, MakeEngineBike(VehicleTransmissionMode::Manual, 0.0f, 1.25f));
	Scope<PhysicsScene> own = Testing::MakeVehicleRide(GroundId, BikeId, MakeEngineBike(VehicleTransmissionMode::Manual, 0.0f, 0.25f));
	std::array<PhysicsScene*, 4> const worlds = {split.get(), whole.get(), control.get(), own.get()};
	// A Manual gearbox in neutral at half throttle, both brakes held: the engine settles where its torque and its damping
	// of 1.25 a second meet, well above idle.
	for (int step = 0; step < 120; step++)
	{
		for (PhysicsScene* world : worlds)
		{
			world->SetVehicleInputs(BikeId, Inputs(0.5f, 1.0f, 1.0f));
			world->Step(Step);
		}
		CHECK(split->ComputeStateHash() == whole->ComputeStateHash());
	}
	float const settled = Testing::ReadVehicleState(*split, BikeId).EngineSpeed;
	CHECK(settled > 2.0f * MinRPM);
	CHECK(settled < Testing::ReadVehicleState(*own, BikeId).EngineSpeed);
	REQUIRE(split->ComputeStateHash() == control->ComputeStateHash());

	// Made Automatic in place, each gearbox keeps neutral (Docs/Architecture.md §8, "Vehicles"), and its first step with a
	// throttle is the only one in neutral: Jolt damps the engine before its update takes first gear.
	auto const makeAutomatic = [](PhysicsScene& target, float engineDamping)
	{
		VehicleDesc const* current = target.FindVehicleSettings(BikeId);
		REQUIRE(current != nullptr);
		VehicleDesc automatic = *current;
		automatic.TransmissionMode = VehicleTransmissionMode::Automatic;
		automatic.EngineAngularDamping = engineDamping;
		Testing::RequireVehicleOk(target.SetVehicleSettings(BikeId, automatic));
	};
	makeAutomatic(*split, 0.25f);
	makeAutomatic(*whole, 1.25f);
	makeAutomatic(*control, 0.25f);
	for (PhysicsScene* world : {split.get(), whole.get(), control.get()})
	{
		REQUIRE(Testing::ReadVehicleState(*world, BikeId).Gear == 0);
		world->SetVehicleInputs(BikeId, Inputs(0.5f, 1.0f, 1.0f));
		world->Step(Step);
		CHECK(Testing::ReadVehicleState(*world, BikeId).Gear == 1);
	}
	CHECK(split->ComputeStateHash() == whole->ComputeStateHash());
	CHECK(split->ComputeStateHash() != control->ComputeStateHash());
	CHECK(Testing::ReadVehicleState(*split, BikeId).EngineSpeed < Testing::ReadVehicleState(*control, BikeId).EngineSpeed);
}

TEST_CASE("PhysicsScene: an automatic gearbox that drops to neutral under the brakes runs its engine down at the neutral damping "
          "from the next step")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = Testing::MakeVehicleRide(
		GroundId, BikeId, MakeEngineBike(VehicleTransmissionMode::Automatic, NeutralDamping, EngineDamping, NewCrank));
	world->SetVehicleInputs(BikeId, Inputs(1.0f));
	world->Step(Step);
	world->SetVehicleInputs(BikeId, Inputs(0.6f));
	for (int step = 0; step < 240; step++)
	{
		world->Step(Step);
	}
	VehicleState const riding = Testing::ReadVehicleState(*world, BikeId);
	REQUIRE(riding.Gear >= 1);
	REQUIRE(riding.ForwardSpeed > 5.0f);
	// The throttle closed on a light front brake: the gearbox shifts down below its shift-down speed, and to neutral from
	// first, since the throttle is closed (VehicleTransmission.cpp:90-96 of the pinned Jolt).
	world->SetVehicleInputs(BikeId, Inputs(0.0f, 0.3f));
	int steps = 0;
	while (steps < 1200 && Testing::ReadVehicleState(*world, BikeId).Gear != 0)
	{
		world->Step(Step);
		steps++;
	}
	REQUIRE(Testing::ReadVehicleState(*world, BikeId).Gear == 0);
	// From there, while above idle, each step takes the engine's speed down by 1 - (0.005 + 2.245) / 60.
	float const damping = EngineDamping + NeutralDamping * (1.0f - 0.0f);
	int measured = 0;
	for (int step = 0; step < 60; step++)
	{
		float const before = Testing::ReadVehicleState(*world, BikeId).EngineSpeed;
		world->Step(Step);
		VehicleState const after = Testing::ReadVehicleState(*world, BikeId);
		REQUIRE(after.Gear == 0);
		if (after.EngineSpeed <= MinRPM)
		{
			break;
		}
		CAPTURE(step);
		CHECK(after.EngineSpeed == doctest::Approx(DampedOnce(before, damping)).epsilon(1e-6));
		measured++;
	}
	CHECK(measured >= 1);
	CHECK(Testing::ReadVehicleState(*world, BikeId).EngineSpeed == MinRPM);
}

TEST_CASE("PhysicsScene: a manual gearbox put into first with the rear wheel still and the throttle closed runs its engine free "
          "once the shift is over, and at its own damping while it shifts")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = Testing::MakeVehicleRide(GroundId, BikeId, MakeEngineBike(VehicleTransmissionMode::Manual, NeutralDamping));
	RevToTheRedLine(*world);
	// The throttle closed and first gear asked for, both brakes held: the rear wheel stands still, so the gearbox holds its
	// clutch open below idle (Docs/Architecture.md §8, "Vehicles").
	world->SetVehicleInputs(BikeId, Inputs(0.0f, 1.0f, 1.0f));
	world->RequestVehicleShift(BikeId, 1);
	float const engineOnly = EngineDamping;
	float const runningFree = EngineDamping + NeutralDamping;
	bool shiftAtStart = true;
	int whileShifting = 0;
	int afterTheShift = 0;
	for (int step = 0; step < 120; step++)
	{
		float const before = Testing::ReadVehicleState(*world, BikeId).EngineSpeed;
		world->Step(Step);
		VehicleState const after = Testing::ReadVehicleState(*world, BikeId);
		REQUIRE(after.Gear == 1);
		CHECK(after.ClutchFriction == 0.0f);
		CHECK(std::abs(after.Wheels[1].AngularVelocity) < 1e-3f);
		if (after.EngineSpeed <= MinRPM)
		{
			break;
		}
		CAPTURE(step);
		// The step that makes the shift and those whose release time runs as they begin are the shift's: the engine's own
		// damping alone, as for every clutch open for a shift.
		float const damping = shiftAtStart ? engineOnly : runningFree;
		CHECK(after.EngineSpeed == doctest::Approx(DampedOnce(before, damping)).epsilon(1e-6));
		(shiftAtStart ? whileShifting : afterTheShift)++;
		shiftAtStart = after.IsShifting;
	}
	// The shift: the step that makes it and the six whose 0.1 s of release runs at their start (a seventh when the six steps'
	// rounding leaves a part of a microsecond).
	CHECK(whileShifting >= 7);
	CHECK(whileShifting <= 8);
	CHECK(afterTheShift >= 40);
	CHECK(Testing::ReadVehicleState(*world, BikeId).EngineSpeed == MinRPM);
}

TEST_CASE("PhysicsScene: the neutral damping is in neither the hash nor the snapshot: a snapshot taken while the engine falls "
          "restores, the steps after run as they did, and with another value the engine falls at that one")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = Testing::MakeVehicleRide(GroundId, BikeId, MakeEngineBike(VehicleTransmissionMode::Manual, NeutralDamping));
	RevToTheRedLine(*world);
	world->SetVehicleInputs(BikeId, Inputs(0.0f, 1.0f, 1.0f));
	for (int step = 0; step < 20; step++)
	{
		world->Step(Step);
	}
	PhysicsSnapshot const snapshot = world->SaveState();
	uint64_t const hash = world->ComputeStateHash();
	float const saved = Testing::ReadVehicleState(*world, BikeId).EngineSpeed;
	REQUIRE(saved > 2.0f * MinRPM);
	std::vector<uint64_t> original;
	for (int step = 0; step < 60; step++)
	{
		world->Step(Step);
		original.push_back(world->ComputeStateHash());
	}

	Testing::RequireVehicleOk(world->RestoreState(snapshot));
	CHECK(world->ComputeStateHash() == hash);
	std::vector<uint64_t> replayed;
	for (int step = 0; step < 60; step++)
	{
		world->Step(Step);
		replayed.push_back(world->ComputeStateHash());
	}
	CHECK(replayed == original);

	// Another value in place: the vehicle keeps it across a restore of a snapshot taken with the first, which holds no
	// damping, and the engine falls at the new one from the next step.
	VehicleDesc const* current = world->FindVehicleSettings(BikeId);
	REQUIRE(current != nullptr);
	VehicleDesc slower = *current;
	slower.EngineNeutralDamping = 1.0f;
	uint64_t const structure = world->GetStructureVersion();
	Testing::RequireVehicleOk(world->SetVehicleSettings(BikeId, slower));
	CHECK(world->GetStructureVersion() == structure);
	Testing::RequireVehicleOk(world->RestoreState(snapshot));
	CHECK(world->ComputeStateHash() == hash);
	REQUIRE(world->FindVehicleSettings(BikeId) != nullptr);
	CHECK(world->FindVehicleSettings(BikeId)->EngineNeutralDamping == 1.0f);
	world->Step(Step);
	CHECK(Testing::ReadVehicleState(*world, BikeId).EngineSpeed == doctest::Approx(DampedOnce(saved, EngineDamping + 1.0f)).epsilon(1e-6));
}
