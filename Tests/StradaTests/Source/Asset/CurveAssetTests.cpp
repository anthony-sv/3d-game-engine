#include "TestUtilities.h"

#include "Strada/Asset/Asset.h"
#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/CurveAsset.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	DataAssetType const& GetCurveType()
	{
		DataAssetType const* type = DataAssetRegistry::FindByName("Curve");
		REQUIRE(type != nullptr);
		return *type;
	}

	Json MakeCurveDocument(Json content)
	{
		Json document = Json::object();
		document["Strada"] = MakeFileHeader("Curve", 1);
		document["Curve"] = std::move(content);
		return document;
	}

	std::string GetErrorOf(Result<Ref<DataAsset>> const& result)
	{
		REQUIRE(result.IsError());
		return result.GetError();
	}
}

TEST_CASE("CurveAsset: the engine registers Curve, its .scurve files and its commands' names")
{
	DataAssetType const& type = GetCurveType();
	CHECK(type.IsEngineType());
	CHECK(DataAssetRegistry::Find<Curve>() == &type);
	CHECK(TypedDataAsset<Curve>::GetStaticType() == type.GetAssetType());
	CHECK(type.GetExtension() == ".scurve");
	CHECK(type.GetFormatVersion() == 1);
	CHECK(DataAssetRegistry::FindByExtension(".SCURVE") == &type);
	CHECK(GetAssetTypeForExtension(".scurve") == type.GetAssetType());
	CHECK(std::string_view(AssetTypeToString(type.GetAssetType())) == "Curve");
	CHECK(AssetTypeFromString("Curve") == type.GetAssetType());
	std::vector<std::string_view> const extensions = GetSupportedAssetExtensions();
	CHECK(std::ranges::find(extensions, ".scurve") != extensions.end());
	CHECK(type.GetCommandDomain() == "curve");
	CHECK(type.GetCommandParameter() == "curve");

	// The fields the inspector and the commands' schema show, in file order, each documented.
	std::vector<std::string_view> names;
	for (FieldDescriptor const& field : type.GetFields())
	{
		names.push_back(field.Name);
		CHECK_FALSE(field.Hints.Description.empty());
	}
	CHECK(names == std::vector<std::string_view>{"PreExtrapolation", "PostExtrapolation", "Keys"});
	CHECK(type.GetFields()[2].Hints.MaxCount == MaxCurveKeys);
	// The value-initialized curve, a new asset's: no key, held at both ends.
	CHECK(type.GetDefaultFields() == Json::parse(R"({ "PreExtrapolation": "Hold", "PostExtrapolation": "Hold", "Keys": [] })"));
}

