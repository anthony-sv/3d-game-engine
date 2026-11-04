#pragma once

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/EditorContext.h"
#include "Editor/PlayMode.h"

#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstdint>
#include <optional>

namespace Strada
{
	class Scene;
	class VisualScriptDebugSession;
	struct VisualScriptLiveEdit;
	struct VisualScriptStop;

	// Registers the visual-script debug commands: graph.breakpoints, graph.target, graph.debug, graph.continue, graph.step,
	// graph.break, graph.wait (asynchronous), graph.trace, graph.values and graph.instances, and the live channel's graph.live.
	// They work headless (the session exists without the editor's UI) and record no history step: breakpoints and targets are
	// view state, and resumes run the game, which play mode never records. The session, play mode and context must outlive
	// every registered command. Documented in Docs/Automation.md.
	[[nodiscard]] Result<void> RegisterGraphDebugCommands(CommandRegistry& registry, VisualScriptDebugSession& session, PlayMode& playMode,
	                                                      EditorContext& context);

	// A stop as the debug commands and play.advance describe it: the debugger's stop in camelCase, its entities with their names
	// in the scene (null for an entity it no longer has) and its graphs and documents with their paths.
	[[nodiscard]] Json DescribeVisualScriptStop(VisualScriptStop const& stop, Scene& scene);

	// The live log's last serial before a command edits a graph, for DescribeVisualScriptLiveEdits after the edit: taken only
	// while the game plays with the scripting runtime (else no graph runs, so no edit has a receipt).
	[[nodiscard]] std::optional<uint64_t> TakeVisualScriptLiveSerial(EditorContext const& context);

	// What a graph command's edit did to the running game (graph.edit's and graph.set's live): { playState, paused, edits },
	// edits being the receipts taken after serial in graph.live's form (none without a serial). Since an edit reaches the
	// running scene inside the command (EditorContext::FinishEdit), its receipts are final when the command returns.
	[[nodiscard]] Json DescribeVisualScriptLiveEdits(EditorContext& context, std::optional<uint64_t> serial);

	// A receipt in graph.live's form (graph.editors' liveReceipt): camelCase, its graphs with their paths, its instances' entities
	// with their names in the scene, and its age.
	[[nodiscard]] Json DescribeVisualScriptLiveEdit(VisualScriptLiveEdit const& edit, Scene& scene);
}
