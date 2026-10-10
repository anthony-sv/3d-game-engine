#pragma once

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorScripts.h"

#include "Strada/Core/Result.h"

namespace Strada
{
	// Registers the script commands: script.status, script.classes, script.create and script.build (asynchronous). The
	// registry, the scripts and the context must outlive every registered command. Documented in Docs/Automation.md.
	[[nodiscard]] Result<void> RegisterScriptCommands(CommandRegistry& registry, EditorScripts& scripts, EditorContext& context);
}