TEST_CASE("CurveAsset: files round trip, and keys out of order or too many are refused")
{
	Curve curve;
	curve.PreExtrapolation = CurveExtrapolation::Linear;
	curve.PostExtrapolation = CurveExtrapolation::PingPong;
	CurveKey first;
	first.Time = -1.0f;
	first.Value = 2.0f;
	first.HasOut = true;
	first.Out = 0.5f;
	CurveKey second;
	second.Time = 3.0f;
	second.Value = -4.0f;
	second.Interpolation = CurveInterpolation::Constant;
	second.HasIn = true;
	second.In = 1.5f;
	curve.Keys = {first, second};

	Ref<DataAsset> const asset = GetCurveType().CreateAsset();
	auto const typed = std::static_pointer_cast<TypedDataAsset<Curve>>(asset);
	REQUIRE(typed->SetData(curve).IsOk());
	Json const document = DataAssetSerializer::Serialize(*asset);
	CHECK(document == Json::parse(R"({
		"Strada": { "Version": 1, "Type": "Curve" },
		"Curve": {
			"PreExtrapolation": "Linear",
			"PostExtrapolation": "PingPong",
			"Keys": [
				{ "Time": -1.0, "Value": 2.0, "Interpolation": "Cubic", "HasIn": false, "In": 0.0, "HasOut": true, "Out": 0.5 },
				{ "Time": 3.0, "Value": -4.0, "Interpolation": "Constant", "HasIn": true, "In": 1.5, "HasOut": false, "Out": 0.0 }
			]
		}
	})"));

	Testing::TemporaryDirectory directory;
	std::filesystem::path const path = directory.GetPath() / "Response.scurve";
	REQUIRE(DataAssetSerializer::SaveToFile(*asset, path).IsOk());
	Result<Ref<DataAsset>> loaded = DataAssetSerializer::LoadFromFile(GetCurveType(), path, {});
	REQUIRE_MESSAGE(loaded.IsOk(), (loaded ? std::string() : loaded.GetError()));
	CHECK(std::static_pointer_cast<TypedDataAsset<Curve>>(loaded.GetValue())->GetData() == curve);

	// Missing key fields keep their defaults (a Cubic key with automatic slopes).
	Result<Ref<DataAsset>> sparse = DataAssetSerializer::Deserialize(
		GetCurveType(), MakeCurveDocument(Json::parse(R"({ "Keys": [{ "Time": 0, "Value": 1 }, { "Time": 2, "Value": 3 }] })")), {});
	REQUIRE(sparse.IsOk());
	Ref<TypedDataAsset<Curve>> const sparseCurve = std::static_pointer_cast<TypedDataAsset<Curve>>(sparse.GetValue());
	Curve const& read = sparseCurve->GetData();
	REQUIRE(read.Keys.size() == 2);
	CHECK(read.Keys[1] == CurveKey{2.0f, 3.0f, CurveInterpolation::Cubic, false, 0.0f, false, 0.0f});
	CHECK(read.Evaluate(1.0) == 2.0f);

	// What the curve cannot be is refused, naming the type and the key.
	CHECK(GetErrorOf(DataAssetSerializer::Deserialize(
			  GetCurveType(), MakeCurveDocument(Json::parse(R"({ "Keys": [{ "Time": 1, "Value": 0 }, { "Time": 0.5, "Value": 0 }] })")),
			  {})) == "Curve: Keys[1].Time: keys are in time order: 0.5 comes after 1");
	CHECK(GetErrorOf(
			  DataAssetSerializer::Deserialize(GetCurveType(), MakeCurveDocument(Json::parse(R"({ "PostExtrapolation": "Mirror" })")), {}))
	          .find("PostExtrapolation: expected one of Hold|Loop|PingPong|Linear") != std::string::npos);
	Json many = Json::array();
	for (size_t index = 0; index <= MaxCurveKeys; index++)
	{
		many.push_back(Json::object({{"Time", static_cast<double>(index)}, {"Value", 0.0}}));
	}
	CHECK_FALSE(DataAssetSerializer::Deserialize(GetCurveType(), MakeCurveDocument(Json::object({{"Keys", many}})), {}).IsOk());
	many.erase(many.size() - 1);
	CHECK(DataAssetSerializer::Deserialize(GetCurveType(), MakeCurveDocument(Json::object({{"Keys", many}})), {}).IsOk());
}

TEST_CASE("CurveAsset: the asset manager loads the FeatureTest project's curve, which evaluates")
{
	Testing::TemporaryDirectory directory;
	Testing::AssetManagerScope const assets;
	std::filesystem::path const project = FileSystem::PathFromUtf8(STRADA_FEATURE_TEST_DIR);
	std::filesystem::path const copy = directory.GetPath() / "Curves" / "Response.scurve";
	REQUIRE(FileSystem::CreateDirectories(copy.parent_path()).IsOk());
	Result<std::string> const seed = FileSystem::ReadTextFile(project / "Assets" / "Curves" / "Response.scurve");
	REQUIRE(seed.IsOk());
	REQUIRE(FileSystem::WriteTextFile(copy, seed.GetValue()).IsOk());
	REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());

	AssetHandle const handle = AssetManager::FindByPath("Curves/Response.scurve");
	REQUIRE(handle.IsValid());
	CHECK(AssetManager::GetAssetType(handle) == GetCurveType().GetAssetType());
	Ref<TypedDataAsset<Curve>> const curve = AssetManager::GetAsset<TypedDataAsset<Curve>>(handle);
	REQUIRE(curve != nullptr);
	Curve const& data = curve->GetData();
	REQUIRE(data.Keys.size() >= 2);
	// It holds its first value up to its first key and evaluates between its keys.
	CHECK(data.Evaluate(static_cast<double>(data.Keys.front().Time)) == data.Keys.front().Value);
	CHECK(std::isfinite(data.Evaluate(0.5 * (static_cast<double>(data.Keys.front().Time) + static_cast<double>(data.Keys.back().Time)))));
}
