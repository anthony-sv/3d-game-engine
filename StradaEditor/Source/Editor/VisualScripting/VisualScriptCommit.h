#pragma once

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// How every writer of a visual script (the graph windows and graph.set) prepares the document it writes, and what the
// graph windows hand to EditorOperations::EditVisualScript. Plain functions of documents: no ImGui, no state, main thread
// or any other.
namespace Strada
{
	// A document ready to write, with what preparing it changed besides the edit.
	struct VisualScriptCommit
	{
		VisualScriptDocument Document;
		// In this order: "variable renamed: <old> → <new> (<old> is kept in FormerNames)" for each former name step 1 of
		// PrepareVisualScriptCommit recorded, then "node updated: <old> → <new>" for each node step 2 stored under its
		// canonical type.
		std::vector<std::string> Warnings;
		// How many of Warnings, from the first, are step 1's renames; the others are step 2's canonical rewrites.
		size_t RenameCount = 0;
	};

	// What every writer of a visual script does to the document it writes, in order:
	//  1. RecordFormerNames: a variable the stored document has (by ID) under another name keeps that name in FormerNames,
	//     so values components store under it keep applying (never a name the document uses already; at most
	//     VisualScriptSerializer::MaxFormerNames, the oldest dropped);
	//  2. with an analysis, ApplyCanonicalNodeTypes: nodes it resolved inexactly (a redirect, or their member's only
	//     overload) whose Type is still the analyzed one are stored under their canonical types, their renamed pins' links,
	//     inputs and split paths following;
	//  3. PlaceUnplacedNodes;
	//  4. the strict check: the document must save and load again as graph.set reads documents, with no repair and
	//     unchanged. Otherwise nothing may be written and the commit fails with "the edit would write an invalid document:
	//     <path>: <message>" (an editor bug, never user input that loaded).
	// stored is the document the file has; an empty one when it has none that loads (no variable can have been renamed). The
	// analysis is passed in, never computed here: graph.set passes a fresh one, a graph window the one it has (nodes it does
	// not describe as they are now are left as they are); without one, step 2 is skipped.
	[[nodiscard]] Result<VisualScriptCommit> PrepareVisualScriptCommit(VisualScriptDocument const& stored, VisualScriptDocument edited,
	                                                                   VisualScriptAnalysis const* analysis);

	// What the canvas, the panes and the controller hand to the panel to execute with EditorOperations::EditVisualScript.
	struct VisualScriptCommitRequest
	{
		// Prepared (PrepareVisualScriptCommit).
		Ref<VisualScriptDocument const> Document;
		// The Edit menu's text: "Move 3 Nodes in Player".
		std::string Description;
		uint64_t MergeKey = 0;
		// Elements of the shown sub-graph to select afterwards (pasted, created).
		std::vector<uint32_t> Select;
		// Shown in a toast: paste notes, canonical rewrites.
		std::vector<std::string> Notes;
	};

	// PrepareVisualScriptCommit, then "<action> in <script name>" as the description (the action alone without a name). The
	// canonical rewrites become Notes; the renames do not, since the user's own rename already says it.
	[[nodiscard]] Result<VisualScriptCommitRequest>
	MakeVisualScriptCommitRequest(VisualScriptDocument const& stored, VisualScriptDocument edited, VisualScriptAnalysis const* analysis,
	                              std::string_view action, std::string_view scriptName, uint64_t mergeKey);

	// Whether writing one document over the other would change nothing: they are the same object, or equal with every value
	// written alike (operator== and HaveSameVisualScriptProgram, so an integer is never the float of the same value).
	[[nodiscard]] bool IsSameVisualScriptDocument(VisualScriptDocument const& a, VisualScriptDocument const& b);
}
