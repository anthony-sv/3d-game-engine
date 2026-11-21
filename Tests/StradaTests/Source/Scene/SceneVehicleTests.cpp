#include "Core/ProfilerTestUtilities.h"
#include "Core/TelemetryTestUtilities.h"
#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/CurveAsset.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/MeshFactory.h"
#include "Strada/Asset/Vehicle.h"
#include "Strada/Asset/VehicleAsset.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Platform.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Core/Telemetry.h"
#include "Strada/Math/Curve.h"
#include "Strada/Math/Math.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	constexpr float Step = 1.0f / 60.0f;
	constexpr uint64_t GroundId = 100;

	template<typename TData>
	AssetHandle AddDataAsset(TData const& values, std::string name)
	{
		TypedDataAssetType<TData> const* type = DataAssetRegistry::Find<TData>();
		REQUIRE(type != nullptr);
		Ref<TypedDataAsset<TData>> const asset = std::static_pointer_cast<TypedDataAsset<TData>>(type->CreateAsset());
		Result<void> const set = asset->SetData(values);
		REQUIRE_MESSAGE(set.IsOk(), (set ? std::string() : set.GetError()));
		return AssetManager::AddMemoryAsset(asset, std::move(name));
	}

	template<typename TData>
	void Edit(AssetHandle handle, TData const& values)
	{
		Ref<TypedDataAsset<TData>> const asset = AssetManager::GetAsset<TypedDataAsset<TData>>(handle);
		REQUIRE(asset != nullptr);
		Result<void> const set = asset->SetData(values);
		REQUIRE_MESSAGE(set.IsOk(), (set ? std::string() : set.GetError()));
	}

	SceneRuntimeSettings PhysicsOnly()
	{
		SceneRuntimeSettings settings;
		settings.RunScripts = false;
		settings.PlayAudio = false;
		return settings;
	}

	void MakeGround(Scene& scene)
	{
		Entity ground = scene.CreateEntityWithUUID(UUID(GroundId), "Ground");
		ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
		ground.AddComponent<BoxColliderComponent>().HalfExtents = {200.0f, 0.5f, 200.0f};
	}

	// A motorcycle's chassis at x: a dynamic 250 kg box from 0.25 m to 1.25 m above its origin, 0.5 m wide and 1.2 m long,
	// whose roll inertia (26 kg m²) carries the lean spring's default damping (Docs/Architecture.md §8, "Vehicles"), and
	// its Vehicle.
	Entity MakeBike(Scene& scene, uint64_t id, float x, AssetHandle definition, glm::vec3 const& velocity = glm::vec3(0.0f))
	{
		Entity bike = scene.CreateEntityWithUUID(UUID(id), "Bike " + std::to_string(id));
		bike.GetComponent<TransformComponent>().Translation = {x, 0.05f, 0.0f};
		RigidBodyComponent& body = bike.AddComponent<RigidBodyComponent>();
		body.Type = RigidBodyType::Dynamic;
		body.Mass = 250.0f;
		body.LinearDamping = 0.0f;
		body.AllowSleep = false;
		body.InitialLinearVelocity = velocity;
		BoxColliderComponent& collider = bike.AddComponent<BoxColliderComponent>();
		collider.HalfExtents = {0.25f, 0.5f, 0.6f};
		collider.Offset = {0.0f, 0.75f, 0.0f};
		bike.AddComponent<VehicleComponent>().Definition = definition;
		return bike;
	}

	void Run(Scene& scene, int frames, float frameTime = Step)
	{
		for (int frame = 0; frame < frames; frame++)
		{
			scene.OnUpdateRuntime(Timestep(frameTime));
		}
	}

	VehicleState StateOf(Scene& scene, uint64_t id)
	{
		std::optional<VehicleState> const state = scene.GetPhysicsScene()->GetVehicleState(UUID(id));
		REQUIRE(state);
		return *state;
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

	// How many log entries since `first` hold every one of the texts.
	size_t CountLogged(uint64_t first, std::initializer_list<std::string_view> texts)
	{
		std::vector<LogEntry> const entries = Log::GetEntries(first);
		return static_cast<size_t>(std::count_if(entries.begin(), entries.end(),
		                                         [texts](LogEntry const& entry)
		                                         {
													 return std::all_of(texts.begin(), texts.end(),
			                                                            [&entry](std::string_view text)
			                                                            {
																			return entry.Message.find(text) != std::string::npos;
																		});
												 }));
	}

	CurveKey Key(float time, float value)
	{
		CurveKey key;
		key.Time = time;
		key.Value = value;
		key.Interpolation = CurveInterpolation::Linear;
		return key;
	}
}

TEST_CASE("ScenePhysics: a Vehicle makes its vehicle as the scene starts and when its chassis is rebuilt, and none without a definition "
          "that loads, a dynamic body or its entity")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	AssetHandle const definition = AddDataAsset(Vehicle{}, "Bike.svehicle");
	AssetHandle const cube = AssetManager::AddMemoryAsset(MeshFactory::CreateCube(AssetHandle()), "Cube");
	Vehicle wrongCurve;
	wrongCurve.Wheels[0].LongitudinalFriction = cube;
	AssetHandle const wrongCurveDefinition = AddDataAsset(wrongCurve, "Wrong Curve.svehicle");

	Scene scene("Vehicles");
	MakeGround(scene);
	Entity bike = MakeBike(scene, 200, 0.0f, definition);
	MakeBike(scene, 300, 5.0f, wrongCurveDefinition);
	// No definition; a definition on a static body.
	MakeBike(scene, 400, 10.0f, AssetHandle());
	Entity parked = scene.CreateEntityWithUUID(UUID(500), "Parked");
	parked.GetComponent<TransformComponent>().Translation = {15.0f, 0.5f, 0.0f};
	parked.AddComponent<BoxColliderComponent>();
	parked.AddComponent<VehicleComponent>().Definition = definition;

	uint64_t const logStart = Log::GetNextEntryIndex();
	scene.OnRuntimeStart(PhysicsOnly());
	PhysicsScene& world = *scene.GetPhysicsScene();
	CHECK(world.GetVehicleCount() == 2);
	CHECK(world.HasVehicle(UUID(200)));
	CHECK(world.HasVehicle(UUID(300)));
	Run(scene, 60);
	// Each reason is said once, whatever the frames do.
	CHECK(CountLogged(logStart, {"Physics: the Vehicle of 'Bike 400' makes no vehicle: it names no Definition"}) == 1);
	CHECK(CountLogged(logStart, {"Physics: the Vehicle of 'Parked' makes no vehicle: its entity has no dynamic RigidBody"}) == 1);
	CHECK(CountLogged(logStart, {"Physics: the Vehicle of 'Bike 300' takes the default for its Wheels[0].LongitudinalFriction",
	                             "is no Curve that loads"}) == 1);
	CHECK(CountLogged(logStart, {"'Bike 200'"}) == 0);

	// A new definition's bike stands on both wheels, its suspensions off their stops, upright.
	VehicleState const standing = StateOf(scene, 200);
	Vehicle const defaults;
	for (size_t wheel = 0; wheel < VehicleWheelCount; wheel++)
	{
		CAPTURE(wheel);
		CHECK(standing.Wheels[wheel].ContactEntity == UUID(GroundId));
		CHECK(standing.Wheels[wheel].SuspensionLength > defaults.Wheels[wheel].SuspensionMinLength + 0.02f);
		CHECK(standing.Wheels[wheel].SuspensionLength < defaults.Wheels[wheel].SuspensionMaxLength - 0.02f);
	}
	CHECK(std::abs(standing.LeanAngle) < 0.05f);

	// Its collider replaced: the body is made again, and its vehicle with it, keeping the rider's inputs.
	uint64_t const structure = world.GetStructureVersion();
	world.SetVehicleInputs(UUID(200), MakeInputs(0.3f, 0.0f, 0.0f, 0.0f));
	BoxColliderComponent wider = bike.GetComponent<BoxColliderComponent>();
	wider.HalfExtents.x = 0.3f;
	bike.AddOrReplaceComponent<BoxColliderComponent>(wider);
	Run(scene, 1);
	CHECK(world.GetStructureVersion() != structure);
	CHECK(world.HasVehicle(UUID(200)));
	CHECK(StateOf(scene, 200).Inputs.Throttle == 0.3f);
	CHECK(world.GetVehicleCount() == 2);

	// A body that stops being dynamic stops being a vehicle; dynamic again, it is one again.
	RigidBodyComponent kinematic = bike.GetComponent<RigidBodyComponent>();
	kinematic.Type = RigidBodyType::Kinematic;
	bike.AddOrReplaceComponent<RigidBodyComponent>(kinematic);
	Run(scene, 2);
	CHECK_FALSE(world.HasVehicle(UUID(200)));
	CHECK(CountLogged(logStart, {"'Bike 200' makes no vehicle: its entity has no dynamic RigidBody"}) == 1);
	kinematic.Type = RigidBodyType::Dynamic;
	bike.AddOrReplaceComponent<RigidBodyComponent>(kinematic);
	Run(scene, 1);
	CHECK(world.HasVehicle(UUID(200)));

	// Its definition unset and set again; the component removed and added again.
	bike.AddOrReplaceComponent<VehicleComponent>(VehicleComponent{});
	Run(scene, 1);
	CHECK_FALSE(world.HasVehicle(UUID(200)));
	bike.AddOrReplaceComponent<VehicleComponent>(VehicleComponent{definition});
	Run(scene, 1);
	CHECK(world.HasVehicle(UUID(200)));
	bike.RemoveComponent<VehicleComponent>();
	Run(scene, 1);
	CHECK_FALSE(world.HasVehicle(UUID(200)));
	CHECK(world.GetVehicleCount() == 1);
	bike.AddComponent<VehicleComponent>().Definition = definition;
	Run(scene, 1);
	CHECK(world.HasVehicle(UUID(200)));

	// A definition that names an asset of another type is no definition.
	Entity other = scene.GetEntityByUUID(UUID(300));
	other.AddOrReplaceComponent<VehicleComponent>(VehicleComponent{cube});
	Run(scene, 1);
	CHECK_FALSE(world.HasVehicle(UUID(300)));
	CHECK(CountLogged(logStart, {"'Bike 300' makes no vehicle: its Definition", "is no Vehicle that loads"}) == 1);

	// Destroyed with its entity.
	scene.DestroyEntity(bike);
	Run(scene, 2);
	CHECK(world.GetVehicleCount() == 0);
	scene.OnRuntimeStop();
}

