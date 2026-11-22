#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/Vehicle.h"
#include "Strada/Asset/VehicleAsset.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Core/UUID.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Strada;

// Scripts reset vehicles (Docs/Architecture.md §8, "The reset"): Tests/TestScripts' VehicleResetRider rides its bike from
// OnFixedUpdate and at its 120th fixed step teleports it back to where it started and resets it.
namespace
{
	constexpr uint64_t GroundId = 600;
	constexpr uint64_t BikeId = 610;
	constexpr uint64_t Steps = 240;
	constexpr char const* Rider = "Strada.Tests.VehicleResetRider";

	AssetHandle AddManualBike()
	{
		Vehicle bike;
		bike.Transmission.Mode = VehicleTransmissionMode::Manual;
		bike.Transmission.SwitchTime = 0.1f;
		bike.Transmission.ClutchReleaseTime = 0.1f;
		bike.Lean.SpringDamping = 800.0f;
		TypedDataAssetType<Vehicle> const* type = DataAssetRegistry::Find<Vehicle>();
		REQUIRE(type != nullptr);
		Ref<TypedDataAsset<Vehicle>> const asset = std::static_pointer_cast<TypedDataAsset<Vehicle>>(type->CreateAsset());
		REQUIRE(asset->SetData(bike).IsOk());
		return AssetManager::AddMemoryAsset(asset, "Reset Bike.svehicle");
	}

	// A ground and a bike on it: a dynamic 250 kg chassis box whose roll inertia carries the lean spring's damping, its
	// Vehicle on the definition and the rider on it, named as given.
	Entity MakeRide(Scene& scene, AssetHandle definition, std::string const& name)
	{
		Entity ground = scene.CreateEntityWithUUID(UUID(GroundId), "Ground");
		ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
		ground.AddComponent<BoxColliderComponent>().HalfExtents = {200.0f, 0.5f, 200.0f};
		Entity bike = scene.CreateEntityWithUUID(UUID(BikeId), name);
		bike.GetComponent<TransformComponent>().Translation = {0.0f, 0.05f, 0.0f};
		RigidBodyComponent& body = bike.AddComponent<RigidBodyComponent>();
		body.Type = RigidBodyType::Dynamic;
		body.Mass = 250.0f;
		body.LinearDamping = 0.0f;
		body.AllowSleep = false;
		BoxColliderComponent& collider = bike.AddComponent<BoxColliderComponent>();
		collider.HalfExtents = {0.25f, 0.5f, 0.6f};
		collider.Offset = {0.0f, 0.75f, 0.0f};
		bike.AddComponent<VehicleComponent>().Definition = definition;
		bike.AddComponent<ScriptComponent>().ClassName = Rider;
		return bike;
	}

	// The "key=value;" pairs of the rider's report.
	std::map<std::string, std::string> ParseReport(std::string const& name)
	{
		std::map<std::string, std::string> report;
		size_t start = 0;
		while (start < name.size())
		{
			size_t const end = name.find(';', start);
			std::string const pair = name.substr(start, end == std::string::npos ? std::string::npos : end - start);
			size_t const equals = pair.find('=');
			if (equals != std::string::npos)
			{
				report[pair.substr(0, equals)] = pair.substr(equals + 1);
			}
			if (end == std::string::npos)
			{
				break;
			}
			start = end + 1;
		}
		return report;
	}

	struct RideResult
	{
		// The hash after every step a frame ended on, by step count.
		std::map<uint64_t, uint64_t> Hashes;
		std::map<std::string, std::string> Report;
		glm::vec3 PlaceAfterReset = glm::vec3(0.0f);
		int GearAtEnd = -1;
	};

	RideResult Ride(AssetHandle definition, float frameTime, std::string const& name)
	{
		Scene scene("Reset Ride");
		Entity bike = MakeRide(scene, definition, name);
		scene.OnRuntimeStart();
		REQUIRE(scene.GetPhysicsScene()->HasVehicle(bike.GetUUID()));
		RideResult result;
		bool placed = false;
		for (int frame = 0; frame < 100000 && scene.GetFixedStepCount() < Steps; frame++)
		{
			scene.OnUpdateRuntime(Timestep(frameTime));
			result.Hashes[scene.GetFixedStepCount()] = scene.ComputePhysicsStateHash();
			// Where the first frame to end after the reset's step finds it (the step after it, or the one after that at 30
			// frames a second).
			if (scene.GetFixedStepCount() > 120 && !placed)
			{
				std::optional<BodyTransform> const body = scene.GetPhysicsScene()->GetBodyTransform(bike.GetUUID());
				REQUIRE(body);
				result.PlaceAfterReset = body->Position;
				placed = true;
			}
		}
		REQUIRE(scene.GetFixedStepCount() == Steps);
		// The rider names its entity once, at the reset.
		result.Report = ParseReport(bike.GetName());
		std::optional<VehicleState> const state = scene.GetPhysicsScene()->GetVehicleState(bike.GetUUID());
		REQUIRE(state);
		result.GearAtEnd = state->Gear;
		scene.OnRuntimeStop();
		return result;
	}

