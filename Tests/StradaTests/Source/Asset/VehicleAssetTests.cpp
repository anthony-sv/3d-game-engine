#include "Fuzzing.h"
#include "Physics/PhysicsTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/Asset.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/Vehicle.h"
#include "Strada/Asset/VehicleAsset.h"
#include "Strada/Math/Curve.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Serialization/StructSerialization.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	DataAssetType const& GetVehicleType()
	{
		DataAssetType const* type = DataAssetRegistry::FindByName("Vehicle");
		REQUIRE(type != nullptr);
		return *type;
	}

	Json MakeVehicleDocument(Json content)
	{
		Json document = Json::object();
		document["Strada"] = MakeFileHeader("Vehicle", 1);
		document["Vehicle"] = std::move(content);
		return document;
	}

	Json MakeVehicleDocument(Vehicle const& vehicle)
	{
		return MakeVehicleDocument(SerializeFields<StructTraits<Vehicle>>(vehicle));
	}

	Result<Ref<DataAsset>> Load(Json const& document)
	{
		return DataAssetSerializer::Deserialize(GetVehicleType(), document, DeserializationContext{});
	}

	std::string ErrorOf(Result<void> const& result)
	{
		return result ? std::string() : result.GetError();
	}

	CurveKey Key(float time, float value, CurveInterpolation interpolation = CurveInterpolation::Linear)
	{
		CurveKey key;
		key.Time = time;
		key.Value = value;
		key.Interpolation = interpolation;
		return key;
	}

	// The tests' chassis: a 250 kg box from 0.25 m to 1.25 m above its origin, 0.5 m wide and 1.2 m long, whose roll
	// inertia (26 kg m²) carries the lean spring's default damping (Docs/Architecture.md §8, "Vehicles").
	BodyDesc MakeChassis(UUID entity)
	{
		BodyDesc desc;
		desc.Entity = entity;
		desc.Type = RigidBodyType::Dynamic;
		desc.Position = glm::vec3(0.0f, 0.1f, 0.0f);
		desc.Mass = 250.0f;
		desc.AllowSleep = false;
		ColliderDesc collider;
		collider.HalfExtents = glm::vec3(0.25f, 0.5f, 0.6f);
		collider.Offset = glm::vec3(0.0f, 0.75f, 0.0f);
		desc.Colliders.push_back(collider);
		return desc;
	}

	BodyDesc MakeGround(UUID entity)
	{
		BodyDesc desc;
		desc.Entity = entity;
		desc.Type = RigidBodyType::Static;
		desc.Position = glm::vec3(0.0f, -0.5f, 0.0f);
		ColliderDesc collider;
		collider.HalfExtents = glm::vec3(200.0f, 0.5f, 200.0f);
		desc.Colliders.push_back(collider);
		return desc;
	}

	struct Spoil
	{
		std::string_view Field;
		void (*Apply)(Vehicle&);
	};

	// Definitions the field tables take and Validate refuses, each naming its field.
	std::array<Spoil, 15> const ValidationRefusals = {{
		{"Wheels: a motorcycle has exactly 2 wheels",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Wheels.resize(1);
		 }},
		{"Wheels: a motorcycle has exactly 2 wheels",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Wheels.clear();
		 }},
		{"Wheels[1].Name: 'FRONT' names Wheels[0] too",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Wheels[1].Name = "FRONT";
		 }},
		{"Wheels[0].Name: a wheel's name has 1 to 64 bytes, not 0",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Wheels[0].Name.clear();
		 }},
		{"Wheels[1].Name: a wheel's name has 1 to 64 bytes, not 65",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Wheels[1].Name = std::string(65, 'r');
		 }},
		{"Wheels[0].SuspensionMaxLength",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Wheels[0].SuspensionMaxLength = 0.2f;
		 }},
		{"Wheels[1].SuspensionDirection: the direction is zero",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Wheels[1].SuspensionDirection = glm::vec3(0.0f);
		 }},
		{"Wheels[0].SteeringAxis: the direction is zero",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Wheels[0].SteeringAxis = glm::vec3(0.0f);
		 }},
		{"Wheels[0].MaxSteerAngle: the front wheel steers",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Wheels[0].MaxSteerAngle = 0.0f;
		 }},
		{"Wheels[1].MaxSteerAngle: the rear wheel does not steer",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Wheels[1].MaxSteerAngle = 10.0f;
		 }},
		{"Up, Forward: the chassis's up and front are neither zero nor parallel",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Forward = glm::vec3(0.0f, -0.5f, 0.0f);
		 }},
		{"Up, Forward: the chassis's up and front are neither zero nor parallel",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Up = glm::vec3(0.0f);
		 }},
		{"Engine.MinRPM: the idle speed 10000 is not below MaxRPM 10000",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Engine.MinRPM = 10000.0f;
		 }},
		{"Transmission.ShiftDownRPM: 8000 is not below ShiftUpRPM 8000",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Transmission.ShiftDownRPM = 8000.0f;
		 }},
		{"Transmission.ShiftUpRPM: an automatic gearbox shifts up below the engine's MaxRPM 10000, not at 10000",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Transmission.ShiftUpRPM = 10000.0f;
		 }},
	}};
}

