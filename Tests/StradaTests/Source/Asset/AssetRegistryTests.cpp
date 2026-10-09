#include "TestUtilities.h"

#include "Strada/Asset/AssetRegistry.h"

#include <doctest/doctest.h>

using namespace Strada;

namespace
{
	AssetMetadata MakeAsset(uint64_t handle, AssetType type, std::string path)
	{
		AssetMetadata metadata;
		metadata.Handle = AssetHandle(UUID(handle));
		metadata.Type = type;
		metadata.Path = std::move(path);
		return metadata;
	}
}

TEST_CASE("AssetRegistry: paths are normalized and validated")
{
	CHECK(NormalizeAssetPath("Meshes/Cube.glb").GetValue() == "Meshes/Cube.glb");
	CHECK(NormalizeAssetPath("Meshes\\Sub\\Cube.glb").GetValue() == "Meshes/Sub/Cube.glb");
	CHECK(NormalizeAssetPath("./Meshes//Cube.glb").GetValue() == "Meshes/Cube.glb");
	CHECK(NormalizeAssetPath("Textures/Wood Floor.png").GetValue() == "Textures/Wood Floor.png");

	CHECK(NormalizeAssetPath("").IsError());
	CHECK(NormalizeAssetPath("./").IsError());
	CHECK(NormalizeAssetPath("../Outside.png").IsError());
	CHECK(NormalizeAssetPath("Meshes/../../Outside.png").IsError());
	CHECK(NormalizeAssetPath("/Absolute.png").IsError());
	CHECK(NormalizeAssetPath("C:/Absolute.png").IsError());
	CHECK(NormalizeAssetPath("\\\\server\\share\\file.png").IsError());
	CHECK(NormalizeAssetPath("What?.png").IsError());
	CHECK(NormalizeAssetPath("Tab\tName.png").IsError());
}

TEST_CASE("AssetRegistry: registration enforces unique handles and paths")
{
	AssetRegistry registry;
	REQUIRE(registry.Add(MakeAsset(5000, AssetType::Mesh, "Meshes/Cube.glb")).IsOk());
	CHECK(registry.Contains(AssetHandle(UUID(5000))));
	CHECK(registry.FindByPath("Meshes/Cube.glb") == AssetHandle(UUID(5000)));
	CHECK(registry.Find(AssetHandle(UUID(5000)))->Type == AssetType::Mesh);

	CHECK(registry.Add(MakeAsset(5000, AssetType::Texture, "Other.png")).IsError());
	CHECK(registry.Add(MakeAsset(5001, AssetType::Texture, "Meshes/Cube.glb")).IsError());
	CHECK(registry.Add(MakeAsset(5002, AssetType::None, "None.png")).IsError());
	CHECK(registry.Add(MakeAsset(0, AssetType::Texture, "Zero.png")).IsError());
	CHECK(registry.Add(MakeAsset(7, AssetType::Texture, "Reserved.png")).IsError());
	CHECK(registry.Add(MakeAsset(5003, AssetType::Texture, "Not\\Normalized.png")).IsError());
	CHECK(registry.GetCount() == 1);

	REQUIRE(registry.SetPath(AssetHandle(UUID(5000)), "Models/Cube.glb").IsOk());
	CHECK_FALSE(registry.FindByPath("Meshes/Cube.glb").IsValid());
	CHECK(registry.FindByPath("Models/Cube.glb") == AssetHandle(UUID(5000)));
	REQUIRE(registry.Add(MakeAsset(5004, AssetType::Texture, "Wood.png")).IsOk());
	CHECK(registry.SetPath(AssetHandle(UUID(5000)), "Wood.png").IsError());
	REQUIRE(registry.SetType(AssetHandle(UUID(5004)), AssetType::Environment).IsOk());
	CHECK(registry.Find(AssetHandle(UUID(5004)))->Type == AssetType::Environment);
	CHECK(registry.SetType(AssetHandle(UUID(5004)), AssetType::None).IsError());

	CHECK(registry.Remove(AssetHandle(UUID(5000))));
	CHECK_FALSE(registry.Remove(AssetHandle(UUID(5000))));
	CHECK_FALSE(registry.FindByPath("Models/Cube.glb").IsValid());
}

TEST_CASE("AssetRegistry: files round trip sorted by path")
{
	Testing::TemporaryDirectory directory;
	AssetRegistry registry;
	REQUIRE(registry.Add(MakeAsset(9001, AssetType::Texture, "b/Wood.png")).IsOk());
	REQUIRE(registry.Add(MakeAsset(9002, AssetType::Mesh, "a/Cube.glb")).IsOk());

	Json const json = registry.Serialize();
	CHECK(json["Strada"]["Type"] == "AssetRegistry");
	REQUIRE(json["Assets"].size() == 2);
	CHECK(json["Assets"][0]["Path"] == "a/Cube.glb");
	CHECK(json["Assets"][0]["Handle"] == "9002");
	CHECK(json["Assets"][0]["Type"] == "Mesh");

	std::filesystem::path const path = directory.GetPath() / "AssetRegistry.sreg";
	REQUIRE(registry.SaveToFile(path).IsOk());
	Result<AssetRegistry> loaded = AssetRegistry::LoadFromFile(path, DeserializationContext{});
	REQUIRE(loaded.IsOk());
	CHECK(loaded.GetValue().GetCount() == 2);
	CHECK(loaded.GetValue().FindByPath("b/Wood.png") == AssetHandle(UUID(9001)));
	CHECK(loaded.GetValue().Serialize() == json);
}

TEST_CASE("AssetRegistry: invalid entries are skipped with warnings, structural errors fail")
{
	Json document = Json::object();
	document["Strada"] = MakeFileHeader("AssetRegistry", AssetRegistry::FormatVersion);
	document["Assets"] = Json::array({
		Json::object({{"Handle", "9001"}, {"Type", "Texture"}, {"Path", "Wood.png"}}),
		Json::object({{"Handle", "9002"}, {"Type", "Hologram"}, {"Path", "Unknown.xyz"}}),
		Json::object({{"Handle", "9001"}, {"Type", "Texture"}, {"Path", "Duplicate.png"}}),
		Json::object({{"Handle", "abc"}, {"Type", "Texture"}, {"Path", "BadHandle.png"}}),
		Json::object({{"Type", "Texture"}, {"Path", "NoHandle.png"}}),
		Json(42),
	});

	std::vector<std::string> warnings;
	DeserializationContext context;
	context.Warnings = &warnings;
	Result<AssetRegistry> registry = AssetRegistry::Deserialize(document, context);
	REQUIRE(registry.IsOk());
	CHECK(registry.GetValue().GetCount() == 1);
	CHECK(warnings.size() == 5);

	CHECK(AssetRegistry::Deserialize(Json::object(), context).IsError());
	Json noAssets = Json::object();
	noAssets["Strada"] = MakeFileHeader("AssetRegistry", AssetRegistry::FormatVersion);
	CHECK(AssetRegistry::Deserialize(noAssets, context).IsError());
}
