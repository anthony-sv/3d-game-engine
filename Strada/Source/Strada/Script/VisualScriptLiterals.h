#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <functional>
#include <optional>
#include <string_view>
#include <vector>

// Literals of visual-script documents (variables' and locals' values, the values stored for inputs, Literal nodes' values)
// as script field values, and back: the C++ side of the literal forms the scripting runtime reads and writes. Only the types
// whose values are script field values have such literals here: the exposable types and enums. The functions keep no state
// and may be called from any thread; those given an enum lookup call it on the calling thread.
namespace Strada
{
	// A type public variables can have, other than an enum: the script field type that stores its values.
	struct VisualScriptExposableType
	{
		ScriptFieldType FieldType = ScriptFieldType::None;
		// Asset references: the type of asset accepted; None accepts any asset.
		AssetType AcceptedAssetType = AssetType::None;

		bool operator==(VisualScriptExposableType const& other) const = default;
	};

	// The exposable type of a variable type ("float", "Strada.Vector3", "Strada.Material", ...); nothing for enums (only the
	// scripting runtime knows them) and for the types public variables cannot have.
	[[nodiscard]] std::optional<VisualScriptExposableType> FindVisualScriptExposableType(std::string_view typeId);

	// Whether a type that is not exposable could be an enum, which only the scripting runtime can tell: every type except
	// exec, the wildcard, Strada.Matrix4, System.Object and arrays.
	[[nodiscard]] bool CanBeVisualScriptEnum(std::string_view typeId);

	// An enum type as the scripting runtime describes it.
	struct VisualScriptEnumInfo
	{
		// What values of the enum are stored as: Int32, UInt32, Int64 or UInt64.
		ScriptFieldType UnderlyingType = ScriptFieldType::Int32;
		// In declaration order; their values have the underlying type.
		std::vector<ScriptEnumerator> Enumerators;
		// A [Flags] enum: values combine enumerators bitwise.
		bool IsFlags = false;

		// Whether it is described as the scripting runtime describes enums: of an integer type, with enumerators of that type.
		// Public variables of other enums are not exposable, and literals of them neither decode nor encode.
		[[nodiscard]] bool IsUsable() const;
	};

	// The enum type of a type ID, or nothing when the type is no enum. Empty while the scripting runtime is unavailable.
	using VisualScriptEnumLookup = std::function<std::optional<VisualScriptEnumInfo>(std::string_view typeId)>;

	// The value a literal of the type stands for: exposable types as the scripting runtime reads them (64-bit integers and
	// asset handles from decimal strings, entities only from "0", floats up to VisualScriptSerializer::MaxFloatMagnitude
	// as the nearest float, quaternions normalized and of zero length the identity), and enums as DecodeVisualScriptEnumLiteral
	// reads them. Nothing when the literal is none of the type, for enums without a lookup that knows them, and for types
	// without such literals (Strada.Matrix4, structs, classes, arrays).
	[[nodiscard]] std::optional<ScriptFieldValue> DecodeVisualScriptLiteral(Json const& literal, std::string_view typeId,
	                                                                        VisualScriptEnumLookup const& enumLookup);

	// The canonical literal of a value of the type, in the forms the scripting runtime writes: 64-bit integers and asset
	// handles as decimal strings, each float (alone or a component of a vector, quaternion or color) as the number its
	// shortest round-trip text reads as (0.2f is 0.2, never 0.20000000298023224; a float without a fraction is an integer,
	// 4.0f is 4), quaternions normalized, colors linear, and enums as EncodeVisualScriptEnumLiteral writes them. A float that
	// text would not give back bit for bit (-0, ±7.038531e-26) is written as its exact value, so every value decodes back to
	// itself. Nothing when the value has another field type than the type's, cannot be a literal (an entity other than none,
	// a number that is not finite), or is of a type without such literals.
	[[nodiscard]] std::optional<Json> EncodeVisualScriptLiteral(ScriptFieldValue const& value, std::string_view typeId,
	                                                            VisualScriptEnumLookup const& enumLookup);

	// A literal of the enum: an enumerator's name, an array of names for [Flags] enums ([] is 0), or the integer of a value
	// without an enumerator (a decimal string for 64-bit enums). Nothing for other literals and for unusable enums.
	[[nodiscard]] std::optional<ScriptFieldValue> DecodeVisualScriptEnumLiteral(Json const& literal, VisualScriptEnumInfo const& enumInfo);

	// The canonical literal of a value of the enum: the name of the first enumerator of its value; for [Flags] enums an array
	// of names: [] for 0, the one enumerator of exactly its bits, else the enumerators whose bits make it up (every one in
	// declaration order). A value no enumerator names, or no enumerators make up, is its integer (a decimal string for 64-bit
	// enums). Nothing for a value of another type than the enum's underlying type and for unusable enums.
	[[nodiscard]] std::optional<Json> EncodeVisualScriptEnumLiteral(ScriptFieldValue const& value, VisualScriptEnumInfo const& enumInfo);
}
