#pragma once

#include "Editor/Commands/ComponentCommands.h"
#include "Editor/UI/FieldEditor.h"

#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Serialization/StructSerialization.h"

#include <span>
#include <string_view>
#include <vector>

// The inspector's view of a Script component's fields: the fields a script class declares, described for FieldEditor,
// and the conversion between the component's stored fields and the editor's values. Main thread only.
namespace Strada::ScriptFieldInspection
{
	// The fields of the class the inspector shows (hidden ones are left out). Enums become enum fields named by their
	// enumerators (zero-valued enumerators of [Flags] enums are left out: they set no flag), colors are colors and asset
	// references keep their asset type. The descriptors refer to the class's strings: rebuild them after the game assembly
	// is reloaded.
	std::vector<FieldDescriptor> DescribeFields(ScriptClassInfo const& scriptClass);

	// A Script component's stored fields ({ "<Name>": { "Type", "Value" } }) as FieldEditor values ({ "<Name>": value }):
	// the stored value when it has the field's type, the class's default otherwise.
	Json ToEditorValues(ScriptClassInfo const& scriptClass, Json const& storedFields);

	// The stored fields with one field set from a FieldEditor value; fails when the class has no such field or the value
	// does not fit it. Stored values of other fields are kept, including those of fields the class no longer has.
	[[nodiscard]] Result<Json> SetEditorValue(ScriptClassInfo const& scriptClass, Json storedFields, std::string_view fieldName,
	                                          Json const& value);

	// The Script component edits applying a FieldEditor change made to the first entity to every entity: each gets the
	// edited value (or vector component) in its own stored fields. components are the entities' Script components in the
	// scene-file format, editorValues their ToEditorValues. Fails when the value does not fit the field.
	[[nodiscard]] Result<std::vector<ComponentEdit>> MakeEdits(ScriptClassInfo const& scriptClass, std::span<UUID const> entities,
	                                                           std::span<Json const> components, std::span<Json const> editorValues,
	                                                           FieldChange const& change);
}
