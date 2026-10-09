#include "Editor/Automation/JsonSchema.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr std::array<std::string_view, 7> TypeNames = {"object", "array", "string", "integer", "number", "boolean", "null"};

		// Every supported keyword, in the order SchemaBuilder::Build writes them.
		constexpr std::array<std::string_view, 18> Keywords = {
			"type",          "title",         "description", "enum",      "default",
			"minimum",       "maximum",       "minLength",   "maxLength", "items",
			"minItems",      "maxItems",      "properties",  "required",  "additionalProperties",
			"minProperties", "maxProperties", "examples"};

		bool IsKeyword(std::string_view key)
		{
			return std::find(Keywords.begin(), Keywords.end(), key) != Keywords.end();
		}

		bool IsTypeName(std::string_view name)
		{
			return std::find(TypeNames.begin(), TypeNames.end(), name) != TypeNames.end();
		}

		bool MatchesType(Json const& value, std::string_view type)
		{
			if (type == "object")
			{
				return value.is_object();
			}
			if (type == "array")
			{
				return value.is_array();
			}
			if (type == "string")
			{
				return value.is_string();
			}
			if (type == "integer")
			{
				if (value.is_number_integer())
				{
					return true;
				}
				if (value.is_number_float())
				{
					double const number = value.get<double>();
					return std::isfinite(number) && std::floor(number) == number;
				}
				return false;
			}
			if (type == "number")
			{
				return value.is_number();
			}
			if (type == "boolean")
			{
				return value.is_boolean();
			}
			return type == "null" && value.is_null();
		}

		std::string_view DescribeType(std::string_view type)
		{
			if (type == "object")
			{
				return "an object";
			}
			if (type == "array")
			{
				return "an array";
			}
			if (type == "string")
			{
				return "a string";
			}
			if (type == "integer")
			{
				return "an integer";
			}
			if (type == "number")
			{
				return "a number";
			}
			if (type == "boolean")
			{
				return "true or false";
			}
			return "null";
		}

		// "a string", "a string or null", "an integer, a string or null".
		std::string DescribeTypes(Json const& type)
		{
			if (type.is_string())
			{
				return std::string(DescribeType(type.get_ref<std::string const&>()));
			}
			std::string text;
			for (size_t i = 0; i < type.size(); i++)
			{
				if (i > 0)
				{
					text += i + 1 == type.size() ? " or " : ", ";
				}
				text += DescribeType(type[i].get_ref<std::string const&>());
			}
			return text;
		}

		std::string JoinValues(Json const& values)
		{
			std::string text;
			for (Json const& value : values)
			{
				text += text.empty() ? "" : ", ";
				text += value.dump(-1, ' ', false, Json::error_handler_t::replace);
			}
			return text;
		}

		std::string ChildPath(std::string const& parent, std::string_view property)
		{
			return parent.empty() ? std::string(property) : fmt::format("{}.{}", parent, property);
		}

		std::string ItemPath(std::string const& parent, size_t index)
		{
			return fmt::format("{}[{}]", parent, index);
		}

		std::string Subject(std::string const& path)
		{
			return path.empty() ? std::string("parameters") : fmt::format("parameter '{}'", path);
		}

		// JSON Schema measures strings in code points; continuation bytes of multi-byte UTF-8 sequences are not counted.
		size_t CountCodePoints(std::string const& text)
		{
			return static_cast<size_t>(std::count_if(text.begin(), text.end(),
			                                         [](char character)
			                                         {
														 return (static_cast<unsigned char>(character) & 0xC0) != 0x80;
													 }));
		}

		std::optional<uint64_t> ReadCount(Json const& schema, std::string_view keyword)
		{
			auto const it = schema.find(keyword);
			if (it == schema.end())
			{
				return std::nullopt;
			}
			return it->get<uint64_t>();
		}

		std::optional<SchemaViolation> CheckCount(size_t count, Json const& schema, std::string_view minimumKeyword,
		                                          std::string_view maximumKeyword, std::string_view unit, std::string const& path)
		{
			if (std::optional<uint64_t> const minimum = ReadCount(schema, minimumKeyword); minimum && count < *minimum)
			{
				if (*minimum == 1)
				{
					return SchemaViolation{path, fmt::format("{} must not be empty", Subject(path))};
				}
				return SchemaViolation{path, fmt::format("{} must have at least {} {}", Subject(path), *minimum, unit)};
			}
			if (std::optional<uint64_t> const maximum = ReadCount(schema, maximumKeyword); maximum && count > *maximum)
			{
				return SchemaViolation{path, fmt::format("{} must have at most {} {}", Subject(path), *maximum, unit)};
			}
			return std::nullopt;
		}

		std::optional<SchemaViolation> ValidateValue(Json const& value, Json const& schema, std::string const& path);

		std::optional<SchemaViolation> ValidateObject(Json const& value, Json const& schema, std::string const& path)
		{
			if (std::optional<SchemaViolation> violation =
			        CheckCount(value.size(), schema, "minProperties", "maxProperties", "properties", path))
			{
				return violation;
			}

			auto const propertiesIt = schema.find("properties");
			Json const* properties = propertiesIt != schema.end() ? &*propertiesIt : nullptr;
			auto const additional = schema.find("additionalProperties");
			bool const rejectsUnknown = additional != schema.end() && additional->is_boolean() && !additional->get<bool>();
			bool const validatesUnknown = additional != schema.end() && additional->is_object();

			if (rejectsUnknown)
			{
				for (auto const& item : value.items())
				{
					if (properties != nullptr && properties->contains(item.key()))
					{
						continue;
					}
					std::string expected;
					if (properties != nullptr && !properties->empty())
					{
						for (auto const& property : properties->items())
						{
							expected += expected.empty() ? "" : ", ";
							expected += property.key();
						}
						expected = fmt::format(" (expected: {})", expected);
					}
					else
					{
						expected = " (none are accepted)";
					}
					std::string const childPath = ChildPath(path, item.key());
					return SchemaViolation{childPath, fmt::format("unknown parameter '{}'{}", childPath, expected)};
				}
			}

			if (auto const required = schema.find("required"); required != schema.end())
			{
				for (Json const& name : *required)
				{
					std::string const& property = name.get_ref<std::string const&>();
					if (!value.contains(property))
					{
						std::string const childPath = ChildPath(path, property);
						return SchemaViolation{childPath, fmt::format("missing required parameter '{}'", childPath)};
					}
				}
			}

			if (properties != nullptr)
			{
				for (auto const& property : properties->items())
				{
					if (auto const it = value.find(property.key()); it != value.end())
					{
						if (std::optional<SchemaViolation> violation =
						        ValidateValue(*it, property.value(), ChildPath(path, property.key())))
						{
							return violation;
						}
					}
				}
			}

			if (validatesUnknown)
			{
				for (auto const& item : value.items())
				{
					if (properties != nullptr && properties->contains(item.key()))
					{
						continue;
					}
					if (std::optional<SchemaViolation> violation = ValidateValue(item.value(), *additional, ChildPath(path, item.key())))
					{
						return violation;
					}
				}
			}
			return std::nullopt;
		}

		std::optional<SchemaViolation> ValidateValue(Json const& value, Json const& schema, std::string const& path)
		{
			if (auto const type = schema.find("type"); type != schema.end())
			{
				bool matches = false;
				if (type->is_string())
				{
					matches = MatchesType(value, type->get_ref<std::string const&>());
				}
				else
				{
					matches = std::any_of(type->begin(), type->end(),
					                      [&value](Json const& name)
					                      {
											  return MatchesType(value, name.get_ref<std::string const&>());
										  });
				}
				if (!matches)
				{
					return SchemaViolation{path, fmt::format("{} must be {}", Subject(path), DescribeTypes(*type))};
				}
			}

			if (auto const values = schema.find("enum"); values != schema.end())
			{
				if (std::none_of(values->begin(), values->end(),
				                 [&value](Json const& allowed)
				                 {
									 return allowed == value;
								 }))
				{
					return SchemaViolation{path, fmt::format("{} must be one of: {}", Subject(path), JoinValues(*values))};
				}
			}

			if (value.is_number())
			{
				double const number = value.get<double>();
				if (auto const minimum = schema.find("minimum"); minimum != schema.end() && number < minimum->get<double>())
				{
					return SchemaViolation{path, fmt::format("{} must be at least {}", Subject(path), minimum->dump())};
				}
				if (auto const maximum = schema.find("maximum"); maximum != schema.end() && number > maximum->get<double>())
				{
					return SchemaViolation{path, fmt::format("{} must be at most {}", Subject(path), maximum->dump())};
				}
			}
			else if (value.is_string())
			{
				size_t const length = CountCodePoints(value.get_ref<std::string const&>());
				if (std::optional<uint64_t> const minimum = ReadCount(schema, "minLength"); minimum && length < *minimum)
				{
					if (*minimum == 1)
					{
						return SchemaViolation{path, fmt::format("{} must not be empty", Subject(path))};
					}
					return SchemaViolation{path, fmt::format("{} must be at least {} characters long", Subject(path), *minimum)};
				}
				if (std::optional<uint64_t> const maximum = ReadCount(schema, "maxLength"); maximum && length > *maximum)
				{
					return SchemaViolation{path, fmt::format("{} must be at most {} characters long", Subject(path), *maximum)};
				}
			}
			else if (value.is_array())
			{
				if (std::optional<SchemaViolation> violation = CheckCount(value.size(), schema, "minItems", "maxItems", "items", path))
				{
					return violation;
				}
				if (auto const items = schema.find("items"); items != schema.end())
				{
					for (size_t i = 0; i < value.size(); i++)
					{
						if (std::optional<SchemaViolation> violation = ValidateValue(value[i], *items, ItemPath(path, i)))
						{
							return violation;
						}
					}
				}
			}
			else if (value.is_object())
			{
				return ValidateObject(value, schema, path);
			}
			return std::nullopt;
		}

		bool IsCount(Json const& value)
		{
			return value.is_number_unsigned() || (value.is_number_integer() && value.get<int64_t>() >= 0);
		}

		Result<void> CheckCountPair(Json const& schema, std::string_view minimumKeyword, std::string_view maximumKeyword,
		                            std::string const& location)
		{
			for (std::string_view const keyword : {minimumKeyword, maximumKeyword})
			{
				if (auto const it = schema.find(keyword); it != schema.end() && !IsCount(*it))
				{
					return MakeError("{}: '{}' must be a non-negative integer", location, keyword);
				}
			}
			auto const minimum = schema.find(minimumKeyword);
			auto const maximum = schema.find(maximumKeyword);
			if (minimum != schema.end() && maximum != schema.end() && minimum->get<uint64_t>() > maximum->get<uint64_t>())
			{
				return MakeError("{}: '{}' is greater than '{}'", location, minimumKeyword, maximumKeyword);
			}
			return {};
		}

		Result<void> CheckSchema(Json const& schema, std::string const& location)
		{
			if (!schema.is_object())
			{
				return MakeError("{}: a schema must be an object", location);
			}
			for (auto const& item : schema.items())
			{
				if (!IsKeyword(item.key()))
				{
					return MakeError("{}: unsupported keyword '{}'", location, item.key());
				}
			}

			if (auto const type = schema.find("type"); type != schema.end())
			{
				if (type->is_string())
				{
					if (!IsTypeName(type->get_ref<std::string const&>()))
					{
						return MakeError("{}: unknown type '{}'", location, type->get_ref<std::string const&>());
					}
				}
				else if (type->is_array() && !type->empty())
				{
					std::unordered_set<std::string> seen;
					for (Json const& name : *type)
					{
						if (!name.is_string() || !IsTypeName(name.get_ref<std::string const&>()) ||
						    !seen.insert(name.get_ref<std::string const&>()).second)
						{
							return MakeError("{}: 'type' lists an unknown or repeated type", location);
						}
					}
				}
				else
				{
					return MakeError("{}: 'type' must be a type name or a non-empty array of type names", location);
				}
			}

			for (std::string_view const keyword : {"description", "title"})
			{
				if (auto const it = schema.find(keyword); it != schema.end() && !it->is_string())
				{
					return MakeError("{}: '{}' must be a string", location, keyword);
				}
			}

			auto const properties = schema.find("properties");
			if (properties != schema.end())
			{
				if (!properties->is_object())
				{
					return MakeError("{}: 'properties' must be an object", location);
				}
				for (auto const& property : properties->items())
				{
					if (Result<void> result = CheckSchema(property.value(), fmt::format("{}.properties.{}", location, property.key()));
					    !result)
					{
						return result;
					}
				}
			}

			if (auto const required = schema.find("required"); required != schema.end())
			{
				if (!required->is_array())
				{
					return MakeError("{}: 'required' must be an array of property names", location);
				}
				std::unordered_set<std::string> seen;
				for (Json const& name : *required)
				{
					if (!name.is_string() || !seen.insert(name.get_ref<std::string const&>()).second)
					{
						return MakeError("{}: 'required' must list distinct property names", location);
					}
					if (properties == schema.end() || !properties->contains(name.get_ref<std::string const&>()))
					{
						return MakeError("{}: required property '{}' is not declared in 'properties'", location,
						                 name.get_ref<std::string const&>());
					}
				}
			}

			if (auto const additional = schema.find("additionalProperties"); additional != schema.end() && !additional->is_boolean())
			{
				if (Result<void> result = CheckSchema(*additional, location + ".additionalProperties"); !result)
				{
					return result;
				}
			}

			if (auto const items = schema.find("items"); items != schema.end())
			{
				if (Result<void> result = CheckSchema(*items, location + ".items"); !result)
				{
					return result;
				}
			}

			if (auto const values = schema.find("enum"); values != schema.end() && (!values->is_array() || values->empty()))
			{
				return MakeError("{}: 'enum' must be a non-empty array", location);
			}

			auto const minimum = schema.find("minimum");
			auto const maximum = schema.find("maximum");
			if ((minimum != schema.end() && !minimum->is_number()) || (maximum != schema.end() && !maximum->is_number()))
			{
				return MakeError("{}: 'minimum' and 'maximum' must be numbers", location);
			}
			if (minimum != schema.end() && maximum != schema.end() && minimum->get<double>() > maximum->get<double>())
			{
				return MakeError("{}: 'minimum' is greater than 'maximum'", location);
			}

			for (auto const& [minimumKeyword, maximumKeyword] : {std::pair<std::string_view, std::string_view>{"minLength", "maxLength"},
			                                                     {"minItems", "maxItems"},
			                                                     {"minProperties", "maxProperties"}})
			{
				if (Result<void> result = CheckCountPair(schema, minimumKeyword, maximumKeyword, location); !result)
				{
					return result;
				}
			}

			if (auto const examples = schema.find("examples"); examples != schema.end() && !examples->is_array())
			{
				return MakeError("{}: 'examples' must be an array", location);
			}

			if (auto const defaultValue = schema.find("default"); defaultValue != schema.end())
			{
				if (std::optional<SchemaViolation> violation = ValidateValue(*defaultValue, schema, {}))
				{
					return MakeError("{}: the default value does not satisfy the schema ({})", location, violation->Message);
				}
			}
			return {};
		}
	}

	Result<void> JsonSchema::CheckDefinition(Json const& schema)
	{
		return CheckSchema(schema, "schema");
	}

	std::optional<SchemaViolation> JsonSchema::Validate(Json const& value, Json const& schema)
	{
		return ValidateValue(value, schema, {});
	}

	SchemaBuilder::SchemaBuilder(std::string_view type, std::string_view description)
		: m_Schema(Json::object())
	{
		m_Schema["type"] = std::string(type);
		if (!description.empty())
		{
			m_Schema["description"] = std::string(description);
		}
	}

	SchemaBuilder SchemaBuilder::Object(std::string_view description)
	{
		SchemaBuilder builder("object", description);
		builder.m_Schema["additionalProperties"] = false;
		return builder;
	}

	SchemaBuilder SchemaBuilder::String(std::string_view description)
	{
		return SchemaBuilder("string", description);
	}

	SchemaBuilder SchemaBuilder::Integer(std::string_view description)
	{
		return SchemaBuilder("integer", description);
	}

	SchemaBuilder SchemaBuilder::Number(std::string_view description)
	{
		return SchemaBuilder("number", description);
	}

	SchemaBuilder SchemaBuilder::Boolean(std::string_view description)
	{
		return SchemaBuilder("boolean", description);
	}

	SchemaBuilder SchemaBuilder::Array(SchemaBuilder items, std::string_view description)
	{
		SchemaBuilder builder("array", description);
		builder.m_Schema["items"] = items.Build();
		return builder;
	}

	SchemaBuilder& SchemaBuilder::Description(std::string_view description)
	{
		m_Schema["description"] = std::string(description);
		return *this;
	}

	SchemaBuilder& SchemaBuilder::Nullable()
	{
		Json& type = m_Schema["type"];
		if (type.is_string())
		{
			if (type.get_ref<std::string const&>() != "null")
			{
				type = Json::array({type, "null"});
			}
		}
		else if (std::find(type.begin(), type.end(), Json("null")) == type.end())
		{
			type.push_back("null");
		}
		return *this;
	}

	SchemaBuilder& SchemaBuilder::Enum(std::initializer_list<std::string_view> values)
	{
		Json list = Json::array();
		for (std::string_view const value : values)
		{
			list.push_back(std::string(value));
		}
		m_Schema["enum"] = std::move(list);
		return *this;
	}

	SchemaBuilder& SchemaBuilder::Minimum(Json minimum)
	{
		m_Schema["minimum"] = std::move(minimum);
		return *this;
	}

	SchemaBuilder& SchemaBuilder::Maximum(Json maximum)
	{
		m_Schema["maximum"] = std::move(maximum);
		return *this;
	}

	SchemaBuilder& SchemaBuilder::MinLength(uint64_t length)
	{
		m_Schema["minLength"] = length;
		return *this;
	}

	SchemaBuilder& SchemaBuilder::MaxLength(uint64_t length)
	{
		m_Schema["maxLength"] = length;
		return *this;
	}

	SchemaBuilder& SchemaBuilder::MinItems(uint64_t count)
	{
		m_Schema["minItems"] = count;
		return *this;
	}

	SchemaBuilder& SchemaBuilder::MaxItems(uint64_t count)
	{
		m_Schema["maxItems"] = count;
		return *this;
	}

	SchemaBuilder& SchemaBuilder::MinProperties(uint64_t count)
	{
		m_Schema["minProperties"] = count;
		return *this;
	}

	SchemaBuilder& SchemaBuilder::Default(Json value)
	{
		m_Schema["default"] = std::move(value);
		return *this;
	}

	SchemaBuilder& SchemaBuilder::Property(std::string_view name, SchemaBuilder schema, bool required)
	{
		m_Schema["properties"][std::string(name)] = schema.Build();
		if (required)
		{
			m_Schema["required"].push_back(std::string(name));
		}
		return *this;
	}

	SchemaBuilder& SchemaBuilder::AdditionalProperties(SchemaBuilder schema)
	{
		m_Schema["additionalProperties"] = schema.Build();
		return *this;
	}

	SchemaBuilder& SchemaBuilder::AllowAdditionalProperties()
	{
		m_Schema["additionalProperties"] = true;
		return *this;
	}

	Json SchemaBuilder::Build() const
	{
		Json schema = Json::object();
		for (std::string_view const keyword : Keywords)
		{
			if (auto const it = m_Schema.find(keyword); it != m_Schema.end())
			{
				schema[std::string(keyword)] = *it;
			}
		}
		return schema;
	}
}
