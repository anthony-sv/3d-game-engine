#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptReferences.h"
#include "Strada/Core/Result.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <cstdint>
#include <functional>
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
		EnableNode,
		// Replaces the bytes of an Expression's formula the diagnostic's Span covers (Replaces) with a name (Argument): an
		// unknown function's suggestion. The formula is set with SetFormula, so its inputs keep their links.
		ReplaceInFormula,
		// Gives a node that can refer to another graph the settings of Argument (a JSON object: each member set, a null one
		// removed) with its pins from what they name (VisualScriptEditing::RetargetVisualScriptNode): "Send by Name" removes
		// a Send's Graph, "Use Damaged" names another event of the target with its parameters.
		Retarget,
		// Sets a Send's or a Broadcast's Parameters to those of Argument (a JSON array of { Name, Type }: the target event's)
		// with SetEventParameters, so links follow the parameters renamed in order.
		UpdateParameters,
		// Navigates: opens the other graph the diagnostic names (VisualScriptDiagnostic::Graph). Argument says what it shows
		// there: "Node" selects the node Target of its event graph (an event), "Variable" the variable Target, "Problems" opens
		// its problems drawer; empty opens it as it is.
		OpenGraph,
		// Removes the diagnostic's variable with the nodes that read or write it (a library's variable).
		DeleteVariable
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

	// The other visual scripts the nodes of a document name, for the fixes of cross-graph diagnostics (spec-slice3 §4.8,
	// §10.4): their documents, the fixed document's own handle resolving to the document fixed (never to its stored asset),
	// and the names labels give them (a file's stem, "Door"). Either may be empty: a fix needs what it reads.
	struct VisualScriptQuickFixGraphs
	{
		VisualScriptGraphResolver Resolve;
		std::function<std::string(AssetHandle graph)> GetName;
	};

	// The fixes of a diagnostic of the document's analysis, in the order the problems list shows them; none for the codes
	// whose message says what to do (UnresolvedWildcard, NoOperator, NoConversion, PureCycle, PureRecursion,
	// InvalidDocument) and for diagnostics that name what the document does not hold. Suggestions (at most 3 names, or
	// every candidate of AmbiguousNode) each give a fix. The library names nodes and types, tells which settings a kind has
	// and which types convert (Insert Convert is offered for the conversions every value has, to string, and numbers have,
	// to another numeric type). The fixes of a node with a Graph setting read that graph through graphs, never the
	// document's own elements of the IDs it names: without graphs (or when it gives no document) they are left out, but for
	// those that need no other document ("Choose a Graph", "Send by Name").
	[[nodiscard]] std::vector<VisualScriptQuickFix> GetVisualScriptQuickFixes(VisualScriptDocument const& document,
	                                                                          VisualScriptAnalysis const& analysis,
	                                                                          VisualScriptDiagnostic const& diagnostic,
	                                                                          VisualScriptLibrary const& library,
	                                                                          VisualScriptQuickFixGraphs const* graphs = nullptr);

	// Applies a fix GetVisualScriptQuickFixes gave for the diagnostic to the document it was given for, with the same graphs.
	// Navigating fixes (GoTo, EditSetting, Rename, OpenGraph) change nothing and succeed. Fails, leaving the document
	// unchanged, when what the fix acts on is gone or the fix would make the document invalid.
	[[nodiscard]] Result<void> ApplyVisualScriptQuickFix(VisualScriptDocument& document, VisualScriptAnalysis const& analysis,
	                                                     VisualScriptDiagnostic const& diagnostic, VisualScriptQuickFix const& fix,
	                                                     VisualScriptQuickFixGraphs const* graphs = nullptr);
}
