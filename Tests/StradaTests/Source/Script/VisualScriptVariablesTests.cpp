#include "Strada/Asset/VisualScriptSerializer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Script/VisualScriptVariables.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	// A shared fixture document (content form), loaded strictly.
	VisualScriptDocument LoadFixture(std::string_view name)
	{
		std::filesystem::path const path =
			FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "VisualScripting" / "Documents" / FileSystem::PathFromUtf8(name);
		Result<std::string> text = FileSystem::ReadTextFile(path);
		REQUIRE_MESSAGE(text.IsOk(), text.GetError());
		Result<Json> content = ParseJson(text.GetValue());
		REQUIRE_MESSAGE(content.IsOk(), content.GetError());
		DeserializationContext context;
		context.UnknownFields = UnknownFieldPolicy::Warn;
		std::vector<VisualScriptIssue> issues;
		Result<VisualScriptDocument> document = VisualScriptSerializer::DeserializeDocument(content.GetValue(), context, &issues);
		REQUIRE_MESSAGE(document.IsOk(), document.GetError());
		REQUIRE(issues.empty());
		return document.TakeValue();
	}

	VisualScriptVariable MakeVariable(uint32_t id, std::string name, std::string type, Json value = nullptr, bool isPublic = true)
	{
		VisualScriptVariable variable;
		variable.ID = id;
		variable.Name = std::move(name);
		variable.Type = std::move(type);
		variable.Value = std::move(value);
		variable.Public = isPublic;
		return variable;
	}

	// The one public variable of a document, described.
	VisualScriptVariableDescription DescribeOne(std::string type, Json value, VisualScriptEnumLookup const& lookup = {})
	{
		VisualScriptDocument document;
		document.Variables.push_back(MakeVariable(1, "Value", std::move(type), std::move(value)));
		return DescribeVisualScriptVariables(document, lookup);
	}

	// The value a public variable of the type with that Value starts with on components.
	ScriptFieldValue GetStartValue(std::string type, Json value, VisualScriptEnumLookup const& lookup = {})
	{
		VisualScriptVariableDescription const description = DescribeOne(std::move(type), std::move(value), lookup);
		REQUIRE(description.ReadOnly.empty());
		REQUIRE(description.Editable.Fields.size() == 1);
		return description.Editable.Fields[0].DefaultValue;
	}

	std::vector<std::string> GetNames(std::vector<ScriptFieldInfo> const& fields)
	{
		std::vector<std::string> names;
		for (ScriptFieldInfo const& field : fields)
		{
			names.push_back(field.Name);
		}
		return names;
	}

	// The enums of the scripting runtime, as the runtime describes them.
	std::optional<VisualScriptEnumInfo> LookUpEnum(std::string_view typeId)
	{
		if (typeId == "Strada.ForceMode")
		{
			return VisualScriptEnumInfo{ScriptFieldType::Int32,
			                            {{"Force", ScriptFieldValue::FromInt32(0)},
			                             {"Impulse", ScriptFieldValue::FromInt32(1)},
			                             {"Acceleration", ScriptFieldValue::FromInt32(2)},
			                             {"VelocityChange", ScriptFieldValue::FromInt32(3)}},
			                            false};
		}
		if (typeId == "Game.Layers")
		{
			return VisualScriptEnumInfo{ScriptFieldType::UInt32,
			                            {{"None", ScriptFieldValue::FromUInt32(0)},
			                             {"Walls", ScriptFieldValue::FromUInt32(1)},
			                             {"Water", ScriptFieldValue::FromUInt32(2)},
			                             {"Air", ScriptFieldValue::FromUInt32(4)}},
			                            true};
		}
		if (typeId == "Game.Distance")
		{
			return VisualScriptEnumInfo{ScriptFieldType::Int64,
			                            {{"Near", ScriptFieldValue::FromInt64(-5)}, {"Far", ScriptFieldValue::FromInt64(9000000000)}},
			                            false};
		}
		// Descriptions no runtime gives: enums of a type that is no integer, and enumerators of another type than theirs.
		if (typeId == "Game.Fractional")
		{
			return VisualScriptEnumInfo{ScriptFieldType::Float, {{"Half", ScriptFieldValue::FromFloat(0.5f)}}, false};
		}
		if (typeId == "Game.Mixed")
		{
			return VisualScriptEnumInfo{ScriptFieldType::Int32, {{"Big", ScriptFieldValue::FromInt64(1)}}, false};
		}
		return std::nullopt;
	}

	ScriptFieldMap MakeMap(std::initializer_list<std::pair<std::string const, ScriptFieldValue>> values)
	{
		return ScriptFieldMap(values);
	}
}

