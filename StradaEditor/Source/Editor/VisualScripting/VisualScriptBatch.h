#pragma once

#include "Editor/VisualScripting/VisualScriptEditing.h"
#include "Editor/VisualScripting/VisualScriptQuickFixes.h"
#include "Editor/VisualScripting/VisualScriptSemantics.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptReferences.h"
#include "Strada/Core/Base.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// graph.edit's executor: an ordered batch of editing operations applied to a copy of a visual script's document, each
// through the editor's own pure function (VisualScriptEditing, the clipboard, the quick fixes, ArrangeNodes), with the pins
// and node sizes of a headless canvas (VisualScriptHeadlessCanvas) and an analysis made again only when an operation needs
// pins that earlier operations may have changed. Layer L3b: no ImGui. Main thread only (it calls the semantics seam).
//
// Operations are JSON objects { "op": "<Kind>", ... } whose fields are camelCase; values inside settings, literals and value
// keep the document's PascalCase. References: an integer names an element of the document as the batch found it; "$label"
// names an element an earlier operation created with "as" (or "labels"); variables, locals and functions may also be named.
namespace Strada
{
	// How Arrange, Align, Distribute and comment framing size nodes.
	enum class VisualScriptExtentsMode : uint8_t
	{
		// A headless canvas with the estimating measurer: heights as a window's, widths at least a window's.
		Measured = 0,
		// GetDefaultVisualScriptNodeExtents: the pitches of PlaceUnplacedNodes.
		Uniform
	};

	// The pins an operation that needs no anchors read from a sub-graph's canvas (every anchor NaN), with what a canvas synced
	// now would be synced with: for tests that compare them with a fresh canvas's (VisualScriptBatchContext::InspectRows).
	struct VisualScriptBatchRows
	{
		uint32_t Function = 0;
		VisualScriptNodePins const& Rows;
		// The working document as the operation finds it.
		VisualScriptDocument const& Document;
		// The analysis the batch reads; null without one.
		VisualScriptAnalysis const* Analysis = nullptr;
		// The document Analysis was made from; null when Analysis describes every node of Document's pins as they are (no pin
		// changed since it was made), so that Document stands for it.
		VisualScriptDocument const* AnalyzedDocument = nullptr;
	};

	// What a batch needs besides the document. Main thread (Semantics calls into the scripting runtime).
	struct VisualScriptBatchContext
	{
		explicit VisualScriptBatchContext(VisualScriptSemantics& semantics)
			: Semantics(semantics)
		{
		}

		VisualScriptSemantics& Semantics;
		// The script edited (asset literals, the clipboard's binding, the analysis' self); invalid in tests without one.
		AssetHandle Graph;
		// The default sub-graph of AddNode, AddComment, Paste and Arrange: 0 for the event graph, else a function's ID.
		uint32_t Function = 0;
		// "asset://..." and "builtin://..." to a handle, nothing for a reference that names no asset; empty: such strings stay
		// as they are.
		std::function<std::optional<AssetHandle>(std::string_view)> FindAsset;
		// The document of another visual script; empty: settings with a Graph keep names unresolved (an error). The executor
		// answers Graph itself with the working document, never this resolver.
		VisualScriptGraphResolver FindDocument;
		// The name the labels of quick fixes give another visual script (its file's stem, "Door"); empty: its handle.
		std::function<std::string(AssetHandle graph)> FindGraphName;
		// The analysis of the stored document when the caller has one (a cache), and the document it was made from: used only
		// when that is the stored document.
		Ref<VisualScriptAnalysis const> StoredAnalysis;
		Ref<VisualScriptDocument const> StoredAnalysisDocument;
		// Tests: called with the rows each operation that reads pins without anchors takes, before it applies; empty calls
		// nothing.
		std::function<void(VisualScriptBatchRows const& rows)> InspectRows;
	};

	enum class VisualScriptBatchFailure : uint8_t
	{
		// An operation that does not apply to the document as the earlier operations left it (InvalidParams).
		Invalid = 0,
		// An operation that needs the scripting runtime, which is not running (Unavailable).
		NeedsRuntime,
		// An operation that needs pins, and the scripting runtime failed to analyze the document (Unavailable).
		AnalysisFailed,
		// An "asset://" or "builtin://" value that names no asset (AssetNotFound).
		AssetNotFound
	};

