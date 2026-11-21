#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/Vehicle.h"
#include "Strada/Asset/VehicleAsset.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Core/UUID.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

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
