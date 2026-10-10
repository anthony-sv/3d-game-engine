#include "Strada/Script/ScriptField.h"
#include "Strada/Script/ScriptFieldSerialization.h"

#include <doctest/doctest.h>

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
