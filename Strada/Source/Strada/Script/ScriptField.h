#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/UUID.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Strada
{
	// Types a script field (a C# script's field, a visual script's public variable) can have when it is exposed to the editor
	// and serialized in scenes. The enumerators are the element types; a list of one of them is the element type with
	// ScriptFieldListFlag set (MakeScriptFieldListType), so that a list type never equals its element type and code that
	// compares a value's type with a field's type tells lists apart without knowing about them. There are no lists of lists
	// and no lists of None. Docs/Architecture.md §7.3 (the file form) and §10.3.
	enum class ScriptFieldType : uint8_t
	{
		None = 0,
		Bool,
		Int32,
		UInt32,
		Int64,
		UInt64,
		Float,
		Double,
		String,
		Vector2,
		Vector3,
		Vector4,
		Quaternion,
		Color,
		Entity,
		Prefab,
		Asset
	};

	// The bit of a list type: the list of an element type is that type with this bit set.
	inline constexpr uint8_t ScriptFieldListFlag = 0x80;
	// The most elements a list holds, in files, in the inspector and across the script boundary (the visual scripts' array
	// literals hold as many, MaxVisualScriptArrayLiteralElements).
	inline constexpr size_t MaxScriptFieldListElements = 4096;

	// Whether the type is a list type (of any element type, valid or not).
	[[nodiscard]] constexpr bool IsScriptFieldListType(ScriptFieldType type)
	{
		return (static_cast<uint8_t>(type) & ScriptFieldListFlag) != 0;
	}

	// The element type of a list type; an element type is its own.
	[[nodiscard]] constexpr ScriptFieldType GetScriptFieldElementType(ScriptFieldType type)
	{
		return static_cast<ScriptFieldType>(static_cast<uint8_t>(type) & static_cast<uint8_t>(~ScriptFieldListFlag));
	}

	// The last element type. A new element type changes every format that holds script fields (scenes, prefabs, save slots
	// and the player's settings: Docs/Architecture.md §7.3, "Adding a version", step 3); the save slot's and the settings'
	// formats pin it with MaxScriptFieldListElements, so that their versions cannot be forgotten.
	inline constexpr ScriptFieldType LastScriptFieldElementType = ScriptFieldType::Asset;

	// Whether the type is one of the element types other than None (Bool to LastScriptFieldElementType).
	[[nodiscard]] constexpr bool IsScriptFieldElementType(ScriptFieldType type)
	{
		return type != ScriptFieldType::None && static_cast<uint8_t>(type) <= static_cast<uint8_t>(LastScriptFieldElementType);
	}

	// The list type of an element type; None for None and for a list type (there are no lists of lists).
	[[nodiscard]] constexpr ScriptFieldType MakeScriptFieldListType(ScriptFieldType elementType)
	{
		return IsScriptFieldElementType(elementType) ? static_cast<ScriptFieldType>(static_cast<uint8_t>(elementType) | ScriptFieldListFlag)
		                                             : ScriptFieldType::None;
	}

	// Whether a field or a value can have the type: an element type other than None, or the list of one.
	[[nodiscard]] constexpr bool IsValidScriptFieldType(ScriptFieldType type)
	{
		return IsScriptFieldElementType(GetScriptFieldElementType(type));
	}

	// The type's name in files and across the script boundary: the element type's name ("Float"), followed by "[]" for a
	// list ("Float[]"); "None" for None and for a type that is not valid.
	char const* ScriptFieldTypeToString(ScriptFieldType type);
	// The type a name stands for: every name ScriptFieldTypeToString gives a valid type, and "None". Nothing for other text
	// ("None[]", "Float[][]").
	std::optional<ScriptFieldType> ScriptFieldTypeFromString(std::string_view name);

	// A typed value stored for a script field. Entity fields hold the referenced entity's UUID; Prefab and Asset fields
	// hold asset handles; Color holds linear RGBA. A list holds its elements, each a value of its element type, at most
	// MaxScriptFieldListElements of them. Values are made by the factories, which keep those rules, so a value's type
	// always tells what it holds. Any thread (values are plain data).
	class ScriptFieldValue
	{
	public:
		using Storage = std::variant<std::monostate, bool, int32_t, uint32_t, int64_t, uint64_t, float, double, std::string, glm::vec2,
		                             glm::vec3, glm::vec4, glm::quat, UUID, AssetHandle, std::vector<ScriptFieldValue>>;

		ScriptFieldValue() = default;

		// Creates the default value for a type (zero, empty string, identity quaternion, invalid reference, an empty list);
		// a value of None for None and for a type that is not valid.
		static ScriptFieldValue CreateDefault(ScriptFieldType type);

		ScriptFieldType GetType() const { return m_Type; }
		bool IsList() const { return IsScriptFieldListType(m_Type); }
		// The element type of a list; a value's own type otherwise.
		ScriptFieldType GetElementType() const { return GetScriptFieldElementType(m_Type); }

		bool GetBool() const { return std::get<bool>(m_Value); }
		int32_t GetInt32() const { return std::get<int32_t>(m_Value); }
		uint32_t GetUInt32() const { return std::get<uint32_t>(m_Value); }
		int64_t GetInt64() const { return std::get<int64_t>(m_Value); }
		uint64_t GetUInt64() const { return std::get<uint64_t>(m_Value); }
		float GetFloat() const { return std::get<float>(m_Value); }
		double GetDouble() const { return std::get<double>(m_Value); }
		std::string const& GetString() const { return std::get<std::string>(m_Value); }
		glm::vec2 GetVector2() const { return std::get<glm::vec2>(m_Value); }
		glm::vec3 GetVector3() const { return std::get<glm::vec3>(m_Value); }
		// Valid for Vector4 and Color.
		glm::vec4 GetVector4() const { return std::get<glm::vec4>(m_Value); }
		glm::quat GetQuaternion() const { return std::get<glm::quat>(m_Value); }
		UUID GetEntity() const { return std::get<UUID>(m_Value); }
		// Valid for Prefab and Asset.
		AssetHandle GetAsset() const { return std::get<AssetHandle>(m_Value); }
		// Valid for lists: the elements, in order, each of the list's element type.
		std::span<ScriptFieldValue const> GetElements() const { return std::get<std::vector<ScriptFieldValue>>(m_Value); }

		static ScriptFieldValue FromBool(bool value) { return ScriptFieldValue(ScriptFieldType::Bool, value); }
		static ScriptFieldValue FromInt32(int32_t value) { return ScriptFieldValue(ScriptFieldType::Int32, value); }
		static ScriptFieldValue FromUInt32(uint32_t value) { return ScriptFieldValue(ScriptFieldType::UInt32, value); }
		static ScriptFieldValue FromInt64(int64_t value) { return ScriptFieldValue(ScriptFieldType::Int64, value); }
		static ScriptFieldValue FromUInt64(uint64_t value) { return ScriptFieldValue(ScriptFieldType::UInt64, value); }
		static ScriptFieldValue FromFloat(float value) { return ScriptFieldValue(ScriptFieldType::Float, value); }
		static ScriptFieldValue FromDouble(double value) { return ScriptFieldValue(ScriptFieldType::Double, value); }
		static ScriptFieldValue FromString(std::string value) { return ScriptFieldValue(ScriptFieldType::String, std::move(value)); }
		static ScriptFieldValue FromVector2(glm::vec2 value) { return ScriptFieldValue(ScriptFieldType::Vector2, value); }
		static ScriptFieldValue FromVector3(glm::vec3 value) { return ScriptFieldValue(ScriptFieldType::Vector3, value); }
		static ScriptFieldValue FromVector4(glm::vec4 value) { return ScriptFieldValue(ScriptFieldType::Vector4, value); }
		static ScriptFieldValue FromQuaternion(glm::quat value) { return ScriptFieldValue(ScriptFieldType::Quaternion, value); }
		static ScriptFieldValue FromColor(glm::vec4 value) { return ScriptFieldValue(ScriptFieldType::Color, value); }
		static ScriptFieldValue FromEntity(UUID value) { return ScriptFieldValue(ScriptFieldType::Entity, value); }
		static ScriptFieldValue FromPrefab(AssetHandle value) { return ScriptFieldValue(ScriptFieldType::Prefab, value); }
		static ScriptFieldValue FromAsset(AssetHandle value) { return ScriptFieldValue(ScriptFieldType::Asset, value); }
		// A list of the element type (an element type other than None) holding the elements, each of that type, at most
		// MaxScriptFieldListElements: a programmer error otherwise (readers of user data check first).
		static ScriptFieldValue FromList(ScriptFieldType elementType, std::vector<ScriptFieldValue> elements);

		Storage const& GetStorage() const { return m_Value; }

		bool operator==(ScriptFieldValue const& other) const { return m_Type == other.m_Type && m_Value == other.m_Value; }

	private:
		template<typename T>
		ScriptFieldValue(ScriptFieldType type, T value)
			: m_Type(type),
			  m_Value(std::move(value))
		{
		}

		ScriptFieldType m_Type = ScriptFieldType::None;
		Storage m_Value;
	};

	// Field name -> value; ordered so serialized scenes are deterministic.
	using ScriptFieldMap = std::map<std::string, ScriptFieldValue>;

	// What a read-back of an instance's changes answers (ScriptEngine::GetChangedInstanceFields,
	// VisualScriptEngine::GetChangedInstanceVariables; Docs/Architecture.md §11, "Long lists").
	struct ScriptFieldChanges
	{
		// The fields or public variables whose values changed since the instance's last read-back of changes, in the stored
		// form, by name.
		ScriptFieldMap Changed;
		// The names of the instance's other fields or public variables, whose values are those that read-back answered.
		std::vector<std::string> Unchanged;
	};
}
