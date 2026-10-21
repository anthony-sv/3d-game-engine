#pragma once

#include "Strada/Asset/Asset.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Script/ScriptEngine.h"
#include "Strada/Script/ScriptField.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The public variables of a visual script as VisualScript components set them, read from the document alone: no scripting
// runtime takes part (the runtime decides what a graph means and reports what is wrong with it). The functions keep no
// state and may be called from any thread; DescribeVisualScriptVariables calls the enum lookup on the calling thread.
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

	// An enum type as the scripting runtime describes it.
	struct VisualScriptEnumInfo
	{
		// What values of the enum are stored as: Int32, UInt32, Int64 or UInt64.
		ScriptFieldType UnderlyingType = ScriptFieldType::Int32;
		// In declaration order; their values have the underlying type.
		std::vector<ScriptEnumerator> Enumerators;
		// A [Flags] enum: values combine enumerators bitwise.
		bool IsFlags = false;
	};

	// The enum type of a type ID, or nothing when the type is no enum. Empty while the scripting runtime is unavailable.
	using VisualScriptEnumLookup = std::function<std::optional<VisualScriptEnumInfo>(std::string_view typeId)>;

	// A public variable components cannot set here, and why ("needs the scripting runtime", "not exposable").
	struct VisualScriptReadOnlyVariable
	{
		std::string Name;
		std::string Type;
		std::string Reason;
	};

	struct VisualScriptVariableDescription
	{
		// The public variables components can set, as the fields of a script class named "Visual Script".
		ScriptClassInfo Editable;
		// The other public variables.
		std::vector<VisualScriptReadOnlyVariable> ReadOnly;
	};

	// The document's public variables, in document order. Those of an exposable type, and enums the lookup knows, are
	// editable: fields of their script field type with the variable's tooltip and range, the accepted asset type of typed
	// asset references, the enumerators of enums, and as default the variable's Value read as a literal of its type (enums
	// by enumerator name, quaternions normalized), or the type's default when the variable has no Value or its Value is no
	// literal of the type. The others are read-only: without a lookup any type that is not exposable may be an enum, so it
	// "needs the scripting runtime", except the types that cannot be enums (exec, wildcards, Strada.Matrix4, System.Object
	// and arrays); with a lookup, the types it does not know are "not exposable", like those.
	[[nodiscard]] VisualScriptVariableDescription DescribeVisualScriptVariables(VisualScriptDocument const& document,
	                                                                            VisualScriptEnumLookup const& enumLookup = {});

	// A VisualScript component's stored values keyed by the current names of the variables they apply to. A value applies
	// to the public variable of its name, else to the public variable that had its name before (FormerNames), so a key that
	// names no public variable but a former name of one is renamed to that variable's name. Of several values for one
	// variable, the one under its current name is kept, else the one under its most recent former name. Values are kept as
	// stored, and so are the keys that name neither a public variable nor a former name of one.
	[[nodiscard]] ScriptFieldMap NormalizeVisualScriptOverrides(VisualScriptDocument const& document, ScriptFieldMap overrides);
}
