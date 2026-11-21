#include "Editor/ScriptFieldInspection.h"
#include "Editor/UI/FieldEditor.h"

#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Script/ScriptFieldSerialization.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

TEST_CASE("ScriptFieldInspection: kept editor values are converted again only where a stored value or its field changed")
{
	ScriptClassInfo info = MakeClass();
	ScriptFieldMap stored = {{"Speed", ScriptFieldValue::FromFloat(7.5f)},
	                         {"Lives", ScriptFieldValue::FromString("three")},
	                         {"Level", ScriptFieldValue::FromInt32(1)},
	                         {"Secret", ScriptFieldValue::FromInt32(9)}};
	auto const expected = [&info, &stored]
	{
		return ScriptFieldInspection::ToEditorValues(info, JsonTraits<ScriptFieldMap>::ToJson(stored));
	};
	ScriptFieldInspection::EditorValueCache cache;
	Json values;
	// The first update converts every shown field (the hidden one is not shown), as ToEditorValues does.
	CHECK(cache.Update(info, stored, values) == 11);
	CHECK(values == expected());
	CHECK(cache.Update(info, stored, values) == 0);

	// A stored value changed, added or of another type: that field alone.
	stored["Speed"] = ScriptFieldValue::FromFloat(1.0f);
	CHECK(cache.Update(info, stored, values) == 1);
	stored["Lives"] = ScriptFieldValue::FromInt32(9);
	CHECK(cache.Update(info, stored, values) == 1);
	stored.erase("Lives");
	CHECK(cache.Update(info, stored, values) == 1);
	CHECK(values == expected());
	// What the class says of a field: its enumerators, its default where nothing stored has its type.
	info.Fields[9].Enumerators[1].Name = "Medium";
	CHECK(cache.Update(info, stored, values) == 1);
	CHECK(values["Level"] == "Medium");
	info.Fields[1].DefaultValue = ScriptFieldValue::FromInt32(4);
	CHECK(cache.Update(info, stored, values) == 1);
	CHECK(values == expected());
	// Another set of shown fields starts over.
	info.Fields.erase(info.Fields.begin());
	CHECK(cache.Update(info, stored, values) == 10);
	CHECK(values == expected());
	CHECK_FALSE(values.contains("Speed"));
}

TEST_CASE("ScriptFieldInspection: kept editor values compare floating-point numbers bit for bit, as the runtime's read-back does")
{
	ScriptClassInfo info;
	info.Name = "Game.Samples";
	info.Fields.push_back(MakeField("Rate", ScriptFieldType::Float, ScriptFieldValue::FromFloat(0.0f)));
	info.Fields.push_back(MakeField("Offset", ScriptFieldType::Vector3, ScriptFieldValue::FromVector3(glm::vec3(0.0f))));
	info.Fields.push_back(MakeField("Samples", MakeScriptFieldListType(ScriptFieldType::Float),
	                                ScriptFieldValue::CreateDefault(MakeScriptFieldListType(ScriptFieldType::Float))));
	float const nan = std::numeric_limits<float>::quiet_NaN();
	auto const makeSamples = [](float last)
	{
		std::vector<ScriptFieldValue> elements(3, ScriptFieldValue::FromFloat(1.0f));
		elements.back() = ScriptFieldValue::FromFloat(last);
		return ScriptFieldValue::FromList(ScriptFieldType::Float, std::move(elements));
	};
	ScriptFieldMap stored = {{"Rate", ScriptFieldValue::FromFloat(nan)},
	                         {"Offset", ScriptFieldValue::FromVector3({1.0f, nan, 2.0f})},
	                         {"Samples", makeSamples(nan)}};
	ScriptFieldInspection::EditorValueCache cache;
	Json values;
	CHECK(cache.Update(info, stored, values) == 3);
	// A NaN that stays, alone, in a vector or in a list, is unchanged: nothing is converted again.
	CHECK(cache.Update(info, stored, values) == 0);
	stored["Samples"] = makeSamples(nan);
	CHECK(cache.Update(info, stored, values) == 0);
	// A change between 0 and -0 is a change.
	stored["Rate"] = ScriptFieldValue::FromFloat(0.0f);
	CHECK(cache.Update(info, stored, values) == 1);
	stored["Rate"] = ScriptFieldValue::FromFloat(-0.0f);
	CHECK(cache.Update(info, stored, values) == 1);
	stored["Samples"] = makeSamples(0.0f);
	CHECK(cache.Update(info, stored, values) == 1);
	stored["Samples"] = makeSamples(-0.0f);
	CHECK(cache.Update(info, stored, values) == 1);
	CHECK(cache.Update(info, stored, values) == 0);
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
	change.Path = {{"Offset"}};
	change.Value = Json::array({9.0, 2.0, 3.0});
	change.Component = 0;
	Result<std::vector<ComponentEdit>> edits = ScriptFieldInspection::MakeEdits(info, entities, components, editorValues, change);
	REQUIRE(edits.IsOk());
	REQUIRE(edits.GetValue().size() == 2);
	CHECK(edits.GetValue()[0].Entity == UUID(1));
	CHECK(edits.GetValue()[0].Component == "Script");
	CHECK(edits.GetValue()[0].Patch["Fields"]["Offset"] == Stored("Vector3", Json::array({9.0, 2.0, 3.0})));
	CHECK(edits.GetValue()[1].Patch["Fields"]["Offset"] == Stored("Vector3", Json::array({9.0, 5.0, 6.0})));

	change.Path = {{"Speed"}};
	change.Value = "fast";
	change.Component.reset();
	CHECK(ScriptFieldInspection::MakeEdits(info, entities, components, editorValues, change).IsError());
}

