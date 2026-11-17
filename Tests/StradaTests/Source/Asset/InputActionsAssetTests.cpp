#include "TestUtilities.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/CurveAsset.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/InputActionMap.h"
#include "Strada/Asset/InputActionsAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/InputActions.h"
#include "Strada/Core/Log.h"
#include "Strada/Math/Curve.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	DataAssetType const& GetInputActionsType()
	{
		DataAssetType const* type = DataAssetRegistry::FindByName("InputActions");
		REQUIRE(type != nullptr);
		return *type;
	}

	Json MakeDocument(Json content)
	{
		Json document = Json::object();
		document["Strada"] = MakeFileHeader("InputActions", 1);
		document["InputActions"] = std::move(content);
		return document;
	}

	std::string LoadError(std::string_view content)
	{
		Result<Ref<DataAsset>> const result =
			DataAssetSerializer::Deserialize(GetInputActionsType(), MakeDocument(Json::parse(content)), {});
		REQUIRE(result.IsError());
		return result.GetError();
	}

	InputActionBinding Bound(std::string controls)
	{
		InputActionBinding binding;
		binding.Controls = std::move(controls);
		return binding;
	}

	InputAction MakeAction(std::string name, InputActionType type, std::vector<InputActionBinding> bindings)
	{
		InputAction action;
		action.Name = std::move(name);
		action.Type = type;
		action.Bindings = std::move(bindings);
		return action;
	}

	std::string ValidationError(InputActionMap const& map)
	{
		Result<void> const result = ValidateInputActionMap(map);
		REQUIRE(result.IsError());
		return result.GetError();
	}

	size_t CountLogged(uint64_t first, std::string_view text)
	{
		std::vector<LogEntry> const entries = Log::GetEntries(first);
		return static_cast<size_t>(std::count_if(entries.begin(), entries.end(),
		                                         [text](LogEntry const& entry)
		                                         {
													 return entry.Message.find(text) != std::string::npos;
												 }));
	}
}

TEST_CASE("InputActionsAsset: the engine registers InputActions, its .sinput files, its fields and its commands' names")
{
	DataAssetType const& type = GetInputActionsType();
	CHECK(type.IsEngineType());
	CHECK(DataAssetRegistry::Find<InputActionMap>() == &type);
	CHECK(TypedDataAsset<InputActionMap>::GetStaticType() == type.GetAssetType());
	CHECK(type.GetExtension() == ".sinput");
	CHECK(type.GetFormatVersion() == 1);
	CHECK(GetAssetTypeForExtension(".SINPUT") == type.GetAssetType());
	CHECK(type.GetCommandDomain() == "input-actions");
	CHECK(type.GetCommandParameter() == "inputActions");
	REQUIRE(type.GetFields().size() == 1);
	CHECK(type.GetFields()[0].Name == "Actions");
	CHECK(type.GetDefaultFields() == Json::parse(R"({ "Actions": [] })"));
}

TEST_CASE("InputActionsAsset: files round trip every field, and missing fields keep their defaults")
{
	InputActionMap map;
	InputActionBinding stick = Bound("Gamepad/LeftStick");
	stick.Gamepad = 3;
	stick.DeadZone = 0.15f;
	stick.ResponseCurve = AssetHandle(UUID(77));
	stick.Invert = true;
	stick.InvertY = true;
	stick.Scale = 0.5f;
	InputAction move = MakeAction("Move", InputActionType::Vector2, {Bound("Key/W Key/S Key/A Key/D"), stick});
	move.PressThreshold = 0.75f;
	move.ReleaseThreshold = 0.25f;
	map.Actions = {MakeAction("Jump", InputActionType::Button, {Bound("Key/Space"), Bound("Gamepad/A")}), move,
	               MakeAction("Steer", InputActionType::Axis, {Bound("Key/A Key/D"), Bound("")})};

	Ref<DataAsset> const asset = GetInputActionsType().CreateAsset();
	auto const typed = std::static_pointer_cast<TypedDataAsset<InputActionMap>>(asset);
	REQUIRE(typed->SetData(map).IsOk());
	Json const document = DataAssetSerializer::Serialize(*asset);
	CHECK(document["Strada"] == Json::parse(R"({ "Version": 1, "Type": "InputActions" })"));
	Json const& written = document["InputActions"]["Actions"][1];
	CHECK(written["Name"] == "Move");
	CHECK(written["Type"] == "Vector2");
	CHECK(written["Bindings"][1]["Controls"] == "Gamepad/LeftStick");
	CHECK(written["Bindings"][1]["ResponseCurve"] == "77");
	CHECK(written["Bindings"][1]["Gamepad"] == 3);

	Testing::TemporaryDirectory const directory;
	std::filesystem::path const path = directory.GetPath() / "Game.sinput";
	REQUIRE(DataAssetSerializer::SaveToFile(*asset, path).IsOk());
	Result<Ref<DataAsset>> loaded = DataAssetSerializer::LoadFromFile(GetInputActionsType(), path, {});
	REQUIRE_MESSAGE(loaded.IsOk(), (loaded ? std::string() : loaded.GetError()));
	CHECK(std::static_pointer_cast<TypedDataAsset<InputActionMap>>(loaded.GetValue())->GetData() == map);

	Result<Ref<DataAsset>> sparse = DataAssetSerializer::Deserialize(
		GetInputActionsType(),
		MakeDocument(Json::parse(R"({ "Actions": [ { "Name": "Fire", "Bindings": [ { "Controls": "Mouse/Left" } ] } ] })")), {});
	REQUIRE_MESSAGE(sparse.IsOk(), (sparse ? std::string() : sparse.GetError()));
	InputActionMap expected;
	expected.Actions = {MakeAction("Fire", InputActionType::Button, {Bound("Mouse/Left")})};
	CHECK(std::static_pointer_cast<TypedDataAsset<InputActionMap>>(sparse.GetValue())->GetData() == expected);
}