	struct VisualScriptBatchError
	{
		VisualScriptBatchFailure Failure = VisualScriptBatchFailure::Invalid;
		// The index of the failing operation.
		size_t Operation = 0;
		// Its kind ("Connect"); empty when the operation names no kind the batch takes.
		std::string Kind;
		// Where in the parameters: "operations[3].from[1]".
		std::string Path;
		// "operations[3] (Connect): node 12 has no output Then".
		std::string Message;
		// What would have been right: { pins } (the node's rows), { labels, names }, { names } (the closest three) or
		// { candidates } (the diagnostics a Fix matches); null when nothing helps.
		Json Data;
	};

	struct VisualScriptBatchResult
	{
		// Edited, not prepared (PrepareVisualScriptCommit).
		VisualScriptDocument Document;
		// Every label and the ID of what it names, in definition order.
		std::vector<std::pair<std::string, uint32_t>> Labels;
		// One object per operation.
		std::vector<Json> Results;
		// Every element (node, comment box, variable, local, function) created, in order.
		std::vector<uint32_t> Created;
		// Of Document when the last analysis still describes it (no change to the program since); else null.
		Ref<VisualScriptAnalysis const> Analysis;
		// How many analyses the batch made.
		uint32_t Analyses = 0;
		// How many copies of the working document its analyses and canvases took: at most one per canvas sync, shared by the
		// canvases of every sub-graph.
		uint32_t Displays = 0;
		// How many times its canvases laid out a sub-graph: a canvas lays out again only when the pins (for the operations that
		// read rows) or the nodes' sizes and anchors (for those that read geometry) may have changed since (spec-slice4 §9.3).
		uint32_t CanvasSyncs = 0;
	};

	// The operation kinds a batch takes, in the order graph.edit's schema lists them.
	[[nodiscard]] std::span<std::string_view const> GetVisualScriptBatchOperations();

	// Applies the operations (an array of objects already checked against graph.edit's schema) to a copy of the document, in
	// order. On failure the error names the operation; the document passed in is never changed.
	[[nodiscard]] std::variant<VisualScriptBatchResult, VisualScriptBatchError>
	ApplyVisualScriptBatch(VisualScriptDocument const& stored, Json const& operations, VisualScriptBatchContext const& context);

	// The quick fixes of a diagnostic of the document's analysis that change the document (GetVisualScriptQuickFixes without
	// the navigating ones, with the other graphs the document's nodes name), each { "label", "operation" }: the complete Fix
	// operation that applies it, with the diagnostic's locating fields, which a later batch takes unchanged.
	[[nodiscard]] Json GetVisualScriptFixOperations(VisualScriptDocument const& document, VisualScriptAnalysis const& analysis,
	                                                VisualScriptDiagnostic const& diagnostic, VisualScriptLibrary const& library,
	                                                VisualScriptQuickFixGraphs const* graphs = nullptr);

	// A silent mistake the analysis does not report, since each is normal while a user builds a graph.
	struct VisualScriptHint
	{
		uint32_t Node = 0;
		// "NeverRuns": an impure node, not an event or a function's entry, with no link into an exec input. "Unused": a pure
		// node with no link from an output. "UnlinkedInput": an Expression input with neither a link nor a stored literal.
		std::string Kind;
		// "Set Euler Angles (node 9) has no flow into Execute: it never runs".
		std::string Message;

		bool operator==(VisualScriptHint const& other) const = default;
	};

	// The links of before that after lacks, in before's order: in time linear in the two lists, as graphs hold up to
	// MaxLinksPerGraph links.
	[[nodiscard]] std::vector<VisualScriptLink> FindVisualScriptLinksGone(std::span<VisualScriptLink const> before,
	                                                                      std::span<VisualScriptLink const> after);

	// The hints of the given nodes (in their order, each node's hints together), from the document's analysis.
	[[nodiscard]] std::vector<VisualScriptHint>
	FindVisualScriptHints(VisualScriptDocument const& document, VisualScriptAnalysis const& analysis, std::span<uint32_t const> nodes);
}
