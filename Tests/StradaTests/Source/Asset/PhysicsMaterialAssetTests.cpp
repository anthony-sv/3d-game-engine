#include "TestUtilities.h"

#include "Strada/Asset/Asset.h"
#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/PhysicsMaterial.h"
#include "Strada/Asset/PhysicsMaterialAsset.h"
#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	DataAssetType const& GetPhysicsMaterialType()
	{
		DataAssetType const* type = DataAssetRegistry::FindByName("PhysicsMaterial");
		REQUIRE(type != nullptr);
		return *type;
	}

	Json MakePhysicsMaterialDocument(Json content)
	{
		Json document = Json::object();
		document["Strada"] = MakeFileHeader("PhysicsMaterial", 1);
		document["PhysicsMaterial"] = std::move(content);
		return document;
	}

	std::string GetErrorOf(Result<Ref<DataAsset>> const& result)
	{
		REQUIRE(result.IsError());
		return result.GetError();
	}

	constexpr std::array<PhysicsMaterialCombine, 5> AllModes = {PhysicsMaterialCombine::GeometricMean, PhysicsMaterialCombine::Average,
	                                                            PhysicsMaterialCombine::Minimum, PhysicsMaterialCombine::Multiply,
	                                                            PhysicsMaterialCombine::Maximum};
}

TEST_CASE("PhysicsMaterialAsset: the engine registers PhysicsMaterial, its .sphysmat files and its commands' names")
{
	DataAssetType const& type = GetPhysicsMaterialType();
	CHECK(type.IsEngineType());
	CHECK(DataAssetRegistry::Find<PhysicsMaterial>() == &type);
	CHECK(TypedDataAsset<PhysicsMaterial>::GetStaticType() == type.GetAssetType());
	CHECK(type.GetExtension() == ".sphysmat");
	CHECK(type.GetFormatVersion() == 1);
	CHECK(DataAssetRegistry::FindByExtension(".SPHYSMAT") == &type);
	CHECK(GetAssetTypeForExtension(".sphysmat") == type.GetAssetType());
	CHECK(std::string_view(AssetTypeToString(type.GetAssetType())) == "PhysicsMaterial");
	CHECK(AssetTypeFromString("PhysicsMaterial") == type.GetAssetType());
	CHECK(type.GetCommandDomain() == "physics-material");
	CHECK(type.GetCommandParameter() == "physicsMaterial");

	// The fields the inspector and the commands' schema show, in file order, each documented.
	std::vector<std::string_view> names;
	for (FieldDescriptor const& field : type.GetFields())
	{
		names.push_back(field.Name);
		CHECK_FALSE(field.Hints.Description.empty());
	}
	CHECK(names == std::vector<std::string_view>{"Friction", "Restitution", "FrictionCombine", "RestitutionCombine", "RollingResistance",
	                                             "Surface", "ImpactSound", "ImpactParticles"});
	CHECK(type.GetFields()[6].Hints.AssetTypeName == "AudioClip");
	CHECK(type.GetFields()[7].Hints.AssetTypeName == "Prefab");
	// The value-initialized material, a new asset's, is what a collider without a material does: its default friction and
	// restitution, combined by the geometric mean and the maximum.
	CHECK(type.GetDefaultFields() == Json::parse(R"({
		"Friction": 0.6000000238418579, "Restitution": 0.0, "FrictionCombine": "GeometricMean", "RestitutionCombine": "Maximum",
		"RollingResistance": 0.0, "Surface": "", "ImpactSound": "0", "ImpactParticles": "0"
	})"));
}

