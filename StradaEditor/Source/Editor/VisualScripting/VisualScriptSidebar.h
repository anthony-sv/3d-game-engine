#pragma once

#include "Editor/VisualScripting/VisualScriptDebugValues.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strada
{
	// My Script, the sidebar of a graph window: the script's graphs (the event graph and its functions), variables, the shown
	// function's locals and the event graph's custom events, filtered by a search field. A click selects a row, which Details
	// shows (VisualScriptPaneShared::Element); a double click or Enter opens it (a graph is shown, a custom event's node and a
	// variable's Get and Set nodes are selected and framed). "+" adds an element and opens its name, which becomes part of
	// the step that added it; rows rename in place, drag onto the canvas (DragDropPayload::VisualScriptElement) and, for
	// variables, onto each other to reorder; Delete asks first when nodes use what goes, and deletes them in the same step.
	// Problems of an element show as a dot or a badge on its row. While playing, every variable's row shows the context entity's
	// live value in a cell between its name and its type (spec-slice4 §15.5), formatted as the debugger formats values; while
	// an execution of the graph is stopped, the stopped frame's instance's. Main thread only.
	class VisualScriptSidebar
	{
	public:
		// Live values are read at most this often (seconds).
		static constexpr double LiveValueInterval = 0.25;
		// A live value that changed flashes this long (seconds), as the window's VisualScriptBlinkSeconds.
		static constexpr double LiveFlashSeconds = 0.6;
		// A variable's live value cell starts at least this many font sizes into its row.
		static constexpr float LiveCellIndent = 6.0f;
		// The base names of new elements, made unique.
		static constexpr char const* NewVariableName = "NewVar";
		static constexpr char const* NewLocalName = "NewLocal";
		static constexpr char const* NewFunctionName = "NewFunction";
		static constexpr char const* NewEventName = "NewEvent";

		VisualScriptPaneResult Draw(VisualScriptPaneContext& context);
		std::optional<VisualScriptCommitRequest> TakePendingEdit() { return m_Commits.TakePendingEdit(); }
		// Whether a widget value is pending (what the window flushes with TakePendingEdit when no item is active).
		bool HasPendingEdit() const { return m_Commits.HasPendingEdit(); }

		// A key the window routes to My Script (spec §8.3): Delete (DeleteWithoutRelink alike), Rename and Duplicate act on the
		// selected row at the next Draw, and OpenNode opens it. Returns whether My Script takes the action: a row is selected
		// and the action is one of these (the event graph's row takes OpenNode only), and the document could be edited at the
		// last Draw for the edits. The action is bound to the row selected now and done only by a Draw in this frame or the
		// next with that row still selected: a hidden My Script takes no keys (the window routes nothing to it), and an action
		// it could not do is dropped, never done later on another row.
		bool HandleAction(VisualScriptEditorAction action, VisualScriptPaneShared const& shared);

		// The screen rectangle (min, max) of a row in the last Draw; nothing when it was not drawn (filtered out, its section
		// closed). The event graph's row is the function 0.
		std::optional<std::pair<glm::vec2, glm::vec2>> FindRow(VisualScriptElement element) const;
		// The screen rectangle of a section's "+" button in the last Draw: Variable, Local, Function or CustomEvent.
		std::optional<std::pair<glm::vec2, glm::vec2>> FindAddButton(VisualScriptElementKind section) const;
		// The screen rectangle of a variable row's eye (public or not) in the last Draw.
		std::optional<std::pair<glm::vec2, glm::vec2>> FindEye(uint32_t variable) const;
		// The row whose name is being typed; nothing when none.
		std::optional<VisualScriptElement> GetRenaming() const { return m_Renaming; }
		// Why the name being typed cannot be given; empty when it can.
		std::string const& GetRenameError() const { return m_Names.GetError(); }
		// The element whose deletion asks for confirmation, while its modal is open (or opens at the next Draw), and what the
		// modal says.
		std::optional<VisualScriptElement> GetPendingDelete() const;
		std::string const& GetDeleteMessage() const { return m_DeleteMessage; }
		// The screen rectangle of a button of the delete modal ("Delete", "Find References", "Cancel") in the last Draw.
		std::optional<std::pair<glm::vec2, glm::vec2>> FindDeleteButton(std::string_view label) const;
		// The live value cells shown, by variable name; empty while not playing.
		std::map<std::string, VisualScriptValueCell> GetLiveValues() const;
		// A variable's live value cell as the last read formatted it; null without one.
		VisualScriptValueCell const* FindLiveCell(uint32_t variable) const;
		// Whether a variable's live value changed at its last read and its cell still flashes.
		bool IsLiveCellFlashing(uint32_t variable, double time) const;
		// The screen rectangles (min, max) of a variable row's live value cell (the dot that opens it included) and of its type
		// text in the last Draw.
		std::optional<std::pair<glm::vec2, glm::vec2>> FindLiveCellRect(uint32_t variable) const;
		std::optional<std::pair<glm::vec2, glm::vec2>> FindTypeRect(uint32_t variable) const;
		// What a variable's live value cell said before its value in the last Draw, for a few seconds after a receipt
		// (VisualScriptLiveTexts::Reset or New); empty when it said nothing.
		std::string_view GetLiveCellLabel(uint32_t variable) const;
		// The tooltip a hovered live value cell showed in the last Draw (a colour's is ImGui's own); empty when none was hovered.
		std::string const& GetLiveTooltip() const { return m_LiveTooltip; }
		// A function library's header line in the last Draw ("Function library · used by 5 nodes in 3 scripts"); empty for a
		// script.
		std::string const& GetLibraryLine() const { return m_LibraryLine; }

	private:
		struct Frame;
		// A row's place in the last Draw.
		struct RowRect
		{
			VisualScriptElement Element;
			glm::vec2 Min{0.0f};
			glm::vec2 Max{0.0f};
		};

		bool BeginSection(Frame& frame, char const* label, VisualScriptElementKind kind, bool canAdd);
		void DrawGraphs(Frame& frame);
		void DrawVariables(Frame& frame);
		void DrawLocals(Frame& frame);
		void DrawCustomEvents(Frame& frame);
		// One row: a selectable with its contents (drawn by the caller after it), its drag source, menu and double click, and
		// with dropped, a drop target for variables' rows (the ID dropped this frame). Returns false when the row is renamed in
		// place instead (the name field is drawn).
		bool DrawRow(Frame& frame, VisualScriptElement element, std::string const& name, bool canRename,
		             std::optional<uint32_t>* dropped = nullptr);
		void DrawProblemMarker(Frame& frame, std::vector<VisualScriptDiagnostic> const& problems, bool badge, float right);
		void DrawEye(Frame& frame, VisualScriptVariable const& variable, float x);
		void Rename(Frame& frame, VisualScriptElement element, std::string const& name);
		void Add(Frame& frame, VisualScriptElementKind kind);
		void Open(Frame& frame, VisualScriptElement element);
		void Delete(Frame& frame, VisualScriptElement element, bool confirmed);
		void Duplicate(Frame& frame, VisualScriptElement element);
		void DrawDeleteModal(Frame& frame);
		void UpdateLiveValues(Frame& frame);
		// A variable row's live value cell, from x to right (screen pixels) at the row's top.
		void DrawLiveCell(Frame& frame, VisualScriptVariable const& variable, float x, float right, float top);
		bool Matches(std::string_view name) const;

		VisualScriptWidgetCommits m_Commits;
		VisualScriptNameField m_Names;
		std::string m_Search;
		std::vector<RowRect> m_Rows;
		std::vector<std::pair<VisualScriptElementKind, std::pair<glm::vec2, glm::vec2>>> m_AddButtons;
		std::vector<std::pair<uint32_t, std::pair<glm::vec2, glm::vec2>>> m_Eyes;
		// The action routed by the window, the row it was routed for and the frame it came in, done at the next Draw.
		struct RoutedAction
		{
			VisualScriptEditorAction Action = VisualScriptEditorAction::OpenNode;
			VisualScriptElement Element;
			int Frame = 0;
		};
		std::optional<RoutedAction> m_Action;
		// Whether the last Draw showed a document that cannot be edited (a read-only file, a memory asset).
		bool m_ReadOnly = false;
		// The row being renamed, whether its field takes the keyboard at the next Draw, and the merge key of the step that
		// created it (0 when it was not just created).
		std::optional<VisualScriptElement> m_Renaming;
		bool m_RenameFocus = false;
		uint64_t m_NameKey = 0;
		// An element just created, renamed once the commit made it a row.
		std::optional<VisualScriptElement> m_RenameCreated;
		// The deletion waiting for its modal's answer.
		std::optional<VisualScriptElement> m_Delete;
		std::string m_DeleteTitle;
		std::string m_DeleteMessage;
		// What the modal's [Find References] lists: the element's uses in other scripts (none when they have none).
		std::optional<VisualScriptReference> m_DeleteReferences;
		std::vector<std::pair<std::string, std::pair<glm::vec2, glm::vec2>>> m_DeleteButtons;
		bool m_OpenDelete = false;
		// Live values: when they were read last, and per variable (by ID) the value read, its cell and when it last changed; the
		// entity read, and where the last Draw put the cells and the type texts.
		struct LiveCell
		{
			VisualScriptDebugValue Value;
			VisualScriptValueCell Cell;
			double Changed = -1.0e30;
		};
		double m_LiveTime = -1.0e30;
		std::map<uint32_t, LiveCell> m_LiveCells;
		UUID m_LiveEntity = UUID::Invalid();
		std::vector<std::pair<uint32_t, std::pair<glm::vec2, glm::vec2>>> m_LiveCellRects;
		std::vector<std::pair<uint32_t, std::pair<glm::vec2, glm::vec2>>> m_TypeRects;
		std::vector<std::pair<uint32_t, std::string_view>> m_LiveCellLabels;
		std::string m_LiveTooltip;
		std::string m_LibraryLine;
	};
}
