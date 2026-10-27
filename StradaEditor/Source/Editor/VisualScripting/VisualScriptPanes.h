#pragma once

#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"
#include "Editor/VisualScripting/VisualScriptCommit.h"
#include "Editor/VisualScripting/VisualScriptEditing.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptEditorTypes.h"
#include "Editor/VisualScripting/VisualScriptPalette.h"
#include "Editor/VisualScripting/VisualScriptSemantics.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct ImDrawList;
struct ImVec2;

// The panes of a graph window (the palette popup, My Script, Details, the drawer and the find bar): what the window gives
// them each frame and what they hand back. Panes are drawn with ImGui; what they edit, they return as commit requests, which
// the window executes as it executes its canvas's (EditorOperations::EditVisualScript). Main thread only.
namespace Strada
{
	class EditSession;

	// What a row of My Script stands for, as VisualScriptElementPayload::Kind holds it.
	enum class VisualScriptElementKind : uint32_t
	{
		Variable = 0,
		Local,
		// A function; the ID 0 is the event graph (My Script's "Event Graph" row).
		Function,
		// An Event.Custom node of the event graph, by its node ID.
		CustomEvent
	};

	// A row of My Script: a variable, a local, a function (0: the event graph) or a custom event.
	struct VisualScriptElement
	{
		VisualScriptElementKind Kind = VisualScriptElementKind::Variable;
		uint32_t ID = 0;

		bool operator==(VisualScriptElement const& other) const = default;
	};

	// What the panes of one window share from frame to frame. The window keeps one and passes it in every context.
	struct VisualScriptPaneShared
	{
		// The My Script row selected, which Details shows. It and the canvas's selection exclude each other: a pane that
		// selects a row clears the canvas's selection, and while the canvas selects something the row is not selected (the
		// panes drop it at their next Draw).
		std::optional<VisualScriptElement> Element;
		// A setting of the canvas's primary node that Details gives the keyboard to once it shows the node (the window copies a
		// result's FocusSetting here, after its commit when it has one). Details clears it once it shows a node.
		std::string FocusSetting;
		// My Script opens this row's name for typing at its next Draw (F2 on a row, a quick fix that renames), and clears it.
		std::optional<VisualScriptElement> Rename;
	};

	// What a pane works with in a frame.
	struct VisualScriptPaneContext
	{
		VisualScriptEditorModel& Model;
		VisualScriptSemantics& Semantics;
		VisualScriptViewSettings& Settings;
		EditSession& Session;
		VisualScriptEditorServices const& Services;
		VisualScriptPaneShared& Shared;
		AssetHandle Graph;
		// The script's name, which commit descriptions end with ("Add Branch in Player").
		std::string_view ScriptName;
		UUID ContextEntity = UUID::Invalid();
		bool Playing = false;
		// Seconds: the clock of the model's Sync.
		double Time = 0.0;
		// The canvas's text measurer of the frame: the palette lays a new node out with it, to put the pin it connects where
		// the wire was released. An empty measurer places that pin by the node's top-left corner instead.
		VisualScriptTextMeasurer Measure;
		// Draws a node as the canvas does (the palette's hover preview), its top-left at the position, at the scale; unset in
		// the panes' own tests and until the window sets it.
		std::function<void(VisualScriptPaletteEntry const&, ImDrawList*, ImVec2 position, float scale)> DrawNodePreview;
		// While playing, the values the public variables of an entity's running instance hold now, by name, as
		// VisualScriptEngine::GetInstanceVariables reads them (My Script shows the context entity's, at most
		// VisualScriptSidebar::LiveValueInterval apart); unset shows none.
		std::function<Result<ScriptFieldMap>(UUID entity)> ReadLiveVariables;
	};

	// What a pane asks of the window after drawing. The window applies the members in this order: Commit (executed, then the
	// model synced in the same frame and the request's Select applied), ShowFunction, Open, SelectAndFrame, FindReferences.
	struct VisualScriptPaneResult
	{
		std::optional<VisualScriptCommitRequest> Commit;
		std::optional<uint32_t> ShowFunction;
		// Another graph, or a jump in this one (find results, problems): ApplyOpenRequest for this graph's own, the window
		// manager for another's.
		std::optional<VisualScriptOpenRequest> Open;
		// Nodes and comments of the shown sub-graph to select and frame.
		std::vector<uint32_t> SelectAndFrame;
		// The drawer lists the references of an element (VisualScriptDrawer::ShowReferences) and opens in Find mode.
		std::optional<VisualScriptReference> FindReferences;
		// Toasts.
		std::vector<std::string> Notes;
		// A setting of the committed node that needs a value (an Event.Custom's Name, a Convert's To): Details gives it the
		// keyboard once it shows the node (VisualScriptPaneShared::FocusSetting). Empty when none.
		std::string FocusSetting;
		// A widget interaction is in progress: its edits merge into one step until it ends.
		bool WidgetActive = false;
	};

