#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"

#include <functional>
#include <optional>

// What a visual script's nodes say about other visual scripts: the "Graph" setting that makes a node refer to another
// graph (a typed Event.Send, a library's Function.Call, a Variable.Get or Variable.Set of another entity's graph), and the
// lookup of another graph's document that editing shares across layers. Pure; any thread.
namespace Strada
{
	// The graph a node refers to: its "Graph" setting, a string holding a decimal asset handle other than "0" (the literal of
	// Strada.VisualScript). Nothing when the node has no such setting or a malformed one (the analysis reports that).
	[[nodiscard]] std::optional<AssetHandle> ReadVisualScriptGraphSetting(VisualScriptNode const& node);

	// Whether a node's Variable, Function and Name settings name elements of its own document: the node has no "Graph"
	// setting at all. A malformed one counts as a reference to another graph, so no reader takes the node's settings for
	// elements of its own document. Every editor reader of those settings checks this first.
	[[nodiscard]] bool IsLocalVisualScriptReference(VisualScriptNode const& node);

	// The document of another visual script, for titles, pins, quick fixes and name lookups; null when it does not load. A
	// caller that edits a document resolves that document's own handle to its working copy, never to the stored asset.
	// Declared with the format so that document editing, the canvas model and the batch editor share it without including
	// one another.
	using VisualScriptGraphResolver = std::function<VisualScriptDocument const*(AssetHandle graph)>;
}