TEST_CASE("ScenePhysics: a vehicle's inputs reach it once per fixed step: the latest of a frame's, none in a frame without a step")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	AssetHandle const definition = AddDataAsset(Vehicle{}, "Bike.svehicle");
	Scene scene("Inputs");
	MakeGround(scene);
	MakeBike(scene, 200, 0.0f, definition);
	scene.OnRuntimeStart(PhysicsOnly());
	PhysicsScene& world = *scene.GetPhysicsScene();
	Run(scene, 30);

	// Two writes between frames: the latest is what the next step applies.
	VehicleInputs const first = MakeInputs(0.5f, 0.0f, 0.0f, 0.2f);
	VehicleInputs const second = MakeInputs(0.25f, 0.1f, 0.2f, -0.3f);
	world.SetVehicleInputs(UUID(200), first);
	world.SetVehicleInputs(UUID(200), second);
	CHECK(StateOf(scene, 200).Inputs == second);
	CHECK(StateOf(scene, 200).AppliedInputs == VehicleInputs{});
	// A frame shorter than a step runs none and applies nothing; the frame that completes the step applies them.
	uint64_t const steps = scene.GetFixedStepCount();
	scene.OnUpdateRuntime(Timestep(0.4f * Step));
	CHECK(scene.GetFixedStepCount() == steps);
	CHECK(StateOf(scene, 200).AppliedInputs == VehicleInputs{});
	scene.OnUpdateRuntime(Timestep(0.6f * Step));
	CHECK(scene.GetFixedStepCount() == steps + 1);
	CHECK(StateOf(scene, 200).AppliedInputs == second);

	// Kept clamped; values that are not numbers are 0.
	world.SetVehicleInputs(UUID(200), MakeInputs(2.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), -3.0f));
	CHECK(StateOf(scene, 200).Inputs == MakeInputs(1.0f, 0.0f, 0.0f, -1.0f));
	world.SetVehicleInputs(UUID(200), first);
	// A frame of eight steps applies the same latest inputs to each.
	scene.OnUpdateRuntime(Timestep(8.0f * Step));
	CHECK(scene.GetFixedStepCount() == steps + 9);
	CHECK(StateOf(scene, 200).AppliedInputs == first);

	// A vehicle added between frames exists once the scene's physics changes are applied (as the scripting API does before
	// it reaches a body), and the next step applies the inputs it is given.
	MakeBike(scene, 300, 5.0f, definition);
	CHECK_FALSE(world.HasVehicle(UUID(300)));
	scene.ApplyPhysicsChanges();
	REQUIRE(world.HasVehicle(UUID(300)));
	world.SetVehicleInputs(UUID(300), second);
	Run(scene, 1);
	CHECK(StateOf(scene, 300).AppliedInputs == second);
	scene.OnRuntimeStop();
}

TEST_CASE("ScenePhysics: a vehicle's definition or curve edited while the scene runs reaches its next step without making it again")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	// Tyres without grip: the engine spins the rear wheel, and the bike goes nowhere.
	Curve slick;
	slick.Keys = {Key(0.0f, 0.0f)};
	AssetHandle const tyre = AddDataAsset(slick, "Tyre.scurve");
	Vehicle values;
	for (VehicleWheelDefinition& wheel : values.Wheels)
	{
		wheel.AngularDamping = 0.0f;
		wheel.LongitudinalFriction = tyre;
	}
	values.Wheels[0].FrontBrakeTorque = 120.0f;
	AssetHandle const definition = AddDataAsset(values, "Bike.svehicle");

	Scene scene("Edits");
	MakeGround(scene);
	MakeBike(scene, 200, 0.0f, definition);
	uint64_t const logStart = Log::GetNextEntryIndex();
	scene.OnRuntimeStart(PhysicsOnly());
	PhysicsScene& world = *scene.GetPhysicsScene();
	Run(scene, 20);
	uint64_t const structure = world.GetStructureVersion();
	world.SetVehicleInputs(UUID(200), MakeInputs(0.3f, 0.0f, 0.0f, 0.0f));
	Run(scene, 30);
	CHECK(StateOf(scene, 200).Wheels[1].AngularVelocity > 10.0f);
	CHECK(std::abs(StateOf(scene, 200).ForwardSpeed) < 0.5f);

	// The curve edited to a tyre that grips: the bike rides off at once.
	Curve grip;
	grip.Keys = {Key(0.0f, 0.0f), Key(0.06f, 1.2f), Key(0.2f, 1.0f)};
	Edit(tyre, grip);
	Run(scene, 30);
	CHECK(StateOf(scene, 200).ForwardSpeed > 1.5f);
	CHECK(world.GetStructureVersion() == structure);

	// Lifted off the ground, free of gravity, the front wheel spins down by the front brake's torque over its inertia at
	// each step; the definition edited to another torque, the next step brakes by it.
	world.SetVehicleInputs(UUID(200), MakeInputs(0.0f, 0.0f, 0.0f, 0.0f));
	Run(scene, 10);
	std::optional<BodyTransform> const chassis = world.GetBodyTransform(UUID(200));
	REQUIRE(chassis);
	world.SetBodyTransform(UUID(200), chassis->Position + glm::vec3(0.0f, 3.0f, 0.0f), chassis->Rotation);
	world.SetGravityFactor(UUID(200), 0.0f);
	world.SetLinearVelocity(UUID(200), glm::vec3(0.0f));
	world.SetAngularVelocity(UUID(200), glm::vec3(0.0f));
	Run(scene, 2);
	REQUIRE_FALSE(StateOf(scene, 200).Wheels[0].ContactEntity.IsValid());
	world.SetVehicleInputs(UUID(200), MakeInputs(0.0f, 1.0f, 0.0f, 0.0f));
	auto const spinDrop = [&scene]
	{
		float const before = StateOf(scene, 200).Wheels[0].AngularVelocity;
		Run(scene, 1);
		return before - StateOf(scene, 200).Wheels[0].AngularVelocity;
	};
	float const inertia = values.Wheels[0].Inertia;
	REQUIRE(StateOf(scene, 200).Wheels[0].AngularVelocity > 4.0f);
	CHECK(spinDrop() == doctest::Approx(120.0f * Step / inertia).epsilon(1e-4));
	values.Wheels[0].FrontBrakeTorque = 40.0f;
	Edit(definition, values);
	CHECK(spinDrop() == doctest::Approx(40.0f * Step / inertia).epsilon(1e-4));
	CHECK(world.GetStructureVersion() == structure);
	CHECK(world.HasVehicle(UUID(200)));
	// Edits in place say nothing.
	std::string_view const madeAgain = "Physics: the Vehicle of 'Bike 200' is made again: its definition's Up or Forward changed";
	CHECK(CountLogged(logStart, {madeAgain}) == 0);

	// The chassis's axes are taken when the vehicle is made: an edit of them makes it again (the structure moves), keeping
	// the rider's inputs, and says so once for the edit.
	values.Forward = glm::vec3(0.0f, 0.02f, -1.0f);
	Edit(definition, values);
	Run(scene, 1);
	CHECK(world.GetStructureVersion() != structure);
	CHECK(world.HasVehicle(UUID(200)));
	CHECK(StateOf(scene, 200).Inputs.FrontBrake == 1.0f);
	CHECK(CountLogged(logStart, {madeAgain, "snapshots taken before are refused"}) == 1);
	Run(scene, 10);
	CHECK(CountLogged(logStart, {madeAgain}) == 1);
	// Another edit of them makes it again, and says so again; one in place after it, nothing.
	uint64_t const remade = world.GetStructureVersion();
	values.Up = glm::vec3(0.0f, 1.0f, 0.02f);
	Edit(definition, values);
	Run(scene, 1);
	CHECK(world.GetStructureVersion() != remade);
	CHECK(CountLogged(logStart, {madeAgain}) == 2);
	uint64_t const again = world.GetStructureVersion();
	values.Wheels[0].FrontBrakeTorque = 60.0f;
	Edit(definition, values);
	Run(scene, 1);
	CHECK(world.GetStructureVersion() == again);
	CHECK(CountLogged(logStart, {madeAgain}) == 2);

	// A definition that stops loading leaves the entity without a vehicle until it loads again.
	AssetManager::RemoveMemoryAsset(definition);
	Run(scene, 1);
	CHECK_FALSE(world.HasVehicle(UUID(200)));
	CHECK(CountLogged(logStart, {"'Bike 200' makes no vehicle: its Definition", "is no Vehicle that loads"}) == 1);
	scene.OnRuntimeStop();
}

