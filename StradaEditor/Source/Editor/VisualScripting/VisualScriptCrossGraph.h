#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptReferences.h"
#include "Strada/Core/Result.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Edits and checks of nodes that refer to other visual scripts (the "Graph" setting of Event.Send, Event.Broadcast,
// Function.Call, Variable.Get, Variable.Set and RunsGraph), and what an edit of one script does to the nodes of the others.
// Pure functions of documents, as VisualScriptEditing's operations: no ImGui, no state, any thread. The lookup of another
// graph's document (VisualScriptGraphResolver) is the format's (VisualScriptReferences.h).
namespace Strada
{
	namespace VisualScriptEditing
	{
		// The pins a cross-graph node has from the declarations of the graph it names, as the scripting runtime gives them: a
		// library function's signature (Execute and Then when it is impure), a variable's type (with the Target entity), a
		// custom event's parameters (a Send's Target, a Broadcast's Count), RunsGraph's Target and Result. Declared types, no
		// analysis. Empty when what the node names does not resolve in target: a call of a graph that is no library or of a
		// function it does not have, a variable or a custom event of the node's Name it does not have (or a library for
		// either), and a node of another type. RunsGraph's pins do not depend on the target.
		[[nodiscard]] std::vector<VisualScriptPin> MakeVisualScriptCrossGraphPins(VisualScriptNode const& node,
		                                                                          VisualScriptDocument const& target);

		// Gives a node of a cross-graph kind new settings that change its pins: each member of settings (an object) is set,
		// a null member removed (so {"Graph": null} makes a typed Send untyped again and keeps its parameters). The links of
		// the node whose pin (a split field's pin: the pin it belongs to) newPins has in that direction stay, the others are
		// removed; so do the input literals and split pins of the pins it keeps. Returns how many links it removed. The node
		// must be an Event.Send, Event.Broadcast, Function.Call, Variable.Get, Variable.Set or RunsGraph; the settings must be
		// values the format takes.
		[[nodiscard]] Result<uint32_t> RetargetNode(VisualScriptDocument& document, uint32_t node, Json settings,
		                                            std::span<VisualScriptPin const> newPins);

		// The pins a node of a cross-graph kind has with the settings it holds, as RetargetNode takes them. With a Graph
		// setting: a Send's or a Broadcast's from its own Parameters (Execute, a Send's Target, the parameters, Then, a
		// Broadcast's Count), as the scripting runtime gives them whatever the target's event says (a mismatch is a warning
		// there); the others' from the graph the resolver gives (MakeVisualScriptCrossGraphPins; none when it gives nothing).
		// Without one: a call's from the document's function, a Get's or a Set's from the document's variable or a local of
		// the function (0: the event graph) whose graph holds the node, and a sender's from its Parameters. RunsGraph always
		// has Target and Result. A node of another type has none.
		[[nodiscard]] std::vector<VisualScriptPin> MakeVisualScriptRetargetPins(VisualScriptDocument const& document, uint32_t function,
		                                                                        VisualScriptNode const& node,
		                                                                        VisualScriptGraphResolver const& resolver);
		// RetargetNode with the pins the node has once the settings are applied (MakeVisualScriptRetargetPins): what Details,
		// the quick fixes and graph.edit's Retarget do. An Event.Send or Event.Broadcast that then names a graph and a Name but
		// holds no Parameters takes those of the custom event of that name in the graph the resolver gives, when it has one
		// (FindVisualScriptEventParameters). Returns how many links it removed.
		[[nodiscard]] Result<uint32_t> RetargetVisualScriptNode(VisualScriptDocument& document, uint32_t node, Json settings,
		                                                        VisualScriptGraphResolver const& resolver);

		// Sets the document's Kind. Changes nothing else: a library's variables and event-graph nodes become errors the user
		// removes.
		[[nodiscard]] Result<void> SetDocumentKind(VisualScriptDocument& document, VisualScriptDocumentKind kind);

		// The Parameters of the target's Event.Custom of the name in its event graph (the enabled one when several have it),
		// [] when it stores none; an error naming the target's custom events when it has none of that name.
		[[nodiscard]] Result<Json> FindVisualScriptEventParameters(VisualScriptDocument const& target, std::string_view name);

		// "<name> is a function library: graphs call its functions; it runs on no entity": why a library is not attached to
		// entities, as the editor and the play run say it.
		[[nodiscard]] std::string DescribeVisualScriptLibraryOnEntity(std::string_view name);

		// What an edit of one graph does to a node of another script that refers to it.
		enum class VisualScriptReferrerProblem : uint8_t
		{
			// The function, variable or custom event the node names is gone, or the graph's Kind no longer has such elements
			// (a library made a script, for its calls; a script made a library, for remote variables, typed sends and
			// RunsGraph).
			Removed = 0,
			// A variable the node reads or writes from another graph became private.
			Private,
			// What the node names changed under it: a parameter the call links or gives a value is gone or retyped (a rename
			// recorded in FormerNames is followed, not a change), its outputs likewise, its purity; the variable's type; the
			// typed event's parameter types.
			Changed,
			// An untyped Event.Send or Event.Broadcast that sends a custom event by a name this graph no longer has: it may
			// have reached this graph and now never does. No diagnostic shows it anywhere.
			Stale
		};

		// "Removed", "Private", "Changed" or "Stale".
		[[nodiscard]] std::string_view VisualScriptReferrerProblemToString(VisualScriptReferrerProblem problem);

		struct VisualScriptReferrer
		{
			AssetHandle Graph;
			uint32_t Node = 0;
			std::string Type;
			VisualScriptReferrerProblem Problem = VisualScriptReferrerProblem::Removed;
			// "Get Health (node 12) reads Health, which is private now".
			std::string Message;

			bool operator==(VisualScriptReferrer const& other) const = default;
		};

		// Whether two documents of one graph offer other graphs the same: their Kind, their interfaces
		// (MakeVisualScriptInterface: variables and custom events, compared as HaveSameVisualScriptProgram compares them) and
		// their functions' IDs, purity and signatures with the parameters' FormerNames, in order. When they do, no other
		// script can break, and callers need not read any.
		[[nodiscard]] bool HaveSameVisualScriptOffer(VisualScriptDocument const& before, VisualScriptDocument const& after);

		// The nodes of the other scripts that the edit before -> after of graph breaks, from what the two documents offer
		// alone (no analysis): the nodes of scripts whose Graph setting names graph, where what they named worked before and
		// does not after, and the untyped senders, anywhere, of a custom event name before had and after lacks (Stale). A
		// node already broken before the edit is not reported. Entries of scripts naming graph itself, and null documents,
		// are skipped. Sorted by graph, then node.
		[[nodiscard]] std::vector<VisualScriptReferrer>
		FindVisualScriptBrokenReferrers(AssetHandle graph, VisualScriptDocument const& before, VisualScriptDocument const& after,
		                                std::span<std::pair<AssetHandle, VisualScriptDocument const*> const> scripts);
	}
}
