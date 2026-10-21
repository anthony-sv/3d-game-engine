#include "stpch.h"
#include "Strada/Script/VisualScriptVariables.h"

#include "Strada/Serialization/JsonSerialization.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <set>
#include <system_error>
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

		constexpr std::array<ExposableTypeEntry, 23> s_ExposableTypes = {{
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

		constexpr std::string_view EditableClassName = "Visual Script";
		constexpr std::string_view NeedsRuntimeReason = "needs the scripting runtime";
		constexpr std::string_view NotExposableReason = "not exposable";

		// Whether a type that is not exposable could be an enum, which only the scripting runtime can tell. The types the
		// type system names itself are no enums (exec, the wildcard and Strada.Matrix4; the others are exposable), nor are
		// System.Object and arrays.
		bool MayBeEnum(std::string_view typeId)
		{
			return !typeId.empty() && typeId != "exec" && typeId != "*" && typeId != "Strada.Matrix4" && typeId != "System.Object" &&
			       !typeId.ends_with("[]");
		}

		bool IsIntegerType(ScriptFieldType type)
		{
			return type == ScriptFieldType::Int32 || type == ScriptFieldType::UInt32 || type == ScriptFieldType::Int64 ||
			       type == ScriptFieldType::UInt64;
		}

		// An enum the lookup describes as its runtime does: of an integer type, with enumerators of that type.
		bool IsUsable(VisualScriptEnumInfo const& info)
		{
			return IsIntegerType(info.UnderlyingType) && std::all_of(info.Enumerators.begin(), info.Enumerators.end(),
			                                                         [&info](ScriptEnumerator const& enumerator)
			                                                         {
																		 return enumerator.Value.GetType() == info.UnderlyingType;
																	 });
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

		// A literal of a document (a variable's Value) as a value of the script field type its type is exposed as; nothing
		// when it is no literal of the type.
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
					return Read(literal, &ScriptFieldValue::FromFloat);
				case ScriptFieldType::Double:
					return Read(literal, &ScriptFieldValue::FromDouble);
				case ScriptFieldType::String:
					return Read(literal, &ScriptFieldValue::FromString);
				case ScriptFieldType::Vector2:
					return Read(literal, &ScriptFieldValue::FromVector2);
				case ScriptFieldType::Vector3:
					return Read(literal, &ScriptFieldValue::FromVector3);
				case ScriptFieldType::Vector4:
					return Read(literal, &ScriptFieldValue::FromVector4);
				case ScriptFieldType::Quaternion:
				{
					std::array<float, 4> components{};
					if (!Detail::ReadFloatArray(literal, components))
					{
						return std::nullopt;
					}
					// Read normalized; a quaternion of zero length is the identity (the scripting runtime warns about it).
					glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
					if (glm::quat normalized; JsonTraits<glm::quat>::FromJson(literal, normalized, DeserializationContext{}))
					{
						rotation = normalized;
					}
					return ScriptFieldValue::FromQuaternion(rotation);
				}
				case ScriptFieldType::Color:
					return Read(literal, &ScriptFieldValue::FromColor);
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

		// An enum literal: an enumerator's name, an array of names for [Flags] enums, or the integer of a value without an
		// enumerator (a decimal string for 64-bit enums).
		std::optional<ScriptFieldValue> ReadEnumLiteral(VisualScriptEnumInfo const& info, Json const& literal)
		{
			auto const find = [&info](Json const& name) -> ScriptEnumerator const*
			{
				if (!name.is_string())
				{
					return nullptr;
				}
				auto const enumerator = std::find_if(info.Enumerators.begin(), info.Enumerators.end(),
				                                     [&name](ScriptEnumerator const& candidate)
				                                     {
														 return candidate.Name == name.get_ref<std::string const&>();
													 });
				return enumerator != info.Enumerators.end() ? &*enumerator : nullptr;
			};
			if (info.IsFlags && literal.is_array())
			{
				uint64_t bits = 0;
				for (Json const& name : literal)
				{
					ScriptEnumerator const* const enumerator = find(name);
					if (enumerator == nullptr)
					{
						return std::nullopt;
					}
					bits |= ToBits(enumerator->Value);
				}
				return FromBits(info.UnderlyingType, bits);
			}
			if (!info.IsFlags)
			{
				if (ScriptEnumerator const* const enumerator = find(literal))
				{
					return enumerator->Value;
				}
			}
			return ReadLiteral(info.UnderlyingType, literal);
		}
	}

	std::optional<VisualScriptExposableType> FindVisualScriptExposableType(std::string_view typeId)
	{
		for (ExposableTypeEntry const& entry : s_ExposableTypes)
		{
			if (entry.TypeId == typeId)
			{
				return entry.Type;
			}
		}
		return std::nullopt;
	}

	VisualScriptVariableDescription DescribeVisualScriptVariables(VisualScriptDocument const& document,
	                                                              VisualScriptEnumLookup const& enumLookup)
	{
		VisualScriptVariableDescription description;
		description.Editable.Name = EditableClassName;
		for (VisualScriptVariable const& variable : document.Variables)
		{
			if (!variable.Public)
			{
				continue;
			}
			ScriptFieldInfo field;
			field.Name = variable.Name;
			field.Tooltip = variable.Tooltip;
			field.Range = variable.Range;
			if (std::optional<VisualScriptExposableType> const exposable = FindVisualScriptExposableType(variable.Type))
			{
				field.Type = exposable->FieldType;
				field.AcceptedAssetType = exposable->AcceptedAssetType;
				field.DefaultValue = ReadLiteral(field.Type, variable.Value).value_or(ScriptFieldValue::CreateDefault(field.Type));
				description.Editable.Fields.push_back(std::move(field));
				continue;
			}

			bool const mayBeEnum = MayBeEnum(variable.Type);
			std::optional<VisualScriptEnumInfo> const enumInfo = mayBeEnum && enumLookup ? enumLookup(variable.Type) : std::nullopt;
			if (!enumInfo || !IsUsable(*enumInfo))
			{
				std::string_view const reason = mayBeEnum && !enumLookup ? NeedsRuntimeReason : NotExposableReason;
				description.ReadOnly.push_back({variable.Name, variable.Type, std::string(reason)});
				continue;
			}
			field.Type = enumInfo->UnderlyingType;
			field.Enumerators = enumInfo->Enumerators;
			field.IsFlags = enumInfo->IsFlags;
			field.DefaultValue = ReadEnumLiteral(*enumInfo, variable.Value).value_or(ScriptFieldValue::CreateDefault(field.Type));
			description.Editable.Fields.push_back(std::move(field));
		}
		return description;
	}

	ScriptFieldMap NormalizeVisualScriptOverrides(VisualScriptDocument const& document, ScriptFieldMap overrides)
	{
		std::set<std::string_view> publicNames;
		for (VisualScriptVariable const& variable : document.Variables)
		{
			if (variable.Public)
			{
				publicNames.insert(variable.Name);
			}
		}

		for (VisualScriptVariable const& variable : document.Variables)
		{
			if (!variable.Public)
			{
				continue;
			}
			bool named = overrides.contains(variable.Name);
			// The most recent former name comes last.
			for (auto formerName = variable.FormerNames.rbegin(); formerName != variable.FormerNames.rend(); ++formerName)
			{
				// The value under the name of a public variable is that variable's.
				if (publicNames.contains(*formerName))
				{
					continue;
				}
				auto const value = overrides.find(*formerName);
				if (value == overrides.end())
				{
					continue;
				}
				if (!named)
				{
					overrides.insert_or_assign(variable.Name, std::move(value->second));
					named = true;
				}
				overrides.erase(value);
			}
		}
		return overrides;
	}
}