	float Number(std::map<std::string, std::string> const& report, std::string const& key)
	{
		auto const found = report.find(key);
		REQUIRE_MESSAGE(found != report.end(), key);
		return std::strtof(found->second.c_str(), nullptr);
	}

	// What the rider read right after its reset: a vehicle as it was made, at rest.
	void CheckReport(std::map<std::string, std::string> const& report)
	{
		CHECK(Number(report, "Reset") == 120.0f);
		CHECK(Number(report, "Gear") == 0.0f);
		CHECK(Number(report, "Engine") == Vehicle{}.Engine.MinRPM);
		CHECK(Number(report, "Speed") == 0.0f);
		CHECK(Number(report, "Throttle") == 0.0f);
		CHECK(Number(report, "RearSpin") == 0.0f);
		CHECK(report.at("RearContact") == "False");
	}
}

TEST_CASE("Scene: a bike a script resets in the middle of its ride from OnFixedUpdate gives the same state hashes at 30, 60 and 144 "
          "frames a second, and is put back where the script took it, as new")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting;
	AssetHandle const definition = AddManualBike();
	std::map<float, RideResult> rides;
	for (float const frameTime : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f})
	{
		CAPTURE(frameTime);
		RideResult ride = Ride(definition, frameTime, "Bike");
		CheckReport(ride.Report);
		// Taken back to where it started (its rigid body's teleport), at rest: the step after the reset leaves it there.
		CHECK(glm::length(ride.PlaceAfterReset - glm::vec3(0.0f, 0.05f, 0.0f)) < 0.05f);
		// The shift asked before the reset was dropped; the one at the next step took first gear.
		CHECK(ride.GearAtEnd == 1);
		rides.emplace(frameTime, std::move(ride));
	}
	// At 30 frames a second every other step ends a frame; every step it exposes has the same hash at the other two rates.
	size_t compared = 0;
	for (auto const& [step, hash] : rides.at(1.0f / 30.0f).Hashes)
	{
		CAPTURE(step);
		CHECK(rides.at(1.0f / 60.0f).Hashes.at(step) == hash);
		CHECK(rides.at(1.0f / 144.0f).Hashes.at(step) == hash);
		compared++;
	}
	CHECK(compared == Steps / 2);
	for (auto const& [step, hash] : rides.at(1.0f / 60.0f).Hashes)
	{
		CAPTURE(step);
		CHECK(rides.at(1.0f / 144.0f).Hashes.at(step) == hash);
	}
	CHECK(rides.at(1.0f / 60.0f).Hashes.size() == Steps);
}

TEST_CASE("Scene: a teleport and a vehicle's reset in one fixed step leave the same ride in either order")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting;
	AssetHandle const definition = AddManualBike();
	RideResult const teleportFirst = Ride(definition, 1.0f / 60.0f, "Bike");
	RideResult const resetFirst = Ride(definition, 1.0f / 60.0f, "Bike ResetFirst");
	CheckReport(teleportFirst.Report);
	CheckReport(resetFirst.Report);
	CHECK(teleportFirst.Hashes == resetFirst.Hashes);
	CHECK(resetFirst.PlaceAfterReset == teleportFirst.PlaceAfterReset);
}

