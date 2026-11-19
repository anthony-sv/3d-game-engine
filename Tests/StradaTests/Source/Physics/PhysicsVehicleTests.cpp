#include "Core/ProfilerTestUtilities.h"
#include "Physics/PhysicsTestUtilities.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Asset/PhysicsMaterial.h"
#include "Strada/Asset/Vehicle.h"
#include "Strada/Core/Platform.h"
#include "Strada/Core/Profiler.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	constexpr float Step = 1.0f / 60.0f;
	constexpr float Gravity = 9.81f;

	UUID const GroundId(301);
	UUID const BikeId(310);
	UUID const OtherBikeId(320);
	UUID const KinematicId(330);
	UUID const TriggerId(340);
	UUID const PendulumId(350);

	Scope<PhysicsScene> CreateWorld(PhysicsWorldSettings const& settings = {})
	{
		Result<Scope<PhysicsScene>> world = PhysicsScene::Create(settings);
		REQUIRE(world.IsOk());
		return world.TakeValue();
	}

	// A flat static ground whose top is at y = 0.
	BodyDesc MakeGround(UUID entity, ColliderSurface const& surface = {})
	{
		BodyDesc desc;
		desc.Entity = entity;
		desc.Type = RigidBodyType::Static;
		desc.Position = glm::vec3(0.0f, -0.5f, 0.0f);
		ColliderDesc collider;
		collider.HalfExtents = glm::vec3(400.0f, 0.5f, 400.0f);
		collider.Surface = surface;
		desc.Colliders.push_back(collider);
		return desc;
	}

	// A 250 kg chassis box from 0.25 m to 1.25 m above its origin, 0.5 m wide and 1.2 m long. Its roll inertia about its
	// front, 250 x (0.5² + 1²) / 12 = 26 kg m², is above the default lean damping times the step, 1000 x 1/60 = 16.7, which
	// Jolt's lean spring needs (Docs/Architecture.md §8, "Vehicles").
	BodyDesc MakeChassis(UUID entity, glm::vec3 const& position, glm::vec3 const& velocity = glm::vec3(0.0f))
	{
		BodyDesc desc;
		desc.Entity = entity;
		desc.Type = RigidBodyType::Dynamic;
		desc.Position = position;
		desc.Mass = 250.0f;
		desc.LinearDamping = 0.0f;
		desc.LinearVelocity = velocity;
		desc.AllowSleep = false;
		ColliderDesc collider;
		collider.HalfExtents = glm::vec3(0.25f, 0.5f, 0.6f);
		collider.Offset = glm::vec3(0.0f, 0.75f, 0.0f);
		desc.Colliders.push_back(collider);
		return desc;
	}

	// A new definition's motorcycle (its front at -Z), its wheels without angular damping so that their spins change only by
	// the brakes, the tyres and the engine, and its springs stiff enough to stand mid-travel under the chassis.
	VehicleDesc MakeBike(UUID entity, VehicleTransmissionMode mode)
	{
		Vehicle definition;
		definition.Transmission.Mode = mode;
		VehicleDesc desc = MakeVehicleDesc(entity, definition);
		for (VehicleWheelDesc& wheel : desc.Wheels)
		{
			wheel.AngularDamping = 0.0f;
			wheel.SuspensionFrequency = 2.5f;
		}
		return desc;
	}

	void RequireOk(Result<void> const& result)
	{
		REQUIRE_MESSAGE(result.IsOk(), (result ? std::string() : result.GetError()));
	}

	std::string ErrorOf(Result<void> const& result)
	{
		return result ? std::string() : result.GetError();
	}

	// A world with the ground and one bike on it at the origin.
	Scope<PhysicsScene> MakeRide(VehicleTransmissionMode mode, ColliderSurface const& ground = {},
	                             glm::vec3 const& velocity = glm::vec3(0.0f))
	{
		Scope<PhysicsScene> world = CreateWorld();
		RequireOk(world->AddBody(MakeGround(GroundId, ground)));
		RequireOk(world->AddBody(MakeChassis(BikeId, {0.0f, 0.1f, 0.0f}, velocity)));
		RequireOk(world->AddVehicle(MakeBike(BikeId, mode)));
		return world;
	}

	void Simulate(PhysicsScene& world, int steps)
	{
		for (int step = 0; step < steps; step++)
		{
			world.Step(Step);
		}
	}

	VehicleState StateOf(PhysicsScene const& world, UUID entity)
	{
		std::optional<VehicleState> const state = world.GetVehicleState(entity);
		REQUIRE(state);
		return *state;
	}

	glm::vec3 PositionOf(PhysicsScene const& world, UUID entity)
	{
		std::optional<BodyTransform> const transform = world.GetBodyTransform(entity);
		REQUIRE(transform);
		return transform->Position;
	}

	VehicleInputs MakeInputs(float throttle, float frontBrake, float rearBrake, float lean)
	{
		VehicleInputs inputs;
		inputs.Throttle = throttle;
		inputs.FrontBrake = frontBrake;
		inputs.RearBrake = rearBrake;
		inputs.Lean = lean;
		return inputs;
	}

	bool IsFinite(VehicleState const& state)
	{
		bool finite = std::isfinite(state.EngineSpeed) && std::isfinite(state.EngineTorque) && std::isfinite(state.ForwardSpeed) &&
		              std::isfinite(state.LeanAngle) && std::isfinite(state.ClutchFriction);
		for (VehicleWheelState const& wheel : state.Wheels)
		{
			finite = finite && std::isfinite(wheel.AngularVelocity) && std::isfinite(wheel.SuspensionLength) &&
			         std::isfinite(wheel.LongitudinalSlip) && std::isfinite(wheel.LateralSlip);
		}
		return finite;
	}

	// The inputs of the riding cases at a step, a function of the step count alone (rule 40's rides).
	VehicleInputs RideInputs(int step, float lean)
	{
		VehicleInputs inputs;
		inputs.Throttle = step < 150 ? 0.6f : 0.2f;
		inputs.Lean = step >= 90 && step < 210 ? lean : 0.0f;
		inputs.FrontBrake = step >= 240 ? 0.3f : 0.0f;
		inputs.RearBrake = step >= 270 ? 0.4f : 0.0f;
		return inputs;
	}

	// A static triangle mesh at y = 0 over x in [-10, 10], in two submeshes: z from -20 to 0 (under the front wheel of a bike
	// at the origin) and from 0 to 20 (under its rear).
	Ref<MeshSource> MakeTwoSurfaceStrip()
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<Submesh> submeshes;
		for (float const start : {-20.0f, 0.0f})
		{
			Submesh submesh;
			submesh.BaseVertex = static_cast<uint32_t>(vertices.size());
			submesh.VertexCount = 4;
			submesh.BaseIndex = static_cast<uint32_t>(indices.size());
			submesh.IndexCount = 6;
			for (glm::vec3 const& corner : {glm::vec3(-10.0f, 0.0f, start), glm::vec3(-10.0f, 0.0f, start + 20.0f),
			                                glm::vec3(10.0f, 0.0f, start + 20.0f), glm::vec3(10.0f, 0.0f, start)})
			{
				Vertex vertex;
				vertex.Position = corner;
				vertices.push_back(vertex);
			}
			// Counter-clockwise seen from above.
			for (uint32_t const index : {0u, 1u, 2u, 0u, 2u, 3u})
			{
				indices.push_back(index);
			}
			submeshes.push_back(submesh);
		}
		Result<Ref<MeshSource>> mesh =
			MeshSource::Create(std::move(vertices), std::move(indices), std::move(submeshes), {AssetHandle(UUID(1))});
		REQUIRE_MESSAGE(mesh.IsOk(), (mesh ? std::string() : mesh.GetError()));
		return mesh.GetValue();
	}

	ColliderSurface MakeSurface(AssetHandle material, float rollingResistance, float tyreGrip)
	{
		ColliderSurface surface;
		surface.Material = material;
		surface.RollingResistance = rollingResistance;
		surface.TyreGrip = tyreGrip;
		return surface;
	}

	void CheckSurface(VehicleWheelState const& wheel, UUID entity, ColliderSurface const& surface)
	{
		CHECK(wheel.ContactEntity == entity);
		CHECK(wheel.Surface == surface.Material);
		CHECK(wheel.RollingResistance == surface.RollingResistance);
		CHECK(wheel.TyreGrip == surface.TyreGrip);
	}
}