TEST_CASE("InputActionsAsset: validation refuses names, thresholds and controls that cannot be, naming the action and the binding")
{
	InputActionMap map;
	map.Actions = {MakeAction("Jump", InputActionType::Button, {}), MakeAction("jUMP", InputActionType::Axis, {})};
	CHECK(ValidationError(map).find("action 'jUMP': another action has that name") != std::string::npos);
	map.Actions = {MakeAction("", InputActionType::Button, {})};
	CHECK(ValidationError(map) == "an action needs a name");
	map.Actions = {MakeAction(std::string(65, 'n'), InputActionType::Button, {})};
	CHECK(ValidationError(map).find("at most 64 bytes") != std::string::npos);

	map.Actions = {MakeAction("Fire", InputActionType::Button, {})};
	map.Actions[0].ReleaseThreshold = 0.8f;
	CHECK(ValidationError(map).find("action 'Fire': thresholds") != std::string::npos);
	map.Actions[0].ReleaseThreshold = 0.0f;
	CHECK(ValidationError(map).find("action 'Fire': thresholds") != std::string::npos);

	map.Actions = {MakeAction("Fire", InputActionType::Button, {Bound("Key/F"), Bound("Key/Nope")})};
	CHECK(ValidationError(map) == "action 'Fire', binding 1: 'Key/Nope' names no control (Key/<key>, Mouse/<button>, Gamepad/<button, "
	                              "axis or stick>)");
	map.Actions = {MakeAction("Move", InputActionType::Vector2, {Bound("Key/A Key/D")})};
	CHECK(ValidationError(map).find("action 'Move', binding 0: a Vector2 action takes") != std::string::npos);
	map.Actions = {MakeAction("Steer", InputActionType::Axis, {Bound("Gamepad/LeftStick")})};
	CHECK(ValidationError(map).find("action 'Steer', binding 0: a stick gives") != std::string::npos);
	map.Actions = {MakeAction("Steer", InputActionType::Axis, {Bound("Key/A Key/B Key/C")})};
	CHECK(ValidationError(map).find("not 3") != std::string::npos);

	// What loads is exactly what validation accepts: the file is refused, naming the reason.
	CHECK(LoadError(R"({ "Actions": [ { "Name": "A" }, { "Name": "a" } ] })").find("another action has that name") != std::string::npos);
	CHECK(LoadError(R"({ "Actions": [ { "Name": "A", "Bindings": [ { "DeadZone": 1.0 } ] } ] })").find("DeadZone") != std::string::npos);
	CHECK(LoadError(R"({ "Actions": [ { "Name": "A", "Type": "Trigger" } ] })").find("Type") != std::string::npos);
	CHECK(LoadError(R"({ "Actions": [ { "Name": "A", "Bindings": [ { "Gamepad": 16 } ] } ] })").find("Gamepad") != std::string::npos);
}

