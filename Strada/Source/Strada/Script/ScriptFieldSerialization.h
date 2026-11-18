#pragma once

#include "Strada/Script/ScriptField.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <string>

namespace Strada
{
	// Script fields serialize as { "<Name>": { "Type": "<ScriptFieldType>", "Value": <value> } }: in scene and prefab files and
	// between the engine and the scripting runtime. Reading refuses the whole map when one field is refused, and leaves the
	// map it reads into as it was then.
	template<>
	struct JsonTraits<ScriptFieldMap>
	{
		static Json ToJson(ScriptFieldMap const& fields);
		static Result<void> FromJson(Json const& json, ScriptFieldMap& out, DeserializationContext const& context);
		static std::string TypeName() { return "scriptFields"; }
	};

	// 64-bit integers, entity IDs and asset handles are decimal strings; vectors, colors and quaternions (x, y, z, w) are
	// number arrays; a list is the array of its elements' values ({ "Type": "Float[]", "Value": [1, 2.5] }). Reading refuses a
	// value of another kind, a list that is no array or holds more than MaxScriptFieldListElements elements, and a list with
	// an element its element type refuses (the error names the element's index). Any thread.
	Json ScriptFieldValueToJson(ScriptFieldValue const& value);
	[[nodiscard]] Result<ScriptFieldValue> ScriptFieldValueFromJson(ScriptFieldType type, Json const& json,
	                                                                DeserializationContext const& context);
}
