#include "stpch.h"
#include "Strada/Script/ScriptField.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <vector>

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

		// The names of the list types, element type by element type (Bool to Asset).
		constexpr std::array<char const*, 16> s_ListTypeNames = {
			"Bool[]",    "Int32[]",   "UInt32[]",  "Int64[]",      "UInt64[]", "Float[]",  "Double[]", "String[]",
			"Vector2[]", "Vector3[]", "Vector4[]", "Quaternion[]", "Color[]",  "Entity[]", "Prefab[]", "Asset[]",
		};
		static_assert(s_ListTypeNames.size() == static_cast<size_t>(ScriptFieldType::Asset), "one list name per element type");

		constexpr std::string_view ListSuffix = "[]";
	}

	char const* ScriptFieldTypeToString(ScriptFieldType type)
	{
		if (IsScriptFieldListType(type))
		{
			return IsValidScriptFieldType(type) ? s_ListTypeNames[static_cast<size_t>(GetScriptFieldElementType(type)) - 1] : "None";
		}
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
		bool const isList = name.ends_with(ListSuffix);
		std::string_view const elementName = isList ? name.substr(0, name.size() - ListSuffix.size()) : name;
		for (auto const& [value, typeName] : s_TypeNames)
		{
			if (elementName == typeName)
			{
				if (!isList)
				{
					return value;
				}
				ScriptFieldType const listType = MakeScriptFieldListType(value);
				return listType != ScriptFieldType::None ? std::optional<ScriptFieldType>(listType) : std::nullopt;
			}
		}
		return std::nullopt;
	}

	ScriptFieldValue ScriptFieldValue::FromList(ScriptFieldType elementType, std::vector<ScriptFieldValue> elements)
	{
		ST_CORE_ASSERT(IsScriptFieldElementType(elementType), "A list's element type is an element type other than None");
		ST_CORE_ASSERT(elements.size() <= MaxScriptFieldListElements, "A list holds at most {} elements", MaxScriptFieldListElements);
		ST_CORE_ASSERT(std::all_of(elements.begin(), elements.end(),
		                           [elementType](ScriptFieldValue const& element)
		                           {
									   return element.GetType() == elementType;
								   }),
		               "Every element of a list has its element type");
		return ScriptFieldValue(MakeScriptFieldListType(elementType), std::move(elements));
	}

	ScriptFieldValue ScriptFieldValue::CreateDefault(ScriptFieldType type)
	{
		if (IsScriptFieldListType(type))
		{
			return IsValidScriptFieldType(type) ? FromList(GetScriptFieldElementType(type), {}) : ScriptFieldValue();
		}
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