TEST_CASE("VehicleAsset: the engine registers Vehicle, its .svehicle files and its commands' names")
{
	DataAssetType const& type = GetVehicleType();
	CHECK(type.IsEngineType());
	CHECK(DataAssetRegistry::Find<Vehicle>() == &type);
	CHECK(TypedDataAsset<Vehicle>::GetStaticType() == type.GetAssetType());
	CHECK(type.GetExtension() == ".svehicle");
	CHECK(type.GetFormatVersion() == 1);
	CHECK(DataAssetRegistry::FindByExtension(".SVEHICLE") == &type);
	CHECK(GetAssetTypeForExtension(".svehicle") == type.GetAssetType());
	CHECK(std::string_view(AssetTypeToString(type.GetAssetType())) == "Vehicle");
	CHECK(type.GetCommandDomain() == "vehicle");
	CHECK(type.GetCommandParameter() == "vehicle");

	// The fields the inspector and the commands' schema show, in file order, each documented.
	std::vector<std::string_view> names;
	for (FieldDescriptor const& field : type.GetFields())
	{
		names.push_back(field.Name);
		CHECK_FALSE(field.Hints.Description.empty());
	}
	CHECK(names == std::vector<std::string_view>{"Wheels", "Up", "Forward", "MaxPitchRoll", "Engine", "Transmission", "Lean"});
	// The wheel list holds at most two, in an order editors keep (the first is the front), each titled by its name, which
	// comes first.
	std::span<FieldDescriptor const> const fields = type.GetFields();
	FieldDescriptor const& wheels = fields[0];
	CHECK(wheels.Hints.MaxCount == VehicleWheelCount);
	CHECK(wheels.Hints.FixedOrder);
	REQUIRE_FALSE(wheels.Fields.empty());
	CHECK(wheels.Fields[0].Name == "Name");
	for (FieldDescriptor const& field : type.GetFields())
	{
		CHECK(field.Hints.FixedOrder == (field.Name == "Wheels"));
	}
	// Curves are references to Curve assets.
	std::vector<std::string_view> curves;
	for (FieldDescriptor const& field : wheels.Fields)
	{
		if (field.Hints.AssetTypeName == "Curve")
		{
			curves.push_back(field.Name);
		}
	}
	CHECK(curves == std::vector<std::string_view>{"LongitudinalFriction", "LateralFriction"});

	// A new definition is a motorcycle: a front wheel that steers and brakes on the front brake, a rear wheel on the rear
	// brake, and it validates.
	Vehicle const vehicle;
	REQUIRE(vehicle.Wheels.size() == VehicleWheelCount);
	CHECK(vehicle.Wheels[0].Name == "Front");
	CHECK(vehicle.Wheels[0].MaxSteerAngle > 0.0f);
	CHECK(vehicle.Wheels[0].FrontBrakeTorque > 0.0f);
	CHECK(vehicle.Wheels[0].RearBrakeTorque == 0.0f);
	CHECK(vehicle.Wheels[1].Name == "Rear");
	CHECK(vehicle.Wheels[1].MaxSteerAngle == 0.0f);
	CHECK(vehicle.Wheels[1].FrontBrakeTorque == 0.0f);
	CHECK(vehicle.Wheels[1].RearBrakeTorque > 0.0f);
	CHECK(ValidateVehicle(vehicle).IsOk());
	CHECK(SerializeFields<StructTraits<Vehicle>>(vehicle) == type.GetDefaultFields());
}

