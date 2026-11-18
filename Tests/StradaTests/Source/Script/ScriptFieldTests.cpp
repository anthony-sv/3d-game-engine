#include "Strada/Script/ScriptField.h"
#include "Strada/Script/ScriptFieldSerialization.h"

#include <doctest/doctest.h>
#include <spdlog/fmt/fmt.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

TEST_CASE("ScriptField: type names round trip")
{
	for (int i = 0; i <= static_cast<int>(ScriptFieldType::Asset); i++)
	{
		ScriptFieldType const type = static_cast<ScriptFieldType>(i);
		CAPTURE(ScriptFieldTypeToString(type));
		CHECK(ScriptFieldTypeFromString(ScriptFieldTypeToString(type)) == std::optional<ScriptFieldType>(type));
	}
	CHECK_FALSE(ScriptFieldTypeFromString("Vector5").has_value());
}

TEST_CASE("ScriptField: defaults have the requested type")
{
	for (int i = 1; i <= static_cast<int>(ScriptFieldType::Asset); i++)
	{
		ScriptFieldType const type = static_cast<ScriptFieldType>(i);
		CAPTURE(ScriptFieldTypeToString(type));
		CHECK(ScriptFieldValue::CreateDefault(type).GetType() == type);
	}
	CHECK(ScriptFieldValue::CreateDefault(ScriptFieldType::Quaternion).GetQuaternion() == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
	CHECK_FALSE(ScriptFieldValue::CreateDefault(ScriptFieldType::Entity).GetEntity().IsValid());
	CHECK(ScriptFieldValue::CreateDefault(ScriptFieldType::None).GetType() == ScriptFieldType::None);
}

TEST_CASE("ScriptField: every type round trips through JSON")
{
	ScriptFieldValue const values[] = {
		ScriptFieldValue::FromBool(true),
		ScriptFieldValue::FromInt32(-5),
		ScriptFieldValue::FromUInt32(5),
		ScriptFieldValue::FromInt64(-9007199254740993LL),
		ScriptFieldValue::FromUInt64(18446744073709551615ULL),
		ScriptFieldValue::FromFloat(1.25f),
		ScriptFieldValue::FromDouble(1e-300),
		ScriptFieldValue::FromString("hello"),
		ScriptFieldValue::FromVector2({1.0f, 2.0f}),
		ScriptFieldValue::FromVector3({1.0f, 2.0f, 3.0f}),
		ScriptFieldValue::FromVector4({1.0f, 2.0f, 3.0f, 4.0f}),
		ScriptFieldValue::FromQuaternion(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)),
		ScriptFieldValue::FromColor({0.5f, 0.25f, 1.0f, 1.0f}),
		ScriptFieldValue::FromEntity(UUID(1234)),
		ScriptFieldValue::FromPrefab(AssetHandle(UUID(55))),
		ScriptFieldValue::FromAsset(AssetHandle(UUID(66))),
	};

	for (ScriptFieldValue const& value : values)
	{
		CAPTURE(ScriptFieldTypeToString(value.GetType()));
		Json const json = ScriptFieldValueToJson(value);
		Result<ScriptFieldValue> const loaded = ScriptFieldValueFromJson(value.GetType(), json, DeserializationContext{});
		REQUIRE_MESSAGE(loaded.IsOk(), loaded.GetError());
		CHECK(loaded.GetValue() == value);
	}
}

TEST_CASE("ScriptField: 64-bit integers are stored as strings and accept numbers")
{
	CHECK(ScriptFieldValueToJson(ScriptFieldValue::FromUInt64(18446744073709551615ULL)) == Json("18446744073709551615"));
	Result<ScriptFieldValue> const fromNumber = ScriptFieldValueFromJson(ScriptFieldType::Int64, Json(42), DeserializationContext{});
	REQUIRE(fromNumber.IsOk());
	CHECK(fromNumber.GetValue().GetInt64() == 42);
	CHECK(ScriptFieldValueFromJson(ScriptFieldType::Int64, Json("12x"), DeserializationContext{}).IsError());
	CHECK(ScriptFieldValueFromJson(ScriptFieldType::UInt64, Json("-1"), DeserializationContext{}).IsError());
}