TEST_CASE("InputActionsAsset: compiling resolves response curves exactly, and a curve that does not load is the straight line")
{
	Testing::AssetManagerScope const assets;
	Input::Reset();
	TypedDataAssetType<Curve> const* curveType = DataAssetRegistry::Find<Curve>();
	REQUIRE(curveType != nullptr);
	Ref<TypedDataAsset<Curve>> const curve = std::static_pointer_cast<TypedDataAsset<Curve>>(curveType->CreateAsset());
	Curve squareish;
	squareish.Keys = {CurveKey{0.0f, 0.0f}, CurveKey{1.0f, 1.0f}};
	squareish.Keys[0].HasOut = true;
	squareish.Keys[0].Out = 0.0f;
	squareish.Keys[1].HasIn = true;
	squareish.Keys[1].In = 2.0f;
	REQUIRE(curve->SetData(squareish).IsOk());
	AssetHandle const handle = AssetManager::AddMemoryAsset(curve, "Square.scurve");

	InputActionBinding throttle = Bound("Gamepad/RightTrigger");
	throttle.ResponseCurve = handle;
	InputActionBinding missing = Bound("Gamepad/LeftTrigger");
	missing.ResponseCurve = AssetHandle(UUID(424242));
	InputActionMap map;
	map.Actions = {MakeAction("Throttle", InputActionType::Button, {throttle, missing}),
	               MakeAction("Again", InputActionType::Button, {throttle})};

	uint64_t const logStart = Log::GetNextEntryIndex();
	std::vector<InputResponseCurveUse> curves;
	Result<Ref<InputActionSet const>> set = CompileInputActions(map, &curves);
	REQUIRE_MESSAGE(set.IsOk(), (set ? std::string() : set.GetError()));
	CHECK(CountLogged(logStart, "does not load; the binding reads as a straight line") == 1);
	REQUIRE(curves.size() == 2);
	CHECK(std::ranges::find(curves, InputResponseCurveUse{handle, curve->GetVersion()}) != curves.end());
	CHECK(std::ranges::find(curves, InputResponseCurveUse{AssetHandle(UUID(424242)), 0}) != curves.end());
	CHECK(GetInputResponseCurveVersion(handle) == curve->GetVersion());
	CHECK(GetInputResponseCurveVersion(AssetHandle(UUID(424242))) == 0);

	InputBindingSpecification const& compiled = set.GetValue()->GetActions()[0].Bindings[0];
	REQUIRE(compiled.Response);
	for (float const x : {0.0f, 0.1f, 0.25f, 0.5f, 0.8f, 1.0f})
	{
		CAPTURE(x);
		CHECK(compiled.Response(x) == squareish.Evaluate(x));
	}
	CHECK_FALSE(set.GetValue()->GetActions()[0].Bindings[1].Response);
	CHECK(set.GetValue()->GetActions()[0].Bindings[0].Controls == ParseInputControls("Gamepad/RightTrigger").GetValue());

	// The set keeps a copy: an edit of the curve reaches it only through a new set.
	uint64_t const compiledVersion = curve->GetVersion();
	Curve flat = squareish;
	flat.Keys[1].Value = 0.0f;
	REQUIRE(curve->SetData(flat).IsOk());
	CHECK(compiled.Response(0.5f) == squareish.Evaluate(0.5));
	CHECK(GetInputResponseCurveVersion(handle) != compiledVersion);

	// A map validation refuses does not compile.
	map.Actions[1].Name = "THROTTLE";
	CHECK(CompileInputActions(map).IsError());
	Input::Reset();
}

TEST_CASE("InputActionsAsset: the asset manager loads the FeatureTest project's actions, every field set away from its default")
{
	Testing::TemporaryDirectory const directory;
	Testing::AssetManagerScope const assets;
	std::filesystem::path const project = FileSystem::PathFromUtf8(STRADA_FEATURE_TEST_DIR);
	std::filesystem::path const copy = directory.GetPath() / "Input" / "FeatureTest.sinput";
	REQUIRE(FileSystem::CreateDirectories(copy.parent_path()).IsOk());
	Result<std::string> const seed = FileSystem::ReadTextFile(project / "Assets" / "Input" / "FeatureTest.sinput");
	REQUIRE(seed.IsOk());
	REQUIRE(FileSystem::WriteTextFile(copy, seed.GetValue()).IsOk());
	REQUIRE(AssetManager::OpenAssetDirectory(directory.GetPath()).IsOk());

	AssetHandle const handle = AssetManager::FindByPath("Input/FeatureTest.sinput");
	REQUIRE(handle.IsValid());
	Ref<TypedDataAsset<InputActionMap>> const asset = AssetManager::GetAsset<TypedDataAsset<InputActionMap>>(handle);
	REQUIRE(asset != nullptr);
	InputActionMap const& map = asset->GetData();
	REQUIRE_FALSE(map.Actions.empty());
	bool anyType = false;
	bool anyThresholds = false;
	bool anyBindingField = false;
	for (InputAction const& action : map.Actions)
	{
		anyType = anyType || action.Type != InputActionType::Button;
		anyThresholds = anyThresholds || (action.PressThreshold != 0.5f && action.ReleaseThreshold != 0.4f);
		for (InputActionBinding const& binding : action.Bindings)
		{
			anyBindingField =
				anyBindingField || (binding.Gamepad != AnyGamepad && binding.DeadZone != 0.0f && binding.ResponseCurve.IsValid() &&
			                        binding.Invert && binding.InvertY && binding.Scale != 1.0f);
		}
	}
	CHECK(anyType);
	CHECK(anyThresholds);
	CHECK(anyBindingField);
	CHECK(CompileInputActions(map).IsOk());
}