TEST_CASE("ScenePhysics: a vehicle whose lean spring cannot settle its chassis is warned of once each time it comes to that: made, "
          "its values edited or its body changed")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	// The two chassis of the lean spring's bound (PhysicsVehicleTests.cpp): the wide one, 250 kg, 0.5 m wide and 1 m high
	// (26 kg m² about its front), clears the default damping of 1,000 times a step of 1/60 s (16.7); the thin one, 200 kg,
	// 0.3 m wide and 0.5 m high (200 x (0.3² + 0.5²) / 12 = 5.67 kg m²), does not, and a damping below 340 settles it.
	AssetHandle const wideDefinition = AddDataAsset(Vehicle{}, "Wide.svehicle");
	Vehicle thinValues;
	AssetHandle const thinDefinition = AddDataAsset(thinValues, "Thin.svehicle");
	Scene scene("Lean Springs");
	MakeGround(scene);
	Entity wide = MakeBike(scene, 200, 0.0f, wideDefinition);
	Entity thin = MakeBike(scene, 300, 5.0f, thinDefinition);
	thin.GetComponent<RigidBodyComponent>().Mass = 200.0f;
	BoxColliderComponent& thinCollider = thin.GetComponent<BoxColliderComponent>();
	thinCollider.HalfExtents = {0.15f, 0.25f, 0.6f};
	thinCollider.Offset = {0.0f, 0.5f, 0.0f};
	uint64_t const logStart = Log::GetNextEntryIndex();
	scene.OnRuntimeStart(PhysicsOnly());
	PhysicsScene& world = *scene.GetPhysicsScene();
	std::optional<float> const wideInertia = world.GetVehicleRollInertia(UUID(200));
	std::optional<float> const thinInertia = world.GetVehicleRollInertia(UUID(300));
	REQUIRE(wideInertia);
	REQUIRE(thinInertia);
	CHECK(*wideInertia == doctest::Approx(250.0f * (0.25f + 1.0f) / 12.0f).epsilon(1e-3));
	CHECK(*thinInertia == doctest::Approx(200.0f * (0.09f + 0.25f) / 12.0f).epsilon(1e-3));
	CHECK_FALSE(world.GetVehicleRollInertia(UUID(GroundId)));

	std::string_view const thrown = "may be thrown over by its lean spring";
	auto const warnings = [logStart, thrown](std::string_view bike)
	{
		return CountLogged(logStart, {bike, thrown});
	};
	// Made: the thin chassis is warned of, naming its inertia, the damping, the step and the damping that settles it.
	CHECK(warnings("'Bike 300'") == 1);
	CHECK(CountLogged(logStart, {"'Bike 300' may be thrown over by its lean spring: its chassis's roll inertia about its front, 5.667 "
	                             "kg m², is at or below its lean spring's damping, 1000, times the collision step, 0.01667 s; a "
	                             "damping below 340 settles it"}) == 1);
	CHECK(warnings("'Bike 200'") == 0);
	Run(scene, 30);
	CHECK(warnings("'Bike 300'") == 1);

	// Its values edited to a damping that settles it: no warning; edited back into it, warned again, once.
	thinValues.Lean.SpringDamping = 200.0f;
	Edit(thinDefinition, thinValues);
	Run(scene, 10);
	CHECK(warnings("'Bike 300'") == 1);
	thinValues.Lean.SpringDamping = 900.0f;
	Edit(thinDefinition, thinValues);
	Run(scene, 10);
	CHECK(warnings("'Bike 300'") == 2);
	CHECK(CountLogged(logStart, {"'Bike 300'", thrown, "its lean spring's damping, 900,"}) == 1);
	// An edit that leaves it there says nothing new.
	thinValues.Lean.SpringConstant = 4000.0f;
	Edit(thinDefinition, thinValues);
	Run(scene, 10);
	CHECK(warnings("'Bike 300'") == 2);

	// The wide chassis's body changed into it: its collider made as thin, and a lighter body.
	RigidBodyComponent lighter = wide.GetComponent<RigidBodyComponent>();
	lighter.Mass = 200.0f;
	wide.AddOrReplaceComponent<RigidBodyComponent>(lighter);
	BoxColliderComponent narrow = wide.GetComponent<BoxColliderComponent>();
	narrow.HalfExtents = {0.15f, 0.25f, 0.6f};
	narrow.Offset = {0.0f, 0.5f, 0.0f};
	wide.AddOrReplaceComponent<BoxColliderComponent>(narrow);
	Run(scene, 2);
	REQUIRE(world.HasVehicle(UUID(200)));
	CHECK(warnings("'Bike 200'") == 1);
	Run(scene, 10);
	CHECK(warnings("'Bike 200'") == 1);
	// No value of the author's changed.
	CHECK(AssetManager::GetAsset<TypedDataAsset<Vehicle>>(thinDefinition)->GetData().Lean.SpringDamping == 900.0f);
	scene.OnRuntimeStop();
}

TEST_CASE("ScenePhysics: a vehicle that leans on a wheel wider than its round cast is warned of once for each wheel and width: made, "
          "its values edited or made again, never for a wheel of 0.1 m or a bike held upright")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	// Docs/Architecture.md §8, "The wheels' cast": a wheel's cast is round up to 0.1 m wide, and a wider one rides on a flat
	// bottom. Bike 200 rides on wheels of exactly 0.1 m, Bike 300 on a rear wheel of 0.2 m, Bike 400 on the same rear wheel
	// with a largest lean of 0, which holds it upright.
	Vehicle roundValues;
	for (VehicleWheelDefinition& wheel : roundValues.Wheels)
	{
		wheel.Width = MaxRoundVehicleWheelWidth;
	}
	Vehicle wideValues;
	wideValues.Wheels[1].Width = 0.2f;
	Vehicle uprightValues = wideValues;
	uprightValues.Lean.MaxLeanAngle = 0.0f;
	AssetHandle const roundDefinition = AddDataAsset(roundValues, "Round.svehicle");
	AssetHandle const wideDefinition = AddDataAsset(wideValues, "Wide.svehicle");
	AssetHandle const uprightDefinition = AddDataAsset(uprightValues, "Upright.svehicle");
	Scene scene("Wheel Widths");
	MakeGround(scene);
	MakeBike(scene, 200, 0.0f, roundDefinition);
	Entity wide = MakeBike(scene, 300, 5.0f, wideDefinition);
	MakeBike(scene, 400, 10.0f, uprightDefinition);
	uint64_t const logStart = Log::GetNextEntryIndex();
	scene.OnRuntimeStart(PhysicsOnly());
	PhysicsScene& world = *scene.GetPhysicsScene();
	REQUIRE(world.GetVehicleCount() == 3);

	std::string_view const leansLess = "leans less than its turns balance";
	auto const warnings = [logStart, leansLess](std::string_view bike)
	{
		return CountLogged(logStart, {bike, leansLess});
	};
	// Made: the wide rear wheel is warned of, naming the wheel, its width and the round cast's bound; a wheel of 0.1 m and a
	// bike that never leans are not.
	CHECK(warnings("'Bike 300'") == 1);
	CHECK(CountLogged(logStart, {"Physics: the Vehicle of 'Bike 300' leans less than its turns balance: its wheel 'Rear' is 0.2 m "
	                             "wide, and a wheel's cast against the ground is round only up to 0.1 m wide"}) == 1);
	CHECK(warnings("'Bike 200'") == 0);
	CHECK(warnings("'Bike 400'") == 0);
	Run(scene, 30);
	CHECK(warnings("'Bike 300'") == 1);

	// An edit that leaves the wheel as it was, and its chassis's body made again, say nothing new.
	wideValues.Wheels[0].FrontBrakeTorque = 400.0f;
	Edit(wideDefinition, wideValues);
	Run(scene, 5);
	BoxColliderComponent wider = wide.GetComponent<BoxColliderComponent>();
	wider.HalfExtents.x = 0.3f;
	wide.AddOrReplaceComponent<BoxColliderComponent>(wider);
	Run(scene, 5);
	REQUIRE(world.HasVehicle(UUID(300)));
	CHECK(warnings("'Bike 300'") == 1);

	// Another width above the bound is a new cause; back at the bound, nothing; above it again, warned again.
	wideValues.Wheels[1].Width = 0.25f;
	Edit(wideDefinition, wideValues);
	Run(scene, 5);
	CHECK(warnings("'Bike 300'") == 2);
	CHECK(CountLogged(logStart, {"'Bike 300'", leansLess, "'Rear' is 0.25 m wide"}) == 1);
	wideValues.Wheels[1].Width = MaxRoundVehicleWheelWidth;
	Edit(wideDefinition, wideValues);
	Run(scene, 5);
	CHECK(warnings("'Bike 300'") == 2);
	wideValues.Wheels[1].Width = 0.25f;
	Edit(wideDefinition, wideValues);
	Run(scene, 5);
	CHECK(warnings("'Bike 300'") == 3);
	// The other wheel is a cause of its own.
	wideValues.Wheels[0].Width = 0.15f;
	Edit(wideDefinition, wideValues);
	Run(scene, 5);
	CHECK(warnings("'Bike 300'") == 4);
	CHECK(CountLogged(logStart, {"'Bike 300'", leansLess, "'Front' is 0.15 m wide"}) == 1);

	// No vehicle for a while (its definition unset), then made again: warned again, once for each wide wheel.
	wide.AddOrReplaceComponent<VehicleComponent>(VehicleComponent{});
	Run(scene, 2);
	REQUIRE_FALSE(world.HasVehicle(UUID(300)));
	wide.AddOrReplaceComponent<VehicleComponent>(VehicleComponent{wideDefinition});
	Run(scene, 2);
	REQUIRE(world.HasVehicle(UUID(300)));
	CHECK(warnings("'Bike 300'") == 6);

	// The upright bike allowed to lean: its wide wheel is warned of.
	uprightValues.Lean.MaxLeanAngle = 30.0f;
	Edit(uprightDefinition, uprightValues);
	Run(scene, 5);
	CHECK(warnings("'Bike 400'") == 1);
	CHECK(warnings("'Bike 200'") == 0);
	// No value of the author's changed.
	CHECK(AssetManager::GetAsset<TypedDataAsset<Vehicle>>(wideDefinition)->GetData().Wheels[1].Width == 0.25f);
	CHECK(world.GetVehicleState(UUID(300)).has_value());
	scene.OnRuntimeStop();
}

