#pragma once

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Result.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <cstdint>
#include <string>
#include <vector>

// Quick fixes of the problems the scripting runtime's analysis finds in a visual script: what the problems list and the
// badges of the visual script editor offer for a diagnostic, and the edit each makes to a copy of the document before it
// commits it. Pure functions; no ImGui. Main thread or any other.
namespace Strada
{
	enum class VisualScriptFixKind : uint8_t
	{
		// Stores the node under another type (Argument): an unknown type's suggestion, or one overload of an ambiguous one.
		ChangeNodeType = 0,
		// Names another pin (Argument) where a link or a stored literal names one the node does not have (Replaces).
		UsePin,
		// Removes a literal the node stores for an input it does not have (Replaces).
		RemoveInput,
		// Removes a setting the node's kind does not have (Replaces).
		RemoveSetting,
		// Keeps an exec output's or a data input's first link and removes the later ones.
		KeepFirstLink,
		// Removes the diagnostic's link.
		BreakLink,
		// Puts a Convert to the input's type (Argument) into the diagnostic's link.
		InsertConvert,
		// Removes a value that is not one of its type (an input's literal, a Literal's Value, a variable's Value), so the
		// default applies.
		ResetValue,
		// Moves an entity a literal cannot hold into a new public Strada.Entity variable, named after the input, whose Get is
		// linked into it: entities are set per component, in the inspector.
		CreatePublicVariable,
		// Makes a quaternion of length zero the identity.
		UseIdentity,
		// Navigates: shows the node's setting (Argument, when known) in Details.
		EditSetting,
		// Names an existing variable or function (Argument, its ID) where the node names one the document does not have.
		UseElement,
		// Names a type (Argument) where a type the library does not know or does not allow there is named (Replaces; empty
		// for a missing type argument).
		UseType,
		MakePrivate,
		// Navigates: shows the element (Target) where it is renamed.
		Rename,
		// Navigates: selects and frames the node (Target).
		GoTo,
		DeleteNode,
		// Adds a Function.Entry to the function (Target).
		AddEntry,
		// Adds a Function.Return to the function (Target).
		AddReturn,
		// Makes the function (Target) impure.
		MakeImpure,
		// Stores the node under its canonical type, with its pins renamed (ApplyCanonicalNodeTypes).
		UpdateNode,
		// Names another setting (Argument) where the node stores one its kind does not have (Replaces).
		UseSetting,
		// Removes a split pin's path (Argument) from the node's Split list, with the links and literals of its fields.
		RemoveSplit,
		// Enables a disabled node the runtime runs anyway.
		EnableNode
	};

	struct VisualScriptQuickFix
	{
		// What the button says: "Use Value", "Keep the First Link", "Make AddScore Impure".
		std::string Label;
		VisualScriptFixKind Kind = VisualScriptFixKind::ChangeNodeType;
		// The new value: a node type, pin, setting or type name, an element's ID in decimal, a setting to show, a split path.
		std::string Argument;
		// The node, variable or function the fix acts on or goes to; 0 when none.
		uint32_t Target = 0;
		// GoTo, EditSetting and Rename change nothing: the problems list navigates.
		bool Navigates = false;
		// What the fix replaces, as the document stores it: an input's key, a setting's name, a type; empty when nothing.
		std::string Replaces;

		bool operator==(VisualScriptQuickFix const& other) const = default;
	};

	// The fixes of a diagnostic of the document's analysis, in the order the problems list shows them; none for the codes
	// whose message says what to do (UnresolvedWildcard, NoOperator, NoConversion, PureCycle, PureRecursion,
	// InvalidDocument) and for diagnostics that name what the document does not hold. Suggestions (at most 3 names, or
	// every candidate of AmbiguousNode) each give a fix. The library names nodes and types, tells which settings a kind has
	// and which types convert (Insert Convert is offered for the conversions every value has, to string, and numbers have,
	// to another numeric type).
	[[nodiscard]] std::vector<VisualScriptQuickFix> GetVisualScriptQuickFixes(VisualScriptDocument const& document,
	                                                                          VisualScriptAnalysis const& analysis,
	                                                                          VisualScriptDiagnostic const& diagnostic,
	                                                                          VisualScriptLibrary const& library);

	// Applies a fix GetVisualScriptQuickFixes gave for the diagnostic to the document it was given for. Navigating fixes
	// (GoTo, EditSetting, Rename) change nothing and succeed. Fails, leaving the document unchanged, when what the fix acts on
	// is gone or the fix would make the document invalid.
	[[nodiscard]] Result<void> ApplyVisualScriptQuickFix(VisualScriptDocument& document, VisualScriptAnalysis const& analysis,
	                                                     VisualScriptDiagnostic const& diagnostic, VisualScriptQuickFix const& fix);
}