TEST_CASE("PhysicsScene: a vehicle needs a dynamic body, refuses values Jolt cannot take, and moves the structure version when it "
          "is made, made again or removed")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = CreateWorld();
	RequireOk(world->AddBody(MakeGround(GroundId)));
	RequireOk(world->AddBody(MakeChassis(BikeId, {0.0f, 0.1f, 0.0f})));
	BodyDesc kinematic = MakeChassis(KinematicId, {5.0f, 0.1f, 0.0f});
	kinematic.Type = RigidBodyType::Kinematic;
	RequireOk(world->AddBody(kinematic));

	// Only a dynamic body carries a vehicle. A refusal that leaves the world's vehicles as they were keeps its structure
	// version, so the snapshots taken before it.
	uint64_t const unrefused = world->GetStructureVersion();
	CHECK(ErrorOf(world->AddVehicle(MakeBike(GroundId, VehicleTransmissionMode::Automatic))).find("no dynamic body") != std::string::npos);
	CHECK(ErrorOf(world->AddVehicle(MakeBike(KinematicId, VehicleTransmissionMode::Automatic))).find("no dynamic body") !=
	      std::string::npos);
	CHECK(ErrorOf(world->AddVehicle(MakeBike(UUID(999), VehicleTransmissionMode::Automatic))).find("no dynamic body") != std::string::npos);
	CHECK(world->GetVehicleCount() == 0);

	// Values outside the definition's ranges, and what Jolt asserts on, are refused naming the field.
	struct Refusal
	{
		std::string_view Field;
		void (*Spoil)(VehicleDesc&);
	};
	std::array<Refusal, 9> const refusals = {{
		{"Wheels[1].Radius",
	     [](VehicleDesc& spoiled)
	     {
			 spoiled.Wheels[1].Radius = 0.0f;
		 }},
		{"Wheels[0].SuspensionMaxLength",
	     [](VehicleDesc& spoiled)
	     {
			 spoiled.Wheels[0].SuspensionMaxLength = 0.1f;
		 }},
		{"Wheels[0].SteeringAxis",
	     [](VehicleDesc& spoiled)
	     {
			 spoiled.Wheels[0].SteeringAxis = glm::vec3(0.0f);
		 }},
		{"Wheels[0].LateralFriction",
	     [](VehicleDesc& spoiled)
	     {
			 spoiled.Wheels[0].LateralFriction = {{0.0f, 0.0f}, {std::numeric_limits<float>::quiet_NaN(), 1.0f}};
		 }},
		{"Up, Forward",
	     [](VehicleDesc& spoiled)
	     {
			 spoiled.Forward = glm::vec3(0.0f, 2.0f, 0.0f);
		 }},
		{"GearRatios",
	     [](VehicleDesc& spoiled)
	     {
			 spoiled.GearRatios.clear();
		 }},
		{"MinRPM",
	     [](VehicleDesc& spoiled)
	     {
			 spoiled.MinRPM = spoiled.MaxRPM;
		 }},
		{"ShiftDownRPM",
	     [](VehicleDesc& spoiled)
	     {
			 spoiled.ShiftDownRPM = spoiled.ShiftUpRPM;
		 }},
		{"MaxTorque",
	     [](VehicleDesc& spoiled)
	     {
			 spoiled.MaxTorque = std::numeric_limits<float>::infinity();
		 }},
	}};
	for (Refusal const& refusal : refusals)
	{
		VehicleDesc desc = MakeBike(BikeId, VehicleTransmissionMode::Automatic);
		refusal.Spoil(desc);
		std::string const error = ErrorOf(world->AddVehicle(desc));
		CHECK_MESSAGE(error.find(refusal.Field) != std::string::npos, error);
		CHECK_FALSE(world->HasVehicle(BikeId));
	}
	CHECK(world->GetStructureVersion() == unrefused);

	// Made, changed in place, made again and removed.
	uint64_t const before = world->GetStructureVersion();
	RequireOk(world->AddVehicle(MakeBike(BikeId, VehicleTransmissionMode::Automatic)));
	CHECK(world->HasVehicle(BikeId));
	CHECK(world->GetVehicleCount() == 1);
	uint64_t const made = world->GetStructureVersion();
	CHECK(made != before);
	VehicleDesc edited = MakeBike(BikeId, VehicleTransmissionMode::Automatic);
	edited.MaxTorque = 180.0f;
	edited.Wheels[0].FrontBrakeTorque = 650.0f;
	RequireOk(world->SetVehicleSettings(BikeId, edited));
	CHECK(world->GetStructureVersion() == made);
	VehicleDesc turned = edited;
	turned.Forward = glm::vec3(0.0f, 0.0f, 1.0f);
	CHECK(ErrorOf(world->SetVehicleSettings(BikeId, turned)).find("made again") != std::string::npos);
	CHECK(ErrorOf(world->SetVehicleSettings(OtherBikeId, edited)).find("not of entity") != std::string::npos);
	CHECK(world->GetStructureVersion() == made);
	RequireOk(world->AddVehicle(edited));
	CHECK(world->GetStructureVersion() != made);
	uint64_t const remade = world->GetStructureVersion();
	world->RemoveVehicle(BikeId);
	CHECK_FALSE(world->HasVehicle(BikeId));
	CHECK(world->GetStructureVersion() != remade);
	uint64_t const removed = world->GetStructureVersion();
	world->RemoveVehicle(BikeId);
	CHECK(world->GetStructureVersion() == removed);
	CHECK_FALSE(world->GetVehicleState(BikeId));
	CHECK_FALSE(world->GetVehicleWheelTransform(BikeId, 0));

	// A refused value leaves the entity without the vehicle it had, which moves the structure version; refused again, with
	// no vehicle left to remove, it does not.
	RequireOk(world->AddVehicle(edited));
	uint64_t const riding = world->GetStructureVersion();
	VehicleDesc broken = edited;
	broken.Wheels[0].Inertia = 0.0f;
	CHECK(ErrorOf(world->AddVehicle(broken)).find("Wheels[0].Inertia") != std::string::npos);
	CHECK_FALSE(world->HasVehicle(BikeId));
	uint64_t const refused = world->GetStructureVersion();
	CHECK(refused != riding);
	CHECK(ErrorOf(world->AddVehicle(broken)).find("Wheels[0].Inertia") != std::string::npos);
	CHECK(world->GetStructureVersion() == refused);

	// Removing the body removes its vehicle.
	RequireOk(world->AddVehicle(edited));
	world->RemoveBody(BikeId);
	CHECK_FALSE(world->HasVehicle(BikeId));
	CHECK(world->GetVehicleCount() == 0);
}

TEST_CASE("PhysicsScene: a chassis's new body carries its vehicle on, keeping its inputs, gear, engine speed and wheels")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = MakeRide(VehicleTransmissionMode::Automatic);
	world->SetVehicleInputs(BikeId, MakeInputs(0.8f, 0.0f, 0.0f, 0.0f));
	Simulate(*world, 120);
	VehicleState const ridden = StateOf(*world, BikeId);
	REQUIRE(ridden.Gear >= 1);
	REQUIRE(ridden.Wheels[1].AngularVelocity > 1.0f);

	// A collider changed: the body is made again where it is, and the vehicle on it.
	std::optional<BodyTransform> const transform = world->GetBodyTransform(BikeId);
	REQUIRE(transform);
	BodyDesc replaced = MakeChassis(BikeId, transform->Position);
	replaced.Rotation = transform->Rotation;
	replaced.Colliders.front().HalfExtents.x = 0.3f;
	uint64_t const structure = world->GetStructureVersion();
	RequireOk(world->AddBody(replaced));
	CHECK(world->GetStructureVersion() != structure);
	REQUIRE(world->HasVehicle(BikeId));
	VehicleState const carried = StateOf(*world, BikeId);
	CHECK(carried.Inputs == ridden.Inputs);
	CHECK(carried.Gear == ridden.Gear);
	CHECK(carried.EngineSpeed == ridden.EngineSpeed);
	for (size_t index = 0; index < VehicleWheelCount; index++)
	{
		CHECK(carried.Wheels[index].AngularVelocity == ridden.Wheels[index].AngularVelocity);
		CHECK(carried.Wheels[index].RotationAngle == ridden.Wheels[index].RotationAngle);
	}
	Simulate(*world, 30);
	CHECK(IsFinite(StateOf(*world, BikeId)));

	// A body that no longer moves by itself carries none.
	replaced.Type = RigidBodyType::Static;
	RequireOk(world->AddBody(replaced));
	CHECK_FALSE(world->HasVehicle(BikeId));
	CHECK(world->GetVehicleCount() == 0);
	Simulate(*world, 2);
}

