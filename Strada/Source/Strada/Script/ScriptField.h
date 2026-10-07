#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/UUID.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace Strada
{
	// Types a C# script field can have when it is exposed to the editor and serialized in scenes.
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

	char const* ScriptFieldTypeToString(ScriptFieldType type);
	std::optional<ScriptFieldType> ScriptFieldTypeFromString(std::string_view name);

	// A typed value stored for a script field. Entity fields hold the referenced entity's UUID; Prefab and Asset fields
	// hold asset handles; Color holds linear RGBA.
	class ScriptFieldValue
	{
	public:
		using Storage = std::variant<std::monostate, bool, int32_t, uint32_t, int64_t, uint64_t, float, double, std::string, glm::vec2,
		                             glm::vec3, glm::vec4, glm::quat, UUID, AssetHandle>;

		ScriptFieldValue() = default;

		// Creates the default value for a type (zero, empty string, identity quaternion, invalid reference).
		static ScriptFieldValue CreateDefault(ScriptFieldType type);

		ScriptFieldType GetType() const { return m_Type; }

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
}