TEST_CASE("SceneTelemetry: each vehicle records its sixteen channels at every fixed step while a recording runs, named by its UUID, "
          "with the values its step left, and recording leaves the state hashes as they are")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	Testing::TelemetryScope const telemetry;
	AssetHandle const definition = AddDataAsset(Vehicle{}, "Bike.svehicle");
	constexpr int Frames = 40;
	// A vehicle's channels' values as Docs/Architecture.md §8 names them: the angles in degrees.
	auto const channelValues = [](VehicleState const& state)
	{
		std::array<float, VehicleChannelCount> values{};
		values[0] = state.AppliedInputs.Throttle;
		values[1] = state.AppliedInputs.FrontBrake;
		values[2] = state.AppliedInputs.RearBrake;
		values[3] = state.AppliedInputs.Lean;
		values[4] = state.EngineSpeed;
		values[5] = static_cast<float>(state.Gear);
		values[6] = state.ForwardSpeed;
		values[7] = glm::degrees(state.LeanAngle);
		for (size_t wheel = 0; wheel < VehicleWheelCount; wheel++)
		{
			values[8 + 4 * wheel] = state.Wheels[wheel].LongitudinalSlip;
			values[9 + 4 * wheel] = glm::degrees(state.Wheels[wheel].LateralSlip);
			values[10 + 4 * wheel] = state.Wheels[wheel].SuspensionLength;
			values[11 + 4 * wheel] = state.Wheels[wheel].TyreGrip;
		}
		return values;
	};
	struct Ride
	{
		std::vector<uint64_t> Hashes;
		std::vector<std::array<float, VehicleChannelCount>> Values;
	};
	auto const ride = [&definition, &channelValues](bool record)
	{
		if (record)
		{
			REQUIRE(Telemetry::Start().IsOk());
		}
		Scene scene("Channels");
		MakeGround(scene);
		// Made first, the bike of the larger UUID records after the other.
		MakeBike(scene, 300, 5.0f, definition);
		MakeBike(scene, 200, 0.0f, definition);
		scene.OnRuntimeStart(PhysicsOnly());
		PhysicsScene& world = *scene.GetPhysicsScene();
		Ride result;
		for (int frame = 0; frame < Frames; frame++)
		{
			float const odd = static_cast<float>(frame % 2);
			world.SetVehicleInputs(UUID(200),
			                       MakeInputs(frame < 25 ? 0.6f : 0.0f, frame < 25 ? 0.0f : 0.5f, 0.25f * odd, frame < 30 ? 0.3f : -0.2f));
			scene.OnUpdateRuntime(Timestep(Step));
			result.Hashes.push_back(scene.ComputePhysicsStateHash());
			result.Values.push_back(channelValues(StateOf(scene, 200)));
		}
		if (record)
		{
			// A vehicle gone records no more (a running scene destroys an entity at the end of the frame, after that frame's
			// step); a frame without a step adds no row.
			scene.DestroyEntity(scene.GetEntityByUUID(UUID(300)));
			Run(scene, 5);
			scene.OnUpdateRuntime(Timestep(0.4f * Step));
			Telemetry::Stop();
		}
		scene.OnRuntimeStop();
		return result;
	};
	Ride const unrecorded = ride(false);
	CHECK(Telemetry::GetSessions().empty());
	Ride const recorded = ride(true);
	CHECK(recorded.Hashes == unrecorded.Hashes);
	CHECK(recorded.Values == unrecorded.Values);

	REQUIRE(Telemetry::GetSessions().size() == 1);
	TelemetrySession const& session = Testing::GetNewestTelemetrySession();
	CHECK(session.FirstStep == 1);
	CHECK(session.Rows == Frames + 5);
	REQUIRE(session.Channels.size() == 2 * VehicleChannelCount);
	for (size_t index = 0; index < VehicleChannelCount; index++)
	{
		CAPTURE(index);
		std::string const first = "Vehicle.200." + std::string(VehicleChannelQuantities[index]);
		std::string const second = "Vehicle.300." + std::string(VehicleChannelQuantities[index]);
		// In the order of the vehicles' UUIDs, whatever the order their entities were made in.
		CHECK(session.Channels[index].Name == first);
		CHECK(session.Channels[VehicleChannelCount + index].Name == second);
		CHECK(session.Channels[index].Count == Frames + 5);
		CHECK(session.Channels[VehicleChannelCount + index].Count == Frames + 1);
		// Each row holds what its step left.
		std::vector<std::optional<float>> const column = Testing::GetTelemetryColumn(session, first);
		for (int row = 0; row < Frames; row++)
		{
			CAPTURE(row);
			REQUIRE(column[static_cast<size_t>(row)].has_value());
			CHECK(*column[static_cast<size_t>(row)] == recorded.Values[static_cast<size_t>(row)][index]);
		}
		CHECK_FALSE(Testing::GetTelemetryColumn(session, second)[Frames + 1].has_value());
	}
	// What they hold: the inputs as applied, a bike that rode in first gear, on its tyres' grip of 1, the lean in degrees.
	CHECK(FindTelemetryChannel(session, "Vehicle.200.Throttle")->Maximum == 0.6f);
	CHECK(FindTelemetryChannel(session, "Vehicle.200.RearBrake")->Maximum == 0.25f);
	CHECK(FindTelemetryChannel(session, "Vehicle.200.Gear")->Maximum >= 1.0f);
	CHECK(FindTelemetryChannel(session, "Vehicle.200.ForwardSpeed")->Maximum > 0.1f);
	CHECK(FindTelemetryChannel(session, "Vehicle.200.Front.Grip")->Minimum == 1.0f);
	CHECK(FindTelemetryChannel(session, "Vehicle.300.Gear")->Maximum == 0.0f);
}

TEST_CASE("Scene: a bike duplicated keeps its definition and its wheels' names, and runs as a vehicle of its own")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	AssetHandle const definition = AddDataAsset(Vehicle{}, "Bike.svehicle");
	Scene scene("Copies");
	MakeGround(scene);
	Entity bike = MakeBike(scene, 200, 0.0f, definition);
	for (std::string const wheel : {"Front", "Rear"})
	{
		Entity entity = scene.CreateEntity(wheel + " Wheel", bike);
		entity.AddComponent<VehicleWheelComponent>().Wheel = wheel;
	}

	// Neither component names another entity (a wheel finds its vehicle above it), so a copy needs no remapping.
	Entity copy = scene.DuplicateEntity(bike);
	REQUIRE(copy);
	CHECK(copy.GetUUID() != bike.GetUUID());
	REQUIRE(copy.HasComponent<VehicleComponent>());
	CHECK(copy.GetComponent<VehicleComponent>().Definition == definition);
	std::vector<std::string> wheels;
	for (UUID const child : copy.GetComponent<RelationshipComponent>().Children)
	{
		Entity const entity = scene.GetEntityByUUID(child);
		REQUIRE(entity.HasComponent<VehicleWheelComponent>());
		wheels.push_back(entity.GetComponent<VehicleWheelComponent>().Wheel);
	}
	CHECK(wheels == std::vector<std::string>{"Front", "Rear"});

	// Moved aside, the copy is a second vehicle.
	copy.GetComponent<TransformComponent>().Translation.x = 5.0f;
	scene.OnRuntimeStart(PhysicsOnly());
	CHECK(scene.GetPhysicsScene()->GetVehicleCount() == 2);
	CHECK(scene.GetPhysicsScene()->HasVehicle(copy.GetUUID()));
	scene.OnRuntimeStop();
}

namespace
{
	constexpr uint64_t BikeId = 200;
	constexpr uint64_t FrontWheelId = 201;
	constexpr uint64_t RearWheelId = 202;
	constexpr float Frame30 = 1.0f / 30.0f;
	constexpr float Frame144 = 1.0f / 144.0f;

	// A ride as a function of the fixed step count alone (rule 40). Every input and shift changes at even counts only, so
	// that a frame of two steps (30 frames a second) gives both the inputs of its first step, as an OnFixedUpdate reading
	// the step count would give them.
	struct RideScript
	{
		VehicleInputs (*Inputs)(uint64_t step) = nullptr;
		int32_t (*Shift)(uint64_t step) = nullptr;
	};

	// The three-rate ride: the throttle opened and eased, a lean to the right and back to the left, both brakes, and in
	// the Manual gearbox shifts up at steps 20, 100 and 200 and down at 300.
	VehicleInputs ExitRideInputs(uint64_t step)
	{
		VehicleInputs inputs;
		inputs.Throttle = step < 150 ? 0.7f : 0.3f;
		inputs.Lean = step >= 90 && step < 210 ? 0.4f : (step >= 210 && step < 260 ? -0.3f : 0.0f);
		inputs.FrontBrake = step >= 240 && step < 320 ? 0.3f : 0.0f;
		inputs.RearBrake = step >= 270 && step < 330 ? 0.4f : 0.0f;
		return inputs;
	}

	int32_t ExitRideShift(uint64_t step)
	{
		return step == 20 || step == 100 || step == 200 ? 1 : (step == 300 ? -1 : 0);
	}

	// The snapshot's ride: at step 200 the bike leans, both brakes are on, the clutch closes after the shift at 160 and a
	// shift is requested for the next step.
	VehicleInputs SnapshotRideInputs(uint64_t step)
	{
		VehicleInputs inputs;
		inputs.Throttle = step < 120 ? 0.6f : 0.4f;
		inputs.Lean = step >= 60 && step < 210 ? 0.4f : (step >= 240 ? -0.2f : 0.0f);
		inputs.FrontBrake = step >= 150 && step < 220 ? 0.25f : 0.0f;
		inputs.RearBrake = step >= 170 && step < 230 ? 0.3f : 0.0f;
		return inputs;
	}

	int32_t SnapshotRideShift(uint64_t step)
	{
		return step == 30 || step == 160 || step == 200 ? 1 : (step == 260 ? -1 : 0);
	}

	// Sets a bike's inputs before each frame from the step count, as the ride's OnFixedUpdate would (the scripting members
	// are VH2's): the frame's steps take the inputs of its first, and each shift is requested once, before the frame whose
	// first step is its count.
	struct Rider
	{
		UUID Bike = UUID::Invalid();
		RideScript Script;
		std::optional<uint64_t> Shifted;

