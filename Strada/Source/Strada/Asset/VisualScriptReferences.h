#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

// What a visual script's nodes say about other visual scripts: the "Graph" setting that makes a node refer to another
// graph (a typed Event.Send or Event.Broadcast, a library's Function.Call, a Variable.Get or Variable.Set of another
// entity's graph, RunsGraph), the references a document makes, what other graphs read of a document (its interface), and
// the lookup of another graph's document that editing shares across layers. Pure; any thread.
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

	// What a document's program needs of a graph it refers to, weakest first: a graph referred to several ways takes the
	// strongest use.
	enum class VisualScriptReferenceUse : uint8_t
	{
		// Event.Send and Event.Broadcast: the analysis checks the node against the graph's events; the program needs nothing
		// of it (the runtime matches the event by name and receivers by handle).
		Verification = 0,
		// Variable.Get, Variable.Set and RunsGraph: the program depends on the graph's Kind and Variables (its link interface).
		Interface,
		// Function.Call: the graph is a function library whose functions are compiled into the program.
		Library
	};

	// A graph a document's nodes refer to.
	struct VisualScriptGraphReference
	{
		AssetHandle Graph;
		VisualScriptReferenceUse Use = VisualScriptReferenceUse::Verification;
		// How many of the document's nodes name it.
		uint32_t Nodes = 0;

		bool operator==(VisualScriptGraphReference const& other) const = default;
	};

	// Every graph the document's nodes refer to through a valid Graph setting (in the event graph and every function graph,
	// disabled nodes included), once, ascending by handle, with the strongest use: Function.Call is a Library use;
	// Variable.Get, Variable.Set and RunsGraph are Interface uses; Event.Send and Event.Broadcast are Verification uses. A
	// Graph setting on a node of any other type refers to nothing. A graph that names the document itself is listed like
	// any other.
	[[nodiscard]] std::vector<VisualScriptGraphReference> CollectVisualScriptReferences(VisualScriptDocument const& document);

	// What other graphs read of a document: its Kind, every member of its Variables, and the event graph's Event.Custom nodes
	// (ID, Type, Settings and Enabled), without their positions, inputs or any other node, link, comment or function.
	[[nodiscard]] VisualScriptDocument MakeVisualScriptInterface(VisualScriptDocument const& document);

	// Whether programs that refer to the two documents through an Interface use are the same: their Kind and Variables
	// alike as HaveSameVisualScriptProgram compares them (representation of values included). Their events, functions and
	// layout do not matter.
	[[nodiscard]] bool HaveSameVisualScriptLinkInterface(VisualScriptDocument const& a, VisualScriptDocument const& b);
}
