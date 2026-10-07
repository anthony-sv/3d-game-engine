#pragma once

#include "Strada/Scene/ComponentTraits.h"

#include <string>
#include <tuple>
#include <type_traits>

namespace Strada
{
	namespace Detail
	{
		template<typename TComponent, typename TField>
		using FieldValueType = std::remove_cvref_t<decltype(std::declval<TComponent&>().*(std::declval<TField>().Member))>;

		template<RegisteredComponent T>
		std::string ListFieldNames()
		{
			std::string names;
			std::apply(
				[&names](auto const&... fields)
				{
					((names += (names.empty() ? "" : ", "), names += fields.Name), ...);
				},
				ComponentTraits<T>::Fields);
			return names;
		}
	}

	// Writes every field of the component.
	template<RegisteredComponent T>
	Json SerializeComponent(T const& component)
	{
		Json json = Json::object();
		std::apply(
			[&json, &component](auto const&... fields)
			{
				((json[std::string(fields.Name)] =
			          JsonTraits<Detail::FieldValueType<T, decltype(fields)>>::ToJson(component.*(fields.Member))),
			     ...);
			},
			ComponentTraits<T>::Fields);
		return json;
	}

	// Applies the fields present in the JSON object (missing fields keep their current values, so this also serves as a
	// partial update). The component is only modified when every field is valid.
	template<RegisteredComponent T>
	[[nodiscard]] Result<void> DeserializeComponent(Json const& json, T& component, DeserializationContext const& context)
	{
		constexpr std::string_view ComponentName = ComponentTraits<T>::Name;
		if (!json.is_object())
		{
			return MakeError("component '{}' must be a JSON object", ComponentName);
		}

		T updated = component;
		for (auto const& item : json.items())
		{
			std::string const& key = item.key();
			Json const& value = item.value();

			bool matched = false;
			Result<void> fieldResult;
			std::apply(
				[&](auto const&... fields)
				{
					(
						[&]
						{
							if (!matched && key == fields.Name)
							{
								matched = true;
								using FieldType = Detail::FieldValueType<T, decltype(fields)>;
								fieldResult = JsonTraits<FieldType>::FromJson(value, updated.*(fields.Member), context);
							}
						}(),
						...);
				},
				ComponentTraits<T>::Fields);

			if (matched)
			{
				if (!fieldResult)
				{
					return MakeError("{}.{}: {}", ComponentName, key, fieldResult.GetError());
				}
				continue;
			}

			if constexpr (requires { ComponentTraits<T>::ReadExtraField(key, value, updated, context); })
			{
				Result<bool> extra = ComponentTraits<T>::ReadExtraField(key, value, updated, context);
				if (!extra)
				{
					return MakeError("{}.{}: {}", ComponentName, key, extra.GetError());
				}
				if (extra.GetValue())
				{
					continue;
				}
			}

			switch (context.UnknownFields)
			{
				case UnknownFieldPolicy::Error:
					return MakeError("component '{}' has no field '{}' (fields: {})", ComponentName, key, Detail::ListFieldNames<T>());
				case UnknownFieldPolicy::Warn:
					context.Warn(fmt::format("ignored unknown field '{}' in component '{}'", key, ComponentName));
					break;
				case UnknownFieldPolicy::Ignore:
					break;
			}
		}

		component = std::move(updated);
		return {};
	}

	// Schema used by the automation API: { "Name": ..., "Fields": [ { "Name", "Type", "Default" }, ... ] }.
	template<RegisteredComponent T>
	Json DescribeComponent()
	{
		T const defaults{};
		Json fields = Json::array();
		std::apply(
			[&fields, &defaults](auto const&... field)
			{
				(
					[&]
					{
						using FieldType = Detail::FieldValueType<T, decltype(field)>;
						Json description = Json::object();
						description["Name"] = std::string(field.Name);
						description["Type"] = JsonTraits<FieldType>::TypeName();
						description["Default"] = JsonTraits<FieldType>::ToJson(defaults.*(field.Member));
						fields.push_back(std::move(description));
					}(),
					...);
			},
			ComponentTraits<T>::Fields);

		Json result = Json::object();
		result["Name"] = std::string(ComponentTraits<T>::Name);
		result["Fields"] = std::move(fields);
		return result;
	}
}
