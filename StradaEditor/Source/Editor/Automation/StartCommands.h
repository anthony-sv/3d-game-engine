#pragma once

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/StartScreen/StartScreenModel.h"

#include "Strada/Core/Result.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
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

	// The editor's modal dialogs as editor.dialog finds them when it is asked to open one (EditorLayer gathers it).
	struct EditorDialogState
	{
		// The name of the modal popup open now, as the last frame left it (ImGui::GetTopMostPopupModal); empty when none.
		std::string OpenModal;
		// The New Project and Set Up for Coding Agents dialogs were asked to open at the next frame.
		bool NewProjectRequested = false;
		bool AgentSetupRequested = false;
		// Another of the editor's dialogs (Unsaved Changes, New Script) was asked to open at the next frame.
		bool OtherRequested = false;
		// A project is open (Set Up for Coding Agents sets it up).
		bool HasProject = false;
	};

	// What opening a dialog takes (DecideEditorDialogOpen).
	enum class EditorDialogOpening : uint8_t
	{
		// It is not shown: open it at the next frame.
		OpenAtNextFrame = 0,
		// It is shown already: keep it as it is, dropping a close asked for since the last frame (its form is not reset).
		KeepShown
	};

	// editor.dialog {open: true}: whether the dialog opens or stays as it is shown, or why it cannot (another dialog is open
	// or opens at the next frame; Set Up for Coding Agents without an open project). The popups are named by
	// NewProjectDialog::PopupName and AgentSetupDialog::PopupName. Pure.
	[[nodiscard]] Result<EditorDialogOpening> DecideEditorDialogOpen(EditorDialog dialog, EditorDialogState const& state);

	// What the start commands need from the running editor besides the start screen's model.
	struct StartCommandsHost
	{
		// Shows or hides the start screen. Unset when the editor has no window (headless), which never shows it; start.show
		// then fails with AutomationErrorCode::Unavailable.
		std::function<void(bool visible)> ShowStartScreen;
		// Opens a dialog of the File menu at the next frame as if the user chose its menu item (open true; one shown stays as
		// it is), or closes it as its Cancel does (false; one that is not open stays closed). Fails, saying why, when it cannot
		// open (DecideEditorDialogOpen). Unset when the editor has no window (headless); editor.dialog
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
