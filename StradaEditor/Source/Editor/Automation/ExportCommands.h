#pragma once

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorExport.h"

#include "Strada/Core/Result.h"

namespace Strada
{
	// Registers project.export (asynchronous). The registry, the export and the context must outlive every registered
	// command. Documented in Docs/Automation.md.
	[[nodiscard]] Result<void> RegisterExportCommands(CommandRegistry& registry, EditorExport& gameExport, EditorContext& context);
}
