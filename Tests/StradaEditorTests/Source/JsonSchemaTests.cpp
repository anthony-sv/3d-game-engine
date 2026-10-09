#include "Editor/Automation/JsonSchema.h"

#include <doctest/doctest.h>

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
