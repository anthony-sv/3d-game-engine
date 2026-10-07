#include "stpch.h"
#include "Strada/Script/ScriptField.h"

#include <array>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr std::array<std::pair<ScriptFieldType, char const*>, 17> s_TypeNames = {{
			{ScriptFieldType::None, "None"},
			{ScriptFieldType::Bool, "Bool"},
			{ScriptFieldType::Int32, "Int32"},
			{ScriptFieldType::UInt32, "UInt32"},
			{ScriptFieldType::Int64, "Int64"},
			{ScriptFieldType::UInt64, "UInt64"},
			{ScriptFieldType::Float, "Float"},
			{ScriptFieldType::Double, "Double"},
			{ScriptFieldType::String, "String"},
			{ScriptFieldType::Vector2, "Vector2"},
			{ScriptFieldType::Vector3, "Vector3"},
			{ScriptFieldType::Vector4, "Vector4"},
			{ScriptFieldType::Quaternion, "Quaternion"},
			{ScriptFieldType::Color, "Color"},
			{ScriptFieldType::Entity, "Entity"},
			{ScriptFieldType::Prefab, "Prefab"},
			{ScriptFieldType::Asset, "Asset"},
		}};
	}

	char const* ScriptFieldTypeToString(ScriptFieldType type)
	{
		for (auto const& [value, name] : s_TypeNames)
		{
			if (value == type)
			{
				return name;
			}
		}
		return "None";
	}

	std::optional<ScriptFieldType> ScriptFieldTypeFromString(std::string_view name)
	{
		for (auto const& [value, typeName] : s_TypeNames)
		{
			if (name == typeName)
			{
				return value;
			}
		}
		return std::nullopt;
	}

	ScriptFieldValue ScriptFieldValue::CreateDefault(ScriptFieldType type)
	{
		switch (type)
		{
			case ScriptFieldType::None:
				return ScriptFieldValue();
			case ScriptFieldType::Bool:
				return FromBool(false);
			case ScriptFieldType::Int32:
				return FromInt32(0);
			case ScriptFieldType::UInt32:
				return FromUInt32(0);
			case ScriptFieldType::Int64:
				return FromInt64(0);
			case ScriptFieldType::UInt64:
				return FromUInt64(0);
			case ScriptFieldType::Float:
				return FromFloat(0.0f);
			case ScriptFieldType::Double:
				return FromDouble(0.0);
			case ScriptFieldType::String:
				return FromString({});
			case ScriptFieldType::Vector2:
				return FromVector2(glm::vec2(0.0f));
			case ScriptFieldType::Vector3:
				return FromVector3(glm::vec3(0.0f));
			case ScriptFieldType::Vector4:
				return FromVector4(glm::vec4(0.0f));
			case ScriptFieldType::Quaternion:
				return FromQuaternion(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			case ScriptFieldType::Color:
				return FromColor(glm::vec4(1.0f));
			case ScriptFieldType::Entity:
				return FromEntity(UUID::Invalid());
			case ScriptFieldType::Prefab:
				return FromPrefab(AssetHandle());
			case ScriptFieldType::Asset:
				return FromAsset(AssetHandle());
		}
		return ScriptFieldValue();
	}
}