TEST_CASE("VisualScriptVariables: the exposable types map to the script field types that store them")
{
	struct Row
	{
		std::string_view TypeId;
		ScriptFieldType FieldType;
		AssetType AcceptedAssetType;
	};
	constexpr std::array<Row, 23> Rows = {{
		{"bool", ScriptFieldType::Bool, AssetType::None},
		{"int", ScriptFieldType::Int32, AssetType::None},
		{"uint", ScriptFieldType::UInt32, AssetType::None},
		{"long", ScriptFieldType::Int64, AssetType::None},
		{"ulong", ScriptFieldType::UInt64, AssetType::None},
		{"float", ScriptFieldType::Float, AssetType::None},
		{"double", ScriptFieldType::Double, AssetType::None},
		{"string", ScriptFieldType::String, AssetType::None},
		{"Strada.Vector2", ScriptFieldType::Vector2, AssetType::None},
		{"Strada.Vector3", ScriptFieldType::Vector3, AssetType::None},
		{"Strada.Vector4", ScriptFieldType::Vector4, AssetType::None},
		{"Strada.Quaternion", ScriptFieldType::Quaternion, AssetType::None},
		{"Strada.Color", ScriptFieldType::Color, AssetType::None},
		{"Strada.Entity", ScriptFieldType::Entity, AssetType::None},
		{"Strada.Prefab", ScriptFieldType::Prefab, AssetType::None},
		{"Strada.AssetHandle", ScriptFieldType::Asset, AssetType::None},
		{"Strada.Mesh", ScriptFieldType::Asset, AssetType::Mesh},
		{"Strada.Material", ScriptFieldType::Asset, AssetType::Material},
		{"Strada.Texture", ScriptFieldType::Asset, AssetType::Texture},
		{"Strada.AudioClip", ScriptFieldType::Asset, AssetType::AudioClip},
		{"Strada.Font", ScriptFieldType::Asset, AssetType::Font},
		{"Strada.EnvironmentMap", ScriptFieldType::Asset, AssetType::Environment},
		{"Strada.VisualScript", ScriptFieldType::Asset, AssetType::VisualScript},
	}};
	for (Row const& row : Rows)
	{
		CAPTURE(row.TypeId);
		CHECK(FindVisualScriptExposableType(row.TypeId) ==
		      std::optional<VisualScriptExposableType>(VisualScriptExposableType{row.FieldType, row.AcceptedAssetType}));

		// A public variable of the type is a field of that type, accepting that type of asset.
		VisualScriptVariableDescription const description = DescribeOne(std::string(row.TypeId), nullptr);
		REQUIRE(description.Editable.Fields.size() == 1);
		CHECK(description.Editable.Fields[0].Type == row.FieldType);
		CHECK(description.Editable.Fields[0].AcceptedAssetType == row.AcceptedAssetType);
		CHECK(description.ReadOnly.empty());
	}

	// Enums are known to the scripting runtime only; script classes, components, other structs and classes, arrays and
	// System.Object are not exposable; type IDs are case-sensitive.
	for (std::string_view const typeId :
	     {"Strada.ForceMode", "Strada.KeyCode", "Strada.Matrix4", "Strada.RaycastHit", "Strada.RigidBodyComponent", "Strada.Asset",
	      "Strada.Entity[]", "System.Object", "Game.Player", "exec", "*", "", "Float", "strada.vector3", "Strada.Vector3 "})
	{
		CAPTURE(typeId);
		CHECK_FALSE(FindVisualScriptExposableType(typeId).has_value());
	}
}