TEST_CASE("Scene: the wheels of a vehicle reset from OnFixedUpdate are drawn at the step after the reset, never between the old ride "
          "and the new")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting;
	AssetHandle const definition = AddManualBike();
	Scene scene("Reset Wheels");
	Entity bike = MakeRide(scene, definition, "Bike");
	// The chassis is drawn between its steps (a rigid body's default), and so are its wheels.
	REQUIRE(bike.GetComponent<RigidBodyComponent>().Interpolation == RigidBodyInterpolation::Interpolate);
	Entity rear = scene.CreateEntity("Rear Wheel", bike);
	rear.AddComponent<VehicleWheelComponent>().Wheel = "Rear";
	scene.OnRuntimeStart();
	// The rear wheel in its chassis's space as the frame draws it.
	auto const inChassis = [&scene, &bike, &rear]
	{
		return glm::inverse(scene.GetWorldTransform(bike)) * scene.GetWorldTransform(rear);
	};
	auto const isNear = [](glm::mat4 const& first, glm::mat4 const& second)
	{
		for (int column = 0; column < 4; column++)
		{
			if (glm::any(glm::greaterThan(glm::abs(first[column] - second[column]), glm::vec4(1e-4f))))
			{
				return false;
			}
		}
		return true;
	};

	// At 144 frames a second a frame that ran a step is followed by one that runs none and draws further into the next. For
	// the step before the reset's (the rider's 110th, in second gear at speed) and for the reset's own (its 120th), the poses
	// of those two frames.
	std::map<uint64_t, std::vector<glm::mat4>> poses;
	for (int frame = 0; frame < 1000 && scene.GetFixedStepCount() < 122; frame++)
	{
		scene.OnUpdateRuntime(Timestep(1.0f / 144.0f));
		uint64_t const step = scene.GetFixedStepCount();
		if (step == 110 || step == 120)
		{
			poses[step].push_back(inChassis());
		}
	}
	REQUIRE(scene.GetFixedStepCount() == 122);
	REQUIRE(poses[110].size() >= 2);
	REQUIRE(poses[120].size() >= 2);
	// The premise: before the reset the frames between two steps draw the spinning wheel on its way from one to the next.
	CHECK_FALSE(isNear(poses[110][0], poses[110][1]));
	// After the reset nothing is drawn between the old ride and the new: from the step after the reset on, the wheel is
	// drawn at that step, whatever part of the next the frame shows.
	for (size_t index = 1; index < poses[120].size(); index++)
	{
		CAPTURE(index);
		CHECK(isNear(poses[120][index], poses[120][0]));
	}
	std::map<std::string, std::string> const report = ParseReport(bike.GetName());
	CHECK(Number(report, "Reset") == 120.0f);
	scene.OnRuntimeStop();
}

// A running scene's snapshot beside a vehicle's reset (Docs/Architecture.md §8, "The reset" and "Snapshots"; the folded hash,
// "The state hash"): the world's side of it is PhysicsVehicleResetTests.cpp's; here the scene's step state is restored
// with it, the fold of every step's hash and the wheel entities as the scene draws them among it ("Interpolated wheels").
namespace
{
	// Scripts off and the fold of every step's hash on, without a hash step.
	SceneRuntimeSettings FoldingRide()
	{
		SceneRuntimeSettings settings;
		settings.RunScripts = false;
		settings.PlayAudio = false;
		settings.FoldPhysicsHashes = true;
		return settings;
	}

	// The inputs the bike rides with before the step about to run: what a script's OnFixedUpdate would give
	// (VehicleComponent.SetDriverInput and ShiftUp set them on the world), as a function of that step alone, so that steps
	// run again after a restore are given the same.
	VehicleInputs SnapshotRideInputs(uint64_t step)
	{
		VehicleInputs inputs;
		inputs.Throttle = step < 200 ? 0.5f : 0.0f;
		inputs.FrontBrake = step >= 200 ? 0.4f : 0.0f;
		inputs.Lean = step >= 60 && step < 120 ? 0.2f : 0.0f;
		return inputs;
	}

	// A Manual gearbox's shifts up at steps 0, 60 and 130: first gear at once, second after a second, and one more gear at
	// step 130 (first again for a bike a reset at step 120 left in neutral, third for one that rode on).
	int32_t SnapshotRideShifts(uint64_t step)
	{
		return step == 0 || step == 60 || step == 130 ? 1 : 0;
	}

	// Frames of one fixed step each, the bike given its inputs before each step: the state hash after each.
	std::vector<uint64_t> RideSteps(Scene& scene, int count)
	{
		PhysicsScene* world = scene.GetPhysicsScene();
		REQUIRE(world != nullptr);
		std::vector<uint64_t> hashes;
		for (int index = 0; index < count; index++)
		{
			uint64_t const step = scene.GetFixedStepCount();
			world->SetVehicleInputs(UUID(BikeId), SnapshotRideInputs(step));
			if (int32_t const shift = SnapshotRideShifts(step); shift != 0)
			{
				world->RequestVehicleShift(UUID(BikeId), shift);
			}
			scene.OnUpdateRuntime(Timestep(scene.GetFixedTimestep()));
			REQUIRE(scene.GetFixedStepCount() == step + 1);
			hashes.push_back(scene.ComputePhysicsStateHash());
		}
		return hashes;
	}