		void BeforeFrame(Scene& scene)
		{
			uint64_t const step = scene.GetFixedStepCount();
			PhysicsScene& world = *scene.GetPhysicsScene();
			world.SetVehicleInputs(Bike, Script.Inputs(step));
			int32_t const shift = Script.Shift(step);
			if (shift != 0 && Shifted != step)
			{
				world.RequestVehicleShift(Bike, shift);
				Shifted = step;
			}
		}
	};

	// A Manual bike whose lean spring settles within the step of the case (Docs/Architecture.md §8, "Vehicles").
	Vehicle MakeManualBike()
	{
		Vehicle values;
		values.Transmission.Mode = VehicleTransmissionMode::Manual;
		return values;
	}

	Entity MakeChild(Scene& scene, uint64_t id, std::string const& name, Entity parent)
	{
		Entity child = scene.CreateEntityWithUUID(UUID(id), name);
		REQUIRE(scene.SetParent(child, parent, false).IsOk());
		return child;
	}

	// The bike of the riding cases at the origin, its wheel entities below it (the front one below a fork that turns
	// nothing), with fixed UUIDs.
	Entity MakeRiddenBike(Scene& scene, AssetHandle definition, bool wheels, RigidBodyInterpolation interpolation)
	{
		Entity bike = MakeBike(scene, BikeId, 0.0f, definition);
		bike.GetComponent<RigidBodyComponent>().Interpolation = interpolation;
		if (wheels)
		{
			Entity fork = MakeChild(scene, 203, "Fork", bike);
			fork.GetComponent<TransformComponent>().Translation = {0.0f, 0.6f, -0.5f};
			MakeChild(scene, FrontWheelId, "Front Wheel", fork).AddComponent<VehicleWheelComponent>().Wheel = "Front";
			MakeChild(scene, RearWheelId, "Rear Wheel", bike).AddComponent<VehicleWheelComponent>().Wheel = "rear";
		}
		return bike;
	}

	// What a ride gave after each step it was read at: the scene's hash and the vehicle's state.
	struct RideLog
	{
		std::map<uint64_t, uint64_t> Hashes;
		std::map<uint64_t, VehicleState> States;
	};

	// Rides the three-rate ride for `steps` steps at frames of frameTime seconds, checking how many steps each frame runs.
	RideLog RideAtRate(float frameTime, uint64_t steps)
	{
		AssetHandle const definition = AddDataAsset(MakeManualBike(), "Bike " + std::to_string(frameTime) + ".svehicle");
		Scene scene("Ride");
		MakeGround(scene);
		MakeRiddenBike(scene, definition, true, RigidBodyInterpolation::Interpolate);
		scene.OnRuntimeStart(PhysicsOnly());
		REQUIRE(scene.GetPhysicsScene() != nullptr);
		REQUIRE(scene.GetPhysicsScene()->HasVehicle(UUID(BikeId)));
		Rider rider{UUID(BikeId), {ExitRideInputs, ExitRideShift}, std::nullopt};
		RideLog log;
		for (int frame = 0; frame < 100000 && scene.GetFixedStepCount() < steps; frame++)
		{
			uint64_t const before = scene.GetFixedStepCount();
			rider.BeforeFrame(scene);
			scene.OnUpdateRuntime(Timestep(frameTime));
			uint64_t const after = scene.GetFixedStepCount();
			// Every step takes the inputs its count gives: a frame of two steps starts at an even count, whose inputs both
			// steps share; a frame at 144 runs one step or none.
			if (frameTime == Frame30)
			{
				REQUIRE(before % 2 == 0);
				REQUIRE(after == before + 2);
			}
			else if (frameTime == Step)
			{
				REQUIRE(after == before + 1);
			}
			else
			{
				REQUIRE(after <= before + 1);
			}
			if (after != before)
			{
				log.Hashes[after] = scene.ComputePhysicsStateHash();
				log.States[after] = StateOf(scene, BikeId);
			}
		}
		REQUIRE(scene.GetFixedStepCount() == steps);
		scene.OnRuntimeStop();
		return log;
	}

	// Every reading of a vehicle, bit for bit.
	bool SameReading(VehicleState const& a, VehicleState const& b)
	{
		bool same = a.Entity == b.Entity && a.Inputs == b.Inputs && a.AppliedInputs == b.AppliedInputs && a.EngineSpeed == b.EngineSpeed &&
		            a.EngineTorque == b.EngineTorque && a.Gear == b.Gear && a.ClutchFriction == b.ClutchFriction &&
		            a.IsShifting == b.IsShifting && a.ForwardSpeed == b.ForwardSpeed && a.LeanAngle == b.LeanAngle &&
		            a.TargetLeanAngle == b.TargetLeanAngle;
		for (size_t index = 0; index < VehicleWheelCount; index++)
		{
			VehicleWheelState const& first = a.Wheels[index];
			VehicleWheelState const& second = b.Wheels[index];
			same = same && first.ContactEntity == second.ContactEntity && first.Surface == second.Surface &&
			       first.TyreGrip == second.TyreGrip && first.RollingResistance == second.RollingResistance &&
			       first.ContactPoint == second.ContactPoint && first.ContactNormal == second.ContactNormal &&
			       first.SuspensionLength == second.SuspensionLength && first.AngularVelocity == second.AngularVelocity &&
			       first.RotationAngle == second.RotationAngle && first.SteerAngle == second.SteerAngle &&
			       first.LongitudinalSlip == second.LongitudinalSlip && first.LateralSlip == second.LateralSlip;
		}
		return same;
	}

	TransformComponent TransformOf(Scene& scene, uint64_t id)
	{
		return scene.GetEntityByUUID(UUID(id)).GetComponent<TransformComponent>();
	}

	bool SamePose(TransformComponent const& a, TransformComponent const& b)
	{
		return a.Translation == b.Translation && a.Rotation == b.Rotation && a.Scale == b.Scale;
	}
}

TEST_CASE("ScenePhysics: a bike ridden by its step count gives the same state hashes at 30, 60 and 144 frames a second")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	// Throttle, lean and both brakes set at the fixed step by the step count, with Manual shifts, the wheels drawn between
	// steps: 360 steps (6 s) at each rate, every even step compared bit for bit.
	constexpr uint64_t Steps = 360;
	RideLog const at30 = RideAtRate(Frame30, Steps);
	RideLog const at60 = RideAtRate(Step, Steps);
	RideLog const at144 = RideAtRate(Frame144, Steps);
	size_t compared = 0;
	bool sameHashes = true;
	bool sameReadings = true;
	for (uint64_t step = 2; step <= Steps; step += 2)
	{
		CAPTURE(step);
		REQUIRE(at30.Hashes.contains(step));
		REQUIRE(at60.Hashes.contains(step));
		REQUIRE(at144.Hashes.contains(step));
		sameHashes = sameHashes && at30.Hashes.at(step) == at60.Hashes.at(step) && at60.Hashes.at(step) == at144.Hashes.at(step);
		sameReadings = sameReadings && SameReading(at30.States.at(step), at60.States.at(step)) &&
		               SameReading(at60.States.at(step), at144.States.at(step));
		compared++;
	}
	CHECK(compared == Steps / 2);
	CHECK(sameHashes);
	CHECK(sameReadings);
	// Each step applied the inputs of its count (the count before it).
	bool applied = true;
	for (auto const& [step, state] : at144.States)
	{
		applied = applied && state.AppliedInputs == ExitRideInputs(step - 1);
	}
	CHECK(applied);

	// It rode as the ride says, in bands (physics is deterministic per platform only; it rides at about 8.4 m/s at step 150,
	// leans 0.25 rad at 200 and -0.12 rad at 300, and slows from 11.4 m/s at 300 to 10.3 m/s at 360): moving at a few meters
	// a second once the throttle has been open for 2.5 s, leaning to its right while the lean input is right and to its left
	// after it, slower once both brakes have been on, and in the gear its shifts leave (up at 20, 100 and 200, down at 300).
	VehicleState const& opened = at60.States.at(150);
	VehicleState const& leaningRight = at60.States.at(200);
	VehicleState const& leaningLeft = at60.States.at(300);
	VehicleState const& braked = at60.States.at(Steps);
	CHECK(opened.ForwardSpeed > 5.0f);
	CHECK(opened.ForwardSpeed < 12.0f);
	CHECK(leaningRight.LeanAngle > 0.1f);
	CHECK(leaningRight.LeanAngle < 0.5f);
	CHECK(leaningLeft.LeanAngle < -0.03f);
	CHECK(braked.ForwardSpeed < leaningLeft.ForwardSpeed);
	CHECK(at60.States.at(150).Gear == 2);
	CHECK(at60.States.at(240).Gear == 3);
	CHECK(braked.Gear == 2);
}

