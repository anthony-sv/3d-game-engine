#include "Core/TelemetryTestUtilities.h"
#include "Physics/PhysicsTestUtilities.h"
#include "Script/ScriptTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/PhysicsMaterial.h"
#include "Strada/Asset/PhysicsMaterialAsset.h"
#include "Strada/Asset/Vehicle.h"
#include "Strada/Asset/VehicleAsset.h"
#include "Strada/Core/Log.h"
#include "Strada/Core/Telemetry.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"
#include "Strada/Script/ScriptEngine.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

// Scripts ride vehicles (Docs/Architecture.md §8, "Vehicles", "Scripts and graphs"): the probes of
// Tests/TestScripts/Source/VehicleProbes.cs.
namespace
{
	constexpr float Step = 1.0f / 60.0f;
	constexpr uint64_t GroundId = 100;
	constexpr uint64_t BikeId = 200;

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

	Vehicle MakeManualBike()
	{
		Vehicle bike;
		bike.Transmission.Mode = VehicleTransmissionMode::Manual;
		bike.Transmission.SwitchTime = 0.1f;
		bike.Transmission.ClutchReleaseTime = 0.1f;
		bike.Lean.SpringDamping = 800.0f;
		return bike;
	}

	// Ground of Asphalt (a physics material whose surface is named so), and a bike on it: a dynamic 250 kg chassis box whose
	// roll inertia carries the lean spring's damping (as SceneVehicleTests' bikes), its Vehicle on the definition, and the
	// script on it.
	Entity MakeRide(Scene& scene, AssetHandle definition, std::string const& script)
	{
		PhysicsMaterial asphalt;
		asphalt.Friction = 1.0f;
		asphalt.Surface = "Asphalt";
		AssetHandle const material = AddDataAsset(asphalt, "Asphalt.sphysmat");
		Entity ground = scene.CreateEntityWithUUID(UUID(GroundId), "Ground");
		ground.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
		BoxColliderComponent& floor = ground.AddComponent<BoxColliderComponent>();
		floor.HalfExtents = {200.0f, 0.5f, 200.0f};
		floor.Material = material;

		Entity bike = scene.CreateEntityWithUUID(UUID(BikeId), "Bike");
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
		if (!script.empty())
		{
			bike.AddComponent<ScriptComponent>().ClassName = script;
		}
		return bike;
	}

	// The "key=value;" pairs of a probe's name.
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
			else if (!pair.empty())
			{
				report[pair] = std::string();
			}
			if (end == std::string::npos)
			{
				break;
			}
			start = end + 1;
		}
		return report;
	}

	float Number(std::map<std::string, std::string> const& report, std::string const& key)
	{
		auto const found = report.find(key);
		REQUIRE_MESSAGE(found != report.end(), key);
		// strtof, not stof: a reading too small for a normal float (a slip near 0) is a value here, never an exception.
		return std::strtof(found->second.c_str(), nullptr);
	}

	std::string Text(std::map<std::string, std::string> const& report, std::string const& key)
	{
		auto const found = report.find(key);
		REQUIRE_MESSAGE(found != report.end(), key);
		return found->second;
	}

	// The reads of a VehicleState as VehicleComponent gives them: the lean and the slip angle in degrees.
	void CheckReport(std::map<std::string, std::string> const& report, VehicleState const& state)
	{
		CHECK(Number(report, "Gear") == static_cast<float>(state.Gear));
		CHECK(Number(report, "Engine") == state.EngineSpeed);
		CHECK(Number(report, "Speed") == state.ForwardSpeed);
		CHECK(Number(report, "Lean") == doctest::Approx(glm::degrees(state.LeanAngle)).epsilon(1e-6));
		CHECK(Number(report, "Throttle") == state.AppliedInputs.Throttle);
		CHECK(Number(report, "FrontBrake") == state.AppliedInputs.FrontBrake);
		CHECK(Number(report, "RearBrake") == state.AppliedInputs.RearBrake);
		CHECK(Number(report, "LeanInput") == state.AppliedInputs.Lean);
		for (auto const& [wheel, index] : std::array<std::pair<char const*, size_t>, 2>{{{"Front", 0}, {"Rear", 1}}})
		{
			CAPTURE(wheel);
			VehicleWheelState const& expected = state.Wheels[index];
			std::string const prefix(wheel);
			bool const contact = expected.ContactEntity.IsValid();
			CHECK(Text(report, prefix + "Contact") == (contact ? "True" : "False"));
			CHECK(Text(report, prefix + "Ground") == (contact ? "Ground" : ""));
			CHECK(Text(report, prefix + "Surface") == (contact ? "Asphalt" : ""));
			CHECK(Text(report, prefix + "Material") == (contact ? "set" : "none"));
			CHECK(Number(report, prefix + "Suspension") == expected.SuspensionLength);
			CHECK(Number(report, prefix + "Spin") == expected.AngularVelocity);
			CHECK(Number(report, prefix + "LongitudinalSlip") == expected.LongitudinalSlip);
			CHECK(Number(report, prefix + "LateralSlip") == doctest::Approx(glm::degrees(expected.LateralSlip)).epsilon(1e-6));
			CHECK(Number(report, prefix + "PointY") == expected.ContactPoint.y);
			CHECK(Number(report, prefix + "NormalY") == expected.ContactNormal.y);
		}
	}
}