TEST_CASE("PhysicsMaterialAsset: files round trip, and values outside their ranges are refused")
{
	PhysicsMaterial material;
	material.Friction = 0.9f;
	material.Restitution = 0.25f;
	material.FrictionCombine = PhysicsMaterialCombine::Minimum;
	material.RestitutionCombine = PhysicsMaterialCombine::Multiply;
	material.RollingResistance = 0.02f;
	material.Surface = "Gravel";
	material.ImpactSound = AssetHandle(UUID(41));
	material.ImpactParticles = AssetHandle(UUID(42));

	Ref<DataAsset> const asset = GetPhysicsMaterialType().CreateAsset();
	auto const typed = std::static_pointer_cast<TypedDataAsset<PhysicsMaterial>>(asset);
	REQUIRE(typed->SetData(material).IsOk());
	Json const document = DataAssetSerializer::Serialize(*asset);
	CHECK(document["Strada"] == Json::parse(R"({ "Version": 1, "Type": "PhysicsMaterial" })"));
	CHECK(document["PhysicsMaterial"]["FrictionCombine"] == "Minimum");
	CHECK(document["PhysicsMaterial"]["RestitutionCombine"] == "Multiply");
	CHECK(document["PhysicsMaterial"]["Surface"] == "Gravel");
	CHECK(document["PhysicsMaterial"]["ImpactSound"] == "41");
	CHECK(document["PhysicsMaterial"]["ImpactParticles"] == "42");

	Testing::TemporaryDirectory directory;
	std::filesystem::path const path = directory.GetPath() / "Gravel.sphysmat";
	REQUIRE(DataAssetSerializer::SaveToFile(*asset, path).IsOk());
	Result<Ref<DataAsset>> loaded = DataAssetSerializer::LoadFromFile(GetPhysicsMaterialType(), path, {});
	REQUIRE_MESSAGE(loaded.IsOk(), (loaded ? std::string() : loaded.GetError()));
	CHECK(std::static_pointer_cast<TypedDataAsset<PhysicsMaterial>>(loaded.GetValue())->GetData() == material);

	// Missing fields keep their defaults.
	Result<Ref<DataAsset>> sparse = DataAssetSerializer::Deserialize(
		GetPhysicsMaterialType(), MakePhysicsMaterialDocument(Json::parse(R"({ "Friction": 0.1, "Surface": "Ice" })")), {});
	REQUIRE(sparse.IsOk());
	PhysicsMaterial expected;
	expected.Friction = 0.1f;
	expected.Surface = "Ice";
	CHECK(std::static_pointer_cast<TypedDataAsset<PhysicsMaterial>>(sparse.GetValue())->GetData() == expected);

	// What a material cannot be is refused, naming the field.
	CHECK(GetErrorOf(DataAssetSerializer::Deserialize(GetPhysicsMaterialType(),
	                                                  MakePhysicsMaterialDocument(Json::parse(R"({ "Friction": -0.5 })")), {}))
	          .find("Friction") != std::string::npos);
	CHECK(GetErrorOf(DataAssetSerializer::Deserialize(GetPhysicsMaterialType(),
	                                                  MakePhysicsMaterialDocument(Json::parse(R"({ "Restitution": 1.5 })")), {}))
	          .find("Restitution") != std::string::npos);
	CHECK(GetErrorOf(DataAssetSerializer::Deserialize(GetPhysicsMaterialType(),
	                                                  MakePhysicsMaterialDocument(Json::parse(R"({ "RollingResistance": -1 })")), {}))
	          .find("RollingResistance") != std::string::npos);
	CHECK(GetErrorOf(DataAssetSerializer::Deserialize(GetPhysicsMaterialType(),
	                                                  MakePhysicsMaterialDocument(Json::parse(R"({ "FrictionCombine": "Harmonic" })")), {}))
	          .find("FrictionCombine: expected one of GeometricMean|Average|Minimum|Multiply|Maximum") != std::string::npos);
}

