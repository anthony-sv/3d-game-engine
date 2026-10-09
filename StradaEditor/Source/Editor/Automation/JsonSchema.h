#pragma once

#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace Strada
{
	// A failed validation: the location of the offending value ("components.Transform", "entities[2]"; empty for the
	// parameters object itself) and a complete, user-facing message.
	struct SchemaViolation
	{
		std::string Path;
		std::string Message;
	};

	// The JSON Schema subset used for automation command parameters:
	//   type (one name or an array of names: object, array, string, integer, number, boolean, null), properties,
	//   required, additionalProperties (true, false or a schema), items, enum, minimum, maximum, minLength, maxLength
	//   (in Unicode code points), minItems, maxItems, minProperties, maxProperties.
	// description, title, default and examples are annotations. Integers may also be written as numbers without a
	// fractional part (3.0), as JSON Schema 2020-12 specifies.
	class JsonSchema
	{
	public:
		// Checks that a schema uses only the supported keywords with well-formed values, that every required property is
		// declared and that defaults satisfy their schema. Used for the schemas of registered commands.
		[[nodiscard]] static Result<void> CheckDefinition(Json const& schema);

		// Returns the first violation, or nothing when the value satisfies the schema. Objects report unknown properties
		// first, then missing required ones, then invalid values (in schema property order), so typos are explained
		// before their consequences.
		static std::optional<SchemaViolation> Validate(Json const& value, Json const& schema);
	};

	// Fluent construction of parameter schemas:
	//   SchemaBuilder::Object().Property("name", SchemaBuilder::String("The new name").MinLength(1), true).Build()
	// Object schemas reject unknown properties unless AdditionalProperties or AllowAdditionalProperties is used.
	class SchemaBuilder
	{
	public:
		static SchemaBuilder Object(std::string_view description = {});
		static SchemaBuilder String(std::string_view description = {});
		static SchemaBuilder Integer(std::string_view description = {});
		static SchemaBuilder Number(std::string_view description = {});
		static SchemaBuilder Boolean(std::string_view description = {});
		static SchemaBuilder Array(SchemaBuilder items, std::string_view description = {});

		SchemaBuilder& Description(std::string_view description);
		// Also accepts null.
		SchemaBuilder& Nullable();
		SchemaBuilder& Enum(std::initializer_list<std::string_view> values);
		SchemaBuilder& Minimum(Json minimum);
		SchemaBuilder& Maximum(Json maximum);
		SchemaBuilder& MinLength(uint64_t length);
		SchemaBuilder& MaxLength(uint64_t length);
		SchemaBuilder& MinItems(uint64_t count);
		SchemaBuilder& MaxItems(uint64_t count);
		SchemaBuilder& MinProperties(uint64_t count);
		SchemaBuilder& Default(Json value);
		SchemaBuilder& Property(std::string_view name, SchemaBuilder schema, bool required = false);
		// Unknown properties are allowed when they satisfy the schema (maps such as component name -> fields).
		SchemaBuilder& AdditionalProperties(SchemaBuilder schema);
		// Unknown properties are allowed with any value.
		SchemaBuilder& AllowAdditionalProperties();

		// The schema with its keywords in a canonical order.
		Json Build() const;

	private:
		explicit SchemaBuilder(std::string_view type, std::string_view description);

		Json m_Schema;
	};
}
