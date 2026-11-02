#include "Editor/Automation/JsonSchema.h"

#include <doctest/doctest.h>

#include <span>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	Json MakeSchema()
	{
		return SchemaBuilder::Object()
		    .Property("name", SchemaBuilder::String("Name").MinLength(1).MaxLength(4), true)
		    .Property("count", SchemaBuilder::Integer("Count").Minimum(0).Maximum(10))
		    .Property("ratio", SchemaBuilder::Number("Ratio").Minimum(0.5))
		    .Property("mode", SchemaBuilder::String("Mode").Enum({"a", "b"}))
		    .Property("tags", SchemaBuilder::Array(SchemaBuilder::String(), "Tags").MinItems(1).MaxItems(2))
		    .Property("parent", SchemaBuilder::String("Parent").Nullable())
		    .Property("map", SchemaBuilder::Object("Map").AdditionalProperties(SchemaBuilder::Integer()))
		    .Build();
	}

	std::string ViolationPath(Json const& value)
	{
		std::optional<SchemaViolation> const violation = JsonSchema::Validate(value, MakeSchema());
		REQUIRE(violation.has_value());
		return violation->Path;
	}
}

TEST_CASE("JsonSchema: builder output is a valid definition")
{
	CHECK(JsonSchema::CheckDefinition(MakeSchema()).IsOk());
	CHECK(JsonSchema::CheckDefinition(Json::object({{"type", "object"}, {"pattern", "x"}})).IsError());
	CHECK(JsonSchema::CheckDefinition(Json::object({{"type", "object"}, {"required", {"missing"}}, {"properties", Json::object()}}))
	          .IsError());
	CHECK(JsonSchema::CheckDefinition(Json::object({{"type", "nonsense"}})).IsError());
}

TEST_CASE("JsonSchema: enums are built from lists known at compile time or at run time alike")
{
	std::vector<std::string_view> const values = {"Rendering", "UI"};
	Json const fromSpan = SchemaBuilder::String("Category").Enum(values).Build();
	CHECK(fromSpan == SchemaBuilder::String("Category").Enum({"Rendering", "UI"}).Build());
	CHECK(fromSpan["enum"] == Json::array({"Rendering", "UI"}));
	CHECK_FALSE(JsonSchema::Validate("UI", fromSpan).has_value());
	CHECK(JsonSchema::Validate("Core", fromSpan).has_value());
	// An empty list accepts nothing, which no command's parameter may do.
	CHECK(JsonSchema::CheckDefinition(
			  SchemaBuilder::Object().Property("category", SchemaBuilder::String().Enum(std::span<std::string_view const>())).Build())
	          .IsError());
}

TEST_CASE("JsonSchema: valid values pass")
{
	Json const value = Json::object({{"name", "abc"},
	                                 {"count", 10},
	                                 {"ratio", 0.5},
	                                 {"mode", "b"},
	                                 {"tags", {"x"}},
	                                 {"parent", nullptr},
	                                 {"map", Json::object({{"k", 1}})}});
	CHECK_FALSE(JsonSchema::Validate(value, MakeSchema()).has_value());
	// Integers may be written as whole numbers.
	CHECK_FALSE(JsonSchema::Validate(Json::object({{"name", "a"}, {"count", 3.0}}), MakeSchema()).has_value());
}

TEST_CASE("JsonSchema: violations point at the offending value")
{
	CHECK(ViolationPath(Json::object()) == "name");
	CHECK(ViolationPath(Json::object({{"name", ""}})) == "name");
	CHECK(ViolationPath(Json::object({{"name", "toolong"}})) == "name");
	CHECK(ViolationPath(Json::object({{"name", "a"}, {"count", 11}})) == "count");
	CHECK(ViolationPath(Json::object({{"name", "a"}, {"count", 1.5}})) == "count");
	CHECK(ViolationPath(Json::object({{"name", "a"}, {"ratio", 0.1}})) == "ratio");
	CHECK(ViolationPath(Json::object({{"name", "a"}, {"mode", "c"}})) == "mode");
	CHECK(ViolationPath(Json::object({{"name", "a"}, {"tags", Json::array()}})) == "tags");
	CHECK(ViolationPath(Json::object({{"name", "a"}, {"tags", {"x", 3}}})) == "tags[1]");
	CHECK(ViolationPath(Json::object({{"name", "a"}, {"map", Json::object({{"k", "v"}})}})) == "map.k");
	// Unknown properties are reported before missing required ones.
	CHECK(ViolationPath(Json::object({{"nmae", "a"}})) == "nmae");
	CHECK(JsonSchema::Validate(Json::array(), MakeSchema()).has_value());
}

