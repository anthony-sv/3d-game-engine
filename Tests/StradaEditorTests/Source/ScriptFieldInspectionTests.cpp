#include "Editor/ScriptFieldInspection.h"

#include "Strada/Script/ScriptEngine.h"

#include <doctest/doctest.h>

#include <array>
#include <string>

using namespace Strada;

namespace
{
	ScriptFieldInfo MakeField(std::string name, ScriptFieldType type, ScriptFieldValue defaultValue)
	{
		ScriptFieldInfo field;
		field.Name = std::move(name);
		field.Type = type;
		field.DefaultValue = std::move(defaultValue);
		return field;
	}

	// A script class with a field of every kind the inspector edits, and a hidden one.
	ScriptClassInfo MakeClass()
	{
		ScriptClassInfo info;
		info.Name = "Game.Player";
		ScriptFieldInfo speed = MakeField("Speed", ScriptFieldType::Float, ScriptFieldValue::FromFloat(2.5f));
		speed.Range = glm::vec2(0.0f, 10.0f);
		speed.Tooltip = "Units per second";
		info.Fields.push_back(speed);
		info.Fields.push_back(MakeField("Lives", ScriptFieldType::Int32, ScriptFieldValue::FromInt32(3)));
		info.Fields.push_back(MakeField("Seed", ScriptFieldType::Int64, ScriptFieldValue::FromInt64(-9000000000)));
		info.Fields.push_back(MakeField("Mask", ScriptFieldType::UInt64, ScriptFieldValue::FromUInt64(18000000000000000000ULL)));
		info.Fields.push_back(MakeField("Tint", ScriptFieldType::Color, ScriptFieldValue::FromColor({1.0f, 0.5f, 0.25f, 1.0f})));
		info.Fields.push_back(MakeField("Offset", ScriptFieldType::Vector3, ScriptFieldValue::FromVector3({1.0f, 2.0f, 3.0f})));
		info.Fields.push_back(MakeField("Target", ScriptFieldType::Entity, ScriptFieldValue::FromEntity(UUID::Invalid())));
		ScriptFieldInfo model = MakeField("Model", ScriptFieldType::Asset, ScriptFieldValue::FromAsset(AssetHandle()));
		model.AcceptedAssetType = AssetType::Mesh;
		info.Fields.push_back(model);
		info.Fields.push_back(MakeField("Spawn", ScriptFieldType::Prefab, ScriptFieldValue::FromPrefab(AssetHandle())));
		ScriptFieldInfo level = MakeField("Level", ScriptFieldType::Int32, ScriptFieldValue::FromInt32(5));
		level.Enumerators = {
			{"Easy", ScriptFieldValue::FromInt32(0)}, {"Normal", ScriptFieldValue::FromInt32(1)}, {"Hard", ScriptFieldValue::FromInt32(5)}};
		info.Fields.push_back(level);
		ScriptFieldInfo access = MakeField("Access", ScriptFieldType::UInt32, ScriptFieldValue::FromUInt32(3));
		access.Enumerators = {{"None", ScriptFieldValue::FromUInt32(0)},
		                      {"Read", ScriptFieldValue::FromUInt32(1)},
		                      {"Write", ScriptFieldValue::FromUInt32(2)},
		                      {"Execute", ScriptFieldValue::FromUInt32(4)}};
		access.IsFlags = true;
		info.Fields.push_back(access);
		ScriptFieldInfo secret = MakeField("Secret", ScriptFieldType::Int32, ScriptFieldValue::FromInt32(1));
		secret.Hidden = true;
		info.Fields.push_back(secret);
		return info;
	}

	Json Stored(char const* type, Json value)
	{
		return Json::object({{"Type", type}, {"Value", std::move(value)}});
	}
}