TEST_CASE("VehicleAsset: a definition saves and loads again to the same values, and a sparse one takes the defaults")
{
	Vehicle vehicle;
	vehicle.Wheels[0].Name = "Fork";
	vehicle.Wheels[0].FrontBrakeTorque = 820.0f;
	vehicle.Wheels[0].LongitudinalFriction = AssetHandle(UUID(41));
	vehicle.Wheels[1].Name = "Swingarm";
	vehicle.Wheels[1].RearBrakeTorque = 310.0f;
	vehicle.Wheels[1].LateralFriction = AssetHandle(UUID(42));
	vehicle.Up = glm::vec3(0.0f, 0.0f, 1.0f);
	vehicle.Forward = glm::vec3(0.0f, -1.0f, 0.0f);
	vehicle.MaxPitchRoll = 180.0f;
	vehicle.Engine.MaxRPM = 16000.0f;
	vehicle.Engine.Torque = AssetHandle(UUID(43));
	vehicle.Transmission.Mode = VehicleTransmissionMode::Manual;
	vehicle.Transmission.GearRatios = {2.714f, 1.9f, 1.304f};
	vehicle.Transmission.FinalDrive = 2.5625f;
	vehicle.Lean.SteeringLimit = false;
	REQUIRE(ValidateVehicle(vehicle).IsOk());

	Result<Ref<DataAsset>> loaded = Load(MakeVehicleDocument(vehicle));
	REQUIRE_MESSAGE(loaded.IsOk(), (loaded ? std::string() : loaded.GetError()));
	auto const typed = std::static_pointer_cast<TypedDataAsset<Vehicle>>(loaded.GetValue());
	CHECK(typed->GetData() == vehicle);
	Json const saved = DataAssetSerializer::Serialize(*typed);
	CHECK(saved["Strada"] == Json::parse(R"({ "Version": 1, "Type": "Vehicle" })"));
	CHECK(saved["Vehicle"]["Transmission"]["Mode"] == "Manual");
	CHECK(saved["Vehicle"]["Wheels"][1]["Name"] == "Swingarm");
	CHECK(saved["Vehicle"]["Engine"]["Torque"] == "43");
	Result<Ref<DataAsset>> again = Load(saved);
	REQUIRE(again.IsOk());
	CHECK(std::static_pointer_cast<TypedDataAsset<Vehicle>>(again.GetValue())->GetData() == vehicle);

	// A document that names only some fields takes the defaults for the others.
	Result<Ref<DataAsset>> sparse = Load(MakeVehicleDocument(Json::parse(R"({ "MaxPitchRoll": 90.0, "Lean": { "MaxLeanAngle": 50.0 } })")));
	REQUIRE(sparse.IsOk());
	Vehicle expected;
	expected.MaxPitchRoll = 90.0f;
	expected.Lean.MaxLeanAngle = 50.0f;
	CHECK(std::static_pointer_cast<TypedDataAsset<Vehicle>>(sparse.GetValue())->GetData() == expected);
}

