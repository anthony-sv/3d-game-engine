#pragma once

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/StartScreen/StartScreenModel.h"

#include "Strada/Core/Result.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace Strada
{
	// The File menu's dialogs that editor.dialog opens, so that they can be looked at (viewport.screenshot).
	enum class EditorDialog : uint8_t
	{
		// File > New Project... (NewProjectDialog)
		NewProject = 0,
		// File > Set Up for Coding Agents... (AgentSetupDialog)
		AgentSetup
	};

	// "new-project", "agent-setup": the names editor.dialog takes.
	[[nodiscard]] std::string_view GetEditorDialogName(EditorDialog dialog);
	[[nodiscard]] std::optional<EditorDialog> ParseEditorDialog(std::string_view name);

	// What the start commands need from the running editor besides the start screen's model.
	struct StartCommandsHost
	{
		// Shows or hides the start screen. Unset when the editor has no window (headless), which never shows it; start.show
		// then fails with AutomationErrorCode::Unavailable.
		std::function<void(bool visible)> ShowStartScreen;
		// Opens a dialog of the File menu at the next frame as if the user chose its menu item (open true), or closes it as
		// its Cancel does (false; one that is not open stays closed). Fails, saying why, when it cannot open: agent-setup
		// without an open project, or another dialog is open. Unset when the editor has no window (headless); editor.dialog
		// then fails with AutomationErrorCode::Unavailable.
		std::function<Result<void>(EditorDialog dialog, bool open)> ShowDialog;
	};

	// The start screen as JSON: { name, path, state } of a recent project, as start.info and editor.recent list them.
	[[nodiscard]] Json DescribeRecentProjectInfo(RecentProjectInfo const& project);

	// Registers start.info (read-only), start.show, editor.recent and editor.dialog: what the start screen shows, showing and
	// hiding it, the recent projects it and the File menu list (removing one, clearing them), and opening the File menu's
	// dialogs. None is an undo step. The registry and the model must outlive every registered command. Documented in
	// Docs/Automation.md.
	[[nodiscard]] Result<void> RegisterStartCommands(CommandRegistry& registry, StartScreenModel& model, StartCommandsHost host);
}
