#pragma once

#include "Strada/Serialization/JsonSerialization.h"

#include <array>
#include <concepts>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Strada
{
	// How editors present a field. Presentation only: the serialized format does not change.
	enum class FieldDisplay : uint8_t
	{
		Default = 0,
		// vec3/vec4: a linear RGB(A) color.
		Color,
		// float/double: an angle in degrees.
		Angle,
		// string: multi-line text.
		MultilineText
	};

	template<>
	struct EnumTraits<FieldDisplay>
	{
		static constexpr std::array<std::pair<FieldDisplay, std::string_view>, 4> Values = {{
			{FieldDisplay::Default, "Default"},
			{FieldDisplay::Color, "Color"},
			{FieldDisplay::Angle, "Angle"},
			{FieldDisplay::MultilineText, "MultilineText"},
		}};
	};

	// Optional metadata of a struct field. Ranges are enforced whenever JSON is read (files, automation, editor edits); the
	// other hints drive editors and the automation schema.
	struct FieldHints
	{
		// Inclusive bounds of numeric values (of every component of vectors and every element of arrays).
		double Min = -std::numeric_limits<double>::infinity();
		double Max = std::numeric_limits<double>::infinity();
		FieldDisplay Display = FieldDisplay::Default;
		// Asset fields: the AssetType name of the assets they reference ("Mesh", "Material", ...); empty for any asset.
		std::string_view AssetTypeName;
		// One sentence for users and agents (tooltips, the automation schema).
		std::string_view Description;

		constexpr bool HasMin() const { return Min != -std::numeric_limits<double>::infinity(); }
		constexpr bool HasMax() const { return Max != std::numeric_limits<double>::infinity(); }
		constexpr bool HasRange() const { return HasMin() || HasMax(); }
	};

	// A named data member of a struct described by a field table, with optional hints added fluently:
	//   Field("Range", &PointLightComponent::Range).AtLeast(0.0).Doc("Distance at which the light fades out.")
	template<typename TStruct, typename TValue>
	struct StructField
	{
		std::string_view Name;
		TValue TStruct::* Member;
		FieldHints Hints = {};

		constexpr StructField Range(double min, double max) const
		{
			StructField field = *this;
			field.Hints.Min = min;
			field.Hints.Max = max;
			return field;
		}

		constexpr StructField AtLeast(double min) const
		{
			StructField field = *this;
			field.Hints.Min = min;
			return field;
		}

		constexpr StructField AsColor() const { return WithDisplay(FieldDisplay::Color); }
		constexpr StructField AsAngle() const { return WithDisplay(FieldDisplay::Angle); }
		constexpr StructField AsMultilineText() const { return WithDisplay(FieldDisplay::MultilineText); }

		// The type of asset an asset field (or array of asset handles) references.
		constexpr StructField References(std::string_view assetTypeName) const
		{
			StructField field = *this;
			field.Hints.AssetTypeName = assetTypeName;
			return field;
		}

		constexpr StructField Doc(std::string_view description) const
		{
			StructField field = *this;
			field.Hints.Description = description;
			return field;
		}

	private:
		constexpr StructField WithDisplay(FieldDisplay display) const
		{
			StructField field = *this;
			field.Hints.Display = display;
			return field;
		}
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

	// Category of a reflected field's C++ type, for generic editors.
	enum class FieldKind : uint8_t
	{
		Bool = 0,
		Int,
		UInt,
		Float,
		Double,
		String,
		UUID,
		Asset,
		Vec2,
		Vec3,
		Vec4,
		Quat,
		BVec3,
		Enum,
		// A nested reflected struct.
		Struct,
		Array,
		// A type with its own JSON format (e.g. script fields); edited by dedicated code only.
		Custom
	};

	// Type-erased description of a reflected field (generic editors, schemas). Built from field tables.
	struct FieldDescriptor
	{
		std::string_view Name;
		FieldKind Kind = FieldKind::Custom;
		// Arrays: the kind of their elements.
		FieldKind ElementKind = FieldKind::Custom;
		FieldHints Hints;
		// Schema type name ("float", "vec3", "Perspective|Orthographic", "asset[]", ...).
		std::string TypeName;
		// Enums (and arrays of enums): the value names in declaration order.
		std::vector<std::string_view> EnumValues;
		// Structs (and arrays of structs): the nested fields.
		std::vector<FieldDescriptor> Fields;
		// The value of a default-constructed owner.
		Json Default;
	};

	// Schema used by the automation API: { "Name", ["Description"], "Fields": [ { "Name", "Type", "Default", ... }, ... ] }.
	// Optional keys per field: "Values" (enums), "Min"/"Max", "Display", "AssetType", "Description" and "Fields" (nested
	// structs).
	Json DescribeFields(std::string_view name, std::span<FieldDescriptor const> fields, std::string_view description = {});

	namespace Detail
	{
		template<typename TStruct, typename TField>
		using StructFieldType = std::remove_cvref_t<decltype(std::declval<TStruct&>().*(std::declval<TField>().Member))>;

		// Element is the element type of std::vector and the type itself otherwise.
		template<typename T>
		struct VectorTraits : std::false_type
		{
			using Element = T;
		};

		template<typename T>
		struct VectorTraits<std::vector<T>> : std::true_type
		{
			using Element = T;
		};

		template<typename T>
		constexpr bool IsFloatVector = std::is_same_v<T, glm::vec2> || std::is_same_v<T, glm::vec3> || std::is_same_v<T, glm::vec4>;

		template<typename T>
		constexpr FieldKind GetFieldKind()
		{
			if constexpr (std::is_same_v<T, bool>)
			{
				return FieldKind::Bool;
			}
			else if constexpr (std::is_integral_v<T>)
			{
				return std::is_signed_v<T> ? FieldKind::Int : FieldKind::UInt;
			}
			else if constexpr (std::is_same_v<T, float>)
			{
				return FieldKind::Float;
			}
			else if constexpr (std::is_same_v<T, double>)
			{
				return FieldKind::Double;
			}
			else if constexpr (std::is_same_v<T, std::string>)
			{
				return FieldKind::String;
			}
			else if constexpr (std::is_same_v<T, UUID>)
			{
				return FieldKind::UUID;
			}
			else if constexpr (std::is_same_v<T, AssetHandle>)
			{
				return FieldKind::Asset;
			}
			else if constexpr (std::is_same_v<T, glm::vec2>)
			{
				return FieldKind::Vec2;
			}
			else if constexpr (std::is_same_v<T, glm::vec3>)
			{
				return FieldKind::Vec3;
			}
			else if constexpr (std::is_same_v<T, glm::vec4>)
			{
				return FieldKind::Vec4;
			}
			else if constexpr (std::is_same_v<T, glm::quat>)
			{
				return FieldKind::Quat;
			}
			else if constexpr (std::is_same_v<T, glm::bvec3>)
			{
				return FieldKind::BVec3;
			}
			else if constexpr (SerializableEnum<T>)
			{
				return FieldKind::Enum;
			}
			else if constexpr (ReflectedStruct<T>)
			{
				return FieldKind::Struct;
			}
			else if constexpr (VectorTraits<T>::value)
			{
				return FieldKind::Array;
			}
			else
			{
				return FieldKind::Custom;
			}
		}

		// Numbers, float vectors and arrays of them can have a range.
		template<typename T>
		constexpr bool SupportsRange()
		{
			if constexpr (VectorTraits<T>::value)
			{
				return SupportsRange<typename VectorTraits<T>::Element>();
			}
			else
			{
				return (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) || IsFloatVector<T>;
			}
		}

		template<typename T>
		constexpr bool IsAssetField()
		{
			if constexpr (VectorTraits<T>::value)
			{
				return std::is_same_v<typename VectorTraits<T>::Element, AssetHandle>;
			}
			else
			{
				return std::is_same_v<T, AssetHandle>;
			}
		}

		// Whether the hints of a field fit its type (checked at compile time for every field table).
		template<typename T>
		constexpr bool AreHintsValid(FieldHints const& hints)
		{
			if (hints.HasRange() && (!SupportsRange<T>() || !(hints.Min <= hints.Max)))
			{
				return false;
			}
			switch (hints.Display)
			{
				case FieldDisplay::Default:
					break;
				case FieldDisplay::Color:
					if (!std::is_same_v<T, glm::vec3> && !std::is_same_v<T, glm::vec4>)
					{
						return false;
					}
					break;
				case FieldDisplay::Angle:
					if (!std::is_floating_point_v<T>)
					{
						return false;
					}
					break;
				case FieldDisplay::MultilineText:
					if (!std::is_same_v<T, std::string>)
					{
						return false;
					}
					break;
			}
			return hints.AssetTypeName.empty() || IsAssetField<T>();
		}

		template<FieldTable TTraits, typename T>
		constexpr bool IsFieldTableValid()
		{
			bool valid = true;
			std::apply(
				[&valid](auto const&... fields)
				{
					((valid = valid && AreHintsValid<StructFieldType<T, decltype(fields)>>(fields.Hints)), ...);
				},
				TTraits::Fields);

			// Field names must be unique.
			std::apply(
				[&valid](auto const&... fields)
				{
					std::array<std::string_view, sizeof...(fields)> const names = {fields.Name...};
					for (size_t i = 0; i < names.size(); i++)
					{
						for (size_t j = i + 1; j < names.size(); j++)
						{
							valid = valid && names[i] != names[j];
						}
					}
				},
				TTraits::Fields);
			return valid;
		}

		template<typename T>
		bool IsInRange(T const& value, double min, double max)
		{
			if constexpr (VectorTraits<T>::value)
			{
				for (auto const& element : value)
				{
					if (!IsInRange(element, min, max))
					{
						return false;
					}
				}
				return true;
			}
			else if constexpr (IsFloatVector<T>)
			{
				for (glm::length_t i = 0; i < T::length(); i++)
				{
					if (!IsInRange(value[i], min, max))
					{
						return false;
					}
				}
				return true;
			}
			else if constexpr (std::is_same_v<T, float>)
			{
				// Bounds are rounded to float first, so a bound like 89.9 accepts the float closest to it.
				return value >= static_cast<float>(min) && value <= static_cast<float>(max);
			}
			else
			{
				double const number = static_cast<double>(value);
				return number >= min && number <= max;
			}
		}

		std::string DescribeRangeViolation(FieldHints const& hints, bool isVector, bool isArray);

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

		template<typename T>
		Result<void> CheckFieldRange(T const& value, FieldHints const& hints)
		{
			if constexpr (SupportsRange<T>())
			{
				if (hints.HasRange() && !IsInRange(value, hints.Min, hints.Max))
				{
					constexpr bool IsArray = VectorTraits<T>::value;
					if constexpr (IsArray)
					{
						return Error{DescribeRangeViolation(hints, IsFloatVector<typename VectorTraits<T>::Element>, true)};
					}
					else
					{
						return Error{DescribeRangeViolation(hints, IsFloatVector<T>, false)};
					}
				}
			}
			return {};
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
	// partial update. Values outside a field's range are rejected. The object is only modified when every field is valid.
	// `kind` names the struct category in messages ("component 'Transform' has no field ...").
	template<FieldTable TTraits, typename T>
	[[nodiscard]] Result<void> DeserializeFields(Json const& json, T& object, DeserializationContext const& context, std::string_view kind)
	{
		static_assert(Detail::IsFieldTableValid<TTraits, T>(), "A field hint does not fit its field's type, or names repeat");
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
								if (fieldResult)
								{
									fieldResult = Detail::CheckFieldRange(updated.*(fields.Member), fields.Hints);
								}
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

	template<FieldTable TTraits, typename T>
	std::vector<FieldDescriptor> GetFieldDescriptors();

	namespace Detail
	{
		// The kind, type name, enum values and nested fields of a type (the parts of a descriptor that only depend on T).
		template<typename T>
		void DescribeFieldType(FieldDescriptor& descriptor)
		{
			descriptor.Kind = GetFieldKind<T>();
			descriptor.TypeName = JsonTraits<T>::TypeName();
			using ValueType = typename VectorTraits<T>::Element;
			if constexpr (VectorTraits<T>::value)
			{
				descriptor.ElementKind = GetFieldKind<ValueType>();
			}
			if constexpr (SerializableEnum<ValueType>)
			{
				for (auto const& [enumerator, name] : EnumTraits<ValueType>::Values)
				{
					descriptor.EnumValues.push_back(name);
				}
			}
			if constexpr (ReflectedStruct<ValueType>)
			{
				descriptor.Fields = GetFieldDescriptors<StructTraits<ValueType>, ValueType>();
			}
		}
	}

	// Descriptors of a field table's fields, in serialization order, with defaults taken from a value-initialized T.
	template<FieldTable TTraits, typename T>
	std::vector<FieldDescriptor> GetFieldDescriptors()
	{
		static_assert(Detail::IsFieldTableValid<TTraits, T>(), "A field hint does not fit its field's type, or names repeat");
		T const defaults{};
		std::vector<FieldDescriptor> descriptors;
		std::apply(
			[&descriptors, &defaults](auto const&... fields)
			{
				(
					[&]
					{
						using FieldType = Detail::StructFieldType<T, decltype(fields)>;
						FieldDescriptor descriptor;
						descriptor.Name = fields.Name;
						descriptor.Hints = fields.Hints;
						Detail::DescribeFieldType<FieldType>(descriptor);
						descriptor.Default = JsonTraits<FieldType>::ToJson(defaults.*(fields.Member));
						descriptors.push_back(std::move(descriptor));
					}(),
					...);
			},
			TTraits::Fields);
		return descriptors;
	}

	// The automation schema of a field table (see DescribeFields above).
	template<FieldTable TTraits, typename T>
	Json DescribeFields()
	{
		return DescribeFields(TTraits::Name, GetFieldDescriptors<TTraits, T>());
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