	// A dragged My Script row as the canvas' drop carries it (VisualScriptCanvasDrop::Payload, one uint64_t): the kind in the
	// high 32 bits, the ID in the low 32. The window accepts rows of its own graph only (VisualScriptElementPayload::Graph).
	[[nodiscard]] constexpr uint64_t PackVisualScriptElement(VisualScriptElementKind kind, uint32_t id)
	{
		return (static_cast<uint64_t>(kind) << 32) | id;
	}
	// The kind and ID of a packed row; nothing for a kind this editor does not know.
	[[nodiscard]] std::optional<std::pair<VisualScriptElementKind, uint32_t>> UnpackVisualScriptElement(uint64_t packed);

	// The pins of a node created in the same edit (a palette node, a dropped variable), which the canvas does not show yet:
	// the ID and the pins of its palette entry or library template, in their order.
	struct VisualScriptNewNodePins
	{
		uint32_t Node = 0;
		std::span<VisualScriptPin const> Pins;
	};

	// The pins of the shown sub-graph's nodes as VisualScriptEditing's operations take them: the canvas's rows of each node
	// (orphans left out) under the names the document stores (VisualScriptEditing::ToStoredVisualScriptPin), with the
	// defaults of the analysis' pin, else the library template's; and the new node's rows, in its pins' order at no
	// particular anchor. Valid until the model's next Sync; the new node's pins must outlive it too.
	[[nodiscard]] VisualScriptNodePins MakeVisualScriptNodePins(VisualScriptEditorModel const& model, VisualScriptLibrary const* library,
	                                                            std::optional<VisualScriptNewNodePins> newNode = std::nullopt);

	// The library pin of a pin of the shown sub-graph as the canvas names it: the analysis' (with its resolved type) while the
	// analysis describes the node, else the library template's with the type the canvas knows; nothing for a pin the node
	// does not have.
	[[nodiscard]] std::optional<VisualScriptPin> FindVisualScriptCanvasPin(VisualScriptEditorModel const& model,
	                                                                       VisualScriptLibrary const* library, uint32_t node,
	                                                                       std::string_view pin, VisualScriptPinDirection direction);

	// The graph of a sub-graph: the event graph for 0, else the function's; null for a function the document does not have.
	[[nodiscard]] VisualScriptGraph const* FindVisualScriptGraph(VisualScriptDocument const& document, uint32_t function);
	[[nodiscard]] VisualScriptGraph* FindVisualScriptGraph(VisualScriptDocument& document, uint32_t function);
	// The sub-graph that holds a node or a comment (0 for the event graph); nothing when no graph holds it.
	[[nodiscard]] std::optional<uint32_t> FindVisualScriptElementGraph(VisualScriptDocument const& document, uint32_t element);
	// The node of an ID in whichever graph holds it; null when none.
	[[nodiscard]] VisualScriptNode const* FindVisualScriptNode(VisualScriptDocument const& document, uint32_t node);
	// A node's title as the canvas shows it: the analysis', else the one the document gives it (DeriveVisualScriptNodeTitle).
	[[nodiscard]] std::string GetVisualScriptNodeTitle(VisualScriptDocument const& document, VisualScriptAnalysis const* analysis,
	                                                   VisualScriptNode const& node);
	// A sub-graph's name: "Event Graph", or the function's name.
	[[nodiscard]] std::string GetVisualScriptGraphName(VisualScriptDocument const& document, uint32_t function);
	// What jumps to elements of a sub-graph of a graph: shows that sub-graph, selects them and frames them.
	[[nodiscard]] VisualScriptOpenRequest MakeVisualScriptJump(AssetHandle graph, uint32_t function, std::vector<uint32_t> select);
	// Shows a My Script row in Details: selects it and clears the canvas's selection.
	void SelectVisualScriptElement(VisualScriptPaneContext& context, VisualScriptElement element);

	// How the panes commit what their widgets change (spec §4.4). A widget interaction is one undo step under the edit
	// session's merge key: the edited document is previewed in the model at once (VisualScriptEditorModel::SetWidgetPreview),
	// its first change commits at once, later ones once CommitInterval has passed since the previous commit, and the last value
	// when the widget is deactivated, or as soon as a change comes from a widget that is no longer active (a check box, a
	// combo's list). Every commit of one step passes the same stored document to MakeVisualScriptCommitRequest: the document
	// from before its first commit (so a variable renamed back and forth within a step records no former name), until the
	// asset's document is no longer the one the step's last commit wrote. Discrete actions (buttons, names) commit at once,
	// each its own step (merge key 0) or under a key of the caller's. A pane's result holds one commit: an edit that finds it
	// taken waits for the pane's next Draw (BeginFrame), and is dropped when the asset's document is then not the one the
	// commit before it wrote (that commit failed, or something else changed the document).
	class VisualScriptWidgetCommits
	{
	public:
		static constexpr double CommitInterval = 0.1;