TEST_CASE("PhysicsScene: each brake slows its own wheels by its torques, and a wheel given both adds them")
{
	Testing::PhysicsSystemScope physics;
	// Rolled at 20 m/s in neutral, then lifted 5 m and held without gravity: the wheels spin freely, with no tyre, no engine
	// and no angular damping to change them, so each step's change is the brakes' alone.
	Scope<PhysicsScene> world = MakeRide(VehicleTransmissionMode::Manual, {}, {0.0f, 0.0f, -20.0f});
	VehicleDesc desc = MakeBike(BikeId, VehicleTransmissionMode::Manual);
	desc.Wheels[0].FrontBrakeTorque = 300.0f;
	desc.Wheels[0].RearBrakeTorque = 0.0f;
	desc.Wheels[1].FrontBrakeTorque = 0.0f;
	desc.Wheels[1].RearBrakeTorque = 250.0f;
	RequireOk(world->SetVehicleSettings(BikeId, desc));
	Simulate(*world, 60);
	std::optional<BodyTransform> const transform = world->GetBodyTransform(BikeId);
	REQUIRE(transform);
	world->SetBodyTransform(BikeId, transform->Position + glm::vec3(0.0f, 5.0f, 0.0f), transform->Rotation);
	world->SetLinearVelocity(BikeId, glm::vec3(0.0f));
	world->SetAngularVelocity(BikeId, glm::vec3(0.0f));
	world->SetGravityFactor(BikeId, 0.0f);
	Simulate(*world, 2);

	float const inertia = desc.Wheels[0].Inertia;
	REQUIRE(inertia == desc.Wheels[1].Inertia);
	// The wheels' spins before each of `steps` steps with the inputs, and after the last.
	auto const brake = [&world](VehicleInputs const& inputs, int steps)
	{
		std::vector<std::array<float, VehicleWheelCount>> spins;
		for (int step = 0; step <= steps; step++)
		{
			VehicleState const state = StateOf(*world, BikeId);
			CHECK_FALSE(state.Wheels[0].ContactEntity.IsValid());
			CHECK_FALSE(state.Wheels[1].ContactEntity.IsValid());
			spins.push_back({state.Wheels[0].AngularVelocity, state.Wheels[1].AngularVelocity});
			if (step < steps)
			{
				world->SetVehicleInputs(BikeId, inputs);
				world->Step(Step);
			}
		}
		world->SetVehicleInputs(BikeId, {});
		return spins;
	};

	// Free: nothing changes the spins.
	std::vector<std::array<float, VehicleWheelCount>> const freeSpins = brake({}, 3);
	REQUIRE(freeSpins.front()[0] > 40.0f);
	REQUIRE(freeSpins.front()[1] > 40.0f);
	CHECK(freeSpins.back() == freeSpins.front());

	// The front brake slows the front wheel by its 300 N m and leaves the rear alone, and the rear brake the reverse.
	std::vector<std::array<float, VehicleWheelCount>> const front = brake(MakeInputs(0.0f, 1.0f, 0.0f, 0.0f), 3);
	for (size_t step = 1; step < front.size(); step++)
	{
		CHECK(front[step][0] == doctest::Approx(front[step - 1][0] - 300.0f * Step / inertia).epsilon(1e-5));
		CHECK(front[step][1] == front[step - 1][1]);
	}
	std::vector<std::array<float, VehicleWheelCount>> const rear = brake(MakeInputs(0.0f, 0.0f, 1.0f, 0.0f), 3);
	for (size_t step = 1; step < rear.size(); step++)
	{
		CHECK(rear[step][0] == rear[step - 1][0]);
		CHECK(rear[step][1] == doctest::Approx(rear[step - 1][1] - 250.0f * Step / inertia).epsilon(1e-5));
	}
	// Half an input, half the torque.
	std::vector<std::array<float, VehicleWheelCount>> const half = brake(MakeInputs(0.0f, 0.5f, 0.0f, 0.0f), 2);
	CHECK(half[1][0] == doctest::Approx(half[0][0] - 150.0f * Step / inertia).epsilon(1e-5));

	// A front wheel that both brakes reach: their torques add.
	desc.Wheels[0].RearBrakeTorque = 200.0f;
	RequireOk(world->SetVehicleSettings(BikeId, desc));
	std::vector<std::array<float, VehicleWheelCount>> const both = brake(MakeInputs(0.0f, 1.0f, 1.0f, 0.0f), 2);
	for (size_t step = 1; step < both.size(); step++)
	{
		CHECK(both[step][0] == doctest::Approx(both[step - 1][0] - 500.0f * Step / inertia).epsilon(1e-5));
		CHECK(both[step][1] == doctest::Approx(both[step - 1][1] - 250.0f * Step / inertia).epsilon(1e-5));
	}
}

TEST_CASE("PhysicsScene: a coasting bike slows by about g times the rolling resistance of the surface under its wheels")
{
	Testing::PhysicsSystemScope physics;
	// The same coast from 20 m/s in neutral on two grounds; the difference of the speeds they lose in 2 s is the rolling
	// drag's alone: g x 0.1 x 2 s = 1.96 m/s, within a fifth (Jolt's tyre passes the wheels' loss to the chassis through
	// its slip, and the load moves between the wheels as the bike pitches).
	constexpr float Resistance = 0.1f;
	std::array<float, 2> lost{};
	for (size_t ground = 0; ground < 2; ground++)
	{
		ColliderSurface const surface = MakeSurface(AssetHandle(UUID(7001)), ground == 0 ? 0.0f : Resistance, 1.0f);
		Scope<PhysicsScene> world = MakeRide(VehicleTransmissionMode::Manual, surface, {0.0f, 0.0f, -20.0f});
		Simulate(*world, 60);
		VehicleState const start = StateOf(*world, BikeId);
		CheckSurface(start.Wheels[0], GroundId, surface);
		CheckSurface(start.Wheels[1], GroundId, surface);
		Simulate(*world, 120);
		VehicleState const end = StateOf(*world, BikeId);
		CHECK(std::abs(glm::degrees(end.LeanAngle)) < 5.0f);
		lost[ground] = start.ForwardSpeed - end.ForwardSpeed;
	}
	float const drag = lost[1] - lost[0];
	float const expected = Gravity * Resistance * 2.0f;
	MESSAGE("speed lost in 2 s: " << lost[0] << " m/s without rolling resistance, " << lost[1] << " m/s with " << Resistance);
	CHECK(drag > 0.8f * expected);
	CHECK(drag < 1.2f * expected);
}