TEST_CASE("VehicleAsset: validation refuses what the field tables cannot state, naming the field, and the file with it")
{
	for (Spoil const& spoil : ValidationRefusals)
	{
		CAPTURE(spoil.Field);
		Vehicle vehicle;
		spoil.Apply(vehicle);
		std::string const error = ErrorOf(ValidateVehicle(vehicle));
		CHECK(error.find(spoil.Field) != std::string::npos);
		Result<Ref<DataAsset>> const loaded = Load(MakeVehicleDocument(vehicle));
		REQUIRE(loaded.IsError());
		CHECK(loaded.GetError().find(spoil.Field) != std::string::npos);
	}
	// The field tables refuse what is out of range, and a third wheel.
	Json tooMany = SerializeFields<StructTraits<Vehicle>>(Vehicle{});
	tooMany["Wheels"].push_back(tooMany["Wheels"][1]);
	tooMany["Wheels"][2]["Name"] = "Sidecar";
	Result<Ref<DataAsset>> const crowded = Load(MakeVehicleDocument(tooMany));
	REQUIRE(crowded.IsError());
	CHECK(crowded.GetError().find("Wheels") != std::string::npos);
	Json flat = SerializeFields<StructTraits<Vehicle>>(Vehicle{});
	flat["Wheels"][1]["Radius"] = 0.0;
	Result<Ref<DataAsset>> const tyreless = Load(MakeVehicleDocument(flat));
	REQUIRE(tyreless.IsError());
	CHECK(tyreless.GetError().find("Radius") != std::string::npos);

	// What only the automatic gearbox needs is not asked of a manual one; directions need not be of length 1, nor square.
	Vehicle manual;
	manual.Transmission.Mode = VehicleTransmissionMode::Manual;
	manual.Transmission.ShiftUpRPM = 20000.0f;
	CHECK(ValidateVehicle(manual).IsOk());
	Vehicle loose;
	loose.Up = glm::vec3(0.0f, 0.5f, 0.0f);
	loose.Forward = glm::vec3(0.0f, 0.3f, -0.8f);
	loose.Wheels[1].SuspensionDirection = glm::vec3(0.0f, -0.2f, 0.0f);
	CHECK(ValidateVehicle(loose).IsOk());
	// Names differ in more than letter case.
	Vehicle named;
	named.Wheels[1].Name = "Front 2";
	CHECK(ValidateVehicle(named).IsOk());
	CHECK(IsSameVehicleWheelName("Rear", "rEAR"));
	CHECK_FALSE(IsSameVehicleWheelName("Rear", "Rear "));
}

TEST_CASE("VehicleAsset: what the physics world refuses, the definition refuses first, so that a definition that loads makes a vehicle")
{
	Testing::PhysicsSystemScope const physics;
	UUID const bike(77);
	auto const added = [&bike](Vehicle const& vehicle)
	{
		Result<Scope<PhysicsScene>> world = PhysicsScene::Create({});
		REQUIRE(world.IsOk());
		REQUIRE(world.GetValue()->AddBody(MakeChassis(bike)).IsOk());
		return world.GetValue()->AddVehicle(MakeVehicleDesc(bike, vehicle));
	};
	CHECK(added(Vehicle{}).IsOk());
	// Each refusal of the world that the field tables do not cover is a refusal of Validate.
	std::array<Spoil, 7> const worldRefusals = {{
		{"SuspensionMaxLength",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Wheels[1].SuspensionMaxLength = 0.1f;
		 }},
		{"SuspensionDirection",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Wheels[0].SuspensionDirection = glm::vec3(0.0f);
		 }},
		{"SteeringAxis",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Wheels[1].SteeringAxis = glm::vec3(0.0f);
		 }},
		{"Up, Forward",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Forward = glm::vec3(0.0f, 1.0f, 0.0005f);
		 }},
		{"MinRPM",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Engine.MinRPM = 12000.0f;
		 }},
		{"ShiftDownRPM",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Transmission.ShiftDownRPM = 9000.0f;
		 }},
		{"ShiftUpRPM",
	     [](Vehicle& vehicle)
	     {
			 vehicle.Transmission.ShiftUpRPM = 9999.0f;
			 vehicle.Engine.MaxRPM = 9000.0f;
		 }},
	}};
	for (Spoil const& spoil : worldRefusals)
	{
		CAPTURE(spoil.Field);
		Vehicle vehicle;
		spoil.Apply(vehicle);
		CHECK(ErrorOf(added(vehicle)).find(spoil.Field) != std::string::npos);
		CHECK(ErrorOf(ValidateVehicle(vehicle)).find(spoil.Field) != std::string::npos);
	}
	// Parallel within a thousandth of a radian, both refuse; just outside it, both take it.
	Vehicle nearly;
	nearly.Forward = glm::vec3(0.0f, 1.0f, 0.002f);
	CHECK(ValidateVehicle(nearly).IsOk());
	CHECK(added(nearly).IsOk());
	Vehicle gearless;
	gearless.Transmission.GearRatios.clear();
	CHECK(ErrorOf(added(gearless)).find("GearRatios") != std::string::npos);
	CHECK(ErrorOf(ValidateVehicle(gearless)).find("Transmission.GearRatios") != std::string::npos);
}