TEST_CASE("ScenePhysics: a snapshot taken in the middle of a ride and restored reproduces every later step of the ride")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	AssetHandle const definition = AddDataAsset(MakeManualBike(), "Bike.svehicle");
	Scene scene("Snapshot");
	MakeGround(scene);
	MakeRiddenBike(scene, definition, true, RigidBodyInterpolation::Interpolate);
	scene.OnRuntimeStart(PhysicsOnly());
	REQUIRE(scene.GetPhysicsScene() != nullptr);
	Rider rider{UUID(BikeId), {SnapshotRideInputs, SnapshotRideShift}, std::nullopt};
	auto const frame = [&scene, &rider](bool drive)
	{
		if (drive)
		{
			rider.BeforeFrame(scene);
		}
		uint64_t const before = scene.GetFixedStepCount();
		scene.OnUpdateRuntime(Timestep(Frame144));
		REQUIRE(scene.GetFixedStepCount() <= before + 1);
	};
	// At 144 frames a second, so that the bike and its wheels are drawn between steps when it is saved.
	while (scene.GetFixedStepCount() < 200)
	{
		frame(true);
	}
	// Saved after this frame's inputs and the shift requested for step 200, before the frame runs: the latest inputs and the
	// pending shift are in it.
	rider.BeforeFrame(scene);
	Result<ScenePhysicsSnapshot> saved = scene.SavePhysicsState();
	REQUIRE(saved.IsOk());
	ScenePhysicsSnapshot const snapshot = saved.TakeValue();
	VehicleState const reading = StateOf(scene, BikeId);
	uint64_t const savedHash = scene.ComputePhysicsStateHash();
	TransformComponent const bikeDrawn = TransformOf(scene, BikeId);
	TransformComponent const frontDrawn = TransformOf(scene, FrontWheelId);
	TransformComponent const rearDrawn = TransformOf(scene, RearWheelId);
	// What the snapshot holds is in play: leaning, both brakes on, the clutch closing after a shift, a shift pending.
	CHECK(reading.AppliedInputs.FrontBrake > 0.0f);
	CHECK(reading.AppliedInputs.RearBrake > 0.0f);
	CHECK(reading.AppliedInputs.Lean > 0.0f);
	CHECK(reading.IsShifting);
	CHECK(reading.ClutchFriction > 0.0f);
	CHECK(reading.ClutchFriction < 1.0f);
	CHECK(std::abs(reading.LeanAngle) > 0.02f);

	// The original ride on to step 320: each step's hash.
	std::map<uint64_t, uint64_t> original;
	bool first = true;
	while (scene.GetFixedStepCount() < 320)
	{
		frame(!first);
		first = false;
		original[scene.GetFixedStepCount()] = scene.ComputePhysicsStateHash();
	}

	// Back to step 200: what a script reads right after the restore is what it read when the snapshot was taken, and the
	// bike and its wheels are drawn where they were.
	Result<void> const restored = scene.RestorePhysicsState(snapshot);
	REQUIRE_MESSAGE(restored.IsOk(), (restored ? std::string() : restored.GetError()));
	CHECK(scene.GetFixedStepCount() == 200);
	CHECK(SameReading(StateOf(scene, BikeId), reading));
	CHECK(scene.ComputePhysicsStateHash() == savedHash);
	CHECK(SamePose(TransformOf(scene, BikeId), bikeDrawn));
	CHECK(SamePose(TransformOf(scene, FrontWheelId), frontDrawn));
	CHECK(SamePose(TransformOf(scene, RearWheelId), rearDrawn));

	// The restored inputs and pending shift drive the steps until the count moves on; from there the ride's own.
	rider.Shifted = 200;
	size_t replayed = 0;
	bool same = true;
	while (scene.GetFixedStepCount() < 320)
	{
		frame(scene.GetFixedStepCount() > 200);
		uint64_t const step = scene.GetFixedStepCount();
		REQUIRE(original.contains(step));
		same = same && scene.ComputePhysicsStateHash() == original.at(step);
		replayed++;
	}
	CHECK(same);
	CHECK(replayed >= 120);
	scene.OnRuntimeStop();
}

TEST_CASE("ScenePhysics: a snapshot is refused, changing nothing, once a vehicle was made, made again or removed; an edit of its "
          "values keeps it")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	Vehicle values = MakeManualBike();
	AssetHandle const definition = AddDataAsset(values, "Bike.svehicle");
	Scene scene("Refusals");
	MakeGround(scene);
	MakeRiddenBike(scene, definition, true, RigidBodyInterpolation::Interpolate);
	// A second chassis without a Vehicle yet.
	Entity other = scene.CreateEntityWithUUID(UUID(300), "Other");
	other.GetComponent<TransformComponent>().Translation = {5.0f, 0.05f, 0.0f};
	RigidBodyComponent& body = other.AddComponent<RigidBodyComponent>();
	body.Type = RigidBodyType::Dynamic;
	body.Mass = 250.0f;
	BoxColliderComponent& collider = other.AddComponent<BoxColliderComponent>();
	collider.HalfExtents = {0.25f, 0.5f, 0.6f};
	collider.Offset = {0.0f, 0.75f, 0.0f};
	scene.OnRuntimeStart(PhysicsOnly());
	PhysicsScene& world = *scene.GetPhysicsScene();
	world.SetVehicleInputs(UUID(BikeId), MakeInputs(0.4f, 0.0f, 0.0f, 0.2f));
	Run(scene, 30);

	auto const refused = [&scene](ScenePhysicsSnapshot const& snapshot)
	{
		uint64_t const hash = scene.ComputePhysicsStateHash();
		uint64_t const step = scene.GetFixedStepCount();
		TransformComponent const wheel = TransformOf(scene, FrontWheelId);
		Result<void> const result = scene.RestorePhysicsState(snapshot);
		CHECK_FALSE(result.IsOk());
		CHECK(scene.ComputePhysicsStateHash() == hash);
		CHECK(scene.GetFixedStepCount() == step);
		CHECK(SamePose(TransformOf(scene, FrontWheelId), wheel));
		return result ? std::string() : result.GetError();
	};

	// A vehicle made: the older snapshot is of another structure.
	Result<ScenePhysicsSnapshot> beforeMade = scene.SavePhysicsState();
	REQUIRE(beforeMade.IsOk());
	uint64_t structure = world.GetStructureVersion();
	other.AddComponent<VehicleComponent>().Definition = definition;
	scene.ApplyPhysicsChanges();
	REQUIRE(world.HasVehicle(UUID(300)));
	CHECK(world.GetStructureVersion() != structure);
	CHECK(refused(beforeMade.GetValue()) == "bodies, characters or vehicles were added or removed since the snapshot");
	Run(scene, 10);

	// Removed.
	Result<ScenePhysicsSnapshot> beforeRemoved = scene.SavePhysicsState();
	REQUIRE(beforeRemoved.IsOk());
	structure = world.GetStructureVersion();
	other.RemoveComponent<VehicleComponent>();
	scene.ApplyPhysicsChanges();
	REQUIRE_FALSE(world.HasVehicle(UUID(300)));
	CHECK(world.GetStructureVersion() != structure);
	CHECK_FALSE(refused(beforeRemoved.GetValue()).empty());
	Run(scene, 10);

	// Its values edited in place: the snapshot still restores, and the steps run on from it.
	Result<ScenePhysicsSnapshot> beforeEdit = scene.SavePhysicsState();
	REQUIRE(beforeEdit.IsOk());
	uint64_t const editedFrom = scene.GetFixedStepCount();
	structure = world.GetStructureVersion();
	values.Wheels[0].FrontBrakeTorque = 300.0f;
	Edit(definition, values);
	Run(scene, 5);
	CHECK(world.GetStructureVersion() == structure);
	Result<void> const kept = scene.RestorePhysicsState(beforeEdit.GetValue());
	CHECK_MESSAGE(kept.IsOk(), (kept ? std::string() : kept.GetError()));
	CHECK(scene.GetFixedStepCount() == editedFrom);

	// Made again (its chassis's axes edited, or its collider replaced): refused.
	Result<ScenePhysicsSnapshot> beforeAxes = scene.SavePhysicsState();
	REQUIRE(beforeAxes.IsOk());
	values.Forward = glm::vec3(0.0f, 0.01f, -1.0f);
	Edit(definition, values);
	Run(scene, 1);
	CHECK_FALSE(refused(beforeAxes.GetValue()).empty());
	Result<ScenePhysicsSnapshot> beforeCollider = scene.SavePhysicsState();
	REQUIRE(beforeCollider.IsOk());
	Entity bike = scene.GetEntityByUUID(UUID(BikeId));
	BoxColliderComponent wider = bike.GetComponent<BoxColliderComponent>();
	wider.HalfExtents.x = 0.3f;
	bike.AddOrReplaceComponent<BoxColliderComponent>(wider);
	scene.ApplyPhysicsChanges();
	CHECK_FALSE(refused(beforeCollider.GetValue()).empty());
	scene.OnRuntimeStop();
}

TEST_CASE("ScenePhysics: two bikes ride alike whatever the order their entities were made in")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	AssetHandle const definition = AddDataAsset(MakeManualBike(), "Bike.svehicle");
	auto const ride = [definition](bool reversed)
	{
		Scene scene("Two Bikes");
		std::vector<uint64_t> const order = reversed ? std::vector<uint64_t>{300, 200} : std::vector<uint64_t>{200, 300};
		for (uint64_t const id : order)
		{
			MakeBike(scene, id, id == 200 ? 0.0f : 4.0f, definition);
		}
		MakeGround(scene);
		scene.OnRuntimeStart(PhysicsOnly());
		REQUIRE(scene.GetPhysicsScene() != nullptr);
		std::array<Rider, 2> riders = {Rider{UUID(200), {ExitRideInputs, ExitRideShift}, std::nullopt},
		                               Rider{UUID(300), {SnapshotRideInputs, SnapshotRideShift}, std::nullopt}};
		std::vector<uint64_t> hashes;
		for (int frame = 0; frame < 240; frame++)
		{
			for (Rider& rider : riders)
			{
				rider.BeforeFrame(scene);
			}
			scene.OnUpdateRuntime(Timestep(Step));
			hashes.push_back(scene.ComputePhysicsStateHash());
		}
		scene.OnRuntimeStop();
		return hashes;
	};
	std::vector<uint64_t> const first = ride(false);
	std::vector<uint64_t> const second = ride(true);
	REQUIRE(first.size() == 240);
	CHECK(first == second);
}

namespace
{
	// A definition's wheel as the scene draws it: its attachment, and its directions normalized.
	VehicleWheelDesc MakeWheelGeometry(VehicleWheelDefinition const& wheel)
	{
		VehicleWheelDesc geometry;
		geometry.Position = wheel.Position;
		geometry.SuspensionDirection = glm::normalize(wheel.SuspensionDirection);
		geometry.SteeringAxis = glm::normalize(wheel.SteeringAxis);
		return geometry;
	}

	glm::mat4 WithoutScale(glm::mat4 const& world)
	{
		glm::vec3 translation;
		glm::quat rotation;
		glm::vec3 scale;
		REQUIRE(Math::DecomposeTransform(world, translation, rotation, scale));
		return Math::ComposeTransform(translation, rotation, glm::vec3(1.0f));
	}