TEST_CASE("PhysicsScene: a positive lean input leans and turns the bike to its right, and a lower tyre grip runs it wider")
{
	Testing::PhysicsSystemScope physics;
	// Coasting at 15 m/s with the lean input at 0.5 for 2 s on two grounds that differ in tyre grip alone.
	std::array<float, 2> sideways{};
	for (size_t ground = 0; ground < 2; ground++)
	{
		float const grip = ground == 0 ? 1.0f : 0.25f;
		ColliderSurface const surface = MakeSurface(AssetHandle(UUID(7002)), 0.0f, grip);
		Scope<PhysicsScene> world = MakeRide(VehicleTransmissionMode::Manual, surface, {0.0f, 0.0f, -15.0f});
		Simulate(*world, 30);
		float const startX = PositionOf(*world, BikeId).x;
		float largestLean = 0.0f;
		for (int step = 0; step < 120; step++)
		{
			world->SetVehicleInputs(BikeId, MakeInputs(0.0f, 0.0f, 0.0f, 0.5f));
			world->Step(Step);
			VehicleState const state = StateOf(*world, BikeId);
			largestLean = std::max(largestLean, std::abs(glm::degrees(state.LeanAngle)));
			if (step == 60)
			{
				CHECK(state.Wheels[0].TyreGrip == grip);
				// Jolt turns a wheel to the left by a positive angle: to the right by a negative one.
				CHECK(state.Wheels[0].SteerAngle < 0.0f);
				CHECK(state.Wheels[1].SteerAngle == 0.0f);
				CHECK(state.LeanAngle > 0.0f);
				CHECK(state.TargetLeanAngle > 0.0f);
			}
		}
		// The bike's right is +X (its front is -Z, its up +Y).
		sideways[ground] = PositionOf(*world, BikeId).x - startX;
		CHECK(sideways[ground] > 0.0f);
		CHECK(largestLean < 60.0f);
	}
	MESSAGE("moved to the right in 2 s: " << sideways[0] << " m on full grip, " << sideways[1] << " m on a grip of 0.25");
	CHECK(sideways[1] < sideways[0]);
}

TEST_CASE("PhysicsScene: the lean spring stands a chassis whose roll inertia is above its damping times the step, and throws over "
          "one whose roll inertia is below it")
{
	Testing::PhysicsSystemScope physics;
	// Docs/Architecture.md §8, "Vehicles": the lean spring's iterations settle only while LeanSpringDamping x the collision
	// step stays below the chassis's roll inertia about its front. At 60 steps a second the default damping of 1,000 needs
	// 16.7 kg m². The wide chassis (250 kg, 0.5 m wide, 1 m high: 26 kg m²) stands with it; the thin one (200 kg, 0.3 m wide,
	// 0.5 m high: 200 x (0.3² + 0.5²) / 12 = 5.7 kg m²) is thrown over at its first contact, spinning at Jolt's largest
	// angular velocity (0.25 pi x 60 = 47 rad/s, BodyCreationSettings.h:112), and stands with a damping of 200 (3.3).
	struct Ride
	{
		float Tilt = 0.0f;
		float Spin = 0.0f;
	};
	auto const ride = [](bool thin, float damping)
	{
		Scope<PhysicsScene> world = CreateWorld();
		RequireOk(world->AddBody(MakeGround(GroundId)));
		BodyDesc chassis = MakeChassis(BikeId, {0.0f, 0.1f, 0.0f});
		if (thin)
		{
			chassis.Mass = 200.0f;
			chassis.Colliders[0].HalfExtents = glm::vec3(0.15f, 0.25f, 0.6f);
			chassis.Colliders[0].Offset = glm::vec3(0.0f, 0.5f, 0.0f);
		}
		RequireOk(world->AddBody(chassis));
		VehicleDesc bike = MakeBike(BikeId, VehicleTransmissionMode::Automatic);
		bike.LeanSpringDamping = damping;
		RequireOk(world->AddVehicle(bike));
		Ride result;
		for (int step = 0; step < 120; step++)
		{
			world->Step(Step);
			result.Spin = std::max(result.Spin, glm::length(world->GetAngularVelocity(BikeId)));
		}
		std::optional<BodyTransform> const transform = world->GetBodyTransform(BikeId);
		REQUIRE(transform);
		glm::vec3 const up = transform->Rotation * glm::vec3(0.0f, 1.0f, 0.0f);
		result.Tilt = glm::degrees(std::acos(std::clamp(up.y, -1.0f, 1.0f)));
		return result;
	};
	Ride const wide = ride(false, 1000.0f);
	Ride const thrown = ride(true, 1000.0f);
	Ride const damped = ride(true, 200.0f);
	MESSAGE("after 2 s: the wide chassis tilts " << wide.Tilt << " degrees (largest spin " << wide.Spin << " rad/s), the thin one "
	                                             << thrown.Tilt << " (" << thrown.Spin << "), the thin one at a damping of 200 "
	                                             << damped.Tilt << " (" << damped.Spin << ")");
	CHECK(wide.Tilt < 10.0f);
	CHECK(wide.Spin < 5.0f);
	CHECK(thrown.Spin > 20.0f);
	CHECK(damped.Tilt < 10.0f);
	CHECK(damped.Spin < 5.0f);
}

TEST_CASE("PhysicsScene: each wheel reads the surface under it: a triangle mesh's submeshes, a body's colliders, never a trigger, "
          "and a material edited mid-ride")
{
	Testing::PhysicsSystemScope physics;
	ColliderSurface const front = MakeSurface(AssetHandle(UUID(7011)), 0.02f, 0.7f);
	ColliderSurface const rear = MakeSurface(AssetHandle(UUID(7012)), 0.08f, 0.5f);

	// One static triangle mesh with a surface per submesh: the front wheel stands on the first, the rear on the second.
	{
		Scope<PhysicsScene> world = CreateWorld();
		BodyDesc strip;
		strip.Entity = GroundId;
		ColliderDesc mesh;
		mesh.Shape = ColliderShape::TriangleMesh;
		mesh.Mesh = MakeTwoSurfaceStrip();
		mesh.SubmeshSurfaces = {front, rear};
		strip.Colliders.push_back(mesh);
		RequireOk(world->AddBody(strip));
		RequireOk(world->AddBody(MakeChassis(BikeId, {0.0f, 0.1f, 0.0f})));
		RequireOk(world->AddVehicle(MakeBike(BikeId, VehicleTransmissionMode::Manual)));
		Simulate(*world, 30);
		VehicleState const state = StateOf(*world, BikeId);
		CheckSurface(state.Wheels[0], GroundId, front);
		CheckSurface(state.Wheels[1], GroundId, rear);
	}

	// One body of two box colliders: each wheel reads the collider it stands on. A trigger collider in the wheels' way
	// carries neither: they stand on the ground through it, as they do without it.
	std::array<std::array<float, VehicleWheelCount>, 2> suspension{};
	for (size_t withTrigger = 0; withTrigger < 2; withTrigger++)
	{
		Scope<PhysicsScene> world = CreateWorld();
		BodyDesc ground;
		ground.Entity = GroundId;
		for (float const z : {-5.0f, 5.0f})
		{
			ColliderDesc collider;
			collider.HalfExtents = glm::vec3(10.0f, 0.5f, 5.0f);
			collider.Offset = glm::vec3(0.0f, -0.5f, z);
			collider.Surface = z < 0.0f ? front : rear;
			ground.Colliders.push_back(collider);
		}
		RequireOk(world->AddBody(ground));
		if (withTrigger == 1)
		{
			BodyDesc trigger;
			trigger.Entity = TriggerId;
			trigger.Position = glm::vec3(0.0f, 0.25f, 0.0f);
			ColliderDesc volume;
			volume.HalfExtents = glm::vec3(2.0f, 0.25f, 3.0f);
			volume.IsTrigger = true;
			trigger.Colliders.push_back(volume);
			RequireOk(world->AddBody(trigger));
		}
		RequireOk(world->AddBody(MakeChassis(BikeId, {0.0f, 0.1f, 0.0f})));
		RequireOk(world->AddVehicle(MakeBike(BikeId, VehicleTransmissionMode::Manual)));
		Simulate(*world, 60);
		VehicleState const state = StateOf(*world, BikeId);
		CheckSurface(state.Wheels[0], GroundId, front);
		CheckSurface(state.Wheels[1], GroundId, rear);
		suspension[withTrigger] = {state.Wheels[0].SuspensionLength, state.Wheels[1].SuspensionLength};
	}
	CHECK(suspension[1][0] == doctest::Approx(suspension[0][0]).epsilon(1e-3));
	CHECK(suspension[1][1] == doctest::Approx(suspension[0][1]).epsilon(1e-3));

	// A material's edit reaches the wheels on it at the next step.
	AssetHandle const asphalt(UUID(7021));
	Scope<PhysicsScene> world = MakeRide(VehicleTransmissionMode::Manual, MakeSurface(asphalt, 0.015f, 1.0f));
	Simulate(*world, 10);
	PhysicsMaterial edited;
	edited.RollingResistance = 0.05f;
	edited.TyreGrip = 0.3f;
	CHECK(world->SetMaterialValues(asphalt, edited) == 1);
	world->Step(Step);
	VehicleState const state = StateOf(*world, BikeId);
	CheckSurface(state.Wheels[0], GroundId, MakeSurface(asphalt, 0.05f, 0.3f));
}

