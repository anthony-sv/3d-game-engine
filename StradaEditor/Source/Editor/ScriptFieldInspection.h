#pragma once

#include "Editor/Commands/ComponentCommands.h"
#include "Editor/UI/FieldEditor.h"

#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Scene/ComponentTraits.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Serialization/StructSerialization.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The inspector's view of a Script component's fields: the fields a script class declares, described for FieldEditor,
// and the conversion between the component's stored fields and the editor's values. The same serves every map of script
// fields a component stores, such as the values a VisualScript component gives the public variables of its graph (described
// as the fields of a class). Main thread only.
namespace Strada::ScriptFieldInspection
{
	// The fields of the class the inspector shows (hidden ones are left out). Enums become enum fields named by their
	// enumerators (zero-valued enumerators of [Flags] enums are left out: they set no flag), colors are colors and asset
	// references keep their asset type. The descriptors refer to the class's strings: rebuild them after the game assembly
	// is reloaded.
	std::vector<FieldDescriptor> DescribeFields(ScriptClassInfo const& scriptClass);

	// A component's stored fields ({ "<Name>": { "Type", "Value" } }, a Script component's Fields) as FieldEditor values
	// ({ "<Name>": value }): the stored value when it has the field's type, the class's default otherwise.
	Json ToEditorValues(ScriptClassInfo const& scriptClass, Json const& storedFields);

	// One object's stored fields as FieldEditor values, kept from one draw to the next so that a draw converts again only what
	// changed (Docs/Architecture.md §11, "Long lists"): Update gives `values` what ToEditorValues gives for the class and the
	// stored fields (in their file form), converting a field again only when its stored value, or what the class says of it
	// (its type, its enumerators and their kind, its default where no stored value has its type), differs from what it last
	// converted, the values compared as the runtime's read-back compares them (floating-point numbers bit for bit, so a list
	// holding a NaN that stays is not converted again). A class whose shown fields are others than last time (names or order)
	// starts over.
	class EditorValueCache
	{
	public:
		// Returns how many fields it converted.
		size_t Update(ScriptClassInfo const& scriptClass, ScriptFieldMap const& storedFields, Json& values);

	private:
		// What a shown field's value was last converted from.
		struct ConvertedField
		{
			std::string Name;
			ScriptFieldType Type = ScriptFieldType::None;
			std::vector<ScriptEnumerator> Enumerators;
			bool IsFlags = false;
			// The stored value when one had the field's type, the class's default otherwise.
			bool FromStored = false;
			ScriptFieldValue Source;
		};

		std::vector<ConvertedField> m_Fields;
	};

	// The stored fields with one field set from a FieldEditor value; fails when the class has no such field or the value
	// does not fit it. Stored values of other fields are kept, including those of fields the class no longer has.
	[[nodiscard]] Result<Json> SetEditorValue(ScriptClassInfo const& scriptClass, Json storedFields, std::string_view fieldName,
	                                          Json const& value);

	// The component edits applying a FieldEditor change made to the first entity to every entity: each gets the edited value
	// (or vector component) in its own stored fields, and each edit writes that entity's whole map of stored fields under
	// fieldsKey. components are the entities' components (named componentName) in the scene-file format, editorValues the
	// ToEditorValues of their maps. Fails when the value does not fit the field.
	[[nodiscard]] Result<std::vector<ComponentEdit>> MakeEdits(ScriptClassInfo const& scriptClass, std::span<UUID const> entities,
	                                                           std::span<Json const> components, std::span<Json const> editorValues,
	                                                           FieldChange const& change,
	                                                           std::string_view componentName = ComponentTraits<ScriptComponent>::Name,
	                                                           std::string_view fieldsKey = "Fields");
}