		// Call first in every Draw: drops a pending value the asset's document has moved past, and commits a waiting edit.
		void BeginFrame(VisualScriptPaneContext& context, VisualScriptPaneResult& result);
		// The document the pane shows and edits: the uncommitted widget value while one is pending (and the asset's document
		// is still the one it was computed from), else the model's display document.
		VisualScriptDocument const& GetDocument(VisualScriptEditorModel const& model) const;
		// The pending widget value GetDocument gives, kept alive for a pane that reads it while a widget changes it again;
		// null when GetDocument gives the display document.
		Ref<VisualScriptDocument const> GetPendingDocument(VisualScriptEditorModel const& model) const;
		// A widget changed the document (edited from GetDocument) this frame; active is whether the widget is still in use.
		void Change(VisualScriptPaneContext& context, VisualScriptDocument edited, std::string action, bool active,
		            VisualScriptPaneResult& result);
		// The widget was deactivated: its pending value commits.
		void End(VisualScriptPaneContext& context, VisualScriptPaneResult& result);
		// A discrete edit (edited from GetDocument): a pending widget value commits first, then this. Select is what the window
		// selects afterwards (nodes and comments of the shown sub-graph).
		void Commit(VisualScriptPaneContext& context, VisualScriptDocument edited, std::string action, uint64_t mergeKey,
		            VisualScriptPaneResult& result, std::vector<uint32_t> select = {});
		// The pending widget value as a commit request, prepared without an analysis (nodes resolved inexactly stay as they
		// are until the next commit); the preview is cleared. Nothing when none is pending.
		std::optional<VisualScriptCommitRequest> TakePendingEdit();
		bool HasPendingEdit() const { return m_Pending != nullptr; }

	private:
		struct WaitingEdit
		{
			VisualScriptDocument Document;
			std::string Action;
			uint64_t MergeKey = 0;
			std::vector<uint32_t> Select;
			// The document the commit before it writes.
			Ref<VisualScriptDocument const> Base;
		};

		// The stored document a commit under the key passes: the run's while the key and the asset's document are the run's.
		Ref<VisualScriptDocument const> GetStored(VisualScriptEditorModel const& model, uint64_t key);
		bool CommitPending(VisualScriptPaneContext& context, VisualScriptPaneResult& result);
		void DropStalePending(VisualScriptEditorModel& model);

		VisualScriptEditorModel* m_Model = nullptr;
		std::optional<WaitingEdit> m_Waiting;
		Ref<VisualScriptDocument const> m_Pending;
		std::string m_PendingAction;
		std::string m_ScriptName;
		// The asset's document the pending value was computed from.
		Ref<VisualScriptDocument const> m_PendingBase;
		// The edit session's key of the interaction the pending value belongs to, and when it last committed.
		uint64_t m_Key = 0;
		double m_LastCommit = 0.0;
		bool m_HasCommitted = false;
		// The run of commits under one key: its key, the document from before its first commit, and the document its last
		// commit wrote.
		uint64_t m_RunKey = 0;
		Ref<VisualScriptDocument const> m_RunStored;
		Ref<VisualScriptDocument const> m_RunWritten;
	};

	// A text field for a name (a variable, a local, a function, a parameter, a custom event): checked as it is typed (a red
	// frame and a tooltip with the reason while the name is not valid) and committed once, on Enter or when the field loses
	// the keyboard with a valid name that differs from the current one. A field opened for typing selects its name, so typing
	// replaces it; Enter on an invalid name keeps the field and its text, the cursor after it; leaving it keeps the current
	// name and says why; Escape keeps the current name. One field is edited at a time: an object serves every name field of a
	// pane.
	class VisualScriptNameField
	{
	public:
		// Why a name cannot be given; nothing when it can.
		using Validator = std::function<std::optional<std::string>(std::string_view name)>;

		struct Outcome
		{
			// The new name, in the frame it is committed.
			std::optional<std::string> Committed;
			// Why the name typed was not taken, in the frame the field was left with it.
			std::optional<std::string> Refused;
			bool Active = false;
		};

		// Draws the field (as wide as the next item) for the name current; focus gives it the keyboard with the cursor after
		// its text. id is unique within the current ID scope.
		Outcome Draw(char const* id, std::string const& current, Validator const& validate, bool focus = false);
		// The reason the name being typed is not valid; empty when it is (or when no name is being typed).
		std::string const& GetError() const { return m_Error; }
		// Whether a field of this object has the keyboard, or gets it back next frame.
		bool IsEditing() const { return m_Editing != 0; }

	private:
		uint32_t m_Editing = 0;
		std::string m_Text;
		std::string m_Error;
		// The text m_Error is about.
		std::string m_CheckedText;
		bool m_Checked = false;
		bool m_Refocus = false;
		// The next focus puts the cursor after the text instead of selecting it (after a refused Enter).
		bool m_CursorAtEnd = false;
		bool m_PlaceCursor = false;
	};
}