TEST_CASE("ScriptFieldInspection: edits of another component's map of script fields write that map under its key")
{
	// The values VisualScript components give the public variables of their graph, described as the fields of a class.
	ScriptClassInfo const info = MakeClass();
	std::array<UUID, 2> const entities = {UUID(1), UUID(2)};
	std::array<Json, 2> const components = {
		Json::object({{"Graph", "77"}, {"Variables", Json::object({{"Lives", Stored("Int32", 9)}, {"Removed", Stored("Int32", 1)}})}}),
		Json::object({{"Graph", "77"}})};
	std::array<Json, 2> const editorValues = {ScriptFieldInspection::ToEditorValues(info, components[0]["Variables"]),
	                                          ScriptFieldInspection::ToEditorValues(info, Json::object())};

	FieldChange change;
	change.Path = {{"Speed"}};
	change.Value = 6.0;
	Result<std::vector<ComponentEdit>> edits =
		ScriptFieldInspection::MakeEdits(info, entities, components, editorValues, change, "VisualScript", "Variables");
	REQUIRE(edits.IsOk());
	REQUIRE(edits.GetValue().size() == 2);
	// Each edit names the component and writes its whole map under the key: the edited value with every stored one kept.
	CHECK(edits.GetValue()[0].Component == "VisualScript");
	CHECK(
		edits.GetValue()[0].Patch ==
		Json::object({{"Variables",
	                   Json::object({{"Lives", Stored("Int32", 9)}, {"Removed", Stored("Int32", 1)}, {"Speed", Stored("Float", 6.0)}})}}));
	CHECK(edits.GetValue()[1].Entity == UUID(2));
	CHECK(edits.GetValue()[1].Patch == Json::object({{"Variables", Json::object({{"Speed", Stored("Float", 6.0)}})}}));

	// Without a key or component name given, the edits are a Script component's: maps under other keys are not read.
	Result<std::vector<ComponentEdit>> scripts = ScriptFieldInspection::MakeEdits(info, entities, components, editorValues, change);
	REQUIRE(scripts.IsOk());
	CHECK(scripts.GetValue()[0].Component == "Script");
	CHECK(scripts.GetValue()[0].Patch == Json::object({{"Fields", Json::object({{"Speed", Stored("Float", 6.0)}})}}));
}

namespace
{
	ScriptFieldValue MakeList(ScriptFieldType elementType, std::vector<ScriptFieldValue> elements)
	{
		return ScriptFieldValue::FromList(elementType, std::move(elements));
	}