TEST_CASE("ScriptField: field maps validate structure and types")
{
	Json fields = Json::object();
	fields["Speed"] = Json::object({{"Type", "Float"}, {"Value", 3.5}});
	ScriptFieldMap map;
	REQUIRE(JsonTraits<ScriptFieldMap>::FromJson(fields, map, DeserializationContext{}).IsOk());
	CHECK(map.at("Speed").GetFloat() == doctest::Approx(3.5f));

	fields["Broken"] = Json::object({{"Type", "Float"}, {"Value", "fast"}});
	Result<void> const wrongValue = JsonTraits<ScriptFieldMap>::FromJson(fields, map, DeserializationContext{});
	REQUIRE(wrongValue.IsError());
	CHECK(wrongValue.GetError() == "field 'Broken': expected a number");
	// The map is unchanged after a failed read.
	CHECK(map.size() == 1);

	Json unknownType = Json::object();
	unknownType["X"] = Json::object({{"Type", "Matrix"}, {"Value", 1}});
	CHECK(JsonTraits<ScriptFieldMap>::FromJson(unknownType, map, DeserializationContext{}).GetError() ==
	      "field 'X': unknown type 'Matrix'");

	Json missingValue = Json::object();
	missingValue["X"] = Json::object({{"Type", "Float"}});
	CHECK(JsonTraits<ScriptFieldMap>::FromJson(missingValue, map, DeserializationContext{}).IsError());
	CHECK(JsonTraits<ScriptFieldMap>::FromJson(Json::array(), map, DeserializationContext{}).IsError());
}

namespace
{
	// One value of each element type, in type order (Bool to Asset).
	std::vector<ScriptFieldValue> MakeElementSamples()
	{
		return {
			ScriptFieldValue::FromBool(true),
			ScriptFieldValue::FromInt32(-5),
			ScriptFieldValue::FromUInt32(4294967295u),
			ScriptFieldValue::FromInt64(-9007199254740993LL),
			ScriptFieldValue::FromUInt64(18446744073709551615ULL),
			ScriptFieldValue::FromFloat(1.25f),
			ScriptFieldValue::FromDouble(1e-300),
			ScriptFieldValue::FromString("hello"),
			ScriptFieldValue::FromVector2({1.0f, 2.0f}),
			ScriptFieldValue::FromVector3({1.0f, 2.0f, 3.0f}),
			ScriptFieldValue::FromVector4({1.0f, 2.0f, 3.0f, 4.0f}),
			ScriptFieldValue::FromQuaternion(glm::quat(0.0f, 1.0f, 0.0f, 0.0f)),
			ScriptFieldValue::FromColor({0.5f, 0.25f, 1.0f, 1.0f}),
			ScriptFieldValue::FromEntity(UUID(1234)),
			ScriptFieldValue::FromPrefab(AssetHandle(UUID(55))),
			ScriptFieldValue::FromAsset(AssetHandle(UUID(66))),
		};
	}
}

TEST_CASE("ScriptField: list types are their element type with the list flag, named with []")
{
	for (int i = 1; i <= static_cast<int>(ScriptFieldType::Asset); i++)
	{
		ScriptFieldType const element = static_cast<ScriptFieldType>(i);
		ScriptFieldType const list = MakeScriptFieldListType(element);
		CAPTURE(ScriptFieldTypeToString(element));
		CHECK(list != element);
		CHECK(IsScriptFieldListType(list));
		CHECK_FALSE(IsScriptFieldListType(element));
		CHECK(GetScriptFieldElementType(list) == element);
		CHECK(GetScriptFieldElementType(element) == element);
		CHECK(IsValidScriptFieldType(list));
		CHECK(IsValidScriptFieldType(element));
		CHECK(std::string(ScriptFieldTypeToString(list)) == std::string(ScriptFieldTypeToString(element)) + "[]");
		CHECK(ScriptFieldTypeFromString(ScriptFieldTypeToString(list)) == std::optional<ScriptFieldType>(list));
		// No lists of lists.
		CHECK(MakeScriptFieldListType(list) == ScriptFieldType::None);
	}
	CHECK(std::string(ScriptFieldTypeToString(MakeScriptFieldListType(ScriptFieldType::Float))) == "Float[]");
	CHECK(MakeScriptFieldListType(ScriptFieldType::None) == ScriptFieldType::None);
	CHECK_FALSE(IsValidScriptFieldType(ScriptFieldType::None));
	CHECK_FALSE(IsValidScriptFieldType(static_cast<ScriptFieldType>(ScriptFieldListFlag)));
	CHECK_FALSE(IsValidScriptFieldType(static_cast<ScriptFieldType>(static_cast<uint8_t>(ScriptFieldType::Asset) + 1)));
	CHECK(std::string(ScriptFieldTypeToString(static_cast<ScriptFieldType>(ScriptFieldListFlag))) == "None");
	CHECK(std::string(ScriptFieldTypeToString(static_cast<ScriptFieldType>(0x7F))) == "None");
	for (std::string_view const name : {"None[]", "Float[][]", "[]", "Float []", "float[]", "Float[", "Vector5[]"})
	{
		CAPTURE(name);
		CHECK_FALSE(ScriptFieldTypeFromString(name).has_value());
	}
}