TEST_CASE("JsonSchema: Types accepts a value of any type it lists, and Any accepts every value")
{
	// A reference: an ID (at least 0) or a name; and a pair of them, as a link end is.
	Json const reference = SchemaBuilder::Types({"integer", "string"}, "An ID or a name").Minimum(0).MinLength(1).Build();
	CHECK(
		reference ==
		Json::object({{"type", Json::array({"integer", "string"})}, {"description", "An ID or a name"}, {"minimum", 0}, {"minLength", 1}}));
	Json const end = SchemaBuilder::Array(SchemaBuilder::Types({"integer", "string"}), "A link end").MinItems(2).MaxItems(2).Build();
	Json const value = SchemaBuilder::Any("A literal").Build();
	CHECK(value == Json::object({{"description", "A literal"}}));
	Json const schema =
		SchemaBuilder::Object()
			.Property("reference", SchemaBuilder::Types({"integer", "string"}, "An ID or a name").Minimum(0).MinLength(1))
			.Property("end", SchemaBuilder::Array(SchemaBuilder::Types({"integer", "string"}), "A link end").MinItems(2).MaxItems(2))
			.Property("value", SchemaBuilder::Any("A literal"))
			.Property("optional", SchemaBuilder::Types({"integer", "string"}).Nullable())
			.Property("anything", SchemaBuilder::Any().Nullable())
			.Build();
	REQUIRE(JsonSchema::CheckDefinition(schema).IsOk());
	CHECK(schema["properties"]["optional"]["type"] == Json::array({"integer", "string", "null"}));
	// Nullable leaves a schema without a type as it is: it accepts null already.
	CHECK(schema["properties"]["anything"] == Json::object());

	for (Json const& accepted : {Json(0), Json(12), Json(3.0), Json("Speed"), Json("$speed")})
	{
		CAPTURE(accepted);
		CHECK_FALSE(JsonSchema::Validate(accepted, reference).has_value());
	}
	for (Json const& refused : {Json(-1), Json(""), Json(1.5), Json(true), Json(), Json::array(), Json::object()})
	{
		CAPTURE(refused);
		std::optional<SchemaViolation> const violation = JsonSchema::Validate(Json::object({{"reference", refused}}), schema);
		REQUIRE(violation.has_value());
		CHECK(violation->Path == "reference");
	}
	CHECK(JsonSchema::Validate(Json::object({{"reference", true}}), schema)->Message.find("an integer or a string") != std::string::npos);

	CHECK_FALSE(JsonSchema::Validate(Json::object({{"end", Json::array({12, "Then"})}}), schema).has_value());
	CHECK_FALSE(JsonSchema::Validate(Json::object({{"end", Json::array({"$branch", "True"})}}), schema).has_value());
	CHECK(JsonSchema::Validate(Json::object({{"end", Json::array({12})}}), schema)->Path == "end");
	CHECK(JsonSchema::Validate(Json::object({{"end", Json::array({12, "Then", 3})}}), schema)->Path == "end");
	CHECK(JsonSchema::Validate(Json::object({{"end", Json::array({12, Json::array()})}}), schema)->Path == "end[1]");

	for (Json const& anything : {Json(), Json(true), Json(-1), Json(2.5), Json("text"), Json::array({1, "a"}),
	                             Json::object({{"Parameters", Json::array({Json::object({{"Name", "A"}, {"Type", "int"}})})}})})
	{
		CAPTURE(anything);
		CHECK_FALSE(JsonSchema::Validate(Json::object({{"value", anything}, {"anything", anything}}), schema).has_value());
	}
	CHECK_FALSE(JsonSchema::Validate(Json::object({{"optional", nullptr}}), schema).has_value());
	CHECK_FALSE(JsonSchema::Validate(Json::object({{"optional", "Speed"}}), schema).has_value());
	CHECK(JsonSchema::Validate(Json::object({{"optional", 1.5}}), schema)->Path == "optional");
}
