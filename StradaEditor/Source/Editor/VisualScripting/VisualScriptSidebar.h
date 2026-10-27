#pragma once

#include "Editor/VisualScripting/VisualScriptPanes.h"

#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
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
	// Problems of an element show as a dot or a badge on its row. While playing, public variables show the context entity's
	// live values. Main thread only.
	class VisualScriptSidebar
	{
	public:
		// Live values are read at most this often (seconds).
		static constexpr double LiveValueInterval = 0.25;
		// The base names of new elements, made unique.
		static constexpr char const* NewVariableName = "NewVar";
		static constexpr char const* NewLocalName = "NewLocal";
		static constexpr char const* NewFunctionName = "NewFunction";
		static constexpr char const* NewEventName = "NewEvent";
		// The merge keys of a creation and its first name ("VSNM" + a counter).
		static constexpr uint64_t NameKeyPrefix = 0x56534E4D00000000ull;

		VisualScriptPaneResult Draw(VisualScriptPaneContext& context);
		std::optional<VisualScriptCommitRequest> TakePendingEdit() { return m_Commits.TakePendingEdit(); }

		// A key the window routes to My Script (spec §8.3): Delete (DeleteWithoutRelink alike), Rename and Duplicate act on the
		// selected row at the next Draw, and OpenNode opens it. Returns whether My Script takes the action: a row is selected
		// and the action is one of these (the event graph's row takes OpenNode only).
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
		// The live values shown, by variable name (literals); empty while not playing.
		std::map<std::string, Json> const& GetLiveValues() const { return m_LiveValues; }

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
		bool Matches(std::string_view name) const;

		VisualScriptWidgetCommits m_Commits;
		VisualScriptNameField m_Names;
		std::string m_Search;
		std::vector<RowRect> m_Rows;
		std::vector<std::pair<VisualScriptElementKind, std::pair<glm::vec2, glm::vec2>>> m_AddButtons;
		std::vector<std::pair<uint32_t, std::pair<glm::vec2, glm::vec2>>> m_Eyes;
		// The action routed by the window, done at the next Draw.
		std::optional<VisualScriptEditorAction> m_Action;
		// The row being renamed, whether its field takes the keyboard at the next Draw, and the merge key of the step that
		// created it (0 when it was not just created).
		std::optional<VisualScriptElement> m_Renaming;
		bool m_RenameFocus = false;
		uint64_t m_NameKey = 0;
		uint64_t m_NameKeyCount = 0;
		// An element just created, renamed once the commit made it a row.
		std::optional<VisualScriptElement> m_RenameCreated;
		// The deletion waiting for its modal's answer.
		std::optional<VisualScriptElement> m_Delete;
		std::string m_DeleteTitle;
		std::string m_DeleteMessage;
		bool m_OpenDelete = false;
		// Live values: when they were read last, and what they read.
		double m_LiveTime = -1.0e30;
		std::map<std::string, Json> m_LiveValues;
	};
}
