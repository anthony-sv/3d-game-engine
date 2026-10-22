#include "stpch.h"
#include "Strada/Script/VisualScriptVariables.h"

#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr std::string_view EditableClassName = "Visual Script";
		constexpr std::string_view NeedsRuntimeReason = "needs the scripting runtime";
		constexpr std::string_view NotExposableReason = "not exposable";
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
			ScriptFieldInfo field;
			field.Name = variable.Name;
			field.Tooltip = variable.Tooltip;
			field.Range = variable.Range;
			if (std::optional<VisualScriptExposableType> const exposable = FindVisualScriptExposableType(variable.Type))
			{
				field.Type = exposable->FieldType;
				field.AcceptedAssetType = exposable->AcceptedAssetType;
				field.DefaultValue = DecodeVisualScriptLiteral(variable.Value, variable.Type, enumLookup)
				                         .value_or(ScriptFieldValue::CreateDefault(field.Type));
				description.Editable.Fields.push_back(std::move(field));
				continue;
			}

			bool const mayBeEnum = CanBeVisualScriptEnum(variable.Type);
			std::optional<VisualScriptEnumInfo> const enumInfo = mayBeEnum && enumLookup ? enumLookup(variable.Type) : std::nullopt;
			if (!enumInfo || !enumInfo->IsUsable())
			{
				std::string_view const reason = mayBeEnum && !enumLookup ? NeedsRuntimeReason : NotExposableReason;
				description.ReadOnly.push_back({variable.Name, variable.Type, std::string(reason)});
				continue;
			}
			field.Type = enumInfo->UnderlyingType;
			field.Enumerators = enumInfo->Enumerators;
			field.IsFlags = enumInfo->IsFlags;
			// The enum found above, not looked up again.
			field.DefaultValue =
				DecodeVisualScriptEnumLiteral(variable.Value, *enumInfo).value_or(ScriptFieldValue::CreateDefault(field.Type));
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