	// A script class with list fields: of numbers with a range, of 64-bit integers, of an enum, of a [Flags] enum, of meshes,
	// of colours and of quaternions.
	ScriptClassInfo MakeListClass()
	{
		ScriptClassInfo info;
		info.Name = "Game.Route";
		ScriptFieldInfo speeds = MakeField("Speeds", MakeScriptFieldListType(ScriptFieldType::Float),
		                                   MakeList(ScriptFieldType::Float, {ScriptFieldValue::FromFloat(2.5f)}));
		speeds.Range = glm::vec2(0.0f, 10.0f);
		speeds.Tooltip = "Units per second, each";
		info.Fields.push_back(speeds);
		info.Fields.push_back(MakeField("Seeds", MakeScriptFieldListType(ScriptFieldType::Int64),
		                                MakeList(ScriptFieldType::Int64, {ScriptFieldValue::FromInt64(-9000000000)})));
		ScriptFieldInfo levels =
			MakeField("Levels", MakeScriptFieldListType(ScriptFieldType::Int32),
		              MakeList(ScriptFieldType::Int32, {ScriptFieldValue::FromInt32(5), ScriptFieldValue::FromInt32(0)}));
		levels.Enumerators = {
			{"Easy", ScriptFieldValue::FromInt32(0)}, {"Normal", ScriptFieldValue::FromInt32(1)}, {"Hard", ScriptFieldValue::FromInt32(5)}};
		info.Fields.push_back(levels);
		ScriptFieldInfo accesses = MakeField("Accesses", MakeScriptFieldListType(ScriptFieldType::UInt32),
		                                     MakeList(ScriptFieldType::UInt32, {ScriptFieldValue::FromUInt32(3)}));
		accesses.Enumerators = {{"None", ScriptFieldValue::FromUInt32(0)},
		                        {"Read", ScriptFieldValue::FromUInt32(1)},
		                        {"Write", ScriptFieldValue::FromUInt32(2)}};
		accesses.IsFlags = true;
		info.Fields.push_back(accesses);
		ScriptFieldInfo shapes = MakeField("Shapes", MakeScriptFieldListType(ScriptFieldType::Asset), MakeList(ScriptFieldType::Asset, {}));
		shapes.AcceptedAssetType = AssetType::Mesh;
		info.Fields.push_back(shapes);
		info.Fields.push_back(MakeField("Tints", MakeScriptFieldListType(ScriptFieldType::Color), MakeList(ScriptFieldType::Color, {})));
		info.Fields.push_back(
			MakeField("Turns", MakeScriptFieldListType(ScriptFieldType::Quaternion), MakeList(ScriptFieldType::Quaternion, {})));
		return info;
	}

	FieldChange MakeListChange(std::string_view field, std::optional<uint32_t> element, Json value,
	                           std::optional<FieldArrayEdit> edit = std::nullopt)
	{
		FieldChange change;
		change.Path = {FieldPathStep{field, element}};
		change.Value = std::move(value);
		change.ArrayEdit = edit;
		return change;
	}
}