	// The angle a wheel that does not steer has turned around its axle, from its transform in its chassis's space: the wheel
	// model's up (its Y) is (0, cos, -sin) of the angle for a chassis whose up is +Y and whose front is -Z
	// (ComposeVehicleWheelTransform).
	float GetAxleAngle(glm::mat4 const& inChassis)
	{
		return std::atan2(-inChassis[1].z, inChassis[1].y);
	}

	// An angle within (-pi, pi].
	float WrapAngle(float angle)
	{
		float const turn = glm::two_pi<float>();
		return angle - turn * std::round(angle / turn);
	}

	bool IsNear(glm::vec3 const& a, glm::vec3 const& b, float tolerance)
	{
		return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(tolerance)));
	}
}

TEST_CASE("ScenePhysics: a bike's wheels are drawn at their transforms between their last two steps, turning forward however far a "
          "step turns them, and the hash never sees them")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	// Steps of a twentieth of a second, so that a wheel rolling at 25 m/s turns more than half a turn in each; a lean spring
	// that settles in such a step.
	constexpr float FixedStep = 0.05f;
	Vehicle values;
	values.Lean.SpringDamping = 300.0f;
	AssetHandle const definition = AddDataAsset(values, "Bike.svehicle");
	SceneRuntimeSettings settings = PhysicsOnly();
	settings.FixedTimestep = FixedStep;
	auto const build = [definition](Scene& scene, bool wheels)
	{
		MakeGround(scene);
		Entity bike = MakeRiddenBike(scene, definition, wheels, RigidBodyInterpolation::Interpolate);
		bike.GetComponent<RigidBodyComponent>().InitialLinearVelocity = {0.0f, 0.0f, -25.0f};
	};
	Scene scene("Wheels");
	build(scene, true);
	// The same scene without the wheels' entities.
	Scene bare("No Wheels");
	build(bare, false);
	scene.OnRuntimeStart(settings);
	bare.OnRuntimeStart(settings);
	REQUIRE(scene.GetPhysicsScene() != nullptr);
	REQUIRE(bare.GetPhysicsScene() != nullptr);
	scene.GetPhysicsScene()->SetVehicleInputs(UUID(BikeId), MakeInputs(0.0f, 0.0f, 0.0f, 0.15f));
	bare.GetPhysicsScene()->SetVehicleInputs(UUID(BikeId), MakeInputs(0.0f, 0.0f, 0.0f, 0.15f));

	std::array<VehicleWheelDesc, VehicleWheelCount> const geometry = {MakeWheelGeometry(values.Wheels[0]),
	                                                                  MakeWheelGeometry(values.Wheels[1])};
	glm::vec3 up = values.Up;
	glm::vec3 forward = values.Forward;
	REQUIRE(NormalizeVehicleAxes(up, forward));
	// The wheels as each step left them, and the scene's accumulator as it keeps it, for the part of a step drawn.
	std::vector<VehicleState> steps;
	float accumulator = 0.0f;
	bool sameHashes = true;
	bool onTheirTransforms = true;
	bool forwardOnly = true;
	float largestAdvance = 0.0f;
	float turned = 0.0f;
	float rolled = 0.0f;
	std::optional<float> lastAngle;
	int drawnFrames = 0;
	for (int frame = 0; frame < 432; frame++)
	{
		uint64_t const before = scene.GetFixedStepCount();
		scene.OnUpdateRuntime(Timestep(Frame144));
		bare.OnUpdateRuntime(Timestep(Frame144));
		uint64_t const after = scene.GetFixedStepCount();
		REQUIRE(after <= before + 1);
		accumulator += Frame144;
		while (accumulator >= FixedStep * (1.0f - 1e-4f))
		{
			accumulator = std::max(accumulator - FixedStep, 0.0f);
		}
		if (after != before)
		{
			steps.push_back(StateOf(scene, BikeId));
		}
		sameHashes = sameHashes && scene.ComputePhysicsStateHash() == bare.ComputePhysicsStateHash();
		if (steps.size() < 2)
		{
			continue;
		}
		drawnFrames++;
		float const alpha = std::clamp(accumulator / FixedStep, 0.0f, 1.0f);
		glm::mat4 const chassis = WithoutScale(scene.GetWorldTransform(scene.GetEntityByUUID(UUID(BikeId))));
		VehicleState const& from = steps[steps.size() - 2];
		VehicleState const& to = steps.back();
		for (size_t index = 0; index < VehicleWheelCount; index++)
		{
			uint64_t const id = index == 0 ? FrontWheelId : RearWheelId;
			glm::mat4 const inChassis = glm::inverse(chassis) * scene.GetWorldTransform(scene.GetEntityByUUID(UUID(id)));
			// Where the suspension and the steering put it, the part of the way from the step before to the last one the
			// frame draws: its centre and its axle.
			VehicleWheelState const& start = from.Wheels[index];
			VehicleWheelState const& end = to.Wheels[index];
			glm::mat4 const expected =
				ComposeVehicleWheelTransform(geometry[index], up, forward, glm::mix(start.SuspensionLength, end.SuspensionLength, alpha),
			                                 glm::mix(start.SteerAngle, end.SteerAngle, alpha), 0.0f);
			onTheirTransforms = onTheirTransforms && IsNear(glm::vec3(inChassis[3]), glm::vec3(expected[3]), 1e-4f) &&
			                    IsNear(glm::vec3(inChassis[0]), glm::vec3(expected[0]), 1e-4f);
		}
		// The rear wheel, which does not steer, turns the way it spins in every frame, by what its spin makes up.
		VehicleWheelState const& rear = to.Wheels[1];
		largestAdvance = std::max(largestAdvance, std::abs(rear.AngularVelocity) * FixedStep);
		float const angle = GetAxleAngle(glm::inverse(chassis) * scene.GetWorldTransform(scene.GetEntityByUUID(UUID(RearWheelId))));
		if (lastAngle)
		{
			float const delta = WrapAngle(angle - *lastAngle);
			forwardOnly = forwardOnly && (rear.AngularVelocity <= 1.0f || delta > 0.0f);
			turned += delta;
			rolled += rear.AngularVelocity * Frame144;
		}
		lastAngle = angle;
	}
	REQUIRE(drawnFrames > 400);
	// The premise: a step turns the rear wheel more than half a turn.
	REQUIRE(largestAdvance > glm::pi<float>());
	CHECK(sameHashes);
	CHECK(onTheirTransforms);
	CHECK(forwardOnly);
	// As far as its spin turned it over the frames, within a step's change of spin.
	CHECK(turned == doctest::Approx(rolled).epsilon(0.05));
	scene.OnRuntimeStop();
	bare.OnRuntimeStop();
}

TEST_CASE("ScenePhysics: OnFixedUpdate finds a bike's wheels exactly where their last step left them, however they were drawn")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	Testing::ScriptEngineScope const scripting;
	AssetHandle const definition = AddDataAsset(Vehicle{}, "Bike.svehicle");
	// A bike drawn between steps, whose front wheel's script notes where it is in each fixed update, beside the same bike
	// drawn at its steps.
	Scene drawn("Drawn");
	MakeGround(drawn);
	MakeRiddenBike(drawn, definition, true, RigidBodyInterpolation::Interpolate);
	Entity probe = drawn.GetEntityByUUID(UUID(FrontWheelId));
	probe.AddComponent<ScriptComponent>().ClassName = "Strada.Tests.FixedStepMover";
	Scene stepped("Stepped");
	MakeGround(stepped);
	MakeRiddenBike(stepped, definition, true, RigidBodyInterpolation::None);
	SceneRuntimeSettings withScripts;
	withScripts.PlayAudio = false;
	drawn.OnRuntimeStart(withScripts);
	stepped.OnRuntimeStart(PhysicsOnly());
	REQUIRE(drawn.GetPhysicsScene() != nullptr);
	REQUIRE(stepped.GetPhysicsScene() != nullptr);
	for (Scene* scene : {&drawn, &stepped})
	{
		scene->GetPhysicsScene()->SetVehicleInputs(UUID(BikeId), MakeInputs(0.5f, 0.0f, 0.0f, -0.3f));
	}

	// The stepped bike's front wheel after each frame is where the last step left it.
	glm::vec3 stepPose = TransformOf(stepped, FrontWheelId).Translation;
	bool readTheSteps = true;
	int checkedReads = 0;
	int drawnApart = 0;
	for (int frame = 0; frame < 288; frame++)
	{
		uint64_t const before = drawn.GetFixedStepCount();
		drawn.OnUpdateRuntime(Timestep(Frame144));
		stepped.OnUpdateRuntime(Timestep(Frame144));
		uint64_t const after = drawn.GetFixedStepCount();
		REQUIRE(stepped.GetFixedStepCount() == after);
		REQUIRE(after <= before + 1);
		if (after == before + 1)
		{
			Result<ScriptFieldMap> const fields = drawn.ReadScriptFields(probe);
			REQUIRE(fields.IsOk());
			readTheSteps = readTheSteps && fields.GetValue().at("FixedRead").GetVector3() == stepPose;
			checkedReads++;
		}
		CHECK(drawn.ComputePhysicsStateHash() == stepped.ComputePhysicsStateHash());
		glm::vec3 const now = TransformOf(stepped, FrontWheelId).Translation;
		drawnApart += TransformOf(drawn, FrontWheelId).Translation != now ? 1 : 0;
		stepPose = now;
	}
	CHECK(readTheSteps);
	CHECK(checkedReads >= 119);
	// The drawn wheel was between its steps in most frames.
	CHECK(drawnApart > 100);
	drawn.OnRuntimeStop();
	stepped.OnRuntimeStop();
}