TEST_CASE("Scene: scripts ride a vehicle from OnFixedUpdate: its inputs and shifts reach the next step, its reads are that "
          "step's")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting;
	uint64_t const logStart = Log::GetNextEntryIndex();
	Scene scene("Ride");
	AssetHandle const definition = AddDataAsset(MakeManualBike(), "Manual Bike.svehicle");
	Entity bike = MakeRide(scene, definition, "Strada.Tests.VehicleProbe");
	scene.OnRuntimeStart();
	REQUIRE(scene.GetPhysicsScene() != nullptr);
	REQUIRE(scene.GetPhysicsScene()->HasVehicle(bike.GetUUID()));
	// The vehicle exists as the scripts start: its definition reads, and a wheel other than 0 and 1 throws.
	CHECK(bike.GetName() == "Created;Threw=True;Definition=Rear;");

	// One step a frame: the report a step's OnFixedUpdate writes is what the step before it left.
	std::vector<VehicleState> states;
	for (int frame = 1; frame <= 60; frame++)
	{
		CAPTURE(frame);
		uint64_t const steps = scene.GetFixedStepCount();
		scene.OnUpdateRuntime(Timestep(Step));
		REQUIRE(scene.GetFixedStepCount() == steps + 1);
		std::map<std::string, std::string> const report = ParseReport(bike.GetName());
		REQUIRE(Number(report, "Step") == static_cast<float>(frame));
		if (frame > 1)
		{
			CheckReport(report, states.back());
		}
		std::optional<VehicleState> const state = scene.GetPhysicsScene()->GetVehicleState(bike.GetUUID());
		REQUIRE(state);
		states.push_back(*state);
	}

	// What each step applied: the inputs set in the OnFixedUpdate before it, the values out of their ranges clamped and the
	// one that is not a number refused (the step after it applies the values before it).
	auto const applied = [&states](int step) -> VehicleInputs const&
	{
		return states.at(static_cast<size_t>(step - 1)).AppliedInputs;
	};
	CHECK(applied(1).Throttle == 0.5f);
	CHECK(applied(1).RearBrake == 0.125f);
	CHECK(applied(2).RearBrake == 0.0f);
	CHECK(applied(26).Lean == 0.25f);
	CHECK(applied(40).Throttle == 1.0f);
	CHECK(applied(40).FrontBrake == 0.0f);
	CHECK(applied(40).RearBrake == 1.0f);
	CHECK(applied(40).Lean == -1.0f);
	CHECK(applied(41) == applied(40));
	CHECK(applied(42).FrontBrake == 0.75f);
	CHECK(Testing::WasLogged(logStart, "VehicleComponent.SetDriverInput: the value is not a finite number"));
	// Each shift request moves one gear at the next step: up from neutral, two up, four down to neutral, one up.
	CHECK(states.at(0).Gear == 1);
	CHECK(states.at(18).Gear == 1);
	CHECK(states.at(19).Gear == 3);
	CHECK(states.at(29).Gear == 0);
	CHECK(states.at(30).Gear == 1);
	// The ride moved the bike forward on the Asphalt, and the brakes slowed it once the throttle closed (with the throttle
	// closed the idling engine pushes it no more, Docs/Architecture.md §8).
	float fastest = 0.0f;
	for (VehicleState const& state : states)
	{
		fastest = std::max(fastest, state.ForwardSpeed);
	}
	CHECK(fastest > 1.0f);
	CHECK(states.back().ForwardSpeed < fastest);
	CHECK(states.back().Wheels[1].Surface.IsValid());
	scene.OnRuntimeStop();
}

