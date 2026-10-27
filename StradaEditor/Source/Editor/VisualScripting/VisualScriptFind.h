#pragma once

#include "Editor/VisualScripting/VisualScriptEditorTypes.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Find in a visual script: the find bar and the find drawer of the visual script editor, and graph.find. A pure function of
// the document; no ImGui. Main thread or any other.
namespace Strada
{
	// What a find result matched.
	enum class VisualScriptFindField : uint8_t
	{
		// A node's title (the analysis', else the one the document gives it).
		Title = 0,
		// A node's subtitle (with an analysis).
		Subtitle,
		// A node's type string.
		Type,
		NodeComment,
		// A label of a node's pins (with an analysis).
		PinLabel,
		// A node's stored literal (its compact JSON) or a text setting (a Format).
		Value,
		// A comment box's text.
		CommentText,
		// A variable's or a local's name.
		VariableName,
		FunctionName,
		// A custom event's name, on an Event.Custom or an Event.Send.
		EventName
	};

	// Exactly one of the two: a text to look for, or the element whose references to list.
	struct VisualScriptFindQuery
	{
		std::string Text;
		std::optional<VisualScriptReference> References;
	};

	struct VisualScriptFindResult
	{
		// The graph: 0 for the event graph (and the document's variables), else a function's ID.
		uint32_t Function = 0;
		// The node matched; 0 when the result is no node.
		uint32_t Node = 0;
		// The comment box matched; 0 when none.
		uint32_t Comment = 0;
		// The variable or local matched; 0 when none.
		uint32_t Variable = 0;
		VisualScriptFindField Field = VisualScriptFindField::Title;
		// The matched text as shown: the title, the comment's text, the value's JSON, the name.
		std::string Text;

		bool operator==(VisualScriptFindResult const& other) const = default;
	};

	// Text: every element whose title, subtitle, type, node comment, pin label, literal or text setting, comment text or name
	// holds the text (ASCII letters in any case), one result per element and field (the first literal or pin label that holds
	// it). References: the Variable.Get and Variable.Set nodes that name a variable or local; the Function.Call nodes that call
	// a function; the Event.Custom and Event.Send nodes of a custom event's name, as Title results. Results come in document
	// order: the event graph (the document's variables, then its nodes, then its comment boxes), then each function in order
	// (its name, its locals, its nodes, its comment boxes). A query with neither or both, or an empty text, finds nothing.
	// Without an analysis titles are derived from the document (DeriveVisualScriptNodeTitle), and subtitles and pin labels
	// are not searched.
	[[nodiscard]] std::vector<VisualScriptFindResult>
	FindInVisualScript(VisualScriptDocument const& document, VisualScriptAnalysis const* analysis, VisualScriptFindQuery const& query);
}
