#include "stpch.h"
#include "Strada/Script/VisualScriptLiterals.h"

#include "Strada/Asset/VisualScriptSerializer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace Strada
{
	namespace
	{
		struct ExposableTypeEntry
		{
			std::string_view TypeId;
			VisualScriptExposableType Type;
		};

		constexpr std::array<ExposableTypeEntry, 23> ExposableTypes = {{
			{"bool", {ScriptFieldType::Bool, AssetType::None}},
			{"int", {ScriptFieldType::Int32, AssetType::None}},
			{"uint", {ScriptFieldType::UInt32, AssetType::None}},
			{"long", {ScriptFieldType::Int64, AssetType::None}},
			{"ulong", {ScriptFieldType::UInt64, AssetType::None}},
			{"float", {ScriptFieldType::Float, AssetType::None}},
			{"double", {ScriptFieldType::Double, AssetType::None}},
			{"string", {ScriptFieldType::String, AssetType::None}},
			{"Strada.Vector2", {ScriptFieldType::Vector2, AssetType::None}},
			{"Strada.Vector3", {ScriptFieldType::Vector3, AssetType::None}},
			{"Strada.Vector4", {ScriptFieldType::Vector4, AssetType::None}},
			{"Strada.Quaternion", {ScriptFieldType::Quaternion, AssetType::None}},
			{"Strada.Color", {ScriptFieldType::Color, AssetType::None}},
			{"Strada.Entity", {ScriptFieldType::Entity, AssetType::None}},
			{"Strada.Prefab", {ScriptFieldType::Prefab, AssetType::None}},
			{"Strada.AssetHandle", {ScriptFieldType::Asset, AssetType::None}},
			{"Strada.Mesh", {ScriptFieldType::Asset, AssetType::Mesh}},
			{"Strada.Material", {ScriptFieldType::Asset, AssetType::Material}},
			{"Strada.Texture", {ScriptFieldType::Asset, AssetType::Texture}},
			{"Strada.AudioClip", {ScriptFieldType::Asset, AssetType::AudioClip}},
			{"Strada.Font", {ScriptFieldType::Asset, AssetType::Font}},
			{"Strada.EnvironmentMap", {ScriptFieldType::Asset, AssetType::Environment}},
			{"Strada.VisualScript", {ScriptFieldType::Asset, AssetType::VisualScript}},
		}};

		bool IsIntegerType(ScriptFieldType type)
		{
			return type == ScriptFieldType::Int32 || type == ScriptFieldType::UInt32 || type == ScriptFieldType::Int64 ||
			       type == ScriptFieldType::UInt64;
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

		template<typename T>
		std::optional<ScriptFieldValue> Read(Json const& literal, ScriptFieldValue (*make)(T))
		{
			T value{};
			// No asset references: graphs hold assets as decimal handles.
			if (!JsonTraits<T>::FromJson(literal, value, DeserializationContext{}))
			{
				return std::nullopt;
			}
			return make(std::move(value));
		}

		// A float, or a vector of floats as the array of its components, with the floats read as documents store them: up to
		// a magnitude a little above float's largest value (VisualScriptSerializer::ReadFloat).
		template<typename T>
		std::optional<T> ReadFloats(Json const& literal)
		{
			if constexpr (std::is_same_v<T, float>)
			{
				return VisualScriptSerializer::ReadFloat(literal);
			}
			else
			{
				constexpr glm::length_t Length = T::length();
				if (!literal.is_array() || literal.size() != static_cast<size_t>(Length))
				{
					return std::nullopt;
				}
				T value{};
				for (glm::length_t i = 0; i < Length; i++)
				{
					std::optional<float> const component = VisualScriptSerializer::ReadFloat(literal[static_cast<size_t>(i)]);
					if (!component)
					{
						return std::nullopt;
					}
					value[i] = *component;
				}
				return value;
			}
		}

		template<typename T>
		std::optional<ScriptFieldValue> ReadFloatLiteral(Json const& literal, ScriptFieldValue (*make)(T))
		{
			std::optional<T> const value = ReadFloats<T>(literal);
			return value ? std::optional<ScriptFieldValue>(make(*value)) : std::nullopt;
		}

		// 64-bit integers and handles are decimal strings.
		template<typename T>
		std::optional<T> ReadDecimal(Json const& literal)
		{
			if (!literal.is_string())
			{
				return std::nullopt;
			}
			std::string const& text = literal.get_ref<std::string const&>();
			T value{};
			auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
			if (error != std::errc() || end != text.data() + text.size())
			{
				return std::nullopt;
			}
			return value;
		}

		std::optional<ScriptFieldValue> ReadHandle(Json const& literal, ScriptFieldValue (*make)(AssetHandle))
		{
			std::optional<uint64_t> const handle = ReadDecimal<uint64_t>(literal);
			return handle ? std::optional<ScriptFieldValue>(make(AssetHandle(UUID(*handle)))) : std::nullopt;
		}

		// A quaternion's components [x, y, z, w] normalized as the engine reads rotations (JsonTraits<glm::quat>): unit ones are
		// kept bit-exact, and one of zero length is the identity (the scripting runtime warns about it).
		glm::quat NormalizeLiteralRotation(glm::vec4 const& components)
		{
			Json const floats = Json::array({components.x, components.y, components.z, components.w});
			glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
			if (glm::quat normalized; JsonTraits<glm::quat>::FromJson(floats, normalized, DeserializationContext{}))
			{
				rotation = normalized;
			}
			return rotation;
		}

		// A literal as a value of the script field type of its type; nothing when it is no literal of the type.
		std::optional<ScriptFieldValue> ReadLiteral(ScriptFieldType type, Json const& literal)
		{
			switch (type)
			{
				case ScriptFieldType::None:
					return std::nullopt;
				case ScriptFieldType::Bool:
					return Read(literal, &ScriptFieldValue::FromBool);
				case ScriptFieldType::Int32:
					return Read(literal, &ScriptFieldValue::FromInt32);
				case ScriptFieldType::UInt32:
					return Read(literal, &ScriptFieldValue::FromUInt32);
				case ScriptFieldType::Int64:
				{
					std::optional<int64_t> const value = ReadDecimal<int64_t>(literal);
					return value ? std::optional<ScriptFieldValue>(ScriptFieldValue::FromInt64(*value)) : std::nullopt;
				}
				case ScriptFieldType::UInt64:
				{
					std::optional<uint64_t> const value = ReadDecimal<uint64_t>(literal);
					return value ? std::optional<ScriptFieldValue>(ScriptFieldValue::FromUInt64(*value)) : std::nullopt;
				}
				case ScriptFieldType::Float:
					return ReadFloatLiteral(literal, &ScriptFieldValue::FromFloat);
				case ScriptFieldType::Double:
					return Read(literal, &ScriptFieldValue::FromDouble);
				case ScriptFieldType::String:
					return Read(literal, &ScriptFieldValue::FromString);
				case ScriptFieldType::Vector2:
					return ReadFloatLiteral(literal, &ScriptFieldValue::FromVector2);
				case ScriptFieldType::Vector3:
					return ReadFloatLiteral(literal, &ScriptFieldValue::FromVector3);
				case ScriptFieldType::Vector4:
					return ReadFloatLiteral(literal, &ScriptFieldValue::FromVector4);
				case ScriptFieldType::Quaternion:
				{
					// [x, y, z, w]
					std::optional<glm::vec4> const components = ReadFloats<glm::vec4>(literal);
					if (!components)
					{
						return std::nullopt;
					}
					return ScriptFieldValue::FromQuaternion(NormalizeLiteralRotation(*components));
				}
				case ScriptFieldType::Color:
					return ReadFloatLiteral(literal, &ScriptFieldValue::FromColor);
				case ScriptFieldType::Entity:
					// Graphs are shared by entities, so they refer to none: entities are set on each component.
					return literal == "0" ? std::optional<ScriptFieldValue>(ScriptFieldValue::FromEntity(UUID::Invalid())) : std::nullopt;
				case ScriptFieldType::Prefab:
					return ReadHandle(literal, &ScriptFieldValue::FromPrefab);
				case ScriptFieldType::Asset:
					return ReadHandle(literal, &ScriptFieldValue::FromAsset);
			}
			return std::nullopt;
		}

		// A float as the scripting runtime writes it: its shortest round-trip text, as the number that text reads as (which
		// serializes as that text). The text "-0" reads as the integer 0, and the shortest texts of ±7.038531e-26 read through
		// a double, as every reader of documents reads floats, round to the neighbouring float: a float its text does not give
		// back bit for bit is written as its exact value instead. Nothing for a float that is not finite.
		std::optional<Json> WriteFloat(float value)
		{
			if (!std::isfinite(value))
			{
				return std::nullopt;
			}
			// A float's shortest text has at most 9 significant digits: it fits.
			std::array<char, 32> text{};
			auto const [end, error] = std::to_chars(text.data(), text.data() + text.size(), value);
			if (error == std::errc())
			{
				// Read as documents are: std::from_chars for doubles is missing from some standard libraries.
				Result<Json> number = ParseJson(std::string_view(text.data(), static_cast<size_t>(end - text.data())));
				if (number)
				{
					std::optional<float> const read = VisualScriptSerializer::ReadFloat(number.GetValue());
					if (read && std::bit_cast<uint32_t>(*read) == std::bit_cast<uint32_t>(value))
					{
						return number.TakeValue();
					}
				}
			}
			return Json(static_cast<double>(value));
		}

		template<typename T>
		std::optional<Json> WriteFloats(T const& components)
		{
			Json floats = Json::array();
			for (glm::length_t i = 0; i < T::length(); i++)
			{
				std::optional<Json> component = WriteFloat(components[i]);
				if (!component)
				{
					return std::nullopt;
				}
				floats.push_back(std::move(*component));
			}
			return floats;
		}

		// The canonical literal of a value of an exposable type's script field type.
		std::optional<Json> WriteLiteral(ScriptFieldValue const& value)
		{
			switch (value.GetType())
			{
				case ScriptFieldType::None:
					return std::nullopt;
				case ScriptFieldType::Bool:
					return Json(value.GetBool());
				case ScriptFieldType::Int32:
					return Json(value.GetInt32());
				case ScriptFieldType::UInt32:
					return Json(value.GetUInt32());
				case ScriptFieldType::Int64:
					return Json(std::to_string(value.GetInt64()));
				case ScriptFieldType::UInt64:
					return Json(std::to_string(value.GetUInt64()));
				case ScriptFieldType::Float:
					return WriteFloat(value.GetFloat());
				case ScriptFieldType::Double:
				{
					double const number = value.GetDouble();
					return std::isfinite(number) ? std::optional<Json>(Json(number)) : std::nullopt;
				}
				case ScriptFieldType::String:
					return Json(value.GetString());
				case ScriptFieldType::Vector2:
					return WriteFloats(value.GetVector2());
				case ScriptFieldType::Vector3:
					return WriteFloats(value.GetVector3());
				case ScriptFieldType::Vector4:
				case ScriptFieldType::Color:
					return WriteFloats(value.GetVector4());
				case ScriptFieldType::Quaternion:
				{
					glm::quat const quaternion = value.GetQuaternion();
					glm::vec4 const components(quaternion.x, quaternion.y, quaternion.z, quaternion.w);
					if (!std::isfinite(components.x) || !std::isfinite(components.y) || !std::isfinite(components.z) ||
					    !std::isfinite(components.w))
					{
						return std::nullopt;
					}
					glm::quat const rotation = NormalizeLiteralRotation(components);
					return WriteFloats(glm::vec4(rotation.x, rotation.y, rotation.z, rotation.w));
				}
				case ScriptFieldType::Entity:
					return value.GetEntity().IsValid() ? std::nullopt : std::optional<Json>(Json("0"));
				case ScriptFieldType::Prefab:
				case ScriptFieldType::Asset:
					return Json(value.GetAsset().ToString());
			}
			return std::nullopt;
		}

		ScriptEnumerator const* FindEnumeratorByName(VisualScriptEnumInfo const& info, Json const& name)
		{
			if (!name.is_string())
			{
				return nullptr;
			}
			std::string const& text = name.get_ref<std::string const&>();
			auto const enumerator = std::find_if(info.Enumerators.begin(), info.Enumerators.end(),
			                                     [&text](ScriptEnumerator const& candidate)
			                                     {
													 return candidate.Name == text;
												 });
			return enumerator != info.Enumerators.end() ? &*enumerator : nullptr;
		}

		// The first enumerator, in declaration order, of exactly these bits.
		ScriptEnumerator const* FindEnumeratorByBits(VisualScriptEnumInfo const& info, uint64_t bits)
		{
			auto const enumerator = std::find_if(info.Enumerators.begin(), info.Enumerators.end(),
			                                     [bits](ScriptEnumerator const& candidate)
			                                     {
													 return ToBits(candidate.Value) == bits;
												 });
			return enumerator != info.Enumerators.end() ? &*enumerator : nullptr;
		}

		// The names of a [Flags] value: none for 0, the enumerator of exactly its bits when there is one, else every enumerator
		// whose bits are among its bits, when together they make it up exactly; nothing when they do not.
		std::optional<Json> WriteFlagNames(VisualScriptEnumInfo const& info, uint64_t bits)
		{
			Json names = Json::array();
			if (bits != 0)
			{
				if (ScriptEnumerator const* const exact = FindEnumeratorByBits(info, bits))
				{
					names.push_back(exact->Name);
					return names;
				}
			}
			uint64_t covered = 0;
			for (ScriptEnumerator const& enumerator : info.Enumerators)
			{
				uint64_t const enumeratorBits = ToBits(enumerator.Value);
				if (enumeratorBits != 0 && (bits & enumeratorBits) == enumeratorBits)
				{
					names.push_back(enumerator.Name);
					covered |= enumeratorBits;
				}
			}
			return covered == bits ? std::optional<Json>(std::move(names)) : std::nullopt;
		}

		// The enum a type is, as the lookup knows it; the lookup is asked only about types that may be enums.
		std::optional<VisualScriptEnumInfo> FindEnum(std::string_view typeId, VisualScriptEnumLookup const& enumLookup)
		{
			if (!enumLookup || !CanBeVisualScriptEnum(typeId))
			{
				return std::nullopt;
			}
			return enumLookup(typeId);
		}
	}

	std::optional<VisualScriptExposableType> FindVisualScriptExposableType(std::string_view typeId)
	{
		for (ExposableTypeEntry const& entry : ExposableTypes)
		{
			if (entry.TypeId == typeId)
			{
				return entry.Type;
			}
		}
		return std::nullopt;
	}

	bool CanBeVisualScriptEnum(std::string_view typeId)
	{
		// The types the type system names itself are no enums (exec, the wildcard and Strada.Matrix4; the others are
		// exposable), nor are System.Object and arrays.
		return !typeId.empty() && typeId != "exec" && typeId != "*" && typeId != "Strada.Matrix4" && typeId != "System.Object" &&
		       !typeId.ends_with("[]");
	}

	bool VisualScriptEnumInfo::IsUsable() const
	{
		return IsIntegerType(UnderlyingType) && std::all_of(Enumerators.begin(), Enumerators.end(),
		                                                    [this](ScriptEnumerator const& enumerator)
		                                                    {
																return enumerator.Value.GetType() == UnderlyingType;
															});
	}

	std::optional<ScriptFieldValue> DecodeVisualScriptLiteral(Json const& literal, std::string_view typeId,
	                                                          VisualScriptEnumLookup const& enumLookup)
	{
		if (std::optional<VisualScriptExposableType> const exposable = FindVisualScriptExposableType(typeId))
		{
			return ReadLiteral(exposable->FieldType, literal);
		}
		std::optional<VisualScriptEnumInfo> const enumInfo = FindEnum(typeId, enumLookup);
		return enumInfo ? DecodeVisualScriptEnumLiteral(literal, *enumInfo) : std::nullopt;
	}

	std::optional<Json> EncodeVisualScriptLiteral(ScriptFieldValue const& value, std::string_view typeId,
	                                              VisualScriptEnumLookup const& enumLookup)
	{
		if (std::optional<VisualScriptExposableType> const exposable = FindVisualScriptExposableType(typeId))
		{
			return value.GetType() == exposable->FieldType ? WriteLiteral(value) : std::nullopt;
		}
		std::optional<VisualScriptEnumInfo> const enumInfo = FindEnum(typeId, enumLookup);
		return enumInfo ? EncodeVisualScriptEnumLiteral(value, *enumInfo) : std::nullopt;
	}

	std::optional<ScriptFieldValue> DecodeVisualScriptEnumLiteral(Json const& literal, VisualScriptEnumInfo const& enumInfo)
	{
		if (!enumInfo.IsUsable())
		{
			return std::nullopt;
		}
		if (enumInfo.IsFlags && literal.is_array())
		{
			uint64_t bits = 0;
			for (Json const& name : literal)
			{
				ScriptEnumerator const* const enumerator = FindEnumeratorByName(enumInfo, name);
				if (enumerator == nullptr)
				{
					return std::nullopt;
				}
				bits |= ToBits(enumerator->Value);
			}
			return FromBits(enumInfo.UnderlyingType, bits);
		}
		if (!enumInfo.IsFlags)
		{
			if (ScriptEnumerator const* const enumerator = FindEnumeratorByName(enumInfo, literal))
			{
				return enumerator->Value;
			}
		}
		return ReadLiteral(enumInfo.UnderlyingType, literal);
	}

	std::optional<Json> EncodeVisualScriptEnumLiteral(ScriptFieldValue const& value, VisualScriptEnumInfo const& enumInfo)
	{
		if (!enumInfo.IsUsable() || value.GetType() != enumInfo.UnderlyingType)
		{
			return std::nullopt;
		}
		uint64_t const bits = ToBits(value);
		if (enumInfo.IsFlags)
		{
			if (std::optional<Json> names = WriteFlagNames(enumInfo, bits))
			{
				return names;
			}
		}
		else if (ScriptEnumerator const* const enumerator = FindEnumeratorByBits(enumInfo, bits))
		{
			return Json(enumerator->Name);
		}
		// The integer: a JSON integer for 32-bit enums, a decimal string for 64-bit ones.
		return WriteLiteral(value);
	}
}