TEST_CASE("VisualScriptVariables: public variables are described in document order with their values, tooltips and ranges")
{
	VisualScriptDocument const document = LoadFixture("Variables.json");
	VisualScriptVariableDescription const description = DescribeVisualScriptVariables(document);
	CHECK(description.Editable.Name == "Visual Script");

	// Every public variable of an exposable type, in document order; the private ones are neither editable nor listed.
	CHECK(GetNames(description.Editable.Fields) ==
	      std::vector<std::string>{"Enabled",  "Lives",        "Coins",   "Seed",     "Highscore", "Speed",   "Gravity",     "Greeting",
	                               "Aim",      "SpawnPoint",   "Weights", "Facing",   "Tint",      "Partner", "EnemyPrefab", "AnyAsset",
	                               "BodyMesh", "BodyMaterial", "Icon",    "HitSound", "LabelFont", "Sky",     "Accent"});
	REQUIRE(description.ReadOnly.size() == 1);
	CHECK(description.ReadOnly[0].Name == "Push");
	CHECK(description.ReadOnly[0].Type == "Strada.ForceMode");
	CHECK(description.ReadOnly[0].Reason == "needs the scripting runtime");

	for (ScriptFieldInfo const& field : description.Editable.Fields)
	{
		CAPTURE(field.Name);
		CHECK(field.DefaultValue.GetType() == field.Type);
		CHECK_FALSE(field.Hidden);
		CHECK(field.Enumerators.empty());
		CHECK_FALSE(field.IsFlags);
	}
	ScriptClassInfo const& variables = description.Editable;
	auto const field = [&variables](std::string_view name) -> ScriptFieldInfo const&
	{
		ScriptFieldInfo const* const found = variables.FindField(name);
		REQUIRE(found != nullptr);
		return *found;
	};

	// The values are the variables' literals; Accent has none and starts with the default color.
	CHECK(field("Enabled").DefaultValue == ScriptFieldValue::FromBool(true));
	CHECK(field("Lives").DefaultValue == ScriptFieldValue::FromInt32(3));
	CHECK(field("Coins").DefaultValue == ScriptFieldValue::FromUInt32(250));
	CHECK(field("Seed").DefaultValue == ScriptFieldValue::FromInt64(-9000000000LL));
	CHECK(field("Highscore").DefaultValue == ScriptFieldValue::FromUInt64(5000000000ULL));
	CHECK(field("Speed").DefaultValue == ScriptFieldValue::FromFloat(4.5f));
	CHECK(field("Gravity").DefaultValue == ScriptFieldValue::FromDouble(-9.75));
	CHECK(field("Greeting").DefaultValue == ScriptFieldValue::FromString("Hello"));
	CHECK(field("Aim").DefaultValue == ScriptFieldValue::FromVector2({0.5f, -0.5f}));
	CHECK(field("SpawnPoint").DefaultValue == ScriptFieldValue::FromVector3({1.0f, 2.0f, 3.0f}));
	CHECK(field("Weights").DefaultValue == ScriptFieldValue::FromVector4(glm::vec4(0.25f)));
	CHECK(field("Facing").DefaultValue == ScriptFieldValue::FromQuaternion(glm::quat::wxyz(0.8f, 0.0f, 0.6f, 0.0f)));
	CHECK(field("Tint").DefaultValue == ScriptFieldValue::FromColor({1.0f, 0.5f, 0.25f, 1.0f}));
	CHECK(field("Partner").DefaultValue == ScriptFieldValue::FromEntity(UUID::Invalid()));
	CHECK(field("EnemyPrefab").DefaultValue == ScriptFieldValue::FromPrefab(AssetHandle()));
	CHECK(field("AnyAsset").DefaultValue == ScriptFieldValue::FromAsset(AssetHandle()));
	CHECK(field("Sky").DefaultValue == ScriptFieldValue::FromAsset(AssetHandle()));
	CHECK(field("Accent").DefaultValue == ScriptFieldValue::FromColor(glm::vec4(1.0f)));

	// Tooltips and ranges as the graph declares them; typed asset references keep their asset type.
	CHECK(field("Enabled").Tooltip == "Whether the probe runs");
	CHECK_FALSE(field("Enabled").Range.has_value());
	CHECK(field("Lives").Range == std::optional<glm::vec2>(glm::vec2(0.0f, 9.0f)));
	CHECK(field("Gravity").Range == std::optional<glm::vec2>(glm::vec2(-20.0f, 0.0f)));
	CHECK(field("Partner").Tooltip == "Set on each component");
	CHECK(field("Aim").Tooltip.empty());
	CHECK(field("EnemyPrefab").Type == ScriptFieldType::Prefab);
	CHECK(field("EnemyPrefab").AcceptedAssetType == AssetType::None);
	CHECK(field("AnyAsset").AcceptedAssetType == AssetType::None);
	CHECK(field("BodyMesh").AcceptedAssetType == AssetType::Mesh);
	CHECK(field("BodyMaterial").AcceptedAssetType == AssetType::Material);
	CHECK(field("Icon").AcceptedAssetType == AssetType::Texture);
	CHECK(field("HitSound").AcceptedAssetType == AssetType::AudioClip);
	CHECK(field("LabelFont").AcceptedAssetType == AssetType::Font);
	CHECK(field("Sky").AcceptedAssetType == AssetType::Environment);

	// A document without public variables describes none.
	VisualScriptDocument hidden = document;
	for (VisualScriptVariable& variable : hidden.Variables)
	{
		variable.Public = false;
	}
	VisualScriptVariableDescription const none = DescribeVisualScriptVariables(hidden);
	CHECK(none.Editable.Fields.empty());
	CHECK(none.ReadOnly.empty());
}

