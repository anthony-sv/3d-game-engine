#pragma once

#include "Editor/VisualScripting/VisualScriptCurveEditor.h"
#include "Editor/VisualScripting/VisualScriptFind.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"
#include "Editor/VisualScripting/VisualScriptQuickFixes.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Script/VisualScriptLibrary.h"
#include "Strada/Script/VisualScriptLive.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

	// A row of the drawer's Runtime section (spec-slice4 §15.4): a runtime problem at a node of the document, its place ("Divide
	// (node 14)", its sub-graph named when it is a function's) and what follows the message ("on Enemy 3 · ×37", and for a
	// library's node " · called from Player (node 22)").
	struct VisualScriptDrawerRuntimeRow
	{
		Ref<VisualScriptRuntimeProblem const> Problem;
		std::string Title;
		std::string Detail;
	};

	// The drawer under a graph window's canvas, in one of three modes. Problems lists the analysis' diagnostics of the whole
	// script, under the runtime problems of its nodes while there are any (the Runtime section: each with [Go to], [Caller] for
	// a library's node and [Select Entity]; after Stop the last run's, with [Clear]): errors first, then by sub-graph (the event graph,
	// then the functions in document order), then by where their node sits (top, then left); each row has its severity, code, message and
	// place ("AddScore > Set Score") and the buttons of its quick fixes (one step each). A click on a row shows its sub-graph and selects
	// and frames what it names. Find lists the matches of a text in this script or in every script of the project, or the references of an
	// element (a custom event's senders in every script, since events are sent by name), grouped by script, then sub-graph; a click opens
	// the match (another script's in its own window). Matches follow the documents as they change. Timeline edits a Flow.Timeline of the
	// shown sub-graph (VisualScriptCurveEditor, spec-slice3 §9.5). Main thread only.
	class VisualScriptDrawer
	{
	public:
		VisualScriptPaneResult Draw(VisualScriptPaneContext& context);
		std::optional<VisualScriptCommitRequest> TakePendingEdit() { return m_Commits.TakePendingEdit(); }
		// Whether a widget value is pending (what the window flushes with TakePendingEdit when no item is active).
		bool HasPendingEdit() const { return m_Commits.HasPendingEdit(); }

		// The mode Draw shows (never Closed: the window does not draw a closed drawer).
		VisualScriptDrawerMode GetMode() const { return m_Mode; }
		// Shows the problems; the find results (and the documents they were found in) are let go.
		void ShowProblems();
		// Find with a text; its field takes the keyboard.
		void ShowFind(std::string text = {});
		// Find lists the references of an element.
		void ShowReferences(VisualScriptReference const& reference);
		// Shows a Flow.Timeline node's tracks and curves, on one of its tracks when given (its row highlighted, its keys
		// framed).
		void ShowTimeline(uint32_t node, std::optional<uint32_t> track = std::nullopt);
		// The node Timeline mode shows (the last one shown in another mode).
		uint32_t GetTimelineNode() const { return m_Timeline.GetNode(); }
		VisualScriptCurveEditor& GetCurveEditor() { return m_Timeline; }
		VisualScriptCurveEditor const& GetCurveEditor() const { return m_Timeline; }
		// Where a text is found: this script or every script of the project (the scope combo of Find).
		VisualScriptFindScope GetScope() const { return m_Scope; }
		void SetScope(VisualScriptFindScope scope) { m_Scope = scope; }

		// The problems listed in the last Draw, in their order.
		std::span<VisualScriptDiagnostic const> GetProblems() const { return m_Problems; }
		// The Runtime section of the last Draw: its header ("Runtime", "Runtime (last run)"; empty when not drawn), its rows, and
		// the screen rectangles (min, max) of a row's buttons ("Go to", "Caller", "Select Entity"; the header's "Clear" is row
		// SIZE_MAX).
		std::string const& GetRuntimeHeader() const { return m_RuntimeHeader; }
		std::span<VisualScriptDrawerRuntimeRow const> GetRuntimeRows() const { return m_RuntimeRows; }
		std::optional<std::pair<glm::vec2, glm::vec2>> FindRuntimeButton(size_t row, std::string_view label) const;
		// The find rows listed in the last Draw, in their order.
		std::span<VisualScriptDrawerFindRow const> GetFindRows() const { return m_FindRows; }
		// How many times Find looked through the scripts: once per change of its query, of this script, or of the project's
		// scripts, never per frame (spec-slice4 §16.2).
		uint64_t GetFindScanCount() const { return m_FindScans; }
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
		void DrawRuntime(VisualScriptPaneContext& context, VisualScriptPaneResult& result);
		void DrawFind(VisualScriptPaneContext& context, VisualScriptPaneResult& result);
		void UpdateFindRows(VisualScriptPaneContext& context);
		// Shows what a diagnostic names, or a find row's match.
		void SortProblems(VisualScriptEditorModel const& model, VisualScriptAnalysis const& analysis);
		void GoTo(VisualScriptPaneContext& context, VisualScriptDiagnostic const& diagnostic, VisualScriptPaneResult& result);
		void GoTo(VisualScriptPaneContext& context, VisualScriptDrawerFindRow const& row, VisualScriptPaneResult& result);

		VisualScriptWidgetCommits m_Commits;
		VisualScriptDrawerMode m_Mode = VisualScriptDrawerMode::Problems;
		VisualScriptCurveEditor m_Timeline;
		// The problems in their order, and what they were ordered from: the analysis (the model's count of analyses and its
		// object) and the stored document, which is not kept alive for it.
		std::vector<VisualScriptDiagnostic> m_Problems;
		bool m_ProblemsSorted = false;
		uint64_t m_SortedAnalysisCount = 0;
		VisualScriptAnalysis const* m_SortedAnalysis = nullptr;
		std::weak_ptr<VisualScriptDocument const> m_SortedDocument;
		std::vector<Rect> m_ProblemRows;
		std::vector<Rect> m_FixButtons;
		// The Runtime section as the last Draw drew it.
		struct RuntimeButton
		{
			size_t Row = 0;
			std::string Label;
			glm::vec2 Min{0.0f};
			glm::vec2 Max{0.0f};
		};
		std::string m_RuntimeHeader;
		std::vector<VisualScriptDrawerRuntimeRow> m_RuntimeRows;
		std::vector<RuntimeButton> m_RuntimeButtons;
		// Find: the text or the reference, the scope, and whether the field takes the keyboard.
		std::string m_Text;
		std::optional<VisualScriptReference> m_Reference;
		// The reference named no graph: it is the window's own element.
		bool m_ReferenceOwnGraph = false;
		VisualScriptFindScope m_Scope = VisualScriptFindScope::ThisScript;
		bool m_FocusText = false;
		// What the rows were found in: the query, this script's stored document and analysis (its object and the model's count of
		// analyses), the project index's revision the model held, and each script's document object (null for one that could
		// not be read).
		std::string m_FoundText;
		std::optional<VisualScriptReference> m_FoundReference;
		VisualScriptFindScope m_FoundScope = VisualScriptFindScope::ThisScript;
		Ref<VisualScriptDocument const> m_FoundStored;
		uint64_t m_FoundProjectRevision = 0;
		VisualScriptAnalysis const* m_FoundAnalysis = nullptr;
		uint64_t m_FoundAnalysisCount = 0;
		std::vector<std::pair<AssetHandle, Ref<VisualScriptDocument const>>> m_FoundDocuments;
		bool m_Found = false;
		uint64_t m_FindScans = 0;
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
	// document (ApplyVisualScriptQuickFix) and commit it in one step, "Fix: <Label>". A fix that fails adds a note. A fix that
	// opens another graph asks for its window (VisualScriptPaneResult::Open).
	// The other graphs a window's quick fixes read: the model's (ResolveGraph, GetGraphName), valid while it lives.
	[[nodiscard]] VisualScriptQuickFixGraphs MakeVisualScriptQuickFixGraphs(VisualScriptEditorModel const& model);
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