TEST_CASE("PhysicsScene: a vehicle's inputs are clamped and reach it once per step, the last written before the step")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = MakeRide(VehicleTransmissionMode::Automatic);
	Simulate(*world, 30);

	// Kept clamped: 0 to 1, the lean -1 to 1, a value that is not finite 0.
	world->SetVehicleInputs(BikeId, MakeInputs(2.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), 5.0f));
	VehicleState const latest = StateOf(*world, BikeId);
	CHECK(latest.Inputs == MakeInputs(1.0f, 0.0f, 0.0f, 1.0f));
	CHECK(latest.AppliedInputs == VehicleInputs());
	world->SetVehicleInputs(BikeId, MakeInputs(0.5f, 0.25f, 0.75f, -3.0f));
	CHECK(StateOf(*world, BikeId).Inputs == MakeInputs(0.5f, 0.25f, 0.75f, -1.0f));
	world->SetVehicleInputs(UUID(999), MakeInputs(1.0f, 0.0f, 0.0f, 0.0f));

	// A step of no time applies nothing; the next step applies the last of the writes.
	world->Step(0.0f);
	CHECK(StateOf(*world, BikeId).AppliedInputs == VehicleInputs());
	world->SetVehicleInputs(BikeId, MakeInputs(1.0f, 0.0f, 0.0f, 0.0f));
	world->Step(Step);
	VehicleState const first = StateOf(*world, BikeId);
	CHECK(first.AppliedInputs == MakeInputs(1.0f, 0.0f, 0.0f, 0.0f));
	CHECK(first.Gear == 1);

	// Eight steps without a write apply the same values to each. The engine's torque at full throttle is the default curve's
	// share of MaxTorque (0.8 to 1), times the clutch's friction while the automatic gearbox lets it in.
	for (int step = 0; step < 8; step++)
	{
		world->Step(Step);
		VehicleState const state = StateOf(*world, BikeId);
		CHECK(state.AppliedInputs == MakeInputs(1.0f, 0.0f, 0.0f, 0.0f));
		CHECK(state.EngineTorque >= 0.8f * 150.0f * state.ClutchFriction - 1e-3f);
		CHECK(state.EngineTorque <= 150.0f * state.ClutchFriction + 1e-3f);
		if (step == 7)
		{
			CHECK(state.EngineTorque > 0.0f);
		}
	}
	world->SetVehicleInputs(BikeId, {});
	world->Step(Step);
	CHECK(StateOf(*world, BikeId).EngineTorque == 0.0f);
}

TEST_CASE("PhysicsScene: a manual gearbox shifts at the next step by the requests' sum, within its gears, with its clutch's ramp")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = MakeRide(VehicleTransmissionMode::Manual);
	VehicleDesc desc = MakeBike(BikeId, VehicleTransmissionMode::Manual);
	desc.SwitchTime = 0.1f;
	desc.ClutchReleaseTime = 0.2f;
	RequireOk(world->SetVehicleSettings(BikeId, desc));
	Simulate(*world, 10);
	CHECK(StateOf(*world, BikeId).Gear == 0);

	// Down in neutral changes nothing.
	world->RequestVehicleShift(BikeId, -1);
	world->Step(Step);
	CHECK(StateOf(*world, BikeId).Gear == 0);
	CHECK_FALSE(StateOf(*world, BikeId).IsShifting);

	// Two requests before a step add up; nothing moves before the step. From neutral the clutch is released alone.
	world->RequestVehicleShift(BikeId, 1);
	world->RequestVehicleShift(BikeId, 1);
	CHECK(StateOf(*world, BikeId).Gear == 0);
	// The clutch's friction each step, as the design computes it: open in the step that shifts, then the switch time
	// counted down with it open, then the release time with it closing in a straight line.
	float switchLeft = 0.0f;
	float releaseLeft = desc.ClutchReleaseTime;
	auto const expectClutch = [&switchLeft, &releaseLeft, &desc](bool shifted)
	{
		if (shifted)
		{
			return 0.0f;
		}
		if (switchLeft > 0.0f)
		{
			switchLeft = std::max(0.0f, switchLeft - Step);
			return 0.0f;
		}
		if (releaseLeft > 0.0f)
		{
			releaseLeft = std::max(0.0f, releaseLeft - Step);
			return std::clamp(1.0f - releaseLeft / desc.ClutchReleaseTime, 0.0f, 1.0f);
		}
		return 1.0f;
	};
	for (int step = 0; step < 20; step++)
	{
		world->Step(Step);
		VehicleState const state = StateOf(*world, BikeId);
		CHECK(state.Gear == 2);
		CHECK(state.ClutchFriction == doctest::Approx(expectClutch(step == 0)).epsilon(1e-6));
	}
	CHECK_FALSE(StateOf(*world, BikeId).IsShifting);

	// Up from a gear: the clutch stays open for the switch time first.
	world->RequestVehicleShift(BikeId, 1);
	switchLeft = desc.SwitchTime;
	releaseLeft = desc.ClutchReleaseTime;
	for (int step = 0; step < 24; step++)
	{
		world->Step(Step);
		VehicleState const state = StateOf(*world, BikeId);
		CHECK(state.Gear == 3);
		CHECK(state.ClutchFriction == doctest::Approx(expectClutch(step == 0)).epsilon(1e-6));
		if (step == 3)
		{
			CHECK(state.IsShifting);
		}
	}

	// Down, then up past the top gear: within the gears.
	world->RequestVehicleShift(BikeId, -1);
	world->Step(Step);
	CHECK(StateOf(*world, BikeId).Gear == 2);
	world->RequestVehicleShift(BikeId, 40);
	world->Step(Step);
	CHECK(StateOf(*world, BikeId).Gear == static_cast<int32_t>(desc.GearRatios.size()));
	world->RequestVehicleShift(BikeId, 1);
	world->Step(Step);
	CHECK(StateOf(*world, BikeId).Gear == static_cast<int32_t>(desc.GearRatios.size()));
}

TEST_CASE("PhysicsScene: a vehicle's gear stays within the gears an edit leaves, after the edit and after a restore, and a change of "
          "mode hands the gear over")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = MakeRide(VehicleTransmissionMode::Manual);
	VehicleDesc const desc = MakeBike(BikeId, VehicleTransmissionMode::Manual);
	REQUIRE(desc.GearRatios.size() == 6);
	world->RequestVehicleShift(BikeId, 6);
	world->SetVehicleInputs(BikeId, MakeInputs(1.0f, 0.0f, 0.0f, 0.0f));
	Simulate(*world, 60);
	REQUIRE(StateOf(*world, BikeId).Gear == 6);
	PhysicsSnapshot const sixth = world->SaveState();

	// Four gears from now on: the bike is in the fourth at once, and rides on in it.
	VehicleDesc fewer = desc;
	fewer.GearRatios.resize(4);
	RequireOk(world->SetVehicleSettings(BikeId, fewer));
	CHECK(StateOf(*world, BikeId).Gear == 4);
	Simulate(*world, 30);
	CHECK(StateOf(*world, BikeId).Gear == 4);
	CHECK(IsFinite(StateOf(*world, BikeId)));

	// The snapshot taken in sixth gear restores (an in-place edit keeps the structure) to a gear the bike has.
	RequireOk(world->RestoreState(sixth));
	CHECK(StateOf(*world, BikeId).Gear <= 4);
	Simulate(*world, 30);
	VehicleState const restored = StateOf(*world, BikeId);
	CHECK(restored.Gear <= 4);
	CHECK(IsFinite(restored));

	// A gearbox made automatic goes on from its gear (shift requests do nothing to it); one made manual again keeps the gear
	// it is in and shifts no more by itself.
	VehicleDesc automatic = fewer;
	automatic.TransmissionMode = VehicleTransmissionMode::Automatic;
	RequireOk(world->SetVehicleSettings(BikeId, automatic));
	world->RequestVehicleShift(BikeId, -2);
	world->Step(Step);
	int32_t const gear = StateOf(*world, BikeId).Gear;
	CHECK(gear >= 3);
	PhysicsSnapshot const automaticSnapshot = world->SaveState();
	RequireOk(world->SetVehicleSettings(BikeId, fewer));
	CHECK(StateOf(*world, BikeId).Gear == gear);
	world->SetVehicleInputs(BikeId, {});
	Simulate(*world, 60);
	CHECK(StateOf(*world, BikeId).Gear == gear);

	// A snapshot of the automatic gearbox restored into the manual one hands the gear over as an edit does: the manual
	// gearbox takes the gear Jolt's state was restored to and keeps it, rather than the neutral its own state held then.
	world->RequestVehicleShift(BikeId, -1);
	world->Step(Step);
	REQUIRE(StateOf(*world, BikeId).Gear == gear - 1);
	RequireOk(world->RestoreState(automaticSnapshot));
	CHECK(StateOf(*world, BikeId).Gear == gear);
	Simulate(*world, 10);
	CHECK(StateOf(*world, BikeId).Gear == gear);
}

