#pragma once

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorScripts.h"

#include "Strada/Core/Result.h"

namespace Strada
{
	// A script build's error or warning as results show it: { severity, file, line, column, code, message }.
	Json DescribeScriptDiagnostic(ScriptDiagnostic const& diagnostic);

	// Registers the script commands: script.status, script.classes, script.create and script.build (asynchronous). The
	// registry, the scripts and the context must outlive every registered command. Documented in Docs/Automation.md.
	[[nodiscard]] Result<void> RegisterScriptCommands(CommandRegistry& registry, EditorScripts& scripts, EditorContext& context);
}