TEST_CASE("VehicleAsset: damaged definitions that load make a vehicle that rides")
{
	Testing::PhysicsSystemScope const physics;
	std::vector<std::string> warnings;
	DeserializationContext const context = Testing::MakeTolerantContext(warnings);
	DataAssetType const& type = GetVehicleType();
	UUID const ground(1);
	UUID const bike(2);
	VehicleInputs inputs;
	inputs.Throttle = 0.7f;
	inputs.FrontBrake = 0.2f;
	inputs.RearBrake = 0.1f;
	inputs.Lean = -0.4f;

	for (std::string_view const file : {"Assets/Vehicles/Bike.svehicle", "Assets/Vehicles/ZUp.svehicle"})
	{
		CAPTURE(file);
		Json const original = Testing::LoadFeatureTestDocument(file);
		Testing::DocumentMutator mutator(file.size() * 31u);
		int ridden = 0;
		int refused = 0;
		for (int round = 0, rounds = Testing::GetFuzzRounds(60); round < rounds; round++)
		{
			CAPTURE(round);
			Result<Ref<DataAsset>> asset = DataAssetSerializer::Deserialize(type, mutator.Damage(original, round), context);
			if (!asset)
			{
				refused++;
				continue;
			}
			Ref<TypedDataAsset<Vehicle>> const loaded = std::static_pointer_cast<TypedDataAsset<Vehicle>>(asset.GetValue());
			Vehicle const& definition = loaded->GetData();
			Result<Scope<PhysicsScene>> created = PhysicsScene::Create({});
			REQUIRE(created.IsOk());
			PhysicsScene& world = *created.GetValue();
			REQUIRE(world.AddBody(MakeGround(ground)).IsOk());
			REQUIRE(world.AddBody(MakeChassis(bike)).IsOk());
			Result<void> const made = world.AddVehicle(MakeVehicleDesc(bike, definition));
			REQUIRE_MESSAGE(made.IsOk(), ErrorOf(made));
			world.SetVehicleInputs(bike, inputs);
			for (int step = 0; step < 20; step++)
			{
				world.Step(1.0f / 60.0f);
			}
			std::optional<VehicleState> const state = world.GetVehicleState(bike);
			REQUIRE(state);
			CHECK(std::isfinite(state->EngineSpeed));
			CHECK(std::isfinite(state->ForwardSpeed));
			std::optional<BodyTransform> const chassis = world.GetBodyTransform(bike);
			REQUIRE(chassis);
			CHECK(std::isfinite(chassis->Position.x + chassis->Position.y + chassis->Position.z));
			ridden++;
		}
		CHECK(ridden > 0);
		CHECK(refused > 0);
	}
}