TEST_CASE("PhysicsScene: a world without vehicles hashes as before vehicles existed, and one with a vehicle hashes its state")
{
	Testing::PhysicsSystemScope physics;
	// A vehicle made and removed before any step leaves a world that hashes as one that never had it, step after step.
	Scope<PhysicsScene> world = MakeRide(VehicleTransmissionMode::Automatic);
	Scope<PhysicsScene> twin = CreateWorld();
	RequireOk(twin->AddBody(MakeGround(GroundId)));
	RequireOk(twin->AddBody(MakeChassis(BikeId, {0.0f, 0.1f, 0.0f})));
	CHECK(world->ComputeStateHash() != twin->ComputeStateHash());
	world->RemoveVehicle(BikeId);
	REQUIRE(world->GetVehicleCount() == 0);
	bool same = true;
	for (int step = 0; step < 60; step++)
	{
		same = same && world->ComputeStateHash() == twin->ComputeStateHash();
		world->Step(Step);
		twin->Step(Step);
	}
	CHECK(same);

	// With a vehicle: the inputs a step applied are in the hash, the latest ones not until a step applies them.
	Scope<PhysicsScene> first = MakeRide(VehicleTransmissionMode::Manual);
	Scope<PhysicsScene> second = MakeRide(VehicleTransmissionMode::Manual);
	Simulate(*first, 30);
	Simulate(*second, 30);
	REQUIRE(first->ComputeStateHash() == second->ComputeStateHash());
	second->SetVehicleInputs(BikeId, MakeInputs(0.0f, 0.0f, 0.0f, 0.5f));
	CHECK(first->ComputeStateHash() == second->ComputeStateHash());
	first->Step(Step);
	second->Step(Step);
	CHECK(first->ComputeStateHash() != second->ComputeStateHash());

	// So is a manual gearbox's gear, and a shift only once a step makes it.
	Scope<PhysicsScene> third = MakeRide(VehicleTransmissionMode::Manual);
	Scope<PhysicsScene> fourth = MakeRide(VehicleTransmissionMode::Manual);
	Simulate(*third, 30);
	Simulate(*fourth, 30);
	fourth->RequestVehicleShift(BikeId, 1);
	CHECK(third->ComputeStateHash() == fourth->ComputeStateHash());
	third->Step(Step);
	fourth->Step(Step);
	REQUIRE(StateOf(*fourth, BikeId).Gear == 1);
	CHECK(third->ComputeStateHash() != fourth->ComputeStateHash());
}

TEST_CASE("PhysicsScene: a saved state brings a riding vehicle back as it was read, and the steps from there run as they did")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = MakeRide(VehicleTransmissionMode::Manual);
	// Shifts up at steps 40, 100 and 230, the ride's inputs by its step count, leaning left from step 220 and right from 290.
	auto const drive = [&world](int step)
	{
		VehicleInputs inputs = RideInputs(step, 0.4f);
		inputs.Lean = step < 220 ? inputs.Lean : (step < 290 ? -0.3f : 0.2f);
		world->SetVehicleInputs(BikeId, inputs);
		if (step == 40 || step == 100 || step == 230)
		{
			world->RequestVehicleShift(BikeId, 1);
		}
	};
	for (int step = 0; step < 250; step++)
	{
		drive(step);
		world->Step(Step);
	}
	// Saved with the next step's inputs set and a shift down pending: leaning, braking, the clutch open after the shift at
	// 230.
	drive(250);
	world->RequestVehicleShift(BikeId, -1);
	PhysicsSnapshot const snapshot = world->SaveState();
	VehicleState const reading = StateOf(*world, BikeId);
	uint64_t const hash = world->ComputeStateHash();
	REQUIRE(reading.IsShifting);
	REQUIRE(reading.AppliedInputs.FrontBrake > 0.0f);
	REQUIRE(reading.AppliedInputs.Lean < 0.0f);
	std::vector<uint64_t> original;
	for (int step = 251; step <= 330; step++)
	{
		world->Step(Step);
		original.push_back(world->ComputeStateHash());
		drive(step);
	}

	RequireOk(world->RestoreState(snapshot));
	VehicleState const restored = StateOf(*world, BikeId);
	CHECK(world->ComputeStateHash() == hash);
	CHECK(restored.Inputs == reading.Inputs);
	CHECK(restored.AppliedInputs == reading.AppliedInputs);
	CHECK(restored.EngineSpeed == reading.EngineSpeed);
	CHECK(restored.Gear == reading.Gear);
	CHECK(restored.ClutchFriction == reading.ClutchFriction);
	CHECK(restored.TargetLeanAngle == reading.TargetLeanAngle);
	for (size_t index = 0; index < VehicleWheelCount; index++)
	{
		CAPTURE(index);
		CHECK(restored.Wheels[index].SteerAngle == reading.Wheels[index].SteerAngle);
		CHECK(restored.Wheels[index].LongitudinalSlip == reading.Wheels[index].LongitudinalSlip);
		CHECK(restored.Wheels[index].LateralSlip == reading.Wheels[index].LateralSlip);
		CHECK(restored.Wheels[index].ContactEntity == reading.Wheels[index].ContactEntity);
		CHECK(restored.Wheels[index].AngularVelocity == reading.Wheels[index].AngularVelocity);
	}
	std::vector<uint64_t> replayed;
	for (int step = 251; step <= 330; step++)
	{
		world->Step(Step);
		replayed.push_back(world->ComputeStateHash());
		drive(step);
	}
	CHECK(replayed == original);
}

TEST_CASE("PhysicsScene: an automatic gearbox shifts up above its shift-up speed and down below its shift-down speed")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = MakeRide(VehicleTransmissionMode::Automatic);
	VehicleDesc desc = MakeBike(BikeId, VehicleTransmissionMode::Automatic);
	desc.ShiftUpRPM = 5000.0f;
	desc.ShiftDownRPM = 2500.0f;
	RequireOk(world->SetVehicleSettings(BikeId, desc));
	Simulate(*world, 30);
	CHECK(StateOf(*world, BikeId).Gear == 0);
	int32_t gear = 0;
	int32_t highest = 0;
	int upshifts = 0;
	int downshifts = 0;
	for (int step = 0; step < 900; step++)
	{
		// Full throttle for 10 s, then closed with both brakes on.
		bool const braking = step >= 600;
		world->SetVehicleInputs(BikeId, MakeInputs(braking ? 0.0f : 1.0f, braking ? 1.0f : 0.0f, braking ? 1.0f : 0.0f, 0.0f));
		world->Step(Step);
		VehicleState const state = StateOf(*world, BikeId);
		if (step == 0)
		{
			// The first throttle takes the first gear from neutral.
			CHECK(state.Gear == 1);
		}
		else if (state.Gear > gear)
		{
			upshifts++;
			CHECK(state.Gear == gear + 1);
			CHECK(state.EngineSpeed > desc.ShiftUpRPM);
		}
		else if (state.Gear < gear)
		{
			downshifts++;
			CHECK(state.Gear == gear - 1);
			CHECK(state.EngineSpeed < desc.ShiftDownRPM);
		}
		gear = state.Gear;
		highest = std::max(highest, gear);
	}
	MESSAGE("highest gear " << highest << ", " << upshifts << " shifts up, " << downshifts << " down");
	CHECK(highest >= 3);
	CHECK(downshifts >= 1);
}

