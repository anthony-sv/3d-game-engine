#pragma once

#include "Strada/Serialization/JsonSerialization.h"

#include <concepts>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Strada
{
	// A named data member of a struct described by a field table.
	template<typename TStruct, typename TValue>
	struct StructField
	{
		std::string_view Name;
		TValue TStruct::* Member;
	};

	template<typename TStruct, typename TValue>
	constexpr StructField<TStruct, TValue> Field(std::string_view name, TValue TStruct::* member)
	{
		return {name, member};
	}

	// A field table describes how a struct is serialized:
	//   static constexpr std::string_view Name;   name used in messages and schemas
	//   static constexpr auto Fields;             tuple of StructField, in serialization order
	// Optionally:
	//   static Result<bool> ReadExtraField(std::string_view key, Json const& value, T& object,
	//                                      DeserializationContext const& context);   alternative input keys
	template<typename TTraits>
	concept FieldTable = requires {
		{ TTraits::Name } -> std::convertible_to<std::string_view>;
		TTraits::Fields;
	};

	// Specialize StructTraits<T> (a field table) right after defining T to make it serializable as a JSON object, including
	// as a field of other structs: JsonTraits<T> is provided automatically.
	template<typename T>
	struct StructTraits;

	template<typename T>
	concept ReflectedStruct = std::is_class_v<T> && FieldTable<StructTraits<T>>;

	namespace Detail
	{
		template<typename TStruct, typename TField>
		using StructFieldType = std::remove_cvref_t<decltype(std::declval<TStruct&>().*(std::declval<TField>().Member))>;

		template<FieldTable TTraits>
		std::string ListFieldNames()
		{
			std::string names;
			std::apply(
				[&names](auto const&... fields)
				{
					((names += (names.empty() ? "" : ", "), names += fields.Name), ...);
				},
				TTraits::Fields);
			return names;
		}
	}

	// Writes every field.
	template<FieldTable TTraits, typename T>
	Json SerializeFields(T const& object)
	{
		Json json = Json::object();
		std::apply(
			[&json, &object](auto const&... fields)
			{
				((json[std::string(fields.Name)] =
			          JsonTraits<Detail::StructFieldType<T, decltype(fields)>>::ToJson(object.*(fields.Member))),
			     ...);
			},
			TTraits::Fields);
		return json;
	}

	// Applies the fields present in the JSON object; missing fields keep their current values, so this also serves as a
	// partial update. The object is only modified when every field is valid. `kind` names the struct category in messages
	// ("component 'Transform' has no field ...").
	template<FieldTable TTraits, typename T>
	[[nodiscard]] Result<void> DeserializeFields(Json const& json, T& object, DeserializationContext const& context, std::string_view kind)
	{
		constexpr std::string_view Name = TTraits::Name;
		if (!json.is_object())
		{
			return MakeError("{} '{}' must be a JSON object", kind, Name);
		}

		T updated = object;
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
								using FieldType = Detail::StructFieldType<T, decltype(fields)>;
								fieldResult = JsonTraits<FieldType>::FromJson(value, updated.*(fields.Member), context);
							}
						}(),
						...);
				},
				TTraits::Fields);

			if (matched)
			{
				if (!fieldResult)
				{
					return MakeError("{}.{}: {}", Name, key, fieldResult.GetError());
				}
				continue;
			}

			if constexpr (requires { TTraits::ReadExtraField(key, value, updated, context); })
			{
				Result<bool> extra = TTraits::ReadExtraField(key, value, updated, context);
				if (!extra)
				{
					return MakeError("{}.{}: {}", Name, key, extra.GetError());
				}
				if (extra.GetValue())
				{
					continue;
				}
			}

			switch (context.UnknownFields)
			{
				case UnknownFieldPolicy::Error:
					return MakeError("{} '{}' has no field '{}' (fields: {})", kind, Name, key, Detail::ListFieldNames<TTraits>());
				case UnknownFieldPolicy::Warn:
					context.Warn(fmt::format("ignored unknown field '{}' in {} '{}'", key, kind, Name));
					break;
				case UnknownFieldPolicy::Ignore:
					break;
			}
		}

		object = std::move(updated);
		return {};
	}

	// Schema used by the automation API: { "Name": ..., "Fields": [ { "Name", "Type", "Default" }, ... ] }, with defaults
	// taken from a value-initialized T.
	template<FieldTable TTraits, typename T>
	Json DescribeFields()
	{
		T const defaults{};
		Json fields = Json::array();
		std::apply(
			[&fields, &defaults](auto const&... field)
			{
				(
					[&]
					{
						using FieldType = Detail::StructFieldType<T, decltype(field)>;
						Json description = Json::object();
						description["Name"] = std::string(field.Name);
						description["Type"] = JsonTraits<FieldType>::TypeName();
						description["Default"] = JsonTraits<FieldType>::ToJson(defaults.*(field.Member));
						fields.push_back(std::move(description));
					}(),
					...);
			},
			TTraits::Fields);

		Json result = Json::object();
		result["Name"] = std::string(TTraits::Name);
		result["Fields"] = std::move(fields);
		return result;
	}

	template<ReflectedStruct T>
	struct JsonTraits<T>
	{
		static Json ToJson(T const& value) { return SerializeFields<StructTraits<T>>(value); }
		static Result<void> FromJson(Json const& json, T& out, DeserializationContext const& context)
		{
			return DeserializeFields<StructTraits<T>>(json, out, context, "object");
		}
		static std::string TypeName() { return std::string(StructTraits<T>::Name); }
	};
}