TEST_CASE("ScriptFieldInspection: list fields are the field editor's arrays of their elements, at most 4096")
{
	// The descriptors refer to the class's strings.
	ScriptClassInfo const info = MakeListClass();
	std::vector<FieldDescriptor> const fields = ScriptFieldInspection::DescribeFields(info);
	REQUIRE(fields.size() == 7);
	for (FieldDescriptor const& field : fields)
	{
		CAPTURE(field.Name);
		CHECK(field.Kind == FieldKind::Array);
		CHECK(field.Hints.MaxCount == MaxScriptFieldListElements);
	}
	// The element's kind and hints: each element's range and the list's tooltip, numbers for 64-bit integers, enumerators by
	// name, the accepted asset type, colours.
	CHECK(fields[0].TypeName == "Float[]");
	CHECK(fields[0].ElementKind == FieldKind::Float);
	CHECK(fields[0].Hints.Min == 0.0);
	CHECK(fields[0].Hints.Max == 10.0);
	CHECK(fields[0].Hints.Description == "Units per second, each");
	CHECK(fields[0].Default == Json::array({2.5}));
	CHECK(fields[1].ElementKind == FieldKind::Int);
	CHECK(fields[1].TypeMax == static_cast<double>(std::numeric_limits<int64_t>::max()));
	CHECK(fields[1].Default == Json::array({int64_t{-9000000000}}));
	CHECK(fields[2].ElementKind == FieldKind::Enum);
	CHECK(fields[2].EnumValues == std::vector<std::string_view>{"Easy", "Normal", "Hard"});
	CHECK(fields[2].Default == Json::array({"Hard", "Easy"}));
	CHECK(fields[3].ElementKind == FieldKind::Enum);
	CHECK(fields[3].IsFlags);
	CHECK(fields[3].EnumValues == std::vector<std::string_view>{"Read", "Write"});
	CHECK(fields[3].Default == Json::array({Json::array({"Read", "Write"})}));
	CHECK(fields[4].ElementKind == FieldKind::Asset);
	CHECK(fields[4].Hints.AssetTypeName == "Mesh");
	CHECK(fields[4].Default == Json::array());
	CHECK(fields[5].ElementKind == FieldKind::Vec4);
	CHECK(fields[5].Hints.Display == FieldDisplay::Color);
	CHECK(fields[6].ElementKind == FieldKind::Quat);
}

TEST_CASE("ScriptFieldInspection: a list is shown element by element and an edit stores it whole in the scene format")
{
	ScriptClassInfo const info = MakeListClass();
	Json const stored = Json::object({{"Seeds", Stored("Int64[]", Json::array({"-5", "6"}))},
	                                  {"Levels", Stored("Int32[]", Json::array({1, 3}))},
	                                  // A single value stored for the list: its default is shown.
	                                  {"Speeds", Stored("Float", 9.0)}});
	Json const values = ScriptFieldInspection::ToEditorValues(info, stored);
	CHECK(values["Seeds"] == Json::array({-5, 6}));
	// An element without an enumerator shows as its number.
	CHECK(values["Levels"] == Json::array({"Normal", "3"}));
	CHECK(values["Speeds"] == Json::array({2.5}));

	auto const set = [&](std::string_view field, Json const& value)
	{
		Result<Json> fields = ScriptFieldInspection::SetEditorValue(info, stored, field, value);
		REQUIRE_MESSAGE(fields.IsOk(), (fields ? std::string() : fields.GetError()));
		return fields.GetValue()[std::string(field)];
	};
	CHECK(set("Speeds", Json::array({1.0, 2.0})) == Stored("Float[]", Json::array({1.0, 2.0})));
	CHECK(set("Seeds", Json::array({int64_t{-7}})) == Stored("Int64[]", Json::array({"-7"})));
	CHECK(set("Levels", Json::array({"Easy", "Hard", "3"})) == Stored("Int32[]", Json::array({0, 5, 3})));
	CHECK(set("Accesses", Json::array({Json::array({"Write"}), Json::array()})) == Stored("UInt32[]", Json::array({2, 0})));
	CHECK(set("Shapes", Json::array({"77", "0"})) == Stored("Asset[]", Json::array({"77", "0"})));
	CHECK(set("Speeds", Json::array()) == Stored("Float[]", Json::array()));

	// An edit that is no list of the elements is refused, naming the element.
	Result<Json> const notList = ScriptFieldInspection::SetEditorValue(info, stored, "Speeds", 1.0);
	REQUIRE(notList.IsError());
	CHECK(notList.GetError() == "Speeds: expected an array of its elements");
	Result<Json> const badElement = ScriptFieldInspection::SetEditorValue(info, stored, "Levels", Json::array({"Easy", "Impossible"}));
	REQUIRE(badElement.IsError());
	CHECK(badElement.GetError() == "Levels element 1: \"Impossible\" is not one of its values");
	CHECK(ScriptFieldInspection::SetEditorValue(info, stored, "Seeds", Json::array({"x"})).IsError());
	Json const longest(MaxScriptFieldListElements, 1.0);
	CHECK(ScriptFieldInspection::SetEditorValue(info, stored, "Speeds", longest).IsOk());
	Json tooLong = longest;
	tooLong.push_back(1.0);
	Result<Json> const refused = ScriptFieldInspection::SetEditorValue(info, stored, "Speeds", tooLong);
	REQUIRE(refused.IsError());
	CHECK(refused.GetError() == "Speeds: holds at most 4096 elements, not 4097");
}