TEST_CASE("VisualScriptVariables: variables without a value start with their type's default")
{
	CHECK(GetStartValue("bool", nullptr) == ScriptFieldValue::FromBool(false));
	CHECK(GetStartValue("int", nullptr) == ScriptFieldValue::FromInt32(0));
	CHECK(GetStartValue("uint", nullptr) == ScriptFieldValue::FromUInt32(0));
	CHECK(GetStartValue("long", nullptr) == ScriptFieldValue::FromInt64(0));
	CHECK(GetStartValue("ulong", nullptr) == ScriptFieldValue::FromUInt64(0));
	CHECK(GetStartValue("float", nullptr) == ScriptFieldValue::FromFloat(0.0f));
	CHECK(GetStartValue("double", nullptr) == ScriptFieldValue::FromDouble(0.0));
	CHECK(GetStartValue("string", nullptr) == ScriptFieldValue::FromString(""));
	CHECK(GetStartValue("Strada.Vector2", nullptr) == ScriptFieldValue::FromVector2(glm::vec2(0.0f)));
	CHECK(GetStartValue("Strada.Vector3", nullptr) == ScriptFieldValue::FromVector3(glm::vec3(0.0f)));
	CHECK(GetStartValue("Strada.Vector4", nullptr) == ScriptFieldValue::FromVector4(glm::vec4(0.0f)));
	CHECK(GetStartValue("Strada.Quaternion", nullptr) == ScriptFieldValue::FromQuaternion(glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f)));
	CHECK(GetStartValue("Strada.Color", nullptr) == ScriptFieldValue::FromColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)));
	CHECK(GetStartValue("Strada.Entity", nullptr) == ScriptFieldValue::FromEntity(UUID::Invalid()));
	CHECK(GetStartValue("Strada.Prefab", nullptr) == ScriptFieldValue::FromPrefab(AssetHandle()));
	CHECK(GetStartValue("Strada.Material", nullptr) == ScriptFieldValue::FromAsset(AssetHandle()));
	CHECK(GetStartValue("Strada.ForceMode", nullptr, LookUpEnum) == ScriptFieldValue::FromInt32(0));
	CHECK(GetStartValue("Game.Layers", nullptr, LookUpEnum) == ScriptFieldValue::FromUInt32(0));
	CHECK(GetStartValue("Game.Distance", nullptr, LookUpEnum) == ScriptFieldValue::FromInt64(0));
}

