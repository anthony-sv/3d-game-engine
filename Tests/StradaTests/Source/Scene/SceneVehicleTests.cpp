#include "Physics/PhysicsTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/CurveAsset.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/MeshFactory.h"
#include "Strada/Asset/Vehicle.h"
#include "Strada/Asset/VehicleAsset.h"
#include "Strada/Core/Log.h"
#include "Strada/Math/Curve.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
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

	// The chassis's axes are taken when the vehicle is made: an edit of them makes it again (the structure moves), keeping
	// the rider's inputs.
	values.Forward = glm::vec3(0.0f, 0.02f, -1.0f);
	Edit(definition, values);
	Run(scene, 1);
	CHECK(world.GetStructureVersion() != structure);
	CHECK(world.HasVehicle(UUID(200)));
	CHECK(StateOf(scene, 200).Inputs.FrontBrake == 1.0f);

	// A definition that stops loading leaves the entity without a vehicle until it loads again.
	AssetManager::RemoveMemoryAsset(definition);
	Run(scene, 1);
	CHECK_FALSE(world.HasVehicle(UUID(200)));
	CHECK(CountLogged(logStart, {"'Bike 200' makes no vehicle: its Definition", "is no Vehicle that loads"}) == 1);
	scene.OnRuntimeStop();
}