TEST_CASE("Scene: vehicle calls without a simulated vehicle are logged and harmless, and read a vehicle at rest")
{
	Testing::AssetManagerScope const assets;
	Testing::ScriptEngineScope scripting;
	SUBCASE("without physics")
	{
		uint64_t const logStart = Log::GetNextEntryIndex();
		Scene scene("No Physics");
		AssetHandle const definition = AddDataAsset(MakeManualBike(), "Manual Bike.svehicle");
		Entity bike = MakeRide(scene, definition, "Strada.Tests.VehicleProbe");
		scene.OnRuntimeStart();
		scene.OnUpdateRuntime(Timestep(Step));
		CHECK(Testing::WasLogged(logStart, "VehicleComponent.ShiftUp: entity"));
		CHECK(Testing::WasLogged(logStart, "VehicleComponent.SetDriverInput: entity"));
		CHECK(Testing::WasLogged(logStart, "has no vehicle (the scene is not simulating it"));
		std::map<std::string, std::string> const report = ParseReport(bike.GetName());
		CHECK(Number(report, "Gear") == 0.0f);
		CHECK(Number(report, "Engine") == 0.0f);
		CHECK(Text(report, "FrontContact") == "False");
		CHECK(Text(report, "RearMaterial") == "none");
		scene.OnRuntimeStop();
	}
	SUBCASE("after its definition is taken away")
	{
		Testing::PhysicsSystemScope physics;
		Scene scene("Taken Away");
		AssetHandle const definition = AddDataAsset(MakeManualBike(), "Manual Bike.svehicle");
		Entity bike = MakeRide(scene, definition, "Strada.Tests.VehicleProbe");
		scene.OnRuntimeStart();
		for (int frame = 0; frame < 30; frame++)
		{
			scene.OnUpdateRuntime(Timestep(Step));
		}
		// Up from neutral at the first step, then two up at the twentieth.
		CHECK(Number(ParseReport(bike.GetName()), "Gear") == 3.0f);
		uint64_t const logStart = Log::GetNextEntryIndex();
		scene.GetRegistry().patch<VehicleComponent>(bike.GetHandle(),
		                                            [](VehicleComponent& component)
		                                            {
														component.Definition = AssetHandle();
													});
		scene.OnUpdateRuntime(Timestep(Step));
		CHECK_FALSE(scene.GetPhysicsScene()->HasVehicle(bike.GetUUID()));
		CHECK(Testing::WasLogged(logStart, "VehicleComponent.SetDriverInput: entity"));
		std::map<std::string, std::string> const report = ParseReport(bike.GetName());
		CHECK(Number(report, "Gear") == 0.0f);
		CHECK(Number(report, "Speed") == 0.0f);
		CHECK(Text(report, "RearContact") == "False");
		scene.OnRuntimeStop();
	}
}