TEST_CASE("VisualScriptVariables: values are read as literals of their variable's type")
{
	// Every integer of the type's range; 64-bit integers are decimal strings.
	CHECK(GetStartValue("int", -2147483648LL) == ScriptFieldValue::FromInt32(-2147483647 - 1));
	CHECK(GetStartValue("uint", 4294967295LL) == ScriptFieldValue::FromUInt32(4294967295u));
	CHECK(GetStartValue("long", "-9223372036854775808") == ScriptFieldValue::FromInt64(-9223372036854775807LL - 1));
	CHECK(GetStartValue("ulong", "18446744073709551615") == ScriptFieldValue::FromUInt64(18446744073709551615ULL));
	// Numbers of floating-point types may be written as integers.
	CHECK(GetStartValue("float", 4) == ScriptFieldValue::FromFloat(4.0f));
	CHECK(GetStartValue("float", 3.4028234663852886e38) == ScriptFieldValue::FromFloat(3.4028234663852886e38f));
	CHECK(GetStartValue("double", 1e300) == ScriptFieldValue::FromDouble(1e300));
	CHECK(GetStartValue("string", "asset://Textures/Oak.png") == ScriptFieldValue::FromString("asset://Textures/Oak.png"));
	// Colors are linear and not clamped.
	CHECK(GetStartValue("Strada.Color", Json::array({2.0, -1.0, 0.5, 1.0})) == ScriptFieldValue::FromColor({2.0f, -1.0f, 0.5f, 1.0f}));
	// Assets are decimal handles.
	CHECK(GetStartValue("Strada.Prefab", "17000004240335447843") ==
	      ScriptFieldValue::FromPrefab(AssetHandle(UUID(17000004240335447843ULL))));
	CHECK(GetStartValue("Strada.Texture", "4242") == ScriptFieldValue::FromAsset(AssetHandle(UUID(4242))));
	CHECK(GetStartValue("Strada.VisualScript", "0") == ScriptFieldValue::FromAsset(AssetHandle()));

	// Quaternions are normalized; one of zero length is the identity.
	ScriptFieldValue const facing = GetStartValue("Strada.Quaternion", Json::array({0.0, 3.0, 0.0, 4.0}));
	REQUIRE(facing.GetType() == ScriptFieldType::Quaternion);
	CHECK(facing.GetQuaternion().x == doctest::Approx(0.0f));
	CHECK(facing.GetQuaternion().y == doctest::Approx(0.6f));
	CHECK(facing.GetQuaternion().z == doctest::Approx(0.0f));
	CHECK(facing.GetQuaternion().w == doctest::Approx(0.8f));
	CHECK(GetStartValue("Strada.Quaternion", Json::array({0.0, 0.0, 0.0, 0.0})) ==
	      ScriptFieldValue::FromQuaternion(glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f)));
}

