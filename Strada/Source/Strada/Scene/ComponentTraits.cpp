#include "stpch.h"
#include "Strada/Scene/ComponentTraits.h"

#include <charconv>
#include <string>

namespace Strada
{
	Json ScriptFieldValueToJson(ScriptFieldValue const& value)
	{
		switch (value.GetType())
		{
			case ScriptFieldType::None:
				return nullptr;
			case ScriptFieldType::Bool:
				return value.GetBool();
			case ScriptFieldType::Int32:
				return value.GetInt32();
			case ScriptFieldType::UInt32:
				return value.GetUInt32();
			case ScriptFieldType::Int64:
				// 64-bit integers are strings so JavaScript-style consumers keep full precision.
				return std::to_string(value.GetInt64());
			case ScriptFieldType::UInt64:
				return std::to_string(value.GetUInt64());
			case ScriptFieldType::Float:
				return value.GetFloat();
			case ScriptFieldType::Double:
				return value.GetDouble();
			case ScriptFieldType::String:
				return value.GetString();
			case ScriptFieldType::Vector2:
				return JsonTraits<glm::vec2>::ToJson(value.GetVector2());
			case ScriptFieldType::Vector3:
				return JsonTraits<glm::vec3>::ToJson(value.GetVector3());
			case ScriptFieldType::Vector4:
			case ScriptFieldType::Color:
				return JsonTraits<glm::vec4>::ToJson(value.GetVector4());
			case ScriptFieldType::Quaternion:
				return JsonTraits<glm::quat>::ToJson(value.GetQuaternion());
			case ScriptFieldType::Entity:
				return JsonTraits<UUID>::ToJson(value.GetEntity());
			case ScriptFieldType::Prefab:
			case ScriptFieldType::Asset:
				return JsonTraits<AssetHandle>::ToJson(value.GetAsset());
		}
		return nullptr;
	}

	namespace
	{
		template<typename T>
		Result<T> ReadValue(Json const& json, DeserializationContext const& context)
		{
			T value{};
			if (Result<void> result = JsonTraits<T>::FromJson(json, value, context); !result)
			{
				return Error{result.GetError()};
			}
			return value;
		}

		template<typename T>
		Result<T> ReadInt64String(Json const& json)
		{
			// Accepts the string form written by the serializer as well as plain JSON integers.
			if (json.is_string())
			{
				std::string const& text = json.get_ref<std::string const&>();
				T value{};
				auto const [pointer, errorCode] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
				if (errorCode == std::errc() && pointer == text.data() + text.size())
				{
					return value;
				}
			}
			else
			{
				T value{};
				if (Result<void> result = JsonTraits<T>::FromJson(json, value, DeserializationContext{}); result)
				{
					return value;
				}
			}
			return Error{std::is_signed_v<T> ? "expected a 64-bit integer" : "expected an unsigned 64-bit integer"};
		}
	}