TEST_CASE("PhysicsScene: Jolt's wheel transform is the one ComposeVehicleWheelTransform composes from the wheel's state")
{
	Testing::PhysicsSystemScope physics;
	Scope<PhysicsScene> world = MakeRide(VehicleTransmissionMode::Automatic);
	VehicleDesc desc = MakeBike(BikeId, VehicleTransmissionMode::Automatic);
	REQUIRE(NormalizeVehicleAxes(desc.Up, desc.Forward));
	for (VehicleWheelDesc& wheel : desc.Wheels)
	{
		wheel.SuspensionDirection = glm::normalize(wheel.SuspensionDirection);
		wheel.SteeringAxis = glm::normalize(wheel.SteeringAxis);
	}
	for (int step = 0; step < 150; step++)
	{
		world->SetVehicleInputs(BikeId, MakeInputs(0.5f, 0.0f, 0.0f, step > 60 ? -0.4f : 0.0f));
		world->Step(Step);
		if (step % 15 != 14)
		{
			continue;
		}
		VehicleState const state = StateOf(*world, BikeId);
		for (size_t index = 0; index < VehicleWheelCount; index++)
		{
			VehicleWheelState const& wheel = state.Wheels[index];
			glm::mat4 const composed = ComposeVehicleWheelTransform(desc.Wheels[index], desc.Up, desc.Forward, wheel.SuspensionLength,
			                                                        wheel.SteerAngle, wheel.RotationAngle);
			std::optional<glm::mat4> const jolt = world->GetVehicleWheelTransform(BikeId, index);
			REQUIRE(jolt);
			for (int column = 0; column < 4; column++)
			{
				for (int row = 0; row < 4; row++)
				{
					CHECK(composed[column][row] == doctest::Approx((*jolt)[column][row]).epsilon(1e-5).scale(1.0));
				}
			}
		}
	}
	// A left lean steers to the left.
	CHECK(StateOf(*world, BikeId).Wheels[0].SteerAngle > 0.0f);
	CHECK_FALSE(world->GetVehicleWheelTransform(BikeId, VehicleWheelCount));

	// The axes as a vehicle takes them.
	glm::vec3 up(0.0f, 2.0f, 0.0f);
	glm::vec3 forward(0.0f, 0.5f, -3.0f);
	REQUIRE(NormalizeVehicleAxes(up, forward));
	CHECK(up == glm::vec3(0.0f, 1.0f, 0.0f));
	CHECK(forward == glm::vec3(0.0f, 0.0f, -1.0f));
	glm::vec3 parallel(0.0f, -4.0f, 0.0f);
	CHECK_FALSE(NormalizeVehicleAxes(up, parallel));
	glm::vec3 zero(0.0f);
	CHECK_FALSE(NormalizeVehicleAxes(zero, forward));
	glm::vec3 notFinite(std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f);
	CHECK_FALSE(NormalizeVehicleAxes(up, notFinite));
	CHECK(up == glm::vec3(0.0f, 1.0f, 0.0f));

	// The values of a definition, its degrees in radians.
	Vehicle definition;
	definition.Wheels[0].MaxSteerAngle = 30.0f;
	definition.Lean.MaxLeanAngle = 50.0f;
	definition.MaxPitchRoll = 180.0f;
	definition.Transmission.FinalDrive = 2.5625f;
	VehicleDesc const made = MakeVehicleDesc(BikeId, definition);
	CHECK(made.Entity == BikeId);
	CHECK(made.Wheels[0].MaxSteerAngle == doctest::Approx(glm::radians(30.0f)));
	CHECK(made.Wheels[1].RearBrakeTorque == definition.Wheels[1].RearBrakeTorque);
	CHECK(made.MaxLeanAngle == doctest::Approx(glm::radians(50.0f)));
	CHECK(made.MaxPitchRoll == doctest::Approx(glm::pi<float>()));
	CHECK(made.FinalDrive == 2.5625f);
	CHECK(made.Wheels[0].LongitudinalFriction.empty());
	CHECK(made.NormalizedTorque.empty());
	RequireOk(world->AddVehicle(made));
}

namespace
{
	// What a world of two bikes riding side by side and a pendulum on a joint reads every ten steps: its bodies' hash and
	// each bike's engine speed, wheel spins and lean, bit for bit.
	struct TwoBikeReading
	{
		uint64_t Hash = 0;
		std::array<float, 8> Values{};

		bool operator==(TwoBikeReading const&) const = default;
	};

	std::vector<TwoBikeReading> RideTwoBikes(bool vehiclesFirst)
	{
		Scope<PhysicsScene> world = CreateWorld();
		RequireOk(world->AddBody(MakeGround(GroundId)));
		RequireOk(world->AddBody(MakeChassis(BikeId, {-3.0f, 0.1f, 0.0f})));
		RequireOk(world->AddBody(MakeChassis(OtherBikeId, {3.0f, 0.1f, 0.0f})));
		BodyDesc pendulum;
		pendulum.Entity = PendulumId;
		pendulum.Type = RigidBodyType::Dynamic;
		pendulum.Position = glm::vec3(8.0f, 3.0f, 0.0f);
		pendulum.Colliders.emplace_back();
		pendulum.Colliders.back().HalfExtents = glm::vec3(0.2f);
		RequireOk(world->AddBody(pendulum));
		JointDesc joint;
		joint.Entity = PendulumId;
		joint.Type = JointType::Ball;
		joint.Point = glm::vec3(10.0f, 3.0f, 0.0f);
		// One world makes the joint before the vehicles and the second bike first, the other the reverse of both.
		if (!vehiclesFirst)
		{
			RequireOk(world->AddJoint(joint));
		}
		std::array<UUID, 2> const order =
			vehiclesFirst ? std::array<UUID, 2>{OtherBikeId, BikeId} : std::array<UUID, 2>{BikeId, OtherBikeId};
		for (UUID const bike : order)
		{
			RequireOk(world->AddVehicle(MakeBike(bike, VehicleTransmissionMode::Automatic)));
		}
		if (vehiclesFirst)
		{
			RequireOk(world->AddJoint(joint));
		}
		std::vector<TwoBikeReading> readings;
		for (int step = 0; step < 300; step++)
		{
			world->SetVehicleInputs(BikeId, RideInputs(step, 0.4f));
			world->SetVehicleInputs(OtherBikeId, RideInputs(step, -0.3f));
			world->Step(Step);
			if (step % 10 != 9)
			{
				continue;
			}
			VehicleState const first = StateOf(*world, BikeId);
			VehicleState const second = StateOf(*world, OtherBikeId);
			readings.push_back(
				{world->ComputeStateHash(),
			     {first.EngineSpeed, first.Wheels[0].AngularVelocity, first.Wheels[1].AngularVelocity, first.LeanAngle, second.EngineSpeed,
			      second.Wheels[0].AngularVelocity, second.Wheels[1].AngularVelocity, second.LeanAngle}});
		}
		return readings;
	}
}

TEST_CASE("PhysicsScene: two vehicles ride alike whatever the order they and a joint were made in")
{
	Testing::PhysicsSystemScope physics;
	std::vector<TwoBikeReading> const first = RideTwoBikes(false);
	std::vector<TwoBikeReading> const second = RideTwoBikes(true);
	REQUIRE(first.size() == 30);
	CHECK(first == second);
	// They rode: the first bike's engine turns above its idle speed.
	CHECK(first.back().Values[0] > 1000.0f);
}