TEST_CASE("PhysicsMaterialAsset: the two sides' modes combine by the later one, symmetrically, and as before without materials")
{
	// Each mode on its own.
	CHECK(CombinePhysicsMaterialValues(PhysicsMaterialCombine::GeometricMean, 0.25f, PhysicsMaterialCombine::GeometricMean, 0.64f) ==
	      doctest::Approx(0.4f));
	CHECK(CombinePhysicsMaterialValues(PhysicsMaterialCombine::Average, 0.2f, PhysicsMaterialCombine::Average, 0.6f) ==
	      doctest::Approx(0.4f));
	CHECK(CombinePhysicsMaterialValues(PhysicsMaterialCombine::Minimum, 0.2f, PhysicsMaterialCombine::Minimum, 0.6f) == 0.2f);
	CHECK(CombinePhysicsMaterialValues(PhysicsMaterialCombine::Multiply, 0.5f, PhysicsMaterialCombine::Multiply, 0.6f) ==
	      doctest::Approx(0.3f));
	CHECK(CombinePhysicsMaterialValues(PhysicsMaterialCombine::Maximum, 0.2f, PhysicsMaterialCombine::Maximum, 0.6f) == 0.6f);

	// When the sides name different modes, the later one in the enum's order wins, whichever side names it.
	CHECK(CombinePhysicsMaterialValues(PhysicsMaterialCombine::GeometricMean, 0.2f, PhysicsMaterialCombine::Minimum, 0.6f) == 0.2f);
	CHECK(CombinePhysicsMaterialValues(PhysicsMaterialCombine::Multiply, 0.5f, PhysicsMaterialCombine::Average, 0.6f) ==
	      doctest::Approx(0.3f));
	CHECK(CombinePhysicsMaterialValues(PhysicsMaterialCombine::Minimum, 0.2f, PhysicsMaterialCombine::Maximum, 0.6f) == 0.6f);

	// Symmetric in the two sides, to the bit, for every pair of modes.
	std::array<float, 5> const values = {0.0f, 0.1f, 0.6f, 1.0f, 2.5f};
	for (PhysicsMaterialCombine const firstMode : AllModes)
	{
		for (PhysicsMaterialCombine const secondMode : AllModes)
		{
			for (float const first : values)
			{
				for (float const second : values)
				{
					float const forward = CombinePhysicsMaterialValues(firstMode, first, secondMode, second);
					float const backward = CombinePhysicsMaterialValues(secondMode, second, firstMode, first);
					CHECK(std::bit_cast<uint32_t>(forward) == std::bit_cast<uint32_t>(backward));
					CHECK(std::isfinite(forward));
					CHECK(forward >= 0.0f);
				}
			}
		}
	}

	// Without materials the collider's own modes are GeometricMean for friction and Maximum for restitution: the very
	// expressions the contact listener used before materials, so such scenes simulate bit for bit as they did.
	PhysicsMaterial const none;
	for (float const first : values)
	{
		for (float const second : values)
		{
			CHECK(std::bit_cast<uint32_t>(CombinePhysicsMaterialValues(none.FrictionCombine, first, none.FrictionCombine, second)) ==
			      std::bit_cast<uint32_t>(std::sqrt(first * second)));
			CHECK(std::bit_cast<uint32_t>(CombinePhysicsMaterialValues(none.RestitutionCombine, first, none.RestitutionCombine, second)) ==
			      std::bit_cast<uint32_t>(std::max(first, second)));
		}
	}
}

TEST_CASE("PhysicsMaterialAsset: the asset manager loads the FeatureTest project's material with its references")
{
	Testing::TemporaryDirectory directory;
	Testing::AssetManagerScope const assets;
	std::filesystem::path const project = FileSystem::PathFromUtf8(STRADA_FEATURE_TEST_DIR);
	std::filesystem::path const copy = directory.GetPath() / "PhysicsMaterials" / "Rubber.sphysmat";
	REQUIRE(FileSystem::CreateDirectories(copy.parent_path()).IsOk());
	Result<std::string> const seed = FileSystem::ReadTextFile(project / "Assets" / "PhysicsMaterials" / "Rubber.sphysmat");
	REQUIRE(seed.IsOk());
	REQUIRE(FileSystem::WriteTextFile(copy, seed.GetValue()).IsOk());
	REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());

	AssetHandle const handle = AssetManager::FindByPath("PhysicsMaterials/Rubber.sphysmat");
	REQUIRE(handle.IsValid());
	CHECK(AssetManager::GetAssetType(handle) == GetPhysicsMaterialType().GetAssetType());
	Ref<TypedDataAsset<PhysicsMaterial>> const material = AssetManager::GetAsset<TypedDataAsset<PhysicsMaterial>>(handle);
	REQUIRE(material != nullptr);
	PhysicsMaterial const& data = material->GetData();
	CHECK(data.Surface == "Rubber");
	CHECK(data.FrictionCombine != PhysicsMaterialCombine::GeometricMean);
	CHECK(data.RestitutionCombine != PhysicsMaterialCombine::Maximum);
	// The sound and the prefab are handles, resolved by whoever plays or spawns them (they are not in this directory).
	CHECK(data.ImpactSound.IsValid());
	CHECK(data.ImpactParticles.IsValid());
}