	Result<ScriptFieldValue> ScriptFieldValueFromJson(ScriptFieldType type, Json const& json, DeserializationContext const& context)
	{
		switch (type)
		{
			case ScriptFieldType::None:
				return Error{"script field has no type"};
			case ScriptFieldType::Bool:
			{
				Result<bool> value = ReadValue<bool>(json, context);
				return value ? Result<ScriptFieldValue>(ScriptFieldValue::FromBool(value.GetValue())) : Error{value.GetError()};
			}
			case ScriptFieldType::Int32:
			{
				Result<int32_t> value = ReadValue<int32_t>(json, context);
				return value ? Result<ScriptFieldValue>(ScriptFieldValue::FromInt32(value.GetValue())) : Error{value.GetError()};
			}
			case ScriptFieldType::UInt32:
			{
				Result<uint32_t> value = ReadValue<uint32_t>(json, context);
				return value ? Result<ScriptFieldValue>(ScriptFieldValue::FromUInt32(value.GetValue())) : Error{value.GetError()};
			}
			case ScriptFieldType::Int64:
			{
				Result<int64_t> value = ReadInt64String<int64_t>(json);
				return value ? Result<ScriptFieldValue>(ScriptFieldValue::FromInt64(value.GetValue())) : Error{value.GetError()};
			}
			case ScriptFieldType::UInt64:
			{
				Result<uint64_t> value = ReadInt64String<uint64_t>(json);
				return value ? Result<ScriptFieldValue>(ScriptFieldValue::FromUInt64(value.GetValue())) : Error{value.GetError()};
			}
			case ScriptFieldType::Float:
			{
				Result<float> value = ReadValue<float>(json, context);
				return value ? Result<ScriptFieldValue>(ScriptFieldValue::FromFloat(value.GetValue())) : Error{value.GetError()};
			}
			case ScriptFieldType::Double:
			{
				Result<double> value = ReadValue<double>(json, context);
				return value ? Result<ScriptFieldValue>(ScriptFieldValue::FromDouble(value.GetValue())) : Error{value.GetError()};
			}
			case ScriptFieldType::String:
			{
				Result<std::string> value = ReadValue<std::string>(json, context);
				return value ? Result<ScriptFieldValue>(ScriptFieldValue::FromString(value.TakeValue())) : Error{value.GetError()};
			}
			case ScriptFieldType::Vector2:
			{
				Result<glm::vec2> value = ReadValue<glm::vec2>(json, context);
				return value ? Result<ScriptFieldValue>(ScriptFieldValue::FromVector2(value.GetValue())) : Error{value.GetError()};
			}
			case ScriptFieldType::Vector3:
			{
				Result<glm::vec3> value = ReadValue<glm::vec3>(json, context);
				return value ? Result<ScriptFieldValue>(ScriptFieldValue::FromVector3(value.GetValue())) : Error{value.GetError()};
			}
			case ScriptFieldType::Vector4:
			{
				Result<glm::vec4> value = ReadValue<glm::vec4>(json, context);
				return value ? Result<ScriptFieldValue>(ScriptFieldValue::FromVector4(value.GetValue())) : Error{value.GetError()};
			}
			case ScriptFieldType::Color:
			{
				Result<glm::vec4> value = ReadValue<glm::vec4>(json, context);
				return value ? Result<ScriptFieldValue>(ScriptFieldValue::FromColor(value.GetValue())) : Error{value.GetError()};
			}
			case ScriptFieldType::Quaternion:
			{
				Result<glm::quat> value = ReadValue<glm::quat>(json, context);
				return value ? Result<ScriptFieldValue>(ScriptFieldValue::FromQuaternion(value.GetValue())) : Error{value.GetError()};
			}
			case ScriptFieldType::Entity:
			{
				Result<UUID> value = ReadValue<UUID>(json, context);
				return value ? Result<ScriptFieldValue>(ScriptFieldValue::FromEntity(value.GetValue())) : Error{value.GetError()};
			}
			case ScriptFieldType::Prefab:
			{
				Result<AssetHandle> value = ReadValue<AssetHandle>(json, context);
				return value ? Result<ScriptFieldValue>(ScriptFieldValue::FromPrefab(value.GetValue())) : Error{value.GetError()};
			}
			case ScriptFieldType::Asset:
			{
				Result<AssetHandle> value = ReadValue<AssetHandle>(json, context);
				return value ? Result<ScriptFieldValue>(ScriptFieldValue::FromAsset(value.GetValue())) : Error{value.GetError()};
			}
		}
		return Error{"unknown script field type"};
	}

	Json JsonTraits<ScriptFieldMap>::ToJson(ScriptFieldMap const& fields)
	{
		Json json = Json::object();
		for (auto const& [name, value] : fields)
		{
			Json field = Json::object();
			field["Type"] = ScriptFieldTypeToString(value.GetType());
			field["Value"] = ScriptFieldValueToJson(value);
			json[name] = std::move(field);
		}
		return json;
	}

	Result<void> JsonTraits<ScriptFieldMap>::FromJson(Json const& json, ScriptFieldMap& out, DeserializationContext const& context)
	{
		if (!json.is_object())
		{
			return Error{"expected an object of script fields"};
		}

		ScriptFieldMap fields;
		for (auto const& item : json.items())
		{
			Json const& field = item.value();
			if (!field.is_object() || !field.contains("Type") || !field.contains("Value") || !field["Type"].is_string())
			{
				return MakeError("field '{}': expected {{ \"Type\": <type>, \"Value\": <value> }}", item.key());
			}

			std::optional<ScriptFieldType> const type = ScriptFieldTypeFromString(field["Type"].get_ref<std::string const&>());
			if (!type || *type == ScriptFieldType::None)
			{
				return MakeError("field '{}': unknown type '{}'", item.key(), field["Type"].get_ref<std::string const&>());
			}

			Result<ScriptFieldValue> value = ScriptFieldValueFromJson(*type, field["Value"], context);
			if (!value)
			{
				return MakeError("field '{}': {}", item.key(), value.GetError());
			}
			fields[item.key()] = value.TakeValue();
		}
		out = std::move(fields);
		return {};
	}
}