TEST_CASE("VisualScriptVariables: values that are no literal of their type start with the type's default")
{
	// The scripting runtime reports these values as errors; the editor shows the type's default for them.
	std::vector<std::pair<std::string, Json>> const invalid = {
		{"bool", "true"},
		{"bool", 1},
		{"int", 3.5},
		{"int", 2147483648LL},
		{"int", "3"},
		{"uint", -1},
		{"uint", 4294967296LL},
		{"long", 5},
		{"long", "5.0"},
		{"long", " 5"},
		{"long", "9223372036854775808"},
		{"ulong", "-1"},
		{"ulong", 5},
		{"ulong", "18446744073709551616"},
		{"float", 1e39},
		{"float", "1"},
		{"double", "x"},
		{"double", true},
		{"string", 5},
		{"string", Json::array({"a"})},
		{"Strada.Vector2", Json::array({1.0})},
		{"Strada.Vector2", Json::array({1.0, "2"})},
		{"Strada.Vector3", Json::array({1.0, 2.0})},
		{"Strada.Vector3", Json::object({{"X", 1.0}})},
		{"Strada.Vector4", Json::array({1.0, 2.0, 3.0})},
		{"Strada.Quaternion", Json::array({0.0, 0.0, 1.0})},
		{"Strada.Quaternion", "identity"},
		{"Strada.Color", Json::array({1.0, 1.0, 1.0})},
		{"Strada.Color", Json::array({1.0, 1.0, 1.0, "1"})},
		// Graphs are shared by entities: entities are set on each component.
		{"Strada.Entity", "12777872994629461483"},
		{"Strada.Entity", 0},
		{"Strada.Entity", ""},
		// Assets are decimal handles, never paths.
		{"Strada.Prefab", "asset://Prefabs/Crate.sprefab"},
		{"Strada.Prefab", 42},
		{"Strada.Material", "builtin://Default"},
		{"Strada.AssetHandle", "-1"},
		{"Strada.Mesh", "4242 "},
	};
	for (auto const& [type, value] : invalid)
	{
		CAPTURE(type);
		CAPTURE(value.dump());
		std::optional<VisualScriptExposableType> const exposable = FindVisualScriptExposableType(type);
		REQUIRE(exposable.has_value());
		CHECK(GetStartValue(type, value) == ScriptFieldValue::CreateDefault(exposable->FieldType));
	}
}

TEST_CASE("VisualScriptVariables: enums are editable with the scripting runtime's description and read-only without it")
{
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(1, "Push", "Strada.ForceMode", "Impulse"));
	document.Variables.push_back(MakeVariable(2, "Blocking", "Game.Layers", Json::array({"Walls", "Air"})));
	document.Variables.push_back(MakeVariable(3, "Range", "Game.Distance", "Far"));
	document.Variables[0].Tooltip = "How pushes apply";

	// Without the runtime nothing tells an enum from other types the engine does not know.
	VisualScriptVariableDescription const withoutRuntime = DescribeVisualScriptVariables(document);
	CHECK(withoutRuntime.Editable.Fields.empty());
	REQUIRE(withoutRuntime.ReadOnly.size() == 3);
	for (VisualScriptReadOnlyVariable const& variable : withoutRuntime.ReadOnly)
	{
		CAPTURE(variable.Name);
		CHECK(variable.Reason == "needs the scripting runtime");
	}
	CHECK(withoutRuntime.ReadOnly[1].Name == "Blocking");
	CHECK(withoutRuntime.ReadOnly[1].Type == "Game.Layers");

	// With it, enums are fields of their underlying type with the runtime's enumerators.
	VisualScriptVariableDescription const withRuntime = DescribeVisualScriptVariables(document, LookUpEnum);
	CHECK(withRuntime.ReadOnly.empty());
	REQUIRE(withRuntime.Editable.Fields.size() == 3);
	ScriptFieldInfo const& push = withRuntime.Editable.Fields[0];
	CHECK(push.Name == "Push");
	CHECK(push.Type == ScriptFieldType::Int32);
	CHECK(push.DefaultValue == ScriptFieldValue::FromInt32(1));
	REQUIRE(push.Enumerators.size() == 4);
	CHECK(push.Enumerators[3].Name == "VelocityChange");
	CHECK(push.Enumerators[3].Value == ScriptFieldValue::FromInt32(3));
	CHECK_FALSE(push.IsFlags);
	CHECK(push.Tooltip == "How pushes apply");
	CHECK(push.AcceptedAssetType == AssetType::None);
	ScriptFieldInfo const& blocking = withRuntime.Editable.Fields[1];
	CHECK(blocking.Type == ScriptFieldType::UInt32);
	CHECK(blocking.IsFlags);
	CHECK(blocking.DefaultValue == ScriptFieldValue::FromUInt32(5));
	ScriptFieldInfo const& range = withRuntime.Editable.Fields[2];
	CHECK(range.Type == ScriptFieldType::Int64);
	CHECK(range.DefaultValue == ScriptFieldValue::FromInt64(9000000000LL));

	// Names and arrays of names ([] = none); values without an enumerator are integers, decimal strings for 64-bit enums.
	CHECK(GetStartValue("Strada.ForceMode", "Acceleration", LookUpEnum) == ScriptFieldValue::FromInt32(2));
	CHECK(GetStartValue("Strada.ForceMode", 7, LookUpEnum) == ScriptFieldValue::FromInt32(7));
	CHECK(GetStartValue("Game.Layers", Json::array(), LookUpEnum) == ScriptFieldValue::FromUInt32(0));
	CHECK(GetStartValue("Game.Layers", Json::array({"Water"}), LookUpEnum) == ScriptFieldValue::FromUInt32(2));
	CHECK(GetStartValue("Game.Layers", 8, LookUpEnum) == ScriptFieldValue::FromUInt32(8));
	CHECK(GetStartValue("Game.Distance", "Near", LookUpEnum) == ScriptFieldValue::FromInt64(-5));
	CHECK(GetStartValue("Game.Distance", "-7000000000", LookUpEnum) == ScriptFieldValue::FromInt64(-7000000000LL));

	// Anything else starts with the value 0 of the underlying type.
	CHECK(GetStartValue("Strada.ForceMode", "Shove", LookUpEnum) == ScriptFieldValue::FromInt32(0));
	CHECK(GetStartValue("Strada.ForceMode", "impulse", LookUpEnum) == ScriptFieldValue::FromInt32(0));
	CHECK(GetStartValue("Strada.ForceMode", Json::array({"Impulse"}), LookUpEnum) == ScriptFieldValue::FromInt32(0));
	CHECK(GetStartValue("Strada.ForceMode", 1.5, LookUpEnum) == ScriptFieldValue::FromInt32(0));
	CHECK(GetStartValue("Game.Layers", "Water", LookUpEnum) == ScriptFieldValue::FromUInt32(0));
	CHECK(GetStartValue("Game.Layers", Json::array({"Water", "Lava"}), LookUpEnum) == ScriptFieldValue::FromUInt32(0));
	CHECK(GetStartValue("Game.Layers", -1, LookUpEnum) == ScriptFieldValue::FromUInt32(0));
	CHECK(GetStartValue("Game.Distance", 9000000000LL, LookUpEnum) == ScriptFieldValue::FromInt64(0));
}

