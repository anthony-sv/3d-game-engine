#include "stpch.h"
#include "Strada/Serialization/StructSerialization.h"

namespace Strada
{
	namespace
	{
		bool IsIntegerKind(FieldKind kind)
		{
			return kind == FieldKind::Int || kind == FieldKind::UInt;
		}

		// Integer fields report integer bounds ("Min": 1 rather than 1.0).
		Json BoundToJson(double bound, FieldDescriptor const& field)
		{
			FieldKind const kind = field.Kind == FieldKind::Array ? field.ElementKind : field.Kind;
			if (IsIntegerKind(kind))
			{
				return static_cast<int64_t>(bound);
			}
			return bound;
		}

		Json DescribeField(FieldDescriptor const& field)
		{
			Json description = Json::object();
			description["Name"] = std::string(field.Name);
			description["Type"] = field.TypeName;
			description["Default"] = field.Default;
			if (!field.EnumValues.empty())
			{
				Json values = Json::array();
				for (std::string_view const value : field.EnumValues)
				{
					values.push_back(std::string(value));
				}
				description["Values"] = std::move(values);
			}
			if (field.Hints.HasMin())
			{
				description["Min"] = BoundToJson(field.Hints.Min, field);
			}
			if (field.Hints.HasMax())
			{
				description["Max"] = BoundToJson(field.Hints.Max, field);
			}
			if (field.Hints.Display != FieldDisplay::Default)
			{
				description["Display"] = std::string(EnumToString(field.Hints.Display));
			}
			if (!field.Hints.AssetTypeName.empty())
			{
				description["AssetType"] = std::string(field.Hints.AssetTypeName);
			}
			if (!field.Hints.Description.empty())
			{
				description["Description"] = std::string(field.Hints.Description);
			}
			if (!field.Fields.empty())
			{
				Json nested = Json::array();
				for (FieldDescriptor const& child : field.Fields)
				{
					nested.push_back(DescribeField(child));
				}
				description["Fields"] = std::move(nested);
			}
			return description;
		}
	}

	Json DescribeFields(std::string_view name, std::span<FieldDescriptor const> fields, std::string_view description)
	{
		Json descriptions = Json::array();
		for (FieldDescriptor const& field : fields)
		{
			descriptions.push_back(DescribeField(field));
		}

		Json result = Json::object();
		result["Name"] = std::string(name);
		if (!description.empty())
		{
			result["Description"] = std::string(description);
		}
		result["Fields"] = std::move(descriptions);
		return result;
	}

	namespace Detail
	{
		std::string DescribeRangeViolation(FieldHints const& hints, bool isVector, bool isArray)
		{
			// "must be at least 0", "every component must be between 0 and 1", ...
			std::string_view const subject =
				isArray ? (isVector ? "every component of every element " : "every element ") : (isVector ? "every component " : "");
			if (hints.HasMin() && hints.HasMax())
			{
				return fmt::format("{}must be between {} and {}", subject, hints.Min, hints.Max);
			}
			if (hints.HasMin())
			{
				return fmt::format("{}must be at least {}", subject, hints.Min);
			}
			return fmt::format("{}must be at most {}", subject, hints.Max);
		}
	}
}
