#include "Physics/PhysicsTestUtilities.h"
#include "TestUtilities.h"

#include "Strada/Asset/Asset.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/Vehicle.h"
#include "Strada/Asset/VehicleAsset.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Serialization/StructSerialization.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <filesystem>
#include <string>

using namespace Strada;

// The engine's NeutralDamping in the Vehicle definition (Docs/Architecture.md §7.3 and §8, "The engine in neutral"): a
// compatible field of version 1.
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

	Result<Ref<DataAsset>> Load(Json const& document)
	{
		return DataAssetSerializer::Deserialize(GetVehicleType(), document, DeserializationContext{});
	}

	Vehicle const& DataOf(Result<Ref<DataAsset>> const& asset)
	{
		REQUIRE_MESSAGE(asset.IsOk(), (asset ? std::string() : asset.GetError()));
		return std::static_pointer_cast<TypedDataAsset<Vehicle>>(asset.GetValue())->GetData();
	}
}

TEST_CASE("VehicleAsset: a version-1 definition written before the engine's NeutralDamping reads it as 0 and saves it")
{
	std::filesystem::path const file = FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "DataAssets" / "Vehicle" / "Version1.svehicle";
	Result<std::string> const text = FileSystem::ReadTextFile(file);
	REQUIRE(text.IsOk());
	Json const document = Json::parse(text.GetValue());
	REQUIRE(document["Strada"]["Version"] == 1);
	REQUIRE(document["Vehicle"]["Engine"].contains("AngularDamping"));
	REQUIRE_FALSE(document["Vehicle"]["Engine"].contains("NeutralDamping"));

	Result<Ref<DataAsset>> const loaded = Load(document);
	Vehicle const& vehicle = DataOf(loaded);
	CHECK(vehicle.Engine.NeutralDamping == 0.0f);
	// Everything else as written: the definition is FeatureTest's Z-up bike as it was before the field.
	CHECK(vehicle.Up == glm::vec3(0.0f, 0.0f, 1.0f));
	CHECK(vehicle.Engine.MaxRPM == 10000.0f);
	Json const saved = DataAssetSerializer::Serialize(*loaded.GetValue());
	CHECK(saved["Strada"] == Json::parse(R"({ "Version": 1, "Type": "Vehicle" })"));
	CHECK(saved["Vehicle"]["Engine"]["NeutralDamping"] == 0.0);
	// The vehicle it makes runs its engine as built.
	CHECK(MakeVehicleDesc(UUID(1), vehicle).EngineNeutralDamping == 0.0f);
}

TEST_CASE("VehicleAsset: the engine's NeutralDamping saves and loads again, reaches the vehicle's values, and is refused out of "
          "its range by the definition and by the physics world, naming it")
{
	Vehicle vehicle;
	vehicle.Engine.NeutralDamping = 2.245f;
	Result<Ref<DataAsset>> const loaded = Load(MakeVehicleDocument(SerializeFields<StructTraits<Vehicle>>(vehicle)));
	CHECK(DataOf(loaded) == vehicle);
	Json const saved = DataAssetSerializer::Serialize(*loaded.GetValue());
	CHECK(saved["Vehicle"]["Engine"]["NeutralDamping"] == doctest::Approx(2.245));
	CHECK(DataOf(Load(saved)).Engine.NeutralDamping == 2.245f);
	CHECK(MakeVehicleDesc(UUID(1), vehicle).EngineNeutralDamping == 2.245f);

	// 0 to 100 a second, as AngularDamping.
	for (double const value : {0.0, 100.0})
	{
		Json content = SerializeFields<StructTraits<Vehicle>>(Vehicle{});
		content["Engine"]["NeutralDamping"] = value;
		CHECK(DataOf(Load(MakeVehicleDocument(content))).Engine.NeutralDamping == static_cast<float>(value));
	}
	for (double const value : {-0.1, 100.1})
	{
		CAPTURE(value);
		Json content = SerializeFields<StructTraits<Vehicle>>(Vehicle{});
		content["Engine"]["NeutralDamping"] = value;
		Result<Ref<DataAsset>> const refused = Load(MakeVehicleDocument(content));
		REQUIRE(refused.IsError());
		CHECK(refused.GetError().find("NeutralDamping") != std::string::npos);
	}

	// The world refuses what the definition refuses, naming the value.
	Testing::PhysicsSystemScope physics;
	Result<Scope<PhysicsScene>> created = PhysicsScene::Create({});
	REQUIRE(created.IsOk());
	PhysicsScene& world = *created.GetValue();
	BodyDesc chassis;
	chassis.Entity = UUID(2);
	chassis.Type = RigidBodyType::Dynamic;
	chassis.Mass = 250.0f;
	ColliderDesc box;
	box.HalfExtents = glm::vec3(0.25f, 0.5f, 0.6f);
	box.Offset = glm::vec3(0.0f, 0.75f, 0.0f);
	chassis.Colliders.push_back(box);
	REQUIRE(world.AddBody(chassis).IsOk());
	for (float const value : {-0.1f, 100.1f})
	{
		CAPTURE(value);
		VehicleDesc desc = MakeVehicleDesc(UUID(2), Vehicle{});
		desc.EngineNeutralDamping = value;
		Result<void> const refused = world.AddVehicle(desc);
		REQUIRE(refused.IsError());
		CHECK(refused.GetError().find("EngineNeutralDamping") != std::string::npos);
	}
	VehicleDesc desc = MakeVehicleDesc(UUID(2), vehicle);
	REQUIRE(world.AddVehicle(desc).IsOk());
	REQUIRE(world.FindVehicleSettings(UUID(2)) != nullptr);
	CHECK(world.FindVehicleSettings(UUID(2))->EngineNeutralDamping == 2.245f);
}