TEST_CASE("ScriptFieldInspection: script fields are described for the field editor")
{
	ScriptClassInfo const info = MakeClass();
	std::vector<FieldDescriptor> const fields = ScriptFieldInspection::DescribeFields(info);
	// The hidden field is left out.
	REQUIRE(fields.size() == 11);

	CHECK(fields[0].Name == "Speed");
	CHECK(fields[0].Kind == FieldKind::Float);
	CHECK(fields[0].Hints.Min == 0.0);
	CHECK(fields[0].Hints.Max == 10.0);
	CHECK(fields[0].Hints.Description == "Units per second");
	CHECK(fields[0].Default == 2.5);
	CHECK(fields[1].Kind == FieldKind::Int);
	CHECK(fields[1].TypeMax == static_cast<double>(std::numeric_limits<int32_t>::max()));
	// 64-bit integers are edited as numbers (scene files store them as strings).
	CHECK(fields[2].Kind == FieldKind::Int);
	CHECK(fields[2].Default == Json(int64_t{-9000000000}));
	CHECK(fields[3].Kind == FieldKind::UInt);
	CHECK(fields[3].Default == Json(uint64_t{18000000000000000000ULL}));
	CHECK(fields[4].Kind == FieldKind::Vec4);
	CHECK(fields[4].Hints.Display == FieldDisplay::Color);
	CHECK(fields[5].Kind == FieldKind::Vec3);
	CHECK(fields[6].Kind == FieldKind::UUID);
	CHECK(fields[7].Kind == FieldKind::Asset);
	CHECK(fields[7].Hints.AssetTypeName == "Mesh");
	CHECK(fields[8].Hints.AssetTypeName == "Prefab");

	CHECK(fields[9].Kind == FieldKind::Enum);
	CHECK_FALSE(fields[9].IsFlags);
	CHECK(fields[9].EnumValues == std::vector<std::string_view>{"Easy", "Normal", "Hard"});
	CHECK(fields[9].Default == "Hard");
	// Flags enums leave out the zero value, which sets no flag.
	CHECK(fields[10].Kind == FieldKind::Enum);
	CHECK(fields[10].IsFlags);
	CHECK(fields[10].EnumValues == std::vector<std::string_view>{"Read", "Write", "Execute"});
	CHECK(fields[10].Default == Json::array({"Read", "Write"}));
}

TEST_CASE("ScriptFieldInspection: stored values are shown while their type still fits")
{
	ScriptClassInfo const info = MakeClass();
	Json stored = Json::object({{"Speed", Stored("Float", 7.5)},
	                            // The field changed type: its default is shown.
	                            {"Lives", Stored("String", "three")},
	                            {"Seed", Stored("Int64", "-5")},
	                            {"Level", Stored("Int32", 1)},
	                            {"Access", Stored("UInt32", 5)},
	                            {"Secret", Stored("Int32", 9)},
	                            {"Removed", Stored("Int32", 1)}});
	Json const values = ScriptFieldInspection::ToEditorValues(info, stored);
	CHECK(values["Speed"] == 7.5);
	CHECK(values["Lives"] == 3);
	CHECK(values["Seed"] == -5);
	CHECK(values["Mask"] == Json(uint64_t{18000000000000000000ULL}));
	CHECK(values["Offset"] == Json::array({1.0, 2.0, 3.0}));
	CHECK(values["Level"] == "Normal");
	CHECK(values["Access"] == Json::array({"Read", "Execute"}));
	CHECK_FALSE(values.contains("Secret"));
	CHECK_FALSE(values.contains("Removed"));

	// An enum value without an enumerator shows as its number.
	stored["Level"]["Value"] = 3;
	CHECK(ScriptFieldInspection::ToEditorValues(info, stored)["Level"] == "3");
	CHECK(ScriptFieldInspection::ToEditorValues(info, Json())["Speed"] == 2.5);
}

