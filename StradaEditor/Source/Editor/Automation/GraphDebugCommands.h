#pragma once

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/EditorContext.h"
#include "Editor/PlayMode.h"

#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

namespace Strada
{
	class Scene;
	class VisualScriptDebugSession;
	struct VisualScriptStop;

	// Registers the visual-script debug commands: graph.breakpoints, graph.target, graph.debug, graph.continue, graph.step,
	// graph.break, graph.wait (asynchronous), graph.trace, graph.values and graph.instances. They work headless (the session
	// exists without the editor's UI) and record no history step: breakpoints and targets are view state, and resumes run the
	// game, which play mode never records. The session, play mode and context must outlive every registered command.
	// Documented in Docs/Automation.md.
	[[nodiscard]] Result<void> RegisterGraphDebugCommands(CommandRegistry& registry, VisualScriptDebugSession& session, PlayMode& playMode,
	                                                      EditorContext& context);

	// A stop as the debug commands and play.advance describe it: the debugger's stop in camelCase, its entities with their names
	// in the scene (null for an entity it no longer has) and its graphs and documents with their paths.
	[[nodiscard]] Json DescribeVisualScriptStop(VisualScriptStop const& stop, Scene& scene);
}