TEST_CASE("ScenePhysics: a VehicleWheel draws the wheel it names, in any letter case, of the nearest Vehicle above it, keeping its "
          "own scale, and says once why it draws none")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	AssetHandle const definition = AddDataAsset(Vehicle{}, "Bike.svehicle");
	Scene scene("Wheel Names");
	MakeGround(scene);
	// Drawn at its steps, so that each frame shows the wheels where Jolt's wheel transforms put them.
	Entity bike = MakeRiddenBike(scene, definition, true, RigidBodyInterpolation::None);
	// The fork turned and scaled; the front wheel below it named in capitals, with a scale of its own.
	Entity forkEntity = scene.GetEntityByUUID(UUID(203));
	TransformComponent& fork = forkEntity.GetComponent<TransformComponent>();
	fork.Rotation = glm::angleAxis(0.3f, glm::vec3(0.0f, 1.0f, 0.0f));
	fork.Scale = glm::vec3(2.0f);
	Entity front = scene.GetEntityByUUID(UUID(FrontWheelId));
	front.GetComponent<VehicleWheelComponent>().Wheel = "FRONT";
	front.GetComponent<TransformComponent>().Scale = glm::vec3(1.5f);
	// No Vehicle above; a name the definition does not have; a collider of its own (a trigger, which carries nothing).
	Entity lost = scene.CreateEntity("Lost Wheel");
	lost.GetComponent<TransformComponent>().Translation = {9.0f, 1.0f, 0.0f};
	lost.AddComponent<VehicleWheelComponent>().Wheel = "Front";
	Entity odd = scene.CreateEntity("Odd Wheel", bike);
	odd.AddComponent<VehicleWheelComponent>().Wheel = "Middle";
	Entity solid = scene.CreateEntity("Solid Wheel", bike);
	solid.GetComponent<TransformComponent>().Translation = {0.0f, 0.3f, 0.7f};
	solid.AddComponent<VehicleWheelComponent>().Wheel = "Rear";
	BoxColliderComponent& trigger = solid.AddComponent<BoxColliderComponent>();
	trigger.HalfExtents = glm::vec3(0.1f);
	trigger.IsTrigger = true;
	TransformComponent const lostStart = lost.GetComponent<TransformComponent>();
	TransformComponent const oddStart = odd.GetComponent<TransformComponent>();
	TransformComponent const solidStart = solid.GetComponent<TransformComponent>();

	uint64_t const logStart = Log::GetNextEntryIndex();
	scene.OnRuntimeStart(PhysicsOnly());
	PhysicsScene& world = *scene.GetPhysicsScene();
	world.SetVehicleInputs(UUID(BikeId), MakeInputs(0.4f, 0.0f, 0.0f, 0.3f));
	bool placed = true;
	for (int frame = 0; frame < 90; frame++)
	{
		Run(scene, 1);
		glm::mat4 const chassis = WithoutScale(scene.GetWorldTransform(bike));
		for (size_t index = 0; index < VehicleWheelCount; index++)
		{
			std::optional<glm::mat4> const wheel = world.GetVehicleWheelTransform(UUID(BikeId), index);
			REQUIRE(wheel);
			glm::mat4 const expected = chassis * *wheel;
			glm::mat4 const shown =
				WithoutScale(scene.GetWorldTransform(scene.GetEntityByUUID(UUID(index == 0 ? FrontWheelId : RearWheelId))));
			for (int column = 0; column < 4; column++)
			{
				placed = placed && IsNear(glm::vec3(shown[column]), glm::vec3(expected[column]), 2e-4f);
			}
		}
	}
	CHECK(placed);
	// Its own scale stays, under the fork's.
	CHECK(front.GetComponent<TransformComponent>().Scale == glm::vec3(1.5f));
	glm::vec3 translation;
	glm::quat rotation;
	glm::vec3 scale;
	REQUIRE(Math::DecomposeTransform(scene.GetWorldTransform(front), translation, rotation, scale));
	CHECK(IsNear(scale, glm::vec3(3.0f), 1e-4f));
	// The others are left as they were, each reason said once.
	CHECK(SamePose(lost.GetComponent<TransformComponent>(), lostStart));
	CHECK(SamePose(odd.GetComponent<TransformComponent>(), oddStart));
	CHECK(SamePose(solid.GetComponent<TransformComponent>(), solidStart));
	CHECK(CountLogged(logStart, {"Physics: the VehicleWheel of 'Lost Wheel' draws no wheel: no entity above it has a Vehicle"}) == 1);
	CHECK(CountLogged(logStart, {"Physics: the VehicleWheel of 'Odd Wheel' draws no wheel: the definition of the Vehicle of 'Bike 200' "
	                             "has no wheel named 'Middle'"}) == 1);
	CHECK(CountLogged(logStart, {"Physics: the VehicleWheel of 'Solid Wheel' draws no wheel: its entity has a body or a character"}) == 1);
	CHECK(CountLogged(logStart, {"'Front Wheel'"}) == 0);
	CHECK(CountLogged(logStart, {"'Rear Wheel'"}) == 0);

	// A VehicleWheel removed and added again is a new one: its reason is said again, once. Destroyed, nothing more is said of
	// it.
	uint64_t const readded = Log::GetNextEntryIndex();
	lost.RemoveComponent<VehicleWheelComponent>();
	Run(scene, 2);
	CHECK(CountLogged(readded, {"'Lost Wheel'"}) == 0);
	lost.AddComponent<VehicleWheelComponent>().Wheel = "Front";
	Run(scene, 5);
	CHECK(CountLogged(readded, {"Physics: the VehicleWheel of 'Lost Wheel' draws no wheel: no entity above it has a Vehicle"}) == 1);
	scene.DestroyEntity(lost);
	Run(scene, 2);
	CHECK(CountLogged(readded, {"'Lost Wheel'"}) == 1);

	// Named again for a wheel it has, it is drawn from the next frame on.
	odd.GetComponent<VehicleWheelComponent>().Wheel = "front";
	Run(scene, 2);
	CHECK_FALSE(SamePose(odd.GetComponent<TransformComponent>(), oddStart));
	// Named again for one it lacks, the new name is said once.
	uint64_t const renamed = Log::GetNextEntryIndex();
	odd.GetComponent<VehicleWheelComponent>().Wheel = "Side";
	Run(scene, 5);
	CHECK(CountLogged(renamed, {"Physics: the VehicleWheel of 'Odd Wheel' draws no wheel: the definition of the Vehicle of 'Bike 200' "
	                            "has no wheel named 'Side'"}) == 1);
	scene.OnRuntimeStop();
}

TEST_CASE("ScenePhysics: vehicle wheel benchmark" * doctest::skip(!Platform::ReadEnvironmentVariable("STRADA_VEHICLE_BENCHMARK")))
{
	// A measurement, never asserted on timings: run with STRADA_VEHICLE_BENCHMARK=1 in a Release build. Eight bikes riding
	// at 144 frames a second, with and without the entities of their wheels: what drawing the wheels costs, in the zones
	// that record (Scene.SyncFromPhysics, once a step, read per call), draw (Scene.Interpolate) and return
	// (Scene.ReturnToSteps) them (once a frame), a vehicle a call.
	constexpr int Bikes = 8;
	constexpr int WarmUpFrames = 144;
	constexpr int Frames = 720;
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope const physics;
	Testing::ProfilerScope const profiler;
	AssetHandle const definition = AddDataAsset(Vehicle{}, "Bike.svehicle");
	std::array<std::string_view, 3> const zones = {"Scene.SyncFromPhysics", "Scene.Interpolate", "Scene.ReturnToSteps"};
	// Twice: a round that reads slower than the one before it tells a machine that changed its clocks, not a cost.
	for (int round = 1; round <= 2; round++)
	{
		std::array<std::array<double, 3>, 2> perCall{};
		for (size_t withWheels = 0; withWheels < 2; withWheels++)
		{
			Scene scene("Benchmark");
			MakeGround(scene);
			for (int bike = 0; bike < Bikes; bike++)
			{
				uint64_t const id = 1000 + static_cast<uint64_t>(bike) * 10;
				Entity chassis = MakeBike(scene, id, static_cast<float>(bike) * 4.0f, definition);
				if (withWheels == 1)
				{
					MakeChild(scene, id + 1, "Front Wheel", chassis).AddComponent<VehicleWheelComponent>().Wheel = "Front";
					MakeChild(scene, id + 2, "Rear Wheel", chassis).AddComponent<VehicleWheelComponent>().Wheel = "Rear";
				}
			}
			scene.OnRuntimeStart(PhysicsOnly());
			REQUIRE(scene.GetPhysicsScene() != nullptr);
			for (int bike = 0; bike < Bikes; bike++)
			{
				scene.GetPhysicsScene()->SetVehicleInputs(UUID(1000 + static_cast<uint64_t>(bike) * 10),
				                                          MakeInputs(0.4f, 0.0f, 0.0f, bike % 2 == 0 ? 0.2f : -0.2f));
			}
			Run(scene, WarmUpFrames, Frame144);
			REQUIRE(Profiler::Start().IsOk());
			for (int frame = 0; frame < Frames; frame++)
			{
				Profiler::BeginFrame();
				scene.OnUpdateRuntime(Timestep(Frame144));
				Profiler::EndFrame();
			}
			Profiler::Stop();
			ProfilerMedians const medians = ComputeProfilerMedians(Profiler::GetRecording());
			for (ProfilerZoneMedian const& zone : medians.Zones)
			{
				for (size_t index = 0; index < zones.size(); index++)
				{
					if (zone.Name == zones[index])
					{
						perCall[withWheels][index] = zone.MillisecondsPerCall * 1000.0;
					}
				}
			}
			scene.OnRuntimeStop();
		}
		for (size_t index = 0; index < zones.size(); index++)
		{
			MESSAGE("Round " << round << ": " << zones[index] << " " << perCall[0][index] << " us a call without the wheels' entities, "
			                 << perCall[1][index] << " us with them: " << (perCall[1][index] - perCall[0][index]) / Bikes
			                 << " us a vehicle a call");
		}
	}
}
