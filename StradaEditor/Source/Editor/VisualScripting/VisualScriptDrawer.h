#pragma once

#include "Editor/VisualScripting/VisualScriptFind.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"
#include "Editor/VisualScripting/VisualScriptQuickFixes.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Strada
{
	// Where the drawer's find looks.
	enum class VisualScriptFindScope : uint8_t
	{
		ThisScript = 0,
		AllScripts
	};

	// A row of the drawer's find results: a match in a script, or a script that could not be read.
	struct VisualScriptDrawerFindRow
	{
		AssetHandle Graph;
		// The script's path in the asset directory (its name for a memory asset).
		std::string Script;
		VisualScriptFindResult Match;
		// What the row shows: the node's title and the matched text, "Comment: <text>", "Variable <name>", ...
		std::string Label;
		// Not empty for a script that could not be read: why ("could not be read: <error>").
		std::string Error;
	};

	// The drawer under a graph window's canvas, in one of two modes. Problems lists the analysis' diagnostics of the whole
	// script: errors first, then by sub-graph (the event graph, then the functions in document order), then by where their
	// node sits (top, then left); each row has its severity, code, message and place ("AddScore > Set Score") and the buttons
	// of its quick fixes (one step each). A click on a row shows its sub-graph and selects and frames what it names. Find
	// lists the matches of a text in this script or in every script of the project, or the references of an element (a
	// custom event's senders in every script, since events are sent by name), grouped by script, then sub-graph; a click
	// opens the match (another script's in its own window). Matches follow the documents as they change. Main thread only.
	class VisualScriptDrawer
	{
	public:
		VisualScriptPaneResult Draw(VisualScriptPaneContext& context);
		std::optional<VisualScriptCommitRequest> TakePendingEdit() { return m_Commits.TakePendingEdit(); }

		// The mode Draw shows (never Closed: the window does not draw a closed drawer).
		VisualScriptDrawerMode GetMode() const { return m_Mode; }
		void ShowProblems() { m_Mode = VisualScriptDrawerMode::Problems; }
		// Find with a text; its field takes the keyboard.
		void ShowFind(std::string text = {});
		// Find lists the references of an element.
		void ShowReferences(VisualScriptReference const& reference);

		// The problems listed in the last Draw, in their order.
		std::span<VisualScriptDiagnostic const> GetProblems() const { return m_Problems; }
		// The find rows listed in the last Draw, in their order.
		std::span<VisualScriptDrawerFindRow const> GetFindRows() const { return m_FindRows; }
		// The screen rectangle (min, max) of a problem's row, of its first fix button, and of a find row, in the last Draw.
		std::optional<std::pair<glm::vec2, glm::vec2>> FindProblemRow(size_t index) const;
		std::optional<std::pair<glm::vec2, glm::vec2>> FindFixButton(size_t index) const;
		std::optional<std::pair<glm::vec2, glm::vec2>> FindFindRow(size_t index) const;

	private:
		struct Rect
		{
			size_t Index = 0;
			glm::vec2 Min{0.0f};
			glm::vec2 Max{0.0f};
		};

		void DrawProblems(VisualScriptPaneContext& context, VisualScriptPaneResult& result);
		void DrawFind(VisualScriptPaneContext& context, VisualScriptPaneResult& result);
		void UpdateFindRows(VisualScriptPaneContext& context);
		// Shows what a diagnostic names, or a find row's match.
		void GoTo(VisualScriptPaneContext& context, VisualScriptDiagnostic const& diagnostic, VisualScriptPaneResult& result);
		void GoTo(VisualScriptPaneContext& context, VisualScriptDrawerFindRow const& row, VisualScriptPaneResult& result);

		VisualScriptWidgetCommits m_Commits;
		VisualScriptDrawerMode m_Mode = VisualScriptDrawerMode::Problems;
		std::vector<VisualScriptDiagnostic> m_Problems;
		std::vector<Rect> m_ProblemRows;
		std::vector<Rect> m_FixButtons;
		// Find: the text or the reference, the scope, and whether the field takes the keyboard.
		std::string m_Text;
		std::optional<VisualScriptReference> m_Reference;
		VisualScriptFindScope m_Scope = VisualScriptFindScope::ThisScript;
		bool m_FocusText = false;
		// What the rows were found in: the query, and each script's document object (null for one that could not be read).
		std::string m_FoundText;
		std::optional<VisualScriptReference> m_FoundReference;
		VisualScriptFindScope m_FoundScope = VisualScriptFindScope::ThisScript;
		std::vector<std::pair<AssetHandle, Ref<VisualScriptDocument const>>> m_FoundDocuments;
		bool m_Found = false;
		std::vector<VisualScriptDrawerFindRow> m_FindRows;
		std::vector<Rect> m_FindRects;
	};

	// The problems of an element as its badge's popup, Details and the problems list show them: for each diagnostic its code
	// in the monospace font (VisualScriptEditorServices::MonospaceFont; the current font without one), its message wrapped at
	// 36 font sizes, and a button per quick fix (GetVisualScriptQuickFixes). A fix chosen is applied with
	// ApplyVisualScriptQuickFixChoice. Diagnostics of the model's analysis; inside an ImGui window or popup.
	void DrawVisualScriptProblems(VisualScriptPaneContext& context, std::span<VisualScriptDiagnostic const> diagnostics,
	                              VisualScriptWidgetCommits& commits, VisualScriptPaneResult& result);
	// The buttons of a diagnostic's quick fixes on one line ("Use Value"); returns whether one was chosen (and applied).
	// firstButton, when given, receives the first button's screen rectangle (min, max).
	bool DrawVisualScriptQuickFixButtons(VisualScriptPaneContext& context, VisualScriptDiagnostic const& diagnostic,
	                                     VisualScriptWidgetCommits& commits, VisualScriptPaneResult& result,
	                                     std::optional<std::pair<glm::vec2, glm::vec2>>* firstButton = nullptr);

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