	// The bike's two wheels as entities the scene draws.
	std::vector<Entity> AddWheels(Scene& scene, Entity bike)
	{
		std::vector<Entity> wheels;
		for (char const* name : {"Front", "Rear"})
		{
			Entity wheel = scene.CreateEntity(std::string(name) + " Wheel", bike);
			wheel.AddComponent<VehicleWheelComponent>().Wheel = name;
			wheels.push_back(wheel);
		}
		return wheels;
	}

	// An entity's own translation and rotation in its parent's space: what the drawing writes and a restore gives back.
	struct LocalPose
	{
		glm::vec3 Translation = glm::vec3(0.0f);
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

		bool operator==(LocalPose const& other) const = default;
	};

	LocalPose LocalPoseOf(Entity const& entity)
	{
		TransformComponent const& transform = entity.GetComponent<TransformComponent>();
		return {transform.Translation, transform.Rotation};
	}

	// What a snapshot is to bring back of the scene's step state: the fold, the step, the state hash, and each wheel entity as
	// the scene draws it (its world transform) and as it stands in its parent's space.
	struct StepState
	{
		std::optional<uint64_t> Fold;
		uint64_t Step = 0;
		uint64_t Hash = 0;
		std::vector<glm::mat4> Wheels;
		std::vector<LocalPose> LocalWheels;
	};

	StepState ReadStepState(Scene& scene, std::vector<Entity> const& wheels)
	{
		StepState state;
		state.Fold = scene.GetFoldedPhysicsHash();
		state.Step = scene.GetFixedStepCount();
		state.Hash = scene.ComputePhysicsStateHash();
		for (Entity const& wheel : wheels)
		{
			state.Wheels.push_back(scene.GetWorldTransform(wheel));
			state.LocalWheels.push_back(LocalPoseOf(wheel));
		}
		return state;
	}

	std::string DescribePlace(glm::mat4 const& transform)
	{
		return std::to_string(transform[3].x) + ", " + std::to_string(transform[3].y) + ", " + std::to_string(transform[3].z);
	}

	// The same step state; a snapshot taken before the first step has no fold.
	void CheckSameStepState(StepState const& now, StepState const& saved)
	{
		CHECK(now.Fold == saved.Fold);
		CHECK(now.Step == saved.Step);
		CHECK(now.Hash == saved.Hash);
		REQUIRE(now.Wheels.size() == saved.Wheels.size());
		for (size_t index = 0; index < now.Wheels.size(); index++)
		{
			CAPTURE(index);
			CAPTURE(DescribePlace(now.Wheels[index]));
			CAPTURE(DescribePlace(saved.Wheels[index]));
			CHECK(now.Wheels[index] == saved.Wheels[index]);
			CHECK(now.LocalWheels[index] == saved.LocalWheels[index]);
		}
	}

	// A second definition: the first's values but a slower gearbox, so a vehicle pointed at it is made again.
	AssetHandle AddSlowerShiftingBike()
	{
		Vehicle bike;
		bike.Transmission.Mode = VehicleTransmissionMode::Manual;
		bike.Transmission.SwitchTime = 0.2f;
		bike.Transmission.ClutchReleaseTime = 0.1f;
		bike.Lean.SpringDamping = 800.0f;
		TypedDataAssetType<Vehicle> const* type = DataAssetRegistry::Find<Vehicle>();
		REQUIRE(type != nullptr);
		Ref<TypedDataAsset<Vehicle>> const asset = std::static_pointer_cast<TypedDataAsset<Vehicle>>(type->CreateAsset());
		REQUIRE(asset->SetData(bike).IsOk());
		return AssetManager::AddMemoryAsset(asset, "Slower Shifting Bike.svehicle");
	}

	// A bike on its ground for the test to ride, without its rider, its two wheels drawn.
	Entity MakeSnapshotRide(Scene& scene, AssetHandle definition, std::vector<Entity>& wheels)
	{
		Entity bike = MakeRide(scene, definition, "Bike");
		bike.RemoveComponent<ScriptComponent>();
		wheels = AddWheels(scene, bike);
		return bike;
	}
}

