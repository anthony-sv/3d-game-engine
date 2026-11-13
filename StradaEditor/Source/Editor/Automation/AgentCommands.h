#pragma once

#include "Editor/Agents/AgentKit.h"
#include "Editor/Automation/CommandRegistry.h"
#include "Editor/EditorContext.h"

#include "Strada/Core/EngineLayout.h"
#include "Strada/Core/Result.h"

#include <optional>

namespace Strada
{
	// The agent kit's view of the open project: its directory, name and project file; nothing without a project.
	[[nodiscard]] std::optional<AgentProject> GetAgentProject(EditorContext const& context);

	// Registers project.agents (read-only) and project.setup-agents, which set the open project up for coding agents with
	// the agent kit of the layout (AgentKit); findTool finds the strada tool on PATH for .mcp.json. Neither is an undo
	// step. The registry and the context must outlive every registered command. Documented in Docs/Automation.md.
	[[nodiscard]] Result<void> RegisterAgentCommands(CommandRegistry& registry, EditorContext& context, EngineLayout layout,
	                                                 AgentToolLookup findTool);
}