TEST_CASE("VehicleAsset: curves are sampled at their keys and at 15 points between, as Jolt's linear curves take them")
{
	// Linear keys: each key, then 15 evenly between it and the next.
	Curve tyre;
	tyre.Keys = {Key(0.0f, 0.0f), Key(0.08f, 1.2f), Key(0.4f, 1.0f)};
	VehicleCurve const points = SampleVehicleCurve(tyre);
	REQUIRE(points.size() == 33);
	CHECK(points[0] == glm::vec2(0.0f, 0.0f));
	CHECK(points[16] == glm::vec2(0.08f, 1.2f));
	CHECK(points[32] == glm::vec2(0.4f, 1.0f));
	CHECK(points[8].x == doctest::Approx(0.04f));
	CHECK(points[8].y == doctest::Approx(0.6f));
	for (size_t index = 1; index < points.size(); index++)
	{
		CHECK(points[index].x > points[index - 1].x);
	}

	// A cubic segment is followed through Curve::Evaluate; the input is scaled (an engine's speed over its largest).
	Curve torque;
	torque.Keys = {Key(0.0f, 0.6f, CurveInterpolation::Cubic), Key(6000.0f, 1.0f, CurveInterpolation::Cubic), Key(12000.0f, 0.7f)};
	float const scale = 1.0f / 12000.0f;
	VehicleCurve const engine = SampleVehicleCurve(torque, scale);
	REQUIRE(engine.size() == 33);
	CHECK(engine.back().x == doctest::Approx(1.0f));
	for (int between = 1; between <= 15; between++)
	{
		double const time = 6000.0 * between / 16.0;
		CAPTURE(between);
		CHECK(engine[static_cast<size_t>(between)].x == doctest::Approx(time * scale));
		CHECK(engine[static_cast<size_t>(between)].y == torque.Evaluate(time));
	}

	// A jump keeps both values at its time; a Constant segment holds its value until the next key.
	Curve stepped;
	stepped.Keys = {Key(0.0f, 0.5f, CurveInterpolation::Constant), Key(1.0f, 0.5f), Key(1.0f, 0.9f), Key(2.0f, 0.9f)};
	VehicleCurve const jump = SampleVehicleCurve(stepped);
	REQUIRE(jump.size() == 1 + 15 + 1 + 1 + 15 + 1);
	CHECK(jump[16] == glm::vec2(1.0f, 0.5f));
	CHECK(jump[17] == glm::vec2(1.0f, 0.9f));
	CHECK(jump[8].y == 0.5f);

	// Values are kept within 0 and MaxVehicleCurveValue.
	Curve wild;
	wild.Keys = {Key(0.0f, -2.0f), Key(1.0f, 1.0e6f)};
	VehicleCurve const kept = SampleVehicleCurve(wild);
	CHECK(kept.front().y == 0.0f);
	CHECK(kept.back().y == MaxVehicleCurveValue);

	// One key is its value everywhere; no key is 0 everywhere, as the curve evaluates.
	Curve single;
	single.Keys = {Key(3.0f, 0.75f)};
	CHECK(SampleVehicleCurve(single) == VehicleCurve{glm::vec2(3.0f, 0.75f)});
	CHECK(SampleVehicleCurve(Curve{}) == VehicleCurve{glm::vec2(0.0f, 0.0f)});

	// A curve of the most keys a file holds stays within the points a vehicle takes; one made longer in code is cut there.
	Curve longest;
	for (size_t index = 0; index < MaxCurveKeys; index++)
	{
		longest.Keys.push_back(Key(static_cast<float>(index), 1.0f));
	}
	CHECK(SampleVehicleCurve(longest).size() == MaxCurveKeys + 15 * (MaxCurveKeys - 1));
	for (size_t index = MaxCurveKeys; index < 2 * MaxCurveKeys; index++)
	{
		longest.Keys.push_back(Key(static_cast<float>(index), 1.0f));
	}
	CHECK(SampleVehicleCurve(longest).size() == MaxVehicleCurvePoints);
}