TEST_CASE("Scene: a bike a script rides from OnFixedUpdate gives the same state hashes at 30, 60 and 144 frames a second, and the "
          "same readings at every step through its telemetry channels")
{
	Testing::AssetManagerScope const assets;
	Testing::PhysicsSystemScope physics;
	Testing::ScriptEngineScope scripting;
	Testing::TelemetryScope const telemetry;
	constexpr uint64_t Steps = 200;
	AssetHandle const definition = AddDataAsset(MakeManualBike(), "Rider Bike.svehicle");
	// Each rate's hash after every step the frames end on, and its bike's channels, a column of one row a step each.
	std::map<float, std::map<uint64_t, uint64_t>> hashes;
	std::map<float, std::vector<std::vector<std::optional<float>>>> readings;
	for (float const frameTime : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f})
	{
		CAPTURE(frameTime);
		REQUIRE(Telemetry::Start().IsOk());
		Scene scene("Rider");
		Entity bike = MakeRide(scene, definition, "Strada.Tests.VehicleRider");
		scene.OnRuntimeStart();
		REQUIRE(scene.GetPhysicsScene()->HasVehicle(bike.GetUUID()));
		std::map<uint64_t, uint64_t>& rate = hashes[frameTime];
		for (int frame = 0; frame < 100000 && scene.GetFixedStepCount() < Steps; frame++)
		{
			scene.OnUpdateRuntime(Timestep(frameTime));
			rate[scene.GetFixedStepCount()] = scene.ComputePhysicsStateHash();
		}
		REQUIRE(scene.GetFixedStepCount() == Steps);
		// The ride did ride: up two gears, the lean and the brakes on the way.
		std::optional<VehicleState> const state = scene.GetPhysicsScene()->GetVehicleState(bike.GetUUID());
		REQUIRE(state);
		CHECK(state->Gear == 2);
		scene.OnRuntimeStop();
		Telemetry::Stop();
		// One row a step, whatever the frame rate, each holding every channel of the bike.
		TelemetrySession const& session = Testing::GetNewestTelemetrySession();
		CHECK(session.FirstStep == 1);
		CHECK(session.Rows == Steps);
		for (std::string_view const quantity : VehicleChannelQuantities)
		{
			CAPTURE(quantity);
			std::string const name = "Vehicle." + std::to_string(BikeId) + "." + std::string(quantity);
			TelemetryChannel const* const channel = FindTelemetryChannel(session, name);
			REQUIRE(channel != nullptr);
			CHECK(channel->Count == Steps);
			readings[frameTime].push_back(Testing::GetTelemetryColumn(session, name));
		}
	}
	// At 30 frames a second every other step ends a frame; every step it exposes has the same hash at the other two rates.
	size_t compared = 0;
	for (auto const& [step, hash] : hashes.at(1.0f / 30.0f))
	{
		CAPTURE(step);
		CHECK(hashes.at(1.0f / 60.0f).at(step) == hash);
		CHECK(hashes.at(1.0f / 144.0f).at(step) == hash);
		compared++;
	}
	CHECK(compared == Steps / 2);
	// At 60 and 144 frames a second every step ends a frame, the odd ones included.
	for (auto const& [step, hash] : hashes.at(1.0f / 60.0f))
	{
		CAPTURE(step);
		CHECK(hashes.at(1.0f / 144.0f).at(step) == hash);
	}
	CHECK(hashes.at(1.0f / 60.0f).size() == Steps);
	// Every step's readings, the odd steps at 30 frames a second included (two steps a frame, whose inputs differ), are the
	// same at the three rates, bit for bit.
	for (size_t index = 0; index < VehicleChannelCount; index++)
	{
		CAPTURE(VehicleChannelQuantities[index]);
		CHECK(readings.at(1.0f / 30.0f).at(index) == readings.at(1.0f / 60.0f).at(index));
		CHECK(readings.at(1.0f / 144.0f).at(index) == readings.at(1.0f / 60.0f).at(index));
	}
	// What they show of the ride: the script's lean of each step, odd and even (VehicleRider: 0.3 at odd steps from 61 to 119,
	// 0.2 at even ones), reaching that step; and the gears it shifted to.
	float const slowRate = 1.0f / 30.0f;
	float const fullRate = 1.0f / 60.0f;
	std::vector<std::optional<float>> const lean = readings.at(slowRate).at(3);
	CHECK(lean.at(60) == std::optional<float>(0.3f));
	CHECK(lean.at(61) == std::optional<float>(0.2f));
	CHECK(lean.at(119) == std::optional<float>(0.0f));
	std::vector<std::optional<float>> const gear = readings.at(fullRate).at(5);
	CHECK(std::find(gear.begin(), gear.end(), std::optional<float>(2.0f)) != gear.end());
}
