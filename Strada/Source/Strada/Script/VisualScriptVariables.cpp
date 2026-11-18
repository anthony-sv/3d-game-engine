#include "stpch.h"
#include "Strada/Script/VisualScriptVariables.h"

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Strada
{
	namespace
	{
		constexpr std::string_view EditableClassName = "Visual Script";
		constexpr std::string_view NeedsRuntimeReason = "needs the scripting runtime";
		constexpr std::string_view NotExposableReason = "not exposable";

		// What a public variable's values are, when components can set them.
		struct VariableValues
		{
			ScriptFieldType ValueType = ScriptFieldType::None;
			AssetType AcceptedAssetType = AssetType::None;
			// The enum, for enums.
			std::optional<VisualScriptEnumInfo> Enum;
		};

		// What values of the type are (an exposable type, or an enum the lookup describes as the runtime does), or the reason
		// components cannot set them.
		std::variant<VariableValues, std::string_view> DescribeValues(std::string_view type, VisualScriptEnumLookup const& enumLookup)
		{
			if (std::optional<VisualScriptExposableType> const exposable = FindVisualScriptExposableType(type))
			{
				return VariableValues{exposable->FieldType, exposable->AcceptedAssetType, std::nullopt};
			}
			bool const mayBeEnum = CanBeVisualScriptEnum(type);
			std::optional<VisualScriptEnumInfo> enumInfo = mayBeEnum && enumLookup ? enumLookup(type) : std::nullopt;
			if (!enumInfo || !enumInfo->IsUsable())
			{
				return mayBeEnum && !enumLookup ? NeedsRuntimeReason : NotExposableReason;
			}
			ScriptFieldType const underlying = enumInfo->UnderlyingType;
			return VariableValues{underlying, AssetType::None, std::move(enumInfo)};
		}

		// A value's literal read as one of the values (the enum found, not looked up again).
		std::optional<ScriptFieldValue> DecodeValue(Json const& literal, std::string_view type, VariableValues const& values)
		{
			return values.Enum ? DecodeVisualScriptEnumLiteral(literal, *values.Enum) : DecodeVisualScriptLiteral(literal, type, {});
		}

		// An array literal of the values: at most MaxScriptFieldListElements literals (DecodeVisualScriptArrayLiteral's rules).
		std::optional<ScriptFieldValue> DecodeList(Json const& literal, std::string_view elementType, VariableValues const& values)
		{
			if (!literal.is_array() || literal.size() > MaxScriptFieldListElements)
			{
				return std::nullopt;
			}
			std::vector<ScriptFieldValue> elements;
			elements.reserve(literal.size());
			for (Json const& element : literal)
			{
				std::optional<ScriptFieldValue> value = DecodeValue(element, elementType, values);
				if (!value)
				{
					return std::nullopt;
				}
				elements.push_back(std::move(*value));
			}
			return ScriptFieldValue::FromList(values.ValueType, std::move(elements));
		}
	}

	VisualScriptVariableDescription DescribeVisualScriptVariables(VisualScriptDocument const& document,
	                                                              VisualScriptEnumLookup const& enumLookup)
	{
		VisualScriptVariableDescription description;
		description.Editable.Name = EditableClassName;
		for (VisualScriptVariable const& variable : document.Variables)
		{
			if (!variable.Public)
			{
				continue;
			}
			// An array of an editable element type is a list field of the element's values; there are no arrays of arrays.
			std::string_view const elementType = GetVisualScriptArrayElementType(variable.Type);
			bool const isList = !elementType.empty();
			std::string_view const valueType = isList ? elementType : std::string_view(variable.Type);
			std::variant<VariableValues, std::string_view> described = DescribeValues(valueType, enumLookup);
			if (std::string_view const* const reason = std::get_if<std::string_view>(&described))
			{
				description.ReadOnly.push_back({variable.Name, variable.Type, std::string(*reason)});
				continue;
			}
			VariableValues& values = std::get<VariableValues>(described);
			ScriptFieldInfo field;
			field.Name = variable.Name;
			field.Tooltip = variable.Tooltip;
			field.Range = variable.Range;
			field.Type = isList ? MakeScriptFieldListType(values.ValueType) : values.ValueType;
			field.AcceptedAssetType = values.AcceptedAssetType;
			std::optional<ScriptFieldValue> value =
				isList ? DecodeList(variable.Value, valueType, values) : DecodeValue(variable.Value, valueType, values);
			field.DefaultValue = value ? std::move(*value) : ScriptFieldValue::CreateDefault(field.Type);
			if (values.Enum)
			{
				field.Enumerators = std::move(values.Enum->Enumerators);
				field.IsFlags = values.Enum->IsFlags;
			}
			description.Editable.Fields.push_back(std::move(field));
		}
		return description;
	}

	ScriptFieldMap NormalizeVisualScriptOverrides(VisualScriptDocument const& document, ScriptFieldMap overrides)
	{
		std::set<std::string_view> publicNames;
		for (VisualScriptVariable const& variable : document.Variables)
		{
			if (variable.Public)
			{
				publicNames.insert(variable.Name);
			}
		}

		for (VisualScriptVariable const& variable : document.Variables)
		{
			if (!variable.Public)
			{
				continue;
			}
			bool named = overrides.contains(variable.Name);
			// The most recent former name comes last.
			for (auto formerName = variable.FormerNames.rbegin(); formerName != variable.FormerNames.rend(); ++formerName)
			{
				// The value under the name of a public variable is that variable's.
				if (publicNames.contains(*formerName))
				{
					continue;
				}
				auto const value = overrides.find(*formerName);
				if (value == overrides.end())
				{
					continue;
				}
				if (!named)
				{
					overrides.insert_or_assign(variable.Name, std::move(value->second));
					named = true;
				}
				overrides.erase(value);
			}
		}
		return overrides;
	}
}