TEST_CASE("VisualScriptVariables: public variables of types components cannot store are read-only")
{
	std::set<std::string, std::less<>> asked;
	VisualScriptEnumLookup const lookup = [&asked](std::string_view typeId)
	{
		asked.emplace(typeId);
		return LookUpEnum(typeId);
	};

	// Types the engine knows are no enums are not exposable, with or without the runtime.
	for (std::string_view const type : {"Strada.Matrix4", "System.Object", "Strada.Entity[]", "int[]", "exec", "*"})
	{
		CAPTURE(type);
		for (bool const runtime : {false, true})
		{
			VisualScriptVariableDescription const description =
				DescribeOne(std::string(type), nullptr, runtime ? lookup : VisualScriptEnumLookup());
			CHECK(description.Editable.Fields.empty());
			REQUIRE(description.ReadOnly.size() == 1);
			CHECK(description.ReadOnly[0].Name == "Value");
			CHECK(description.ReadOnly[0].Type == type);
			CHECK(description.ReadOnly[0].Reason == "not exposable");
		}
	}
	// Other types may be enums until the runtime tells: then structs, classes, components and unknown types are not.
	for (std::string_view const type :
	     {"Strada.RaycastHit", "Strada.RigidBodyComponent", "Game.Player", "Strada.Vectr3", "Game.Fractional", "Game.Mixed"})
	{
		CAPTURE(type);
		VisualScriptVariableDescription const withoutRuntime = DescribeOne(std::string(type), nullptr);
		REQUIRE(withoutRuntime.ReadOnly.size() == 1);
		CHECK(withoutRuntime.ReadOnly[0].Reason == "needs the scripting runtime");
		VisualScriptVariableDescription const withRuntime = DescribeOne(std::string(type), nullptr, lookup);
		CHECK(withRuntime.Editable.Fields.empty());
		REQUIRE(withRuntime.ReadOnly.size() == 1);
		CHECK(withRuntime.ReadOnly[0].Reason == "not exposable");
	}

	// The runtime is asked about the types that may be enums only.
	CHECK(asked == std::set<std::string, std::less<>>{"Strada.RaycastHit", "Strada.RigidBodyComponent", "Game.Player", "Strada.Vectr3",
	                                                  "Game.Fractional", "Game.Mixed"});
	asked.clear();
	(void)DescribeVisualScriptVariables(LoadFixture("Variables.json"), lookup);
	CHECK(asked == std::set<std::string, std::less<>>{"Strada.ForceMode"});
}

