#pragma once

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/StartScreen/StartScreenModel.h"

#include "Strada/Core/Result.h"

#include <functional>

namespace Strada
{
	// What the start commands need from the running editor besides the start screen's model.
	struct StartCommandsHost
	{
		// Shows or hides the start screen. Unset when the editor has no window (headless), which never shows it; start.show
		// then fails with AutomationErrorCode::Unavailable.
		std::function<void(bool visible)> ShowStartScreen;
	};

	// The start screen as JSON: { name, path, state } of a recent project, as start.info and editor.recent list them.
	[[nodiscard]] Json DescribeRecentProjectInfo(RecentProjectInfo const& project);

	// Registers start.info (read-only), start.show and editor.recent: what the start screen shows, showing and hiding it, and
	// the recent projects it and the File menu list (removing one, clearing them). None is an undo step. The registry and the
	// model must outlive every registered command. Documented in Docs/Automation.md.
	[[nodiscard]] Result<void> RegisterStartCommands(CommandRegistry& registry, StartScreenModel& model, StartCommandsHost host);
}