TEST_CASE("ScriptField: a list holds elements of its type and never equals a value of another type")
{
	for (int i = 1; i <= static_cast<int>(ScriptFieldType::Asset); i++)
	{
		ScriptFieldType const element = static_cast<ScriptFieldType>(i);
		CAPTURE(ScriptFieldTypeToString(element));
		ScriptFieldValue const empty = ScriptFieldValue::CreateDefault(MakeScriptFieldListType(element));
		CHECK(empty.GetType() == MakeScriptFieldListType(element));
		CHECK(empty.IsList());
		CHECK(empty.GetElementType() == element);
		CHECK(empty.GetElements().empty());
		CHECK_FALSE(ScriptFieldValue::CreateDefault(element).IsList());
	}
	CHECK(ScriptFieldValue::CreateDefault(static_cast<ScriptFieldType>(ScriptFieldListFlag)).GetType() == ScriptFieldType::None);

	ScriptFieldValue const one = ScriptFieldValue::FromList(ScriptFieldType::Float, {ScriptFieldValue::FromFloat(2.0f)});
	REQUIRE(one.GetElements().size() == 1);
	CHECK(one.GetElements()[0] == ScriptFieldValue::FromFloat(2.0f));
	// A list of one element is not that element, nor a list of another type with an equal value.
	CHECK_FALSE(one == ScriptFieldValue::FromFloat(2.0f));
	CHECK_FALSE(one == ScriptFieldValue::FromList(ScriptFieldType::Double, {ScriptFieldValue::FromDouble(2.0)}));
	CHECK_FALSE(ScriptFieldValue::FromList(ScriptFieldType::Float, {}) == ScriptFieldValue::FromList(ScriptFieldType::Double, {}));
	CHECK(one == ScriptFieldValue::FromList(ScriptFieldType::Float, {ScriptFieldValue::FromFloat(2.0f)}));
	CHECK_FALSE(one == ScriptFieldValue::FromList(ScriptFieldType::Float, {ScriptFieldValue::FromFloat(3.0f)}));
	CHECK_FALSE(one == ScriptFieldValue::FromList(ScriptFieldType::Float, {}));
}

TEST_CASE("ScriptField: lists of every element type round trip through JSON")
{
	for (ScriptFieldValue const& sample : MakeElementSamples())
	{
		ScriptFieldType const element = sample.GetType();
		CAPTURE(ScriptFieldTypeToString(element));
		for (size_t const count : {size_t{0}, size_t{1}, size_t{3}})
		{
			std::vector<ScriptFieldValue> elements(count, sample);
			if (count == 3)
			{
				elements[1] = ScriptFieldValue::CreateDefault(element);
			}
			ScriptFieldValue const list = ScriptFieldValue::FromList(element, elements);
			Json const json = ScriptFieldValueToJson(list);
			REQUIRE(json.is_array());
			CHECK(json.size() == count);
			for (size_t i = 0; i < count; i++)
			{
				// Each element is written as a value of the element type is.
				CHECK(json[i] == ScriptFieldValueToJson(elements[i]));
			}
			Result<ScriptFieldValue> const loaded = ScriptFieldValueFromJson(list.GetType(), json, DeserializationContext{});
			REQUIRE_MESSAGE(loaded.IsOk(), loaded.GetError());
			CHECK(loaded.GetValue() == list);
		}
	}

	// In a field map, a list is { "Type": "<Element>[]", "Value": [ ... ] }.
	ScriptFieldMap map;
	map["Path"] = ScriptFieldValue::FromList(ScriptFieldType::Vector3, {ScriptFieldValue::FromVector3({0.0f, 1.0f, 0.0f})});
	map["Speed"] = ScriptFieldValue::FromFloat(4.0f);
	Json const json = JsonTraits<ScriptFieldMap>::ToJson(map);
	CHECK(json ==
	      Json::parse(R"({ "Path": { "Type": "Vector3[]", "Value": [[0.0, 1.0, 0.0]] }, "Speed": { "Type": "Float", "Value": 4.0 } })"));
	ScriptFieldMap loaded;
	REQUIRE(JsonTraits<ScriptFieldMap>::FromJson(json, loaded, DeserializationContext{}).IsOk());
	CHECK(loaded == map);
}

