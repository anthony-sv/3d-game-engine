#pragma once

#include "Strada/Script/ScriptField.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <string>

namespace Strada
{
	// Script fields serialize as { "<Name>": { "Type": "<ScriptFieldType>", "Value": <value> } }: in scene files and between
	// the engine and the scripting runtime.
	template<>
	struct JsonTraits<ScriptFieldMap>
	{
		static Json ToJson(ScriptFieldMap const& fields);
		static Result<void> FromJson(Json const& json, ScriptFieldMap& out, DeserializationContext const& context);
		static std::string TypeName() { return "scriptFields"; }
	};

	// 64-bit integers, entity IDs and asset handles are decimal strings; vectors, colors and quaternions (x, y, z, w) are
	// number arrays.
	Json ScriptFieldValueToJson(ScriptFieldValue const& value);
	[[nodiscard]] Result<ScriptFieldValue> ScriptFieldValueFromJson(ScriptFieldType type, Json const& json,
	                                                                DeserializationContext const& context);
}