TEST_CASE("VisualScriptVariables: stored values follow the renames of their variables")
{
	VisualScriptDocument document;
	document.Variables.push_back(MakeVariable(1, "Lives", "int"));
	document.Variables[0].FormerNames = {"Health"};
	document.Variables.push_back(MakeVariable(2, "Speed", "float"));
	document.Variables.push_back(MakeVariable(3, "Tries", "int"));
	document.Variables[2].FormerNames = {"Attempts", "Retries"};
	document.Variables.push_back(MakeVariable(4, "Secret", "int", nullptr, false));
	document.Variables[3].FormerNames = {"Hidden"};

	ScriptFieldValue const five = ScriptFieldValue::FromInt32(5);
	ScriptFieldValue const seven = ScriptFieldValue::FromInt32(7);

	// A value under a former name applies to the variable under its current name; values under current names, under
	// names no public variable has had and under former names of private variables stay as they are.
	CHECK(NormalizeVisualScriptOverrides(document, MakeMap({{"Health", five},
	                                                        {"Speed", ScriptFieldValue::FromFloat(2.0f)},
	                                                        {"Unknown", ScriptFieldValue::FromBool(true)},
	                                                        {"Hidden", seven}})) == MakeMap({{"Lives", five},
	                                                                                         {"Speed", ScriptFieldValue::FromFloat(2.0f)},
	                                                                                         {"Unknown", ScriptFieldValue::FromBool(true)},
	                                                                                         {"Hidden", seven}}));
	// Values are kept as stored, also when their type is not the variable's.
	CHECK(NormalizeVisualScriptOverrides(document, MakeMap({{"Health", ScriptFieldValue::FromString("x")}})) ==
	      MakeMap({{"Lives", ScriptFieldValue::FromString("x")}}));
	// The value under the current name wins over one under a former name; among former names, the most recent one wins.
	CHECK(NormalizeVisualScriptOverrides(document, MakeMap({{"Health", five}, {"Lives", seven}})) == MakeMap({{"Lives", seven}}));
	CHECK(NormalizeVisualScriptOverrides(document, MakeMap({{"Attempts", five}, {"Retries", seven}})) == MakeMap({{"Tries", seven}}));
	CHECK(NormalizeVisualScriptOverrides(document, MakeMap({{"Attempts", five}})) == MakeMap({{"Tries", five}}));
	CHECK(NormalizeVisualScriptOverrides(document, MakeMap({{"Attempts", five}, {"Tries", seven}, {"Retries", five}})) ==
	      MakeMap({{"Tries", seven}}));
	CHECK(NormalizeVisualScriptOverrides(document, {}).empty());

	// A name a public variable has now is that variable's, even where another variable lists it as a former name (which
	// documents that load never do).
	VisualScriptDocument reused = document;
	reused.Variables[1].FormerNames = {"Lives"};
	CHECK(NormalizeVisualScriptOverrides(reused, MakeMap({{"Lives", five}})) == MakeMap({{"Lives", five}}));

	// The shared fixture renamed Health to Lives.
	CHECK(NormalizeVisualScriptOverrides(LoadFixture("Variables.json"),
	                                     MakeMap({{"Health", five}, {"Speed", ScriptFieldValue::FromFloat(1.0f)}})) ==
	      MakeMap({{"Lives", five}, {"Speed", ScriptFieldValue::FromFloat(1.0f)}}));
}