TEST_CASE("ScriptFieldInspection: each list edit writes the field's whole list, each selected entity's own")
{
	ScriptClassInfo const info = MakeListClass();
	std::array<UUID, 2> const entities = {UUID(1), UUID(2)};
	std::array<Json, 2> const components = {
		Json::object(
			{{"ClassName", "Game.Route"}, {"Fields", Json::object({{"Speeds", Stored("Float[]", Json::array({1.0, 2.0, 3.0}))}})}}),
		Json::object({{"ClassName", "Game.Route"}, {"Fields", Json::object({{"Speeds", Stored("Float[]", Json::array({7.0}))}})}})};
	std::array<Json, 2> const editorValues = {ScriptFieldInspection::ToEditorValues(info, components[0]["Fields"]),
	                                          ScriptFieldInspection::ToEditorValues(info, components[1]["Fields"])};
	auto const edit = [&](FieldChange const& change)
	{
		Result<std::vector<ComponentEdit>> edits = ScriptFieldInspection::MakeEdits(info, entities, components, editorValues, change);
		REQUIRE_MESSAGE(edits.IsOk(), (edits ? std::string() : edits.GetError()));
		REQUIRE(edits.GetValue().size() == 2);
		return std::pair(edits.GetValue()[0].Patch["Fields"]["Speeds"], edits.GetValue()[1].Patch["Fields"]["Speeds"]);
	};

	// An element changed: each entity's element at that index, where it has one.
	auto const [changedFirst, changedSecond] = edit(MakeListChange("Speeds", 1, 9.0));
	CHECK(changedFirst == Stored("Float[]", Json::array({1.0, 9.0, 3.0})));
	CHECK(changedSecond == Stored("Float[]", Json::array({7.0})));
	// Added at the end, removed, moved: each list as a list.
	auto const [addedFirst, addedSecond] =
		edit(MakeListChange("Speeds", std::nullopt, 0.0, FieldArrayEdit{FieldArrayEditKind::Insert, FieldArrayEdit::End, 0}));
	CHECK(addedFirst == Stored("Float[]", Json::array({1.0, 2.0, 3.0, 0.0})));
	CHECK(addedSecond == Stored("Float[]", Json::array({7.0, 0.0})));
	auto const [removedFirst, removedSecond] =
		edit(MakeListChange("Speeds", std::nullopt, nullptr, FieldArrayEdit{FieldArrayEditKind::Remove, 0, 0}));
	CHECK(removedFirst == Stored("Float[]", Json::array({2.0, 3.0})));
	CHECK(removedSecond == Stored("Float[]", Json::array()));
	auto const [movedFirst, movedSecond] =
		edit(MakeListChange("Speeds", std::nullopt, nullptr, FieldArrayEdit{FieldArrayEditKind::Move, 0, 2}));
	CHECK(movedFirst == Stored("Float[]", Json::array({2.0, 3.0, 1.0})));
	CHECK(movedSecond == Stored("Float[]", Json::array({7.0})));
	// Past the most elements a list holds, nothing is written.
	Json full = Json::object({{"Speeds", Stored("Float[]", Json(MaxScriptFieldListElements, 1.0))}});
	std::array<Json, 1> const fullComponents = {Json::object({{"ClassName", "Game.Route"}, {"Fields", full}})};
	std::array<Json, 1> const fullValues = {ScriptFieldInspection::ToEditorValues(info, full)};
	std::array<UUID, 1> const one = {UUID(1)};
	CHECK(ScriptFieldInspection::MakeEdits(
			  info, one, fullComponents, fullValues,
			  MakeListChange("Speeds", std::nullopt, 0.0, FieldArrayEdit{FieldArrayEditKind::Insert, FieldArrayEdit::End, 0}))
	          .IsError());
}
