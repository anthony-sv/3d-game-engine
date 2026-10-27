#pragma once

#include "Editor/VisualScripting/VisualScriptPanes.h"
#include "Editor/VisualScripting/VisualScriptQuickFixes.h"

#include "Strada/Script/VisualScriptLibrary.h"

#include <span>

namespace Strada
{
	// The problems of an element as its badge's popup, Details and the problems list show them: for each diagnostic its code
	// in the monospace font (VisualScriptEditorServices::MonospaceFont; the current font without one), its message wrapped at
	// 36 font sizes, and a button per quick fix (GetVisualScriptQuickFixes). A fix chosen is applied with
	// ApplyVisualScriptQuickFixChoice. Diagnostics of the model's analysis; inside an ImGui window or popup.
	void DrawVisualScriptProblems(VisualScriptPaneContext& context, std::span<VisualScriptDiagnostic const> diagnostics,
	                              VisualScriptWidgetCommits& commits, VisualScriptPaneResult& result);
	// The buttons of a diagnostic's quick fixes on one line ("Fix: Use Value"); returns whether one was chosen (and applied).
	bool DrawVisualScriptQuickFixButtons(VisualScriptPaneContext& context, VisualScriptDiagnostic const& diagnostic,
	                                     VisualScriptWidgetCommits& commits, VisualScriptPaneResult& result);

	// What choosing a quick fix of a diagnostic of the model's analysis does: a fix that navigates shows its target (GoTo
	// selects and frames the node; EditSetting also gives the keyboard to the setting in Details; Rename opens the name of
	// the variable, local or function in My Script, or a custom event's Name in Details); the others apply to a copy of the
	// document (ApplyVisualScriptQuickFix) and commit it in one step, "Fix: <Label>". A fix that fails adds a note.
	void ApplyVisualScriptQuickFixChoice(VisualScriptPaneContext& context, VisualScriptDiagnostic const& diagnostic,
	                                     VisualScriptQuickFix const& fix, VisualScriptWidgetCommits& commits,
	                                     VisualScriptPaneResult& result);

	// The diagnostics of the model's analysis that concern a node, a variable or local, or a function (one that names the
	// function and no node), in the analysis' order; empty without an analysis.
	[[nodiscard]] std::vector<VisualScriptDiagnostic> GetVisualScriptNodeProblems(VisualScriptAnalysis const* analysis, uint32_t node);
	[[nodiscard]] std::vector<VisualScriptDiagnostic> GetVisualScriptVariableProblems(VisualScriptAnalysis const* analysis,
	                                                                                  uint32_t variable);
	[[nodiscard]] std::vector<VisualScriptDiagnostic> GetVisualScriptFunctionProblems(VisualScriptAnalysis const* analysis,
	                                                                                  uint32_t function);
}
