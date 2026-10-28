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
	// What a commit spent, in milliseconds (steady_clock), phase by phase: filled by PrepareVisualScriptCommit (the first
	// three), SetVisualScriptCommand (the next four) and VisualScriptEditorModel::Sync (the last four); read by the
	// performance tests. A phase that did not run is 0.
	struct VisualScriptCommitTimings
	{
		// RecordFormerNames, ApplyCanonicalNodeTypes, PlaceUnplacedNodes.
		double Normalize = 0.0;
		// The strict check: VisualScriptSerializer::Validate.
		double Check = 0.0;
		// The file's text: VisualScriptSerializer::WriteText (SetVisualScriptCommand's own when it was given no text).
		double Text = 0.0;
		// RecordBefore: the file's bytes and their comparison with the remembered text.
		double ReadBack = 0.0;
		// HasEffect's IsSameVisualScriptDocument.
		double Compare = 0.0;
		// FileSystem::WriteTextFile.
		double Write = 0.0;
		// The asset, RememberVisualScriptText, RecordFileWrite.
		double SetDocument = 0.0;
		// The model's AdoptDocument (RecordCommittedLinks, the display copy).
		double Adopt = 0.0;
		// RefreshAnalysis: the check against the analyzed document and, when the program changed, the analysis (the text,
		// the runtime, the parse).
		double Analysis = 0.0;
		// VisualScriptCanvasModel::Sync.
		double Canvas = 0.0;
		// VisualScriptSpatialIndex::Build.
		double Index = 0.0;
	};

	// A document ready to write, with what preparing it changed besides the edit.
	struct VisualScriptCommit
	{
		VisualScriptDocument Document;
		// The canonical text of Document's file (VisualScriptSerializer::WriteText(Document)), written once the strict check
		// passed: writers hand it to EditorOperations::EditVisualScript, so a commit writes its document's text once.
		std::string Text;
		// In this order: "variable renamed: <old> → <new> (<old> is kept in FormerNames)" for each former name of a variable
		// and "parameter renamed: <function>'s <old> → <new> (<old> is kept in FormerNames)" for each of a library's
		// parameter step 1 of PrepareVisualScriptCommit recorded, then "node updated: <old> → <new>" for each node step 2
		// stored under its canonical type and "pin renamed: <title> (node <ID>) <old> → <new>" for each pin it renamed on a
		// node it kept.
		std::vector<std::string> Warnings;
		// How many of Warnings, from the first, are step 1's renames; the others are step 2's canonical rewrites.
		size_t RenameCount = 0;
		// Normalize (steps 1 to 3), Check (step 4) and Text.
		VisualScriptCommitTimings Timings;
	};

	// What every writer of a visual script does to the document it writes, in order:
	//  1. RecordFormerNames: a variable the stored document has (by ID) under another name keeps that name in FormerNames,
	//     so values components store under it keep applying (never a name the document uses already; at most
	//     VisualScriptSerializer::MaxFormerNames, the oldest dropped); in a library, a parameter renamed by
	//     SetFunctionSignature's in-order rule keeps its stored name the same way, so calls in other files keep working;
	//  2. with an analysis, ApplyCanonicalNodeTypes: nodes it resolved inexactly (a redirect, or their member's only
	//     overload) whose Type is still the analyzed one are stored under their canonical types, their renamed pins' links,
	//     inputs and split paths following; nodes it resolved exactly that stored has with the same Type and Settings keep
	//     their Type and have the pins it renamed (a former parameter name of a library function) follow the same way;
	//  3. PlaceUnplacedNodes;
	//  4. the strict check, VisualScriptSerializer::Validate: the file to write must load again as graph.set reads documents,
	//     with no repair and unchanged. Otherwise nothing may be written and the commit fails with "the edit would write an
	//     invalid document: <path>: <message>" (an editor bug, never user input that loaded); then its text, WriteText.
	// stored is the document from before the undo step the commit belongs to: the document the file has, except for a commit
	// that merges into the step of earlier ones (the same graph and merge key), which passes the stored document of the
	// step's first commit. So a variable added and then given its first name in one step (Add Variable) is new to stored when
	// its name is committed, and the name it was added under never becomes a former name. An empty document when the file
	// has none that loads (no variable can have been renamed). The analysis is passed in, never computed here: graph.set
	// passes a fresh one, a graph window the one it has (nodes it does not describe as they are now are left as they are);
	// without one, step 2 is skipped.
	[[nodiscard]] Result<VisualScriptCommit> PrepareVisualScriptCommit(VisualScriptDocument const& stored, VisualScriptDocument edited,
	                                                                   VisualScriptAnalysis const* analysis);

	// What the canvas, the panes and the controller hand to the panel to execute with EditorOperations::EditVisualScript.
	struct VisualScriptCommitRequest
	{
		// Prepared (PrepareVisualScriptCommit).
		Ref<VisualScriptDocument const> Document;
		// The canonical text of Document's file (VisualScriptCommit::Text), for EditVisualScript to write as it is.
		std::string Text;
		// The Edit menu's text: "Move 3 Nodes in Player".
		std::string Description;
		uint64_t MergeKey = 0;
		// Elements of the shown sub-graph to select afterwards (pasted, created).
		std::vector<uint32_t> Select;
		// Shown in a toast: paste notes, canonical rewrites.
		std::vector<std::string> Notes;
		// The edit changed the layout only, and preparing it rewrote nothing: Document has the program of the document it was
		// made from (HaveSameVisualScriptProgram). Set by the controller for its layout intents (a move, Arrange, Align,
		// Distribute, Straighten, a comment box's geometry or colour, collapsing), never by the panes; the window then adopts
		// the commit without analyzing it again, giving the palette the document again or laying out more than moved.
		bool LayoutOnly = false;
		// VisualScriptCommit::Timings of the preparation.
		VisualScriptCommitTimings Timings;
	};

	// PrepareVisualScriptCommit (stored as it says: the document from before the step, for each commit of a step), then
	// "<action> in <script name>" as the description (the action alone without a name). The canonical rewrites become Notes;
	// the renames do not, since the user's own rename already says it.
	[[nodiscard]] Result<VisualScriptCommitRequest>
	MakeVisualScriptCommitRequest(VisualScriptDocument const& stored, VisualScriptDocument edited, VisualScriptAnalysis const* analysis,
	                              std::string_view action, std::string_view scriptName, uint64_t mergeKey);

	// Whether writing one document over the other would change nothing: they are the same object, or equal with every value
	// written alike (operator== and HaveSameVisualScriptProgram, so an integer is never the float of the same value).
	[[nodiscard]] bool IsSameVisualScriptDocument(VisualScriptDocument const& a, VisualScriptDocument const& b);
}
