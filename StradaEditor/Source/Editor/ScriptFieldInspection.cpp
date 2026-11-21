#include "Editor/ScriptFieldInspection.h"

#include "Strada/Asset/Asset.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Script/ScriptFieldSerialization.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Strada::ScriptFieldInspection
{
	namespace
	{
		// Whether two stored values are the same as the runtime's read-back compares the running values (ReadBackCopy in
		// Strada.ScriptCore): of one type, floating-point numbers and the math values made of them bit for bit (so a NaN that
		// stays is the same and a change between 0 and -0 is a change), lists element by element, every other value by
		// value.
		bool IsSameStoredValue(ScriptFieldValue const& first, ScriptFieldValue const& second)
		{
			if (first.GetType() != second.GetType() || first.GetStorage().index() != second.GetStorage().index())
			{
				return false;
			}
			return std::visit(
				[&second](auto const& value)
				{
					using TValue = std::decay_t<decltype(value)>;
					TValue const& other = std::get<TValue>(second.GetStorage());
					if constexpr (std::is_same_v<TValue, std::vector<ScriptFieldValue>>)
					{
						return std::equal(value.begin(), value.end(), other.begin(), other.end(), IsSameStoredValue);
					}
					else if constexpr (std::is_same_v<TValue, float> || std::is_same_v<TValue, double> ||
				                       std::is_same_v<TValue, glm::vec2> || std::is_same_v<TValue, glm::vec3> ||
				                       std::is_same_v<TValue, glm::vec4> || std::is_same_v<TValue, glm::quat>)
					{
						// Numbers alone: glm's vectors and quaternions hold their components without padding.
						static_assert(sizeof(TValue) % sizeof(float) == 0 && std::is_trivially_copyable_v<TValue>);
						return std::memcmp(&value, &other, sizeof(TValue)) == 0;
					}
					else
					{
						return value == other;
					}
				},
				first.GetStorage());
		}

		// An integer value as bits (two's complement for signed types), so enumerators of every size combine alike.
		uint64_t ToBits(ScriptFieldValue const& value)
		{
			switch (value.GetType())
			{
				case ScriptFieldType::Int32:
					return static_cast<uint64_t>(static_cast<int64_t>(value.GetInt32()));
				case ScriptFieldType::UInt32:
					return value.GetUInt32();
				case ScriptFieldType::Int64:
					return static_cast<uint64_t>(value.GetInt64());
				case ScriptFieldType::UInt64:
					return value.GetUInt64();
				default:
					return 0;
			}
		}

		ScriptFieldValue FromBits(ScriptFieldType type, uint64_t bits)
		{
			switch (type)
			{
				case ScriptFieldType::Int32:
					return ScriptFieldValue::FromInt32(static_cast<int32_t>(static_cast<uint32_t>(bits)));
				case ScriptFieldType::UInt32:
					return ScriptFieldValue::FromUInt32(static_cast<uint32_t>(bits));
				case ScriptFieldType::Int64:
					return ScriptFieldValue::FromInt64(static_cast<int64_t>(bits));
				default:
					return ScriptFieldValue::FromUInt64(bits);
			}
		}

		std::string FormatNumber(ScriptFieldValue const& value)
		{
			switch (value.GetType())
			{
				case ScriptFieldType::Int32:
					return std::to_string(value.GetInt32());
				case ScriptFieldType::UInt32:
					return std::to_string(value.GetUInt32());
				case ScriptFieldType::Int64:
					return std::to_string(value.GetInt64());
				default:
					return std::to_string(value.GetUInt64());
			}
		}

		// The number an enum value without an enumerator is shown as, read back.
		std::optional<ScriptFieldValue> ParseNumber(ScriptFieldType type, std::string_view text)
		{
			char const* const end = text.data() + text.size();
			if (type == ScriptFieldType::UInt32 || type == ScriptFieldType::UInt64)
			{
				uint64_t number = 0;
				auto const [last, error] = std::from_chars(text.data(), end, number);
				if (error != std::errc() || last != end ||
				    (type == ScriptFieldType::UInt32 && number > std::numeric_limits<uint32_t>::max()))
				{
					return std::nullopt;
				}
				return FromBits(type, number);
			}
			int64_t number = 0;
			auto const [last, error] = std::from_chars(text.data(), end, number);
			if (error != std::errc() || last != end ||
			    (type == ScriptFieldType::Int32 &&
			     (number < std::numeric_limits<int32_t>::min() || number > std::numeric_limits<int32_t>::max())))
			{
				return std::nullopt;
			}
			return FromBits(type, static_cast<uint64_t>(number));
		}

		// A value as the field editor edits it; a list as the array of its elements' (an enum's element by its name).
		Json ToEditorValue(ScriptFieldInfo const& field, ScriptFieldValue const& value)
		{
			if (value.IsList())
			{
				Json elements = Json::array();
				for (ScriptFieldValue const& element : value.GetElements())
				{
					elements.push_back(ToEditorValue(field, element));
				}
				return elements;
			}
			if (!field.Enumerators.empty())
			{
				if (field.IsFlags)
				{
					uint64_t const bits = ToBits(value);
					Json names = Json::array();
					for (ScriptEnumerator const& enumerator : field.Enumerators)
					{
						uint64_t const flag = ToBits(enumerator.Value);
						if (flag != 0 && (bits & flag) == flag)
						{
							names.push_back(enumerator.Name);
						}
					}
					return names;
				}
				for (ScriptEnumerator const& enumerator : field.Enumerators)
				{
					if (enumerator.Value == value)
					{
						return enumerator.Name;
					}
				}
				// A value without an enumerator shows as its number.
				return FormatNumber(value);
			}
			switch (value.GetType())
			{
				// Scene files store 64-bit integers as strings; the editor edits numbers.
				case ScriptFieldType::Int64:
					return value.GetInt64();
				case ScriptFieldType::UInt64:
					return value.GetUInt64();
				default:
					return ScriptFieldValueToJson(value);
			}
		}

		// An edited value of the type (the field's own, or its element type for an element of a list), named in errors.
		Result<ScriptFieldValue> FromEditorValue(ScriptFieldInfo const& field, ScriptFieldType type, std::string_view name,
		                                         Json const& value)
		{
			if (!field.Enumerators.empty())
			{
				if (field.IsFlags)
				{
					if (!value.is_array())
					{
						return MakeError("{}: expected the names of the set flags", name);
					}
					uint64_t bits = 0;
					for (Json const& flag : value)
					{
						auto const enumerator =
							std::find_if(field.Enumerators.begin(), field.Enumerators.end(),
						                 [&flag](ScriptEnumerator const& candidate)
						                 {
											 return flag.is_string() && flag.get_ref<std::string const&>() == candidate.Name;
										 });
						if (enumerator == field.Enumerators.end())
						{
							return MakeError("{}: {} is not one of its flags", name,
							                 flag.dump(-1, ' ', false, Json::error_handler_t::replace));
						}
						bits |= ToBits(enumerator->Value);
					}
					return FromBits(type, bits);
				}
				if (value.is_string())
				{
					std::string const& enumeratorName = value.get_ref<std::string const&>();
					for (ScriptEnumerator const& enumerator : field.Enumerators)
					{
						if (enumerator.Name == enumeratorName)
						{
							return enumerator.Value;
						}
					}
					if (std::optional<ScriptFieldValue> number = ParseNumber(type, enumeratorName))
					{
						return *number;
					}
				}
				return MakeError("{}: {} is not one of its values", name, value.dump(-1, ' ', false, Json::error_handler_t::replace));
			}
			switch (type)
			{
				case ScriptFieldType::Int64:
					if (value.is_number_integer() &&
					    !(value.is_number_unsigned() && value.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())))
					{
						return ScriptFieldValue::FromInt64(value.get<int64_t>());
					}
					return MakeError("{}: expected a 64-bit integer, not {}", name,
					                 value.dump(-1, ' ', false, Json::error_handler_t::replace));
				case ScriptFieldType::UInt64:
					if (value.is_number_unsigned() || (value.is_number_integer() && value.get<int64_t>() >= 0))
					{
						return ScriptFieldValue::FromUInt64(value.get<uint64_t>());
					}
					return MakeError("{}: expected an unsigned 64-bit integer, not {}", name,
					                 value.dump(-1, ' ', false, Json::error_handler_t::replace));
				default:
				{
					Result<ScriptFieldValue> parsed = ScriptFieldValueFromJson(type, value, DeserializationContext{});
					if (!parsed)
					{
						return MakeError("{}: {}", name, parsed.GetError());
					}
					return parsed;
				}
			}
		}

		// An edited value of the field: a list from the array of its elements' edited values, at most
		// MaxScriptFieldListElements of them.
		Result<ScriptFieldValue> FromEditorValue(ScriptFieldInfo const& field, Json const& value)
		{
			if (!IsScriptFieldListType(field.Type))
			{
				return FromEditorValue(field, field.Type, field.Name, value);
			}
			if (!value.is_array())
			{
				return MakeError("{}: expected an array of its elements", field.Name);
			}
			if (value.size() > MaxScriptFieldListElements)
			{
				return MakeError("{}: holds at most {} elements, not {}", field.Name, MaxScriptFieldListElements, value.size());
			}
			ScriptFieldType const elementType = GetScriptFieldElementType(field.Type);
			std::vector<ScriptFieldValue> elements;
			elements.reserve(value.size());
			for (size_t index = 0; index < value.size(); index++)
			{
				Result<ScriptFieldValue> element =
					FromEditorValue(field, elementType, fmt::format("{} element {}", field.Name, index), value[index]);
				if (!element)
				{
					return element;
				}
				elements.push_back(element.TakeValue());
			}
			return ScriptFieldValue::FromList(elementType, std::move(elements));
		}

		// The kind and hints of values of an element type (the field's, or a list's elements').
		void SetKind(ScriptFieldType type, AssetType acceptedAssetType, FieldDescriptor& descriptor)
		{
			switch (type)
			{
				case ScriptFieldType::Bool:
					descriptor.Kind = FieldKind::Bool;
					break;
				case ScriptFieldType::Int32:
					descriptor.Kind = FieldKind::Int;
					descriptor.TypeMin = std::numeric_limits<int32_t>::min();
					descriptor.TypeMax = std::numeric_limits<int32_t>::max();
					break;
				case ScriptFieldType::UInt32:
					descriptor.Kind = FieldKind::UInt;
					descriptor.TypeMin = 0.0;
					descriptor.TypeMax = std::numeric_limits<uint32_t>::max();
					break;
				case ScriptFieldType::Int64:
					descriptor.Kind = FieldKind::Int;
					descriptor.TypeMin = static_cast<double>(std::numeric_limits<int64_t>::min());
					descriptor.TypeMax = static_cast<double>(std::numeric_limits<int64_t>::max());
					break;
				case ScriptFieldType::UInt64:
					descriptor.Kind = FieldKind::UInt;
					descriptor.TypeMin = 0.0;
					descriptor.TypeMax = static_cast<double>(std::numeric_limits<uint64_t>::max());
					break;
				case ScriptFieldType::Float:
					descriptor.Kind = FieldKind::Float;
					descriptor.TypeMin = -std::numeric_limits<float>::max();
					descriptor.TypeMax = std::numeric_limits<float>::max();
					break;
				case ScriptFieldType::Double:
					descriptor.Kind = FieldKind::Double;
					break;
				case ScriptFieldType::String:
					descriptor.Kind = FieldKind::String;
					break;
				case ScriptFieldType::Vector2:
					descriptor.Kind = FieldKind::Vec2;
					break;
				case ScriptFieldType::Vector3:
					descriptor.Kind = FieldKind::Vec3;
					break;
				case ScriptFieldType::Vector4:
					descriptor.Kind = FieldKind::Vec4;
					break;
				case ScriptFieldType::Quaternion:
					descriptor.Kind = FieldKind::Quat;
					break;
				case ScriptFieldType::Color:
					descriptor.Kind = FieldKind::Vec4;
					descriptor.Hints.Display = FieldDisplay::Color;
					break;
				case ScriptFieldType::Entity:
					descriptor.Kind = FieldKind::UUID;
					break;
				case ScriptFieldType::Prefab:
					descriptor.Kind = FieldKind::Asset;
					descriptor.Hints.AssetTypeName = AssetTypeToString(AssetType::Prefab);
					break;
				case ScriptFieldType::Asset:
					descriptor.Kind = FieldKind::Asset;
					if (acceptedAssetType != AssetType::None)
					{
						descriptor.Hints.AssetTypeName = AssetTypeToString(acceptedAssetType);
					}
					break;
				case ScriptFieldType::None:
				default:
					descriptor.Kind = FieldKind::Custom;
					break;
			}
		}

		ScriptFieldInfo const* FindField(ScriptClassInfo const& scriptClass, std::string_view name)
		{
			ScriptFieldInfo const* field = scriptClass.FindField(name);
			return field != nullptr && !field->Hidden ? field : nullptr;
		}

		// A field's value as the editor edits it, from its stored value in the file form (null for none, or one of another
		// type): the stored value when it reads as a value of the field's type, the class's default otherwise.
		Json ToEditorValue(ScriptFieldInfo const& field, Json const* storedValue)
		{
			if (storedValue != nullptr)
			{
				if (Result<ScriptFieldValue> parsed = ScriptFieldValueFromJson(field.Type, *storedValue, DeserializationContext{}))
				{
					return ToEditorValue(field, parsed.GetValue());
				}
			}
			return ToEditorValue(field, field.DefaultValue);
		}
	}

	std::vector<FieldDescriptor> DescribeFields(ScriptClassInfo const& scriptClass)
	{
		std::vector<FieldDescriptor> descriptors;
		descriptors.reserve(scriptClass.Fields.size());
		for (ScriptFieldInfo const& field : scriptClass.Fields)
		{
			if (field.Hidden)
			{
				continue;
			}
			FieldDescriptor descriptor;
			descriptor.Name = field.Name;
			descriptor.TypeName = ScriptFieldTypeToString(field.Type);
			descriptor.Hints.Description = field.Tooltip;
			if (field.Range)
			{
				descriptor.Hints.Min = field.Range->x;
				descriptor.Hints.Max = field.Range->y;
			}
			descriptor.Default = ToEditorValue(field, field.DefaultValue);
			if (field.Enumerators.empty())
			{
				SetKind(GetScriptFieldElementType(field.Type), field.AcceptedAssetType, descriptor);
			}
			else
			{
				descriptor.Kind = FieldKind::Enum;
				descriptor.IsFlags = field.IsFlags;
				for (ScriptEnumerator const& enumerator : field.Enumerators)
				{
					if (!field.IsFlags || ToBits(enumerator.Value) != 0)
					{
						descriptor.EnumValues.push_back(enumerator.Name);
					}
				}
			}
			// A list is the field editor's array of its elements' values, with the kind and hints set above.
			if (IsScriptFieldListType(field.Type))
			{
				descriptor.ElementKind = descriptor.Kind;
				descriptor.Kind = FieldKind::Array;
				descriptor.Hints.MaxCount = MaxScriptFieldListElements;
			}
			descriptors.push_back(std::move(descriptor));
		}
		return descriptors;
	}

	Json ToEditorValues(ScriptClassInfo const& scriptClass, Json const& storedFields)
	{
		Json values = Json::object();
		for (ScriptFieldInfo const& field : scriptClass.Fields)
		{
			if (field.Hidden)
			{
				continue;
			}
			Json const* storedValue = nullptr;
			if (storedFields.is_object())
			{
				auto const stored = storedFields.find(field.Name);
				if (stored != storedFields.end() && stored->is_object() && stored->contains("Type") && stored->contains("Value") &&
				    (*stored)["Type"] == ScriptFieldTypeToString(field.Type))
				{
					storedValue = &(*stored)["Value"];
				}
			}
			values[field.Name] = ToEditorValue(field, storedValue);
		}
		return values;
	}

	size_t EditorValueCache::Update(ScriptClassInfo const& scriptClass, ScriptFieldMap const& storedFields, Json& values)
	{
		// The shown fields, in order, are those converted last time, or the conversion starts over.
		size_t shown = 0;
		bool sameFields = values.is_object();
		for (ScriptFieldInfo const& field : scriptClass.Fields)
		{
			if (field.Hidden)
			{
				continue;
			}
			sameFields = sameFields && shown < m_Fields.size() && m_Fields[shown].Name == field.Name;
			shown++;
		}
		if (!sameFields || shown != m_Fields.size())
		{
			m_Fields.clear();
			values = Json::object();
		}

		size_t converted = 0;
		size_t index = 0;
		for (ScriptFieldInfo const& field : scriptClass.Fields)
		{
			if (field.Hidden)
			{
				continue;
			}
			auto const stored = storedFields.find(field.Name);
			bool const fromStored = stored != storedFields.end() && stored->second.GetType() == field.Type;
			ScriptFieldValue const& source = fromStored ? stored->second : field.DefaultValue;
			if (index < m_Fields.size())
			{
				ConvertedField const& last = m_Fields[index];
				if (last.Type == field.Type && last.IsFlags == field.IsFlags && last.FromStored == fromStored &&
				    IsSameStoredValue(last.Source, source) && last.Enumerators == field.Enumerators)
				{
					index++;
					continue;
				}
			}
			else
			{
				m_Fields.emplace_back();
			}
			// Read in its file form, as ToEditorValues reads it, so that both show the same value whatever its reader makes of
			// it.
			Json const storedValue = fromStored ? ScriptFieldValueToJson(stored->second) : Json();
			values[field.Name] = ToEditorValue(field, fromStored ? &storedValue : nullptr);
			ConvertedField& record = m_Fields[index];
			record.Name = field.Name;
			record.Type = field.Type;
			record.Enumerators = field.Enumerators;
			record.IsFlags = field.IsFlags;
			record.FromStored = fromStored;
			record.Source = source;
			converted++;
			index++;
		}
		return converted;
	}

	Result<Json> SetEditorValue(ScriptClassInfo const& scriptClass, Json storedFields, std::string_view fieldName, Json const& value)
	{
		ScriptFieldInfo const* const field = FindField(scriptClass, fieldName);
		if (field == nullptr)
		{
			return MakeError("{} has no field {}", scriptClass.Name, fieldName);
		}
		Result<ScriptFieldValue> parsed = FromEditorValue(*field, value);
		if (!parsed)
		{
			return Error{parsed.GetError()};
		}
		if (!storedFields.is_object())
		{
			storedFields = Json::object();
		}
		storedFields[field->Name] =
			Json::object({{"Type", ScriptFieldTypeToString(field->Type)}, {"Value", ScriptFieldValueToJson(parsed.GetValue())}});
		return storedFields;
	}

	Result<std::vector<ComponentEdit>> MakeEdits(ScriptClassInfo const& scriptClass, std::span<UUID const> entities,
	                                             std::span<Json const> components, std::span<Json const> editorValues,
	                                             FieldChange const& change, std::string_view componentName, std::string_view fieldsKey)
	{
		if (change.Path.empty() || entities.size() != components.size() || entities.size() != editorValues.size())
		{
			return Error{"the edit does not match the selection"};
		}
		std::string const fieldName(change.Path.front().Field);
		std::string const key(fieldsKey);
		std::vector<ComponentEdit> edits;
		edits.reserve(entities.size());
		for (size_t i = 0; i < entities.size(); i++)
		{
			Json const patch = change.MakePatch(editorValues[i]);
			auto const edited = patch.find(fieldName);
			if (edited == patch.end())
			{
				return MakeError("{} has no field {}", scriptClass.Name, fieldName);
			}
			auto const stored = components[i].find(key);
			Result<Json> fields = SetEditorValue(scriptClass, stored != components[i].end() ? *stored : Json::object(), fieldName, *edited);
			if (!fields)
			{
				return Error{fields.GetError()};
			}
			edits.push_back({entities[i], std::string(componentName), Json::object({{key, fields.TakeValue()}})});
		}
		return edits;
	}
}
