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
#include "Strada/Script/VisualScriptLiterals.h"

#include <glm/glm.hpp>

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

	// A setting of a node that Details gives the keyboard to once it shows that node (an Event.Custom's Name, a Convert's To).
	struct VisualScriptSettingFocus
	{
		uint32_t Node = 0;
		std::string Setting;

		bool operator==(VisualScriptSettingFocus const& other) const = default;
	};

	// A comment box created with its first title still to type (spec §9.6 rule 1): the window opens its in-place title editor
	// over the comment with "Comment" selected, and commits the title typed under MergeKey, the key the creation was committed
	// under, so that the creation and its first title are one step (§4.4).
	struct VisualScriptCommentTitleRequest
	{
		uint32_t Comment = 0;
		uint64_t MergeKey = 0;

		bool operator==(VisualScriptCommentTitleRequest const& other) const = default;
	};

	// The merge keys of a pane's creation and its first name or title ("VSNM" + a counter), which merge into one undo step. The
	// panes count their runs in VisualScriptPaneShared::NameRuns, in the upper half of the counter's range: the window's own
	// creations (the canvas's Add Comment Box) count theirs from the bottom, so two runs in a row never share a key.
	inline constexpr uint64_t VisualScriptPaneNameMergeKey = 0x56534E4D80000000ull;

	// What the panes of one window share from frame to frame. The window keeps one and passes it in every context.
	struct VisualScriptPaneShared
	{
		// The My Script row selected, which Details shows. It and the canvas's selection exclude each other: a pane that
		// selects a row clears the canvas's selection, and while the canvas selects something the row is not selected (the
		// panes drop it at their next Draw).
		std::optional<VisualScriptElement> Element;
		// A setting Details gives the keyboard to once it shows its node (the window copies a result's FocusSetting here, after
		// its commit when it has one). Details clears it once it shows that node, or when it has shown other subjects for
		// FocusSettingFrames Draws (the selection a request makes reaches the model a frame or two later).
		std::optional<VisualScriptSettingFocus> FocusSetting;
		// My Script opens this row's name for typing at its next Draw (F2 on a row, a quick fix that renames), and clears it.
		std::optional<VisualScriptElement> Rename;
		// The panes' creation-and-name runs so far (MakeVisualScriptPaneNameMergeKey).
		uint32_t NameRuns = 0;
	};

	// The merge key of a new run of a creation and its first name or title.
	[[nodiscard]] uint64_t MakeVisualScriptPaneNameMergeKey(VisualScriptPaneShared& shared);

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
	// model synced in the same frame and the request's Select applied when it is not empty), EditCommentTitle, ShowFunction,
	// Open, SelectAndFrame, FindReferences; FocusSetting is copied to VisualScriptPaneShared::FocusSetting.
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
		// A setting of a node that needs a value (an Event.Custom's Name, a Convert's To): Details gives it the keyboard once
		// it shows the node (VisualScriptPaneShared::FocusSetting).
		std::optional<VisualScriptSettingFocus> FocusSetting;
		// A comment box whose first title the window's in-place editor takes next, in the creation's step.
		std::optional<VisualScriptCommentTitleRequest> EditCommentTitle;
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
	// Shows a My Script row in Details: selects it and clears the canvas's selection. With a sub-graph to show (a local's
	// function, a function's own graph), the model shows it first, so that the selection cleared is that sub-graph's (showing
	// a sub-graph restores the selection it was left with, which would replace the row).
	void SelectVisualScriptElement(VisualScriptPaneContext& context, VisualScriptElement element,
	                               std::optional<uint32_t> show = std::nullopt);

	// What the panes have in common.

	// The loader's words for a name that is not one (VisualScriptSerializer::IsValidName and MaxNameBytes).
	[[nodiscard]] std::string GetVisualScriptNameRuleMessage();
	// The library's enum lookup for VisualScriptEditing's operations; empty without a library.
	[[nodiscard]] VisualScriptEnumLookup MakeVisualScriptEnumLookup(VisualScriptLibrary const* library);
	// A type as the panes name it: the library's title for it, else its ID.
	[[nodiscard]] std::string DescribeVisualScriptType(VisualScriptLibrary const* library, std::string_view type);
	// Whether a variable of the type can be public: components can store it (an exposable type, FindVisualScriptExposableType,
	// or an enum, which the library knows).
	[[nodiscard]] bool IsVisualScriptPublicType(std::string_view type, VisualScriptLibrary const* library);
	// Whether text holds part, ASCII letters compared without case; an empty part is in every text.
	[[nodiscard]] bool ContainsVisualScriptTextIgnoringCase(std::string_view text, std::string_view part);
	// The nearest point of the canvas's grid (VisualScriptCanvasMetrics::GridSize).
	[[nodiscard]] glm::vec2 SnapVisualScriptToGrid(glm::vec2 position);
	// Whether a node type is a C# member's ("Call:", "Get:", "Set:", "New:" and the member): its type string names the member.
	[[nodiscard]] bool IsVisualScriptReflectedType(std::string_view type);
	// The nodes of a graph that get or set a variable or local (Kind Variable or Local), or call a function (Kind Function).
	[[nodiscard]] std::vector<uint32_t> FindVisualScriptUses(VisualScriptGraph const& graph, VisualScriptElementKind kind, uint32_t id);
	// How many nodes of the document's graphs use a variable, local or function: what "Used by", "Called by" and the delete
	// modal count. A function's calls in its own graph are not counted: they go with it.
	[[nodiscard]] uint32_t CountVisualScriptUses(VisualScriptDocument const& document, VisualScriptElementKind kind, uint32_t id);

	// Whether a node offers the Enabled states (spec §9.5): nodes with an exec input and an exec output, and event nodes other
	// than Event.Send, do; pure nodes show them disabled; Function.Entry and Function.Return do not show them.
	enum class VisualScriptEnabledOffer : uint8_t
	{
		Offered = 0,
		// The states show disabled, with VisualScriptPureNodesRefusal.
		PureNode,
		// Function.Entry and Function.Return: the states are not shown.
		FunctionEnd,
		// A node the flow does not pass through (no exec input or no exec output): disabled, with VisualScriptNoFlowRefusal.
		NoFlow
	};
	inline constexpr char const* VisualScriptPureNodesRefusal = "Pure nodes run only when a node reads them: disable that node";
	inline constexpr char const* VisualScriptNoFlowRefusal = "Only nodes the flow passes through can be disabled";
	// From the canvas's layout of the node when the shown sub-graph has it, else from the library's template of its type; a node
	// of a type neither describes offers them (disabling it ends its chain).
	[[nodiscard]] VisualScriptEnabledOffer GetVisualScriptEnabledOffer(VisualScriptEditorModel const& model,
	                                                                   VisualScriptLibrary const* library, VisualScriptNode const& node);

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

		// Call first in every Draw: drops a pending value the asset's document has moved past, commits a waiting edit, and
		// commits the pending value of an interaction still in progress once CommitInterval has passed since its last commit
		// (a drag held still).
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
		// The pending widget value as a commit request, prepared with the model's analysis as every commit is; the preview is
		// cleared. Nothing when none is pending.
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
	// the keyboard with a valid name that differs from the current one, whatever takes it (a widget drawn before or after the
	// field, another pane, another name field of this object). A field opened for typing selects its name, so typing replaces
	// it; Enter on an invalid name keeps the field and its text, the cursor after it; leaving it keeps the current name and
	// says why; Escape keeps the current name; a field that is not drawn for a frame (its pane hidden) keeps the current name.
	// One field is edited at a time: an object serves every name field of a pane.
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
		// Another field of this object takes the keyboard: the one edited until now settles its name at its next Draw.
		void Leave(int frame);

		uint32_t m_Editing = 0;
		std::string m_Text;
		// The frame the field edited was last drawn in.
		int m_DrawnFrame = 0;
		// A field left for another field of this object, its text, and the frame it was left in.
		uint32_t m_Left = 0;
		std::string m_LeftText;
		int m_LeftFrame = 0;
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