TEST_CASE("Scene: a snapshot taken before a vehicle's reset restores after it with the folded hash and the wheels as drawn, and the "
          "steps run again give the same hashes and the same fold")
{
	// A vehicle with steps at the save: the restore draws its wheels from the saved steps, which keeping the wheel entities
	// of a vehicle with no step leaves as it was; and the fold comes back beside a reset.
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope physics;
	AssetHandle const definition = AddManualBike();
	Scene scene("Reset Snapshot");
	std::vector<Entity> wheels;
	Entity bike = MakeSnapshotRide(scene, definition, wheels);
	scene.OnRuntimeStart(FoldingRide());
	REQUIRE(scene.GetPhysicsScene()->HasVehicle(bike.GetUUID()));
	RideSteps(scene, 120);
	Result<ScenePhysicsSnapshot> saved = scene.SavePhysicsState();
	REQUIRE_MESSAGE(saved.IsOk(), (saved ? std::string() : saved.GetError()));
	StepState const atSave = ReadStepState(scene, wheels);
	REQUIRE(atSave.Fold.has_value());
	std::vector<uint64_t> const straight = RideSteps(scene, 60);
	std::optional<uint64_t> const straightFold = scene.GetFoldedPhysicsHash();
	REQUIRE(straightFold.has_value());

	// The reset, and steps after it: the wheels stand elsewhere in their parent's space.
	REQUIRE(scene.ResetVehicle(bike));
	RideSteps(scene, 20);
	CHECK(ReadStepState(scene, wheels).LocalWheels != atSave.LocalWheels);

	REQUIRE(scene.RestorePhysicsState(saved.GetValue()).IsOk());
	CheckSameStepState(ReadStepState(scene, wheels), atSave);
	std::vector<uint64_t> const replayed = RideSteps(scene, 60);
	CHECK(replayed == straight);
	CHECK(scene.GetFoldedPhysicsHash() == straightFold);
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: a snapshot taken right after a vehicle's reset, before any step, restores with the folded hash and the wheels as "
          "drawn, and the steps run again give the same hashes and the same fold")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope physics;
	AssetHandle const definition = AddManualBike();
	Scene scene("Reset Snapshot");
	std::vector<Entity> wheels;
	Entity bike = MakeSnapshotRide(scene, definition, wheels);
	scene.OnRuntimeStart(FoldingRide());
	REQUIRE(scene.GetPhysicsScene()->HasVehicle(bike.GetUUID()));
	RideSteps(scene, 120);
	REQUIRE(scene.ResetVehicle(bike));
	Result<ScenePhysicsSnapshot> saved = scene.SavePhysicsState();
	REQUIRE_MESSAGE(saved.IsOk(), (saved ? std::string() : saved.GetError()));
	StepState const atSave = ReadStepState(scene, wheels);
	REQUIRE(atSave.Fold.has_value());
	std::vector<uint64_t> const straight = RideSteps(scene, 90);
	std::optional<uint64_t> const straightFold = scene.GetFoldedPhysicsHash();
	REQUIRE(straightFold.has_value());
	// The premise: the ride after the reset moves the wheels in their parent's space.
	CHECK(ReadStepState(scene, wheels).LocalWheels != atSave.LocalWheels);

	REQUIRE(scene.RestorePhysicsState(saved.GetValue()).IsOk());
	CheckSameStepState(ReadStepState(scene, wheels), atSave);
	std::vector<uint64_t> const replayed = RideSteps(scene, 90);
	CHECK(replayed == straight);
	CHECK(scene.GetFoldedPhysicsHash() == straightFold);
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: a snapshot taken after a vehicle's body is rebuilt, which the world carries the vehicle over to with its wheels' "
          "steps, restores with the wheels drawn from those steps whatever the wheel entities showed, and the steps run again give "
          "the same hashes and the same fold")
{
	// A rebuilt body keeps its vehicle (the world carries it over, Scene::UpdateVehicle) and its wheels' steps, so this passes
	// without the wheel entities a vehicle with no step keeps: it guards that such a vehicle is not taken for one.
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope physics;
	AssetHandle const definition = AddManualBike();
	Scene scene("Rebuilt Body Snapshot");
	std::vector<Entity> wheels;
	Entity bike = MakeSnapshotRide(scene, definition, wheels);
	scene.OnRuntimeStart(FoldingRide());
	REQUIRE(scene.GetPhysicsScene()->HasVehicle(bike.GetUUID()));
	RideSteps(scene, 60);
	// A wider chassis collider rebuilds the body; the vehicle goes over to the new body with its values and its steps.
	uint64_t const structure = scene.GetPhysicsScene()->GetStructureVersion();
	BoxColliderComponent wider = bike.GetComponent<BoxColliderComponent>();
	wider.HalfExtents.x = 0.3f;
	bike.AddOrReplaceComponent<BoxColliderComponent>(wider);
	scene.ApplyPhysicsChanges();
	REQUIRE(scene.GetPhysicsScene()->HasVehicle(bike.GetUUID()));
	REQUIRE(scene.GetPhysicsScene()->GetStructureVersion() != structure);
	// The wheels as the kept steps draw them; then the entities are put elsewhere before the save.
	StepState const drawn = ReadStepState(scene, wheels);
	wheels[0].GetComponent<TransformComponent>().Translation = {0.0f, 2.0f, 1.5f};
	wheels[1].GetComponent<TransformComponent>().Translation = {0.0f, 2.0f, -1.5f};
	Result<ScenePhysicsSnapshot> saved = scene.SavePhysicsState();
	REQUIRE_MESSAGE(saved.IsOk(), (saved ? std::string() : saved.GetError()));
	StepState const atSave = ReadStepState(scene, wheels);
	REQUIRE(atSave.Fold.has_value());
	CHECK(atSave.Hash == drawn.Hash);
	std::vector<uint64_t> const straight = RideSteps(scene, 30);
	std::optional<uint64_t> const straightFold = scene.GetFoldedPhysicsHash();
	REQUIRE(straightFold.has_value());

	// The restore draws the wheels from the saved steps, not where the entities stood at the save.
	REQUIRE(scene.RestorePhysicsState(saved.GetValue()).IsOk());
	StepState const restored = ReadStepState(scene, wheels);
	CheckSameStepState(restored, drawn);
	CHECK(restored.LocalWheels != atSave.LocalWheels);
	std::vector<uint64_t> const replayed = RideSteps(scene, 30);
	CHECK(replayed == straight);
	CHECK(scene.GetFoldedPhysicsHash() == straightFold);
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: a snapshot taken right after a vehicle is made again for another definition, before its next step, restores with "
          "its wheels' own translation and rotation, and the steps run again give the same hashes and the same fold")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope physics;
	AssetHandle const definition = AddManualBike();
	AssetHandle const other = AddSlowerShiftingBike();
	Scene scene("Made Again Snapshot");
	std::vector<Entity> wheels;
	Entity bike = MakeSnapshotRide(scene, definition, wheels);
	scene.OnRuntimeStart(FoldingRide());
	REQUIRE(scene.GetPhysicsScene()->HasVehicle(bike.GetUUID()));
	RideSteps(scene, 60);
	// Another definition whose values differ: the scene makes the vehicle again on its body (PhysicsScene::AddVehicle), with
	// no step yet.
	uint64_t const structure = scene.GetPhysicsScene()->GetStructureVersion();
	bike.AddOrReplaceComponent<VehicleComponent>(VehicleComponent{other});
	scene.ApplyPhysicsChanges();
	REQUIRE(scene.GetPhysicsScene()->HasVehicle(bike.GetUUID()));
	REQUIRE(scene.GetPhysicsScene()->GetStructureVersion() != structure);
	// Where the wheels stand before the vehicle's next step draws them: places of the test's own, which no step draws.
	wheels[0].GetComponent<TransformComponent>().Translation = {0.0f, 2.0f, 1.5f};
	wheels[1].GetComponent<TransformComponent>().Translation = {0.0f, 2.0f, -1.5f};
	Result<ScenePhysicsSnapshot> saved = scene.SavePhysicsState();
	REQUIRE_MESSAGE(saved.IsOk(), (saved ? std::string() : saved.GetError()));
	StepState const atSave = ReadStepState(scene, wheels);
	REQUIRE(atSave.Fold.has_value());
	std::vector<uint64_t> const straight = RideSteps(scene, 30);
	std::optional<uint64_t> const straightFold = scene.GetFoldedPhysicsHash();
	REQUIRE(straightFold.has_value());
	// The premise: the steps after it move the wheels in their parent's space.
	CHECK(ReadStepState(scene, wheels).LocalWheels != atSave.LocalWheels);

	REQUIRE(scene.RestorePhysicsState(saved.GetValue()).IsOk());
	CheckSameStepState(ReadStepState(scene, wheels), atSave);
	std::vector<uint64_t> const replayed = RideSteps(scene, 30);
	CHECK(replayed == straight);
	CHECK(scene.GetFoldedPhysicsHash() == straightFold);
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: a snapshot taken as a run starts, before a vehicle's first step, restores with its wheels where they stood, again "
          "each time it is restored, and the steps run again give the same hashes and the same fold")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope physics;
	AssetHandle const definition = AddManualBike();
	Scene scene("Start Snapshot");
	std::vector<Entity> wheels;
	Entity bike = MakeSnapshotRide(scene, definition, wheels);
	// Where the wheels stand before the vehicle's first step draws them: places of the test's own.
	wheels[0].GetComponent<TransformComponent>().Translation = {0.0f, 2.0f, 1.5f};
	wheels[1].GetComponent<TransformComponent>().Translation = {0.0f, 2.0f, -1.5f};
	scene.OnRuntimeStart(FoldingRide());
	REQUIRE(scene.GetPhysicsScene()->HasVehicle(bike.GetUUID()));
	Result<ScenePhysicsSnapshot> saved = scene.SavePhysicsState();
	REQUIRE_MESSAGE(saved.IsOk(), (saved ? std::string() : saved.GetError()));
	StepState const atSave = ReadStepState(scene, wheels);
	CHECK(atSave.Step == 0);
	CHECK_FALSE(atSave.Fold.has_value());
	std::vector<uint64_t> const straight = RideSteps(scene, 60);
	std::optional<uint64_t> const straightFold = scene.GetFoldedPhysicsHash();
	REQUIRE(straightFold.has_value());
	// The premise: the first steps place the wheels elsewhere in their parent's space.
	CHECK(ReadStepState(scene, wheels).LocalWheels != atSave.LocalWheels);

	// Restored, ridden on, and restored again: the same state each time.
	for (int restore = 0; restore < 2; restore++)
	{
		CAPTURE(restore);
		REQUIRE(scene.RestorePhysicsState(saved.GetValue()).IsOk());
		CheckSameStepState(ReadStepState(scene, wheels), atSave);
		std::vector<uint64_t> const replayed = RideSteps(scene, 60);
		CHECK(replayed == straight);
		CHECK(scene.GetFoldedPhysicsHash() == straightFold);
	}
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: a restore gives back only the wheel entities a made vehicle draws, their translation and rotation with their scale "
          "kept, while they have the parent they had, and leaves every other entity with a VehicleWheel as it is")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope physics;
	AssetHandle const definition = AddManualBike();
	Scene scene("Wheel Snapshot");
	std::vector<Entity> wheels;
	Entity bike = MakeSnapshotRide(scene, definition, wheels);
	Entity front = wheels[0];
	Entity rear = wheels[1];
	front.GetComponent<TransformComponent>().Translation = {0.0f, 2.0f, 1.5f};
	// A second rear wheel the bike draws, re-parented after the save below a fork of the bike's (an entity without a body).
	Entity fork = scene.CreateEntity("Fork", bike);
	fork.GetComponent<TransformComponent>().Translation = {0.0f, 1.0f, 0.0f};
	Entity moved = scene.CreateEntity("Moved Wheel", bike);
	moved.AddComponent<VehicleWheelComponent>().Wheel = "Rear";
	// VehicleWheel entities no vehicle draws: one below the bike naming a wheel its definition lacks, one with no Vehicle
	// above it, and one below a Vehicle that makes no vehicle (no body).
	Entity sidecar = scene.CreateEntity("Sidecar Wheel", bike);
	sidecar.AddComponent<VehicleWheelComponent>().Wheel = "Sidecar";
	sidecar.GetComponent<TransformComponent>().Translation = {1.0f, 0.5f, 0.0f};
	Entity stray = scene.CreateEntity("Stray Wheel");
	stray.AddComponent<VehicleWheelComponent>().Wheel = "Front";
	stray.GetComponent<TransformComponent>().Translation = {5.0f, 0.5f, 5.0f};
	Entity cart = scene.CreateEntity("Cart");
	cart.GetComponent<TransformComponent>().Translation = {-5.0f, 0.0f, 0.0f};
	cart.AddComponent<VehicleComponent>().Definition = definition;
	Entity cartWheel = scene.CreateEntity("Cart Wheel", cart);
	cartWheel.AddComponent<VehicleWheelComponent>().Wheel = "Front";
	cartWheel.GetComponent<TransformComponent>().Translation = {0.0f, 0.5f, 1.0f};
	// And one on an entity with a body of its own, which the drawing leaves alone: a dynamic body that falls.
	Entity loose = scene.CreateEntity("Loose Wheel");
	loose.GetComponent<TransformComponent>().Translation = {5.0f, 3.0f, -5.0f};
	loose.AddComponent<VehicleWheelComponent>().Wheel = "Front";
	RigidBodyComponent& looseBody = loose.AddComponent<RigidBodyComponent>();
	looseBody.Type = RigidBodyType::Dynamic;
	looseBody.AllowSleep = false;
	loose.AddComponent<BoxColliderComponent>().HalfExtents = {0.3f, 0.3f, 0.1f};
	scene.OnRuntimeStart(FoldingRide());
	REQUIRE(scene.GetPhysicsScene()->HasVehicle(bike.GetUUID()));
	REQUIRE_FALSE(scene.GetPhysicsScene()->HasVehicle(cart.GetUUID()));

	// Before the bike's first step: its wheels have no steps to be drawn from.
	Result<ScenePhysicsSnapshot> saved = scene.SavePhysicsState();
	REQUIRE_MESSAGE(saved.IsOk(), (saved ? std::string() : saved.GetError()));
	LocalPose const frontAtSave = LocalPoseOf(front);
	LocalPose const movedAtSave = LocalPoseOf(moved);
	glm::vec3 const looseAtSave = loose.GetComponent<TransformComponent>().Translation;

	// After the save: the front wheel's scale edited, the second rear wheel re-parented below the fork, the wheels no vehicle
	// draws moved, the rear wheel destroyed and a wheel added; then the bike rides.
	front.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f);
	REQUIRE(scene.SetParent(moved, fork).IsOk());
	sidecar.GetComponent<TransformComponent>().Translation = {1.0f, 0.75f, 0.25f};
	stray.GetComponent<TransformComponent>().Translation = {6.0f, 0.5f, 4.0f};
	cartWheel.GetComponent<TransformComponent>().Translation = {0.0f, 0.75f, 1.25f};
	UUID const rearId = rear.GetUUID();
	scene.DestroyEntity(rear);
	Entity added = scene.CreateEntity("Added Wheel", bike);
	added.AddComponent<VehicleWheelComponent>().Wheel = "Front";
	RideSteps(scene, 30);
	REQUIRE_FALSE(scene.HasEntity(rearId));
	LocalPose const movedBefore = LocalPoseOf(moved);
	LocalPose const addedBefore = LocalPoseOf(added);
	// The premises: the steps placed the front wheel and the re-parented one elsewhere, and the loose wheel fell.
	CHECK(LocalPoseOf(front) != frontAtSave);
	CHECK(movedBefore != movedAtSave);
	CHECK(loose.GetComponent<TransformComponent>().Translation != looseAtSave);

	// The restore does not fail for the wheel gone since. The front wheel gets back its translation and rotation, its scale
	// kept; the re-parented and the added wheels stay as the last step drew them, the wheels no vehicle draws where the test
	// put them; the loose wheel's entity, a body's, comes back with the bodies.
	REQUIRE(scene.RestorePhysicsState(saved.GetValue()).IsOk());
	CHECK(LocalPoseOf(front) == frontAtSave);
	CHECK(front.GetComponent<TransformComponent>().Scale == glm::vec3(2.0f));
	CHECK(LocalPoseOf(moved) == movedBefore);
	CHECK(LocalPoseOf(added) == addedBefore);
	CHECK(sidecar.GetComponent<TransformComponent>().Translation == glm::vec3(1.0f, 0.75f, 0.25f));
	CHECK(stray.GetComponent<TransformComponent>().Translation == glm::vec3(6.0f, 0.5f, 4.0f));
	CHECK(cartWheel.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f, 0.75f, 1.25f));
	CHECK(loose.GetComponent<TransformComponent>().Translation == looseAtSave);
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: a vehicle's reset without a simulated vehicle is logged and changes nothing")
{
	Testing::AssetManagerScope const assets;
	Testing::ScriptEngineScope scripting;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("No Physics");
	AssetHandle const definition = AddManualBike();
	Entity bike = MakeRide(scene, definition, "Bike");
	scene.OnRuntimeStart();
	CHECK_FALSE(scene.ResetVehicle(bike));
	for (int frame = 0; frame < 121; frame++)
	{
		scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
	}
	CHECK(Testing::WasLogged(logStart, "VehicleComponent.ResetVehicle: entity"));
	CHECK(Testing::WasLogged(logStart, "has no vehicle (the scene is not simulating it"));
	std::map<std::string, std::string> const report = ParseReport(bike.GetName());
	CHECK(Number(report, "Gear") == 0.0f);
	CHECK(Number(report, "Engine") == 0.0f);
	scene.OnRuntimeStop();
	CHECK_FALSE(scene.ResetVehicle(bike));
}
