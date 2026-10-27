#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Result.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Copy, paste and duplicate of the visual script editor. The clipboard holds text, versioned JSON whose payload is a
// visual script of the content form:
//   { "Strada": { "Version": 1, "Type": "VisualScriptClipboard" },
//     "VisualScriptClipboard": { "Source": { "Graph": "<handle>", "Function": <id>, "Locals": [ { "ID": <id>, "Name": "<name>" } ] },
//                                "Document": { <content> } } }
// so the format's reader checks what comes back with every rule and limit of documents. Its graph holds the copied nodes,
// the links between them and the copied comment boxes; its variables and functions those the copied nodes use. Pure
// functions of their arguments, no ImGui; main thread or any other.
namespace Strada
{
	// A local of the function copied from, written to the clipboard as a variable of its own (a flattened local): the
	// variable's ID and the local's own name, which pasting into a function matches.
	struct VisualScriptClipboardLocal
	{
		uint32_t ID = 0;
		std::string Name;

		bool operator==(VisualScriptClipboardLocal const& other) const = default;
	};

	// What a clipboard text holds.
	struct VisualScriptClipboardContent
	{
		// The visual script copied from; invalid when none.
		AssetHandle Source;
		// The graph copied from: 0 for the event graph, else a function's ID.
		uint32_t Function = 0;
		std::vector<VisualScriptClipboardLocal> Locals;
		// Its graph holds what was copied, placed with the top-left corner of its bounds at (0, 0).
		VisualScriptDocument Document;

		bool operator==(VisualScriptClipboardContent const& other) const = default;
	};

	// What a paste added and what it tells the user.
	struct VisualScriptPasteResult
	{
		// The pasted nodes, then the pasted comment boxes: the selection after the paste.
		std::vector<uint32_t> Pasted;
		// The variables, locals and functions it added ("added variable Score (int)"), and what it left out.
		std::vector<std::string> Notes;
		uint32_t NodeCount = 0;
	};

	// Texts longer than this are not read: no copy of the editor's makes one near it.
	inline constexpr size_t MaxVisualScriptClipboardBytes = 16 * 1024 * 1024;

	// The clipboard text of nodes and comment boxes of a graph (0 for the event graph, else a function's ID); IDs that are
	// neither are left out. The text holds, renumbered above the document's largest ID:
	// - the nodes and comment boxes, where the canvas shows them (unplaced nodes where PlaceUnplacedNodes puts them), with
	//   the top-left corner of their bounds at (0, 0), and the links between the nodes;
	// - the variables the nodes use (without their former names), and the functions they call, transitively, with their
	//   graphs and the variables those use. A setting names a variable or function when its library setting is of that
	//   kind; without the library, the Variable setting of Variable.Get and Variable.Set and the Function setting of
	//   Function.Call do;
	// - copied from a function's graph, the function's locals the nodes use, each written as a variable of a new ID with a
	//   name of its own among the text's variables and its functions' locals, and listed with its own name in Locals.
	[[nodiscard]] std::string WriteVisualScriptClipboard(VisualScriptDocument const& document, uint32_t function,
	                                                     std::span<uint32_t const> elements, AssetHandle source,
	                                                     VisualScriptLibrary const* library);

	// Whether a text may be a visual script clipboard, cheaply enough to enable Paste every frame: after white space it
	// starts with '{', and its first 256 bytes hold "VisualScriptClipboard". Never parses.
	[[nodiscard]] bool MayHoldVisualScriptClipboard(std::string_view text);

	// Reads a clipboard text, which comes from anywhere and is never trusted. In order: a text longer than
	// MaxVisualScriptClipboardBytes, one ParseJson refuses, one without the header of a clipboard of version 1, one with a
	// key it does not know (in the header, the payload, its Source and Locals), a Source whose Graph is not a handle in
	// decimal, whose Function is not an ID or 0, or whose Locals name no variable of the document or name it twice or have
	// a name a variable cannot have, and a Document the format's reader repairs anything in (each repair, an unknown key
	// included) are refused, with "the clipboard holds no visual script: <reason>".
	[[nodiscard]] Result<VisualScriptClipboardContent> ReadVisualScriptClipboard(std::string_view text);

	// Pastes a clipboard's content into a graph of a document (0 for the event graph, else a function's ID); targetGraph is
	// the document's visual script, invalid when it has none. The top-left corner of the content's bounds lands at `at`;
	// the nodes and comment boxes take new IDs (the document's largest plus one upward) and the links between them come
	// with them. What the nodes use is bound to the document:
	// - a variable: pasted into the visual script copied from, the variable of the same ID, name and type; else a variable
	//   of the same name and type; else a new variable copied from it (no former names, a name of its own);
	// - a flattened local (by its own name): into a function's graph, the function's local of that name and type, else a
	//   new local; into the event graph, a variable as above;
	// - a function: pasted into the visual script copied from, the function of the same ID, name and signature (Pure, and
	//   the names and types of its inputs and outputs, which name the pins of its calls); else a function of the same name
	//   and signature; else a new function copied from it with its locals and graph, whose own uses are bound the same way;
	// - a setting that names nothing the content holds stays as it is.
	// Each variable, local and function added is a note. Left out, with a note: event nodes pasted into a function's graph
	// (events belong in the event graph), event nodes the event graph has already (one of each standard event, of each
	// input event per key or button, of each custom event per name), and a Function.Entry pasted into a graph that has one;
	// links of what is left out go too. Fails, changing nothing, when the paste would exceed a limit of the format (naming
	// it) or `at` is not finite, and for a graph the document does not have.
	[[nodiscard]] Result<VisualScriptPasteResult> PasteVisualScriptClipboard(VisualScriptDocument& target, AssetHandle targetGraph,
	                                                                         uint32_t function, VisualScriptClipboardContent const& content,
	                                                                         glm::vec2 at, VisualScriptLibrary const* library);

	// Duplicate: pastes a copy of nodes and comment boxes of a graph into the same graph, 32 right of and 32 below where the
	// canvas shows them, without a clipboard text (graph is the document's visual script, invalid when none). What the copy
	// uses binds to what the originals use.
	[[nodiscard]] Result<VisualScriptPasteResult> DuplicateVisualScriptElements(VisualScriptDocument& document, AssetHandle graph,
	                                                                            uint32_t function, std::span<uint32_t const> elements,
	                                                                            VisualScriptLibrary const* library);
}