TEST_CASE("ScriptFieldInspection: edited values are stored in the scene format")
{
	ScriptClassInfo const info = MakeClass();
	Json const stored = Json::object({{"Removed", Stored("Int32", 1)}});
	auto const set = [&](std::string_view field, Json const& value)
	{
		Result<Json> fields = ScriptFieldInspection::SetEditorValue(info, stored, field, value);
		REQUIRE_MESSAGE(fields.IsOk(), (fields ? std::string() : fields.GetError()));
		// Values of fields the class no longer has are kept.
		CHECK(fields.GetValue().contains("Removed"));
		return fields.GetValue()[std::string(field)];
	};
	CHECK(set("Speed", 4.0) == Stored("Float", 4.0));
	CHECK(set("Seed", int64_t{-7}) == Stored("Int64", "-7"));
	CHECK(set("Mask", uint64_t{18446744073709551615ULL}) == Stored("UInt64", "18446744073709551615"));
	CHECK(set("Tint", Json::array({0.1, 0.2, 0.3, 0.4}))["Type"] == "Color");
	CHECK(set("Target", "123") == Stored("Entity", "123"));
	CHECK(set("Model", "77") == Stored("Asset", "77"));
	CHECK(set("Level", "Easy") == Stored("Int32", 0));
	CHECK(set("Level", "3") == Stored("Int32", 3));
	CHECK(set("Access", Json::array({"Read", "Execute"})) == Stored("UInt32", 5));
	CHECK(set("Access", Json::array()) == Stored("UInt32", 0));

	CHECK(ScriptFieldInspection::SetEditorValue(info, stored, "Missing", 1).IsError());
	CHECK(ScriptFieldInspection::SetEditorValue(info, stored, "Secret", 1).IsError());
	CHECK(ScriptFieldInspection::SetEditorValue(info, stored, "Speed", "fast").IsError());
	CHECK(ScriptFieldInspection::SetEditorValue(info, stored, "Level", "Impossible").IsError());
	CHECK(ScriptFieldInspection::SetEditorValue(info, stored, "Access", Json::array({"Nope"})).IsError());
	CHECK(ScriptFieldInspection::SetEditorValue(info, stored, "Mask", -1).IsError());
}

TEST_CASE("ScriptFieldInspection: an edit reaches every selected entity's own values")
{
	ScriptClassInfo const info = MakeClass();
	std::array<UUID, 2> const entities = {UUID(1), UUID(2)};
	std::array<Json, 2> const components = {
		Json::object(
			{{"ClassName", "Game.Player"}, {"Fields", Json::object({{"Offset", Stored("Vector3", Json::array({1.0, 2.0, 3.0}))}})}}),
		Json::object(
			{{"ClassName", "Game.Player"}, {"Fields", Json::object({{"Offset", Stored("Vector3", Json::array({4.0, 5.0, 6.0}))}})}})};
	std::array<Json, 2> const editorValues = {ScriptFieldInspection::ToEditorValues(info, components[0]["Fields"]),
	                                          ScriptFieldInspection::ToEditorValues(info, components[1]["Fields"])};

	// X of Offset changed on the first entity: each keeps its own Y and Z.
	FieldChange change;
	change.Path = {"Offset"};
	change.Value = Json::array({9.0, 2.0, 3.0});
	change.Component = 0;
	Result<std::vector<ComponentEdit>> edits = ScriptFieldInspection::MakeEdits(info, entities, components, editorValues, change);
	REQUIRE(edits.IsOk());
	REQUIRE(edits.GetValue().size() == 2);
	CHECK(edits.GetValue()[0].Entity == UUID(1));
	CHECK(edits.GetValue()[0].Component == "Script");
	CHECK(edits.GetValue()[0].Patch["Fields"]["Offset"] == Stored("Vector3", Json::array({9.0, 2.0, 3.0})));
	CHECK(edits.GetValue()[1].Patch["Fields"]["Offset"] == Stored("Vector3", Json::array({9.0, 5.0, 6.0})));

	change.Path = {"Speed"};
	change.Value = "fast";
	change.Component.reset();
	CHECK(ScriptFieldInspection::MakeEdits(info, entities, components, editorValues, change).IsError());
}