TEST_CASE("ScriptField: a list holds at most 4096 elements")
{
	ScriptFieldType const ints = MakeScriptFieldListType(ScriptFieldType::Int32);
	Json full = Json::array();
	for (size_t i = 0; i < MaxScriptFieldListElements; i++)
	{
		full.push_back(static_cast<int32_t>(i));
	}
	Result<ScriptFieldValue> const atLimit = ScriptFieldValueFromJson(ints, full, DeserializationContext{});
	REQUIRE(atLimit.IsOk());
	CHECK(atLimit.GetValue().GetElements().size() == MaxScriptFieldListElements);
	CHECK(atLimit.GetValue().GetElements().back() == ScriptFieldValue::FromInt32(4095));
	CHECK(ScriptFieldValueToJson(atLimit.GetValue()) == full);

	full.push_back(0);
	CHECK(ScriptFieldValueFromJson(ints, full, DeserializationContext{}).GetError() == "expected an array of at most 4096 elements");
}

TEST_CASE("ScriptField: damaged lists are refused with the element that is damaged, and the map is kept")
{
	ScriptFieldType const floats = MakeScriptFieldListType(ScriptFieldType::Float);
	CHECK(ScriptFieldValueFromJson(floats, Json(1.0), DeserializationContext{}).GetError() == "expected an array of at most 4096 elements");
	CHECK(ScriptFieldValueFromJson(floats, Json::object(), DeserializationContext{}).IsError());
	CHECK(ScriptFieldValueFromJson(floats, Json(), DeserializationContext{}).IsError());
	CHECK(ScriptFieldValueFromJson(floats, Json::parse(R"([1, "fast"])"), DeserializationContext{}).GetError() ==
	      "element 1: expected a number");
	// No lists of lists, in values either.
	CHECK(ScriptFieldValueFromJson(floats, Json::parse("[[1.0]]"), DeserializationContext{}).GetError() == "element 0: expected a number");
	CHECK(
		ScriptFieldValueFromJson(MakeScriptFieldListType(ScriptFieldType::Int64), Json::parse(R"(["1", "12x"])"), DeserializationContext{})
			.GetError() == "element 1: expected a 64-bit integer");
	CHECK(ScriptFieldValueFromJson(MakeScriptFieldListType(ScriptFieldType::Vector3), Json::parse("[[1.0, 2.0]]"), DeserializationContext{})
	          .IsError());
	CHECK(ScriptFieldValueFromJson(static_cast<ScriptFieldType>(ScriptFieldListFlag), Json::array(), DeserializationContext{}).IsError());

	ScriptFieldMap map;
	map["Kept"] = ScriptFieldValue::FromInt32(1);
	Json fields = Json::object();
	fields["Speed"] = Json::object({{"Type", "Float[]"}, {"Value", Json::array({1.0, 2.0})}});
	fields["Broken"] = Json::object({{"Type", "Float[]"}, {"Value", Json::array({1.0, true})}});
	Result<void> const refused = JsonTraits<ScriptFieldMap>::FromJson(fields, map, DeserializationContext{});
	REQUIRE(refused.IsError());
	CHECK(refused.GetError() == "field 'Broken': element 1: expected a number");
	CHECK(map == ScriptFieldMap{{"Kept", ScriptFieldValue::FromInt32(1)}});

	for (std::string_view const type : {"None[]", "Float[][]", "Matrix[]"})
	{
		Json unknown = Json::object();
		unknown["X"] = Json::object({{"Type", type}, {"Value", Json::array()}});
		CHECK(JsonTraits<ScriptFieldMap>::FromJson(unknown, map, DeserializationContext{}).GetError() ==
		      fmt::format("field 'X': unknown type '{}'", type));
	}
}