TEST_CASE("PhysicsScene: vehicles stepped on Jolt's workers beside bodies and characters give the same world twice")
{
	Testing::PhysicsSystemScope physics;
	// Twelve bikes, forty boxes falling among them and two characters, in two collision steps a step: the step listener
	// runs as a job, on a worker or on this thread, beside the other jobs (ThreadSanitizer runs this case).
	constexpr int Bikes = 12;
	auto const ride = []()
	{
		PhysicsWorldSettings settings;
		settings.CollisionSteps = 2;
		Scope<PhysicsScene> world = CreateWorld(settings);
		RequireOk(world->AddBody(MakeGround(GroundId)));
		for (int bike = 0; bike < Bikes; bike++)
		{
			UUID const id(1000 + static_cast<uint64_t>(bike));
			RequireOk(world->AddBody(MakeChassis(id, {static_cast<float>(bike % 4) * 4.0f, 0.1f, static_cast<float>(bike / 4) * 6.0f})));
		}
		for (int box = 0; box < 40; box++)
		{
			BodyDesc desc;
			desc.Entity = UUID(2000 + static_cast<uint64_t>(box));
			desc.Type = RigidBodyType::Dynamic;
			desc.Position = glm::vec3(static_cast<float>(box % 8) * 2.0f - 2.0f, 2.0f + static_cast<float>(box / 8), -3.0f);
			desc.Colliders.emplace_back();
			desc.Colliders.back().HalfExtents = glm::vec3(0.3f);
			RequireOk(world->AddBody(desc));
		}
		for (int character = 0; character < 2; character++)
		{
			CharacterDesc desc;
			desc.Entity = UUID(3000 + static_cast<uint64_t>(character));
			desc.Position = glm::vec3(-4.0f, 1.0f, static_cast<float>(character) * 5.0f);
			RequireOk(world->AddCharacter(desc));
		}
		for (int bike = 0; bike < Bikes; bike++)
		{
			RequireOk(world->AddVehicle(MakeBike(UUID(1000 + static_cast<uint64_t>(bike)), VehicleTransmissionMode::Automatic)));
		}
		std::vector<uint64_t> hashes;
		for (int step = 0; step < 240; step++)
		{
			for (int bike = 0; bike < Bikes; bike++)
			{
				world->SetVehicleInputs(UUID(1000 + static_cast<uint64_t>(bike)), RideInputs(step, bike % 2 == 0 ? 0.3f : -0.3f));
			}
			world->MoveCharacter(UUID(3000), glm::vec3(1.0f, 0.0f, 0.0f));
			world->Step(Step);
			hashes.push_back(world->ComputeStateHash());
		}
		VehicleState const state = StateOf(*world, UUID(1000 + Bikes - 1));
		CHECK(IsFinite(state));
		hashes.push_back(std::bit_cast<uint32_t>(state.EngineSpeed));
		return hashes;
	};
	std::vector<uint64_t> const first = ride();
	std::vector<uint64_t> const second = ride();
	CHECK(first == second);
}

TEST_CASE("PhysicsScene: vehicle benchmark" * doctest::skip(!Platform::ReadEnvironmentVariable("STRADA_VEHICLE_BENCHMARK")))
{
	// A measurement, never asserted on timings: run with STRADA_VEHICLE_BENCHMARK=1 in a Release build. The same world with
	// eight chassis held off the ground without vehicles (T0) and with eight bikes riding on it (T8): what a vehicle costs a
	// step is (T8 - T0) / 8, beside the Physics.Vehicles zone's median per call over 8.
	constexpr int Bikes = 8;
	constexpr int WarmUpSteps = 120;
	constexpr int Steps = 600;
	Testing::PhysicsSystemScope physics;
	Testing::ProfilerScope const profiler;
	using Clock = std::chrono::steady_clock;
	// Twice: a round that reads slower than the one before it tells a machine that changed its clocks, not a cost.
	for (int round = 1; round <= 2; round++)
	{
		std::array<double, 2> perStep{};
		std::array<double, 2> hashCost{};
		std::array<double, 2> saveCost{};
		double vehiclesZone = 0.0;
		for (size_t withVehicles = 0; withVehicles < 2; withVehicles++)
		{
			Scope<PhysicsScene> world = CreateWorld();
			RequireOk(world->AddBody(MakeGround(GroundId)));
			for (int bike = 0; bike < Bikes; bike++)
			{
				UUID const id(1000 + static_cast<uint64_t>(bike));
				// Without a vehicle the chassis floats a meter up without gravity, so that it touches nothing.
				BodyDesc chassis = MakeChassis(id, {static_cast<float>(bike) * 4.0f, withVehicles == 1 ? 0.1f : 1.0f, 0.0f});
				chassis.GravityFactor = withVehicles == 1 ? 1.0f : 0.0f;
				RequireOk(world->AddBody(chassis));
				if (withVehicles == 1)
				{
					RequireOk(world->AddVehicle(MakeBike(id, VehicleTransmissionMode::Automatic)));
				}
			}
			std::vector<double> durations;
			REQUIRE(Profiler::Start().IsOk());
			for (int step = 0; step < WarmUpSteps + Steps; step++)
			{
				for (int bike = 0; bike < Bikes; bike++)
				{
					world->SetVehicleInputs(UUID(1000 + static_cast<uint64_t>(bike)), RideInputs(step % 300, bike % 2 == 0 ? 0.3f : -0.3f));
				}
				Profiler::BeginFrame();
				auto const start = Clock::now();
				world->Step(Step);
				auto const end = Clock::now();
				Profiler::EndFrame();
				if (step >= WarmUpSteps)
				{
					durations.push_back(std::chrono::duration<double, std::micro>(end - start).count());
				}
			}
			Profiler::Stop();
			std::sort(durations.begin(), durations.end());
			perStep[withVehicles] = durations[durations.size() / 2];
			// The state hash and a snapshot of the world as the ride left it: their medians, the vehicles' share of each.
			std::vector<double> hashes;
			std::vector<double> saves;
			uint64_t const expected = world->ComputeStateHash();
			bool sameHash = true;
			for (int call = 0; call < 200; call++)
			{
				auto const hashStart = Clock::now();
				sameHash = sameHash && world->ComputeStateHash() == expected;
				auto const hashEnd = Clock::now();
				PhysicsSnapshot const snapshot = world->SaveState();
				auto const saveEnd = Clock::now();
				REQUIRE(snapshot.GetSize() > 0);
				hashes.push_back(std::chrono::duration<double, std::micro>(hashEnd - hashStart).count());
				saves.push_back(std::chrono::duration<double, std::micro>(saveEnd - hashEnd).count());
			}
			CHECK(sameHash);
			std::sort(hashes.begin(), hashes.end());
			std::sort(saves.begin(), saves.end());
			hashCost[withVehicles] = hashes[hashes.size() / 2];
			saveCost[withVehicles] = saves[saves.size() / 2];
			if (withVehicles == 1)
			{
				ProfilerMedians const medians = ComputeProfilerMedians(Profiler::GetRecording());
				for (ProfilerZoneMedian const& zone : medians.Zones)
				{
					if (zone.Name == "Physics.Vehicles")
					{
						vehiclesZone = zone.MillisecondsPerCall * 1000.0;
					}
				}
			}
		}
		MESSAGE("Round " << round << ": a step of " << Bikes << " chassis without vehicles " << perStep[0] << " us, with vehicles "
		                 << perStep[1] << " us: " << (perStep[1] - perStep[0]) / Bikes << " us a vehicle a step; Physics.Vehicles "
		                 << vehiclesZone << " us a call: " << vehiclesZone / Bikes << " us a vehicle (target 50 us); the state hash "
		                 << hashCost[0] << " and " << hashCost[1] << " us: " << (hashCost[1] - hashCost[0]) / Bikes
		                 << " us a vehicle; a snapshot " << saveCost[0] << " and " << saveCost[1]
		                 << " us: " << (saveCost[1] - saveCost[0]) / Bikes << " us a vehicle");
	}
}
