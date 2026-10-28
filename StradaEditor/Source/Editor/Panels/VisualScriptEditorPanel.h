#pragma once

#include "Editor/VisualScripting/VisualScriptBrushes.h"
#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"
#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptCanvasRenderer.h"
#include "Editor/VisualScripting/VisualScriptCommit.h"
#include "Editor/VisualScripting/VisualScriptController.h"
#include "Editor/VisualScripting/VisualScriptDetails.h"
#include "Editor/VisualScripting/VisualScriptDrawer.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptEditorTypes.h"
#include "Editor/VisualScripting/VisualScriptFindBar.h"
#include "Editor/VisualScripting/VisualScriptLiteralEditor.h"
#include "Editor/VisualScripting/VisualScriptPalettePopup.h"
#include "Editor/VisualScripting/VisualScriptPanes.h"
#include "Editor/VisualScripting/VisualScriptSidebar.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <glm/glm.hpp>
#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strada
{
	class EditorOperations;
	class EditSession;
	class VisualScriptSemantics;

	// The parts of a graph window that can have the keyboard: the keys the window claims go to the focused one.
	enum class VisualScriptWindowPart : uint8_t
	{
		None = 0,
		Canvas,
		Sidebar,
		Details
	};

	// How a graph window's body is shared between its panes and its canvas, in pixels.
	struct VisualScriptWindowLayout
	{
		// The panes' widths beside the canvas; 0 for a pane that is not beside it (hidden, auto-hidden, or in the stacked column).
		float Sidebar = 0.0f;
		float Details = 0.0f;
		float Canvas = 0.0f;
		// Stacked: both panes in one column of width Column on the sidebar's side of the canvas, My Script on top and Details
		// below a horizontal splitter (both states Shown).
		bool Stacked = false;
		float Column = 0.0f;
		// Shown, Hidden (turned off) or AutoHidden (turned on, but the window is too narrow for it); never Overlay, which is
		// the window's state on top of AutoHidden.
		VisualScriptPaneState SidebarState = VisualScriptPaneState::Shown;
		VisualScriptPaneState DetailsState = VisualScriptPaneState::Shown;
	};

	// The layout rule (spec §10.1 rule 2 of slice 2, with slice 3's §11.2), from the width alone each frame; the user's toggles
	// do not change. Each shown pane takes its width (font sizes), at least its minimum and at most VisualScriptPaneMaxShare of
	// the window, and the canvas keeps at least VisualScriptCanvasMinWidth font sizes:
	// 1. side by side, each pane with a splitter of splitter pixels beside it, when both fit so;
	// 2. else, with both panes on, stacked in one column (a gap of splitter pixels between it and the canvas) when the canvas
	//    leaves it at least VisualScriptDetailsMinWidth font sizes: the column is as wide as the wider pane, at most what the
	//    canvas leaves;
	// 3. else Details is auto-hidden first, then the sidebar. With one pane on, rules 1 and 3 apply to it alone. Pure.
	[[nodiscard]] VisualScriptWindowLayout ComputeVisualScriptWindowLayout(float width, float fontSize, float splitter,
	                                                                       VisualScriptPaneSettings const& panes);

	// The items of a graph window that tests and automation locate: the toolbar's, in its order, then the buttons of its notices,
	// cards and hints. GetItemRect gives where the last draw put each.
	enum class VisualScriptPanelItem : uint8_t
	{
		SidebarToggle = 0,
		Back,
		Forward,
		Breadcrumb,
		Instance,
		Live,
		Problems,
		SaveStatus,
		Arrange,
		Find,
		Bookmarks,
		SnapToGrid,
		Minimap,
		Zoom,
		ViewMenu,
		Shortcuts,
		DetailsToggle,
		// The degraded notice's [Show Console], the normalization notice's [Update Nodes], the dismiss buttons of the repaired
		// file's and the new function's notices, the read-only file's [Retry].
		ShowConsole,
		UpdateNodes,
		DismissRepaired,
		DismissNewFunction,
		Retry,
		// The first canvas's [Got it].
		GotIt,
		// The card of a missing or refused file: [Close]; the empty event graph's card: its three buttons.
		CloseCard,
		AddCreateEvent,
		AddUpdateEvent,
		AddNode,
		Count
	};

	// The toolbar's items: the first VisualScriptToolbarItemCount of VisualScriptPanelItem.
	inline constexpr size_t VisualScriptToolbarItemCount = static_cast<size_t>(VisualScriptPanelItem::DetailsToggle) + 1;

	// What a graph window did that leaves no other trace, counted for tests: the degraded notice's blinks (a gesture that needs
	// the scripting runtime was refused), the save status's flashes (Ctrl+S), and the reads of the context entity's live
	// variable values.
	struct VisualScriptPanelActivity
	{
		uint64_t DegradedBlinks = 0;
		uint64_t SaveFlashes = 0;
		uint64_t LiveValueReads = 0;
	};

	// The live value widget a graph window drew over a value chip: the chip and, in screen pixels, the widget's frame (all its
	// cells) and its last cell's frame (tests check them against the chip's cells).
	struct VisualScriptLiveWidgetFrames
	{
		VisualScriptLiveChip Chip;
		VisualScriptRect Frame;
		VisualScriptRect LastCell;
	};

	// The context entity's live values are read at most this often (seconds): 4 times a second (spec §10.6 rule 4).
	inline constexpr double VisualScriptLiveValueInterval = 0.25;
	// How long the degraded notice blinks and the save status flashes (seconds).
	inline constexpr double VisualScriptBlinkSeconds = 0.6;
	// The hint shown once per user over the first canvas (VisualScriptViewSettings::IsHintSeen).
	inline constexpr std::string_view VisualScriptCanvasHint = "Canvas";

	// The fixed texts of a graph window: its notices, cards, pills and hints (spec §10.4, §10.6), all listed in All for the glyph
	// test (the editor's UI font must have each character).
	namespace VisualScriptPanelTexts
	{
		// The controller refuses edits with the notices' texts (spec §9.1).
		inline constexpr std::string_view Degraded = VisualScriptRefusals::Degraded;
		inline constexpr std::string_view ReadOnlyMemory = VisualScriptRefusals::ReadOnlyMemory;
		inline constexpr std::string_view NewFunction = "A function starts at Entry. Add inputs and outputs in Details.";
		inline constexpr std::string_view EmptyGraph = "This script does nothing yet.";
		inline constexpr std::string_view EmptyGraphHelp = "Right-click or press Tab to add nodes; drag variables from My Script.";
		inline constexpr std::string_view Live = "LIVE";
		inline constexpr std::string_view LastWorking = "LIVE \xC2\xB7 last working version";
		inline constexpr std::string_view Simulating = "SIMULATING";
		inline constexpr std::string_view SimulatingFull = "SIMULATING \xC2\xB7 visual scripts do not run";
		inline constexpr std::string_view LiveTooltip = "Edits apply to the running game within a moment and stay undoable after Stop. "
														"Waiting nodes survive edits that keep them; new variables start at their value.";
		inline constexpr std::string_view SimulatingTooltip =
			"SIMULATING \xC2\xB7 visual scripts do not run while simulating: Play runs them.";
		inline constexpr std::string_view CanvasHint =
			"Tab or right-click: add a node \xC2\xB7 Drag from a pin: connect \xC2\xB7 Middle-drag: pan "
			"\xC2\xB7 Wheel: zoom (view menu: Scroll) \xC2\xB7 F1: shortcuts";
		inline constexpr std::string_view CanvasHintPan =
			"Tab or right-click: add a node \xC2\xB7 Drag from a pin: connect \xC2\xB7 Middle-drag: "
			"pan \xC2\xB7 Wheel: pan \xC2\xB7 Ctrl+wheel: zoom (view menu: Scroll) \xC2\xB7 F1: shortcuts";
		inline constexpr std::string_view WireHint = "Release on empty canvas: add a connected node \xC2\xB7 Esc: cancel";
		inline constexpr std::string_view MoveSnappedHint = "Alt: no snapping \xC2\xB7 Esc: cancel";
		inline constexpr std::string_view MoveFreeHint = "Alt: snap to grid \xC2\xB7 Esc: cancel";
		inline constexpr std::string_view PanHint = "Drag to pan";
		inline constexpr std::string_view NoBookmarks = "No bookmarks yet";
		inline constexpr std::string_view ShortcutSheet = "Visual Script Shortcuts";
		// The toolbar's problems pill without counts to show.
		inline constexpr std::string_view NoProblems = "No problems";
		inline constexpr std::string_view NotAnalyzed = "Not analyzed";

		inline constexpr std::array<std::string_view, 23> All = {
			Degraded,      ReadOnlyMemory, NewFunction,     EmptyGraph,       EmptyGraphHelp,    Live,
			LastWorking,   Simulating,     SimulatingFull,  LiveTooltip,      SimulatingTooltip, CanvasHint,
			CanvasHintPan, WireHint,       MoveSnappedHint, MoveFreeHint,     PanHint,           NoBookmarks,
			ShortcutSheet, NoProblems,     NotAnalyzed,     "Instance: None", "Not saved"};
	}

	// The pane sizes of spec §10.1, in font sizes.
	inline constexpr float VisualScriptSidebarMinWidth = 10.0f;
	inline constexpr float VisualScriptDetailsMinWidth = 14.0f;
	inline constexpr float VisualScriptCanvasMinWidth = 40.0f;
	inline constexpr float VisualScriptDrawerDefaultHeight = 10.0f;
	inline constexpr float VisualScriptDrawerMinHeight = 4.0f;
	// The stacked column's share above its splitter (My Script's) when a window opens; dragging the splitter changes it for the
	// window, and it is not stored. Each pane of the column keeps at least VisualScriptStackedPaneMinHeight font sizes while the
	// column is high enough for both.
	inline constexpr float VisualScriptStackShare = 0.45f;
	inline constexpr float VisualScriptStackedPaneMinHeight = 4.0f;
	// A pane beside the canvas takes at most this share of the window; an overlay pane at most this share of the canvas.
	inline constexpr float VisualScriptPaneMaxShare = 0.4f;
	inline constexpr float VisualScriptOverlayMaxShare = 0.8f;
	// The splitters between the panes and the canvas, in logical pixels.
	inline constexpr float VisualScriptSplitterLogicalPixels = 4.0f;
	// The toolbar's breadcrumb keeps at least this many font sizes when the toolbar is too narrow for it.
	inline constexpr float VisualScriptBreadcrumbMinWidth = 8.0f;

	// The window of one open visual script (spec §10): a toolbar with the breadcrumb, the canvas between the My Script sidebar
	// and Details, the drawer under it (problems, find results), the find bar and the minimap over the canvas, and the node
	// palette. Each frame it syncs its canvas model with the asset, turns the user's input into the model's input, has its
	// controller resolve what the model asks to edit, executes each edit as one step of the editor's history
	// (EditorOperations::EditVisualScript), draws the canvas and its panes, and executes what the panes ask for as it executes
	// its own edits. Only OnImGuiRender calls ImGui; every other member records or reads state, so automation can drive a
	// window between frames. Main thread only.
	class VisualScriptEditorPanel final : private VisualScriptLiveChipHost
	{
	public:
		// Toasts stay this long (seconds), longer while hovered; failures stay until dismissed.
		static constexpr double ToastSeconds = 4.0;
		// The oldest toast leaves when more would show.
		static constexpr size_t MaxToasts = 6;

		// The services (the monospace font and the console callback) are kept by reference: the window manager owns them, as
		// it owns the operations, the semantics, the settings and the edit session every window shares.
		VisualScriptEditorPanel(AssetHandle graph, EditorOperations& operations, VisualScriptSemantics& semantics,
		                        VisualScriptViewSettings& settings, EditSession& session, VisualScriptEditorServices const& services);
		~VisualScriptEditorPanel() override;
		VisualScriptEditorPanel(VisualScriptEditorPanel const&) = delete;
		VisualScriptEditorPanel& operator=(VisualScriptEditorPanel const&) = delete;

		// Draws the window: SetNextWindowDockID(*dockTarget, ImGuiCond_FirstUseEver) when given, then Begin with the flags of
		// spec §8.1. A hidden window (a tab not in front) neither updates its model nor claims keys.
		void OnImGuiRender(bool& open, std::optional<ImGuiID> dockTarget);
		// Applied at the next draw: the sub-graph, selection and view (the model's), and the focus.
		void Apply(VisualScriptOpenRequest const& request);
		// An action of the Edit menu while this window is the edit target; applied at the next draw, routed as its key would be
		// (a row key goes to the My Script row Details shows). Refused, returning false, while the last draw found the window
		// hidden (a tab not in front): an action taken then would run whenever the window is shown again. A window that is
		// hidden afterwards drops the actions it took.
		[[nodiscard]] bool RequestAction(VisualScriptEditorAction action);
		// After an undo or redo while this window is the edit target: "Undone: <description>" or "Redone: <description>", with
		// [Show] when the step edited the scene (an invalid editedAsset) or another visual script.
		void NotifyHistoryStep(bool undone, std::string const& description, AssetHandle editedAsset);
		// Whether a widget of the window holds an edit it has not committed: the in-place text editor, a value of Details, My
		// Script or the drawer, a quick fix's value in a problems popup, or the canvas's live value widget.
		bool HasPendingWidgetEdit() const;
		// Commits those edits now (spec §4.4 rule 2): the window manager calls it, before any window draws, for windows whose
		// widget is no longer active (hidden, closed, a project change).
		void FlushWidgetEdit();
		// Opens the node palette as graph.palette asks (spec §13.5), shown at the next draw; its entries are those of the draw
		// after (GetPaletteState). Fails, opening nothing, without the node library ("the node palette needs the scripting
		// runtime"), for a From node the shown sub-graph does not have, and for a From pin that node does not have.
		[[nodiscard]] Result<VisualScriptPaletteState> OpenPalette(VisualScriptPaletteRequest const& request);
		void ClosePalette();
		// What the palette showed in the last draw (Open false while it is closed, and from OpenPalette until it is drawn);
		// GetState's PaletteOpen and PaletteQuery are the same.
		VisualScriptPaletteState GetPaletteState() const;
		// What graph.editors reports of the window, as its last draw saw it.
		VisualScriptEditorState GetState() const;
		VisualScriptEditorModel const& GetModel() const { return m_Model; }
		// What preparing the last commit the window executed spent (VisualScriptCommitRequest::Timings: Normalize, Check, Text);
		// the performance tests read it with the step's and the model's.
		VisualScriptCommitTimings const& GetLastCommitTimings() const { return m_LastCommitTimings; }
		AssetHandle GetGraph() const { return m_Graph; }
		// The title the window shows; empty shows the file's stem (the window manager names two graphs with one stem by their
		// folders). The window keeps its identity across titles.
		void SetTitle(std::string title) { m_Title = std::move(title); }
		// What the panes asked to open in other graphs' windows since the last call (a find result or a reference in another
		// script, a history toast's [Show] for another script's step): the window manager opens them, or brings them to the
		// front.
		std::vector<VisualScriptOpenRequest> TakeOtherGraphRequests() { return std::exchange(m_OtherGraphRequests, {}); }
		// The panes, as the last draw left them (tests and automation find their rows through them).
		VisualScriptPalettePopup const& GetPalette() const { return m_Palette; }
		VisualScriptSidebar const& GetSidebar() const { return m_Sidebar; }
		VisualScriptDetails const& GetDetails() const { return m_Details; }
		VisualScriptDrawer const& GetDrawer() const { return m_Drawer; }
		VisualScriptFindBar const& GetFindBar() const { return m_FindBar; }
		VisualScriptPaneShared const& GetPaneShared() const { return m_PaneShared; }
		// What the problems popup of a badge (a node's or a link's) listed in the last draw; empty while none is open.
		std::span<VisualScriptDiagnostic const> GetPopupProblems() const { return m_PopupProblems; }
		// The live value widget the last draw drew; none when no chip was live.
		std::optional<VisualScriptLiveWidgetFrames> const& GetLiveWidget() const { return m_LiveWidget; }
		// The canvas's renderer, as the last frame left it (its counters).
		VisualScriptCanvasRenderer const& GetRenderer() const { return m_Renderer; }
		// The part of the window that had the keyboard in the last draw.
		VisualScriptWindowPart GetFocusedPart() const { return m_FocusedPart; }
		// Screen pixels: where the last draw put the item; none when it drew none (a toolbar item the overflow moved into the
		// view menu, a notice not shown).
		std::optional<VisualScriptRect> GetItemRect(VisualScriptPanelItem item) const;
		VisualScriptPanelActivity const& GetActivity() const { return m_Activity; }
		// The context entity (spec §10.5): where entity drops write values and whose live values the window shows; invalid
		// when none, and while the entity chosen is not in the scene shown (deleted; undoing the deletion brings it back).
		UUID GetContextEntity() const;
		// The context entity's public variables as its running instance holds them, read while playing at most
		// VisualScriptLiveValueInterval apart; none while not playing, without a context entity or without its instance.
		std::optional<ScriptFieldMap> const& GetLiveValues() const { return m_LiveValues; }

		// "<title>###VisualScript<handle as decimal>": the window keeps its ID (and its dock node) when the title changes.
		static std::string GetWindowName(AssetHandle graph, std::string_view title);

	private:
		struct Toast
		{
			std::string Text;
			// When it disappears; none for a failure, which stays until dismissed.
			std::optional<double> Expires;
			bool ShowConsole = false;
			// [Show]: focuses the scene's view (an invalid handle) or opens the window of another visual script; with ShowElements,
			// frames those nodes and comments of the sub-graph ShowFunction instead.
			bool HasShow = false;
			AssetHandle ShowAsset;
			std::vector<uint32_t> ShowElements;
			uint32_t ShowFunction = 0;
		};

		// The notices of spec §10.4, in the order they are drawn under the toolbar.
		enum class NoticeKind : uint8_t
		{
			Degraded = 0,
			ReadOnlyMemory,
			ReadOnlyFile,
			Repaired,
			Normalization,
			NewFunction
		};

		// A menu the canvas opened, drawn until it closes.
		enum class MenuKind : uint8_t
		{
			None = 0,
			Node,
			Pin,
			Link,
			Comment,
			// The pins a collapsed node's anchor stands for.
			AnchorPins,
			// Get or Set of a variable dropped from My Script.
			ElementDrop
		};

		struct OpenMenu
		{
			MenuKind Kind = MenuKind::None;
			// ImGui::OpenPopup is still to be called (in the window's ID scope).
			bool OpenPending = false;
			// Picks count only after the mouse button that opened it was released: the anchor's menu opens at a press.
			bool Armed = false;
			VisualScriptContextMenuIntent Context;
			VisualScriptPinMenuIntent Anchor;
			std::optional<VisualScriptElementDropMenu> Drop;
			// Canvas units: where the menu was opened.
			glm::vec2 Position{0.0f};
			// Screen pixels: where it shows.
			glm::vec2 ScreenPosition{0.0f};
		};

		// The problems popup of a node's badge or of a link's.
		struct ProblemsPopup
		{
			std::optional<uint32_t> Node;
			std::optional<VisualScriptLink> Link;
			// ImGui::OpenPopup is still to be called (in the canvas's ID scope).
			bool OpenPending = true;
		};

		// A literal the live value widget changed this frame, committed once the canvas is drawn.
		struct LiveChange
		{
			// The node and the input as the document stores it, and the words of the step's description.
			uint32_t Node = 0;
			std::string Stored;
			std::string Label;
			std::string Title;
			Json Literal;
		};

		// A chip of the live value widget resolved (ResolveLiveChip): its key, then what it shows and edits.
		struct LiveChipData
		{
			// Defaulted in the .cpp. Clang reads the default member initializers of a type nested in a class only once that class
			// is complete, and libstdc++'s std::optional, instantiated for a member before then, records the type as not
			// default-constructible, which refuses emplace(). A constructor declared here is known at once.
			LiveChipData();

			uint32_t Node = 0;
			std::string Pin;
			std::weak_ptr<VisualScriptDocument const> Pending;
			std::weak_ptr<VisualScriptDocument const> Stored;
			uint64_t Analysis = 0;
			uint64_t Revision = 0;
			VisualScriptLibrary const* Library = nullptr;
			// The node is shown and the literal editor edits the pin's type.
			bool Editable = false;
			std::string Type;
			// The input as the document stores it, the label the step's description uses, and the node's title.
			std::string StoredPin;
			std::string Label;
			std::string Title;
			Json Literal;
			Json Fallback;
		};

		// The popup a compact live widget asked for (a colour, an enum, an asset), drawn at the window's normal size.
		struct LivePopup
		{
			VisualScriptPinReference Pin;
			bool OpenPending = true;
		};

		// The palette's hover preview: its entry's node laid out alone, kept while the entry, the document (by identity, never
		// kept alive), the sub-graph, the library and the measurer stay.
		struct NodePreview
		{
			// Defaulted in the .cpp, as LiveChipData's.
			NodePreview();

			// The node's ID in its scratch document.
			static constexpr uint32_t Node = 1;
			std::string NodeType;
			Json Settings;
			std::weak_ptr<VisualScriptDocument const> Document;
			uint32_t Function = 0;
			VisualScriptLibrary const* Library = nullptr;
			uint64_t Signature = 0;
			VisualScriptCanvasModel Canvas;
			std::optional<uint32_t> Index;
			// The sub-graph the node was laid out in (the event graph when the shown function is gone).
			uint32_t ShownFunction = 0;
		};

		// The in-place text editor over the canvas: a node comment or a comment box's title.
		struct TextEdit
		{
			VisualScriptTextEditKind Kind = VisualScriptTextEditKind::NodeComment;
			uint32_t Element = 0;
			std::string Text;
			std::string Original;
			// The field still has to get the keyboard.
			bool Focus = true;
			bool WasActive = false;
		};

		// A creation followed by its first name (Add Comment Box and its title): one undo step (spec §4.4's "VSNM" row).
		struct NameRun
		{
			uint64_t Key = 0;
			// The document from before the creation, and the document the run's last commit wrote.
			Ref<VisualScriptDocument const> Stored;
			Ref<VisualScriptDocument const> Written;
		};

		// The frame.
		std::string GetTitle() const;
		// The context entity while the scene shown has it; invalid otherwise.
		UUID GetPresentContextEntity() const;
		VisualScriptTextMeasurer MakeMeasurer() const;
		void SyncModel();
		void OnHidden();
		void ClaimKeys();
		// After the model's sync: what the window shows around the canvas (the context entity, the live state and values, the
		// notices and the card, the toast of an outside change).
		void UpdateWindowState();
		void UpdateContextEntity();
		void UpdateLiveState();
		void UpdateNotices();
		void UpdateNormalizationCount();
		void ReportOutsideChange();
		// Frames nodes and comments of the shown sub-graph, keeping the selection.
		void FrameElements(std::span<uint32_t const> elements);
		bool IsShownGraphEmpty() const;
		bool IsShownFunctionNew() const;
		// Whether the context entity's VisualScriptComponent runs this graph in the scene shown (the running copy while
		// playing).
		bool RunsGraph(UUID entity) const;
		void SetContextEntity(UUID entity, bool chosen);
		void RecordItem(VisualScriptPanelItem item);
		void DrawToolbar();
		void DrawInstanceCombo(float width);
		void DrawLivePill(float width, std::string const& text);
		void DrawBookmarksMenu();
		void DrawNotices();
		void DrawNotice(NoticeKind kind);
		void DrawCard(ImVec2 canvasMin, ImVec2 canvasMax);
		void DrawCanvasHint(ImVec2 canvasMin, ImVec2 canvasMax);
		void DrawShortcutSheet();
		void DrawBody();
		void DrawPane(char const* id, VisualScriptWindowPart part, ImVec2 size, bool border = false);
		void DrawPaneContents(VisualScriptWindowPart part);
		void DrawDrawer(ImVec2 size);
		void DrawFindBar(ImVec2 canvasMin, ImVec2 canvasMax);
		void DrawPalette();
		void DrawNodePreview(VisualScriptPaletteEntry const& entry, ImDrawList* drawList, ImVec2 position, float scale);
		bool DrawSplitter(char const* id, ImVec2 size, bool vertical, float& delta);
		void DrawCanvas(ImVec2 size);
		void UpdateCanvas(VisualScriptCanvasInput& input);
		void AcceptDrops(VisualScriptCanvasInput& input);
		// Whether the pin a payload hovers at the mouse (screen pixels) cannot take it (CanDropOnVisualScriptPin).
		bool IsDropRefusedByPin(VisualScriptDropKind kind, uint64_t payload, glm::vec2 mouse);
		void DrawCanvasOverlays(ImVec2 canvasMin, ImVec2 canvasMax);
		void DrawHeaderTooltip();
		// A click on a node's problem badge, or on a link's, opens the popup of its problems.
		void TrackProblemBadges(VisualScriptCanvasInput const& input);
		void DrawProblemsPopup();
		void DrawLivePopup();
		void CommitLiveChange();
		// The chip of a node's input (its canvas name) as slot resolved it, resolved again when its key changed; null when the
		// window cannot edit it (the node or pin is gone, or the literal editor has no widget for its type).
		LiveChipData const* ResolveLiveChip(std::optional<LiveChipData>& slot, uint32_t node, std::string_view pin);
		// The live value widget (VisualScriptLiveChipHost): the literal editor's compact form over the chip.
		void DrawLiveChip(VisualScriptLiveChip const& chip) override;
		void DrawGestureHint(ImVec2 canvasMin, ImVec2 canvasMax);
		void DrawToasts(ImVec2 canvasMin, ImVec2 canvasMax);
		void DrawTextEdit();
		void DrawFocusFrame(VisualScriptWindowPart part);
		void UpdateCursor();

		// Menus.
		void DrawMenus();
		void DrawNodeMenu();
		void DrawPinMenu();
		void DrawLinkMenu();
		void DrawCommentMenu();
		void DrawAnchorMenu();
		void DrawElementDropMenu();
		void DrawViewMenuItems();
		void DrawSubGraphMenu();

		// Intents, edits and their commits.
		VisualScriptEditContext MakeContext(uint64_t mergeKey) const;
		// What the panes work with this frame.
		VisualScriptPaneContext MakePaneContext();
		// Applies what a pane asked for, in VisualScriptPaneResult's order.
		void ApplyPaneResult(VisualScriptPaneResult result);
		// A shortcut of class 1 (spec §8.3) for the part of the window that has the keyboard.
		void RouteFocusedAction(VisualScriptEditorAction action);
		// A row key (Delete, Rename, Duplicate, OpenNode) for the My Script row Details shows.
		void RouteRowAction(VisualScriptEditorAction action);
		// Shows My Script over the canvas when it is not beside it, for the rest of an interaction it has to draw.
		void OpenSidebarOverlay();
		// Shows the drawer in a mode (Problems or Find), or closes it.
		void ShowDrawer(VisualScriptDrawerMode mode);
		void ExecuteIntent(VisualScriptEditorIntent const& intent);
		void ExecuteEdit(VisualScriptCanvasEdit const& edit, uint64_t mergeKey = 0, VisualScriptDocument const* stored = nullptr);
		// Applies what the controller asked for; returns whether its commit was made.
		bool HandleResult(Result<VisualScriptControllerResult> result, std::string_view action);
		bool Commit(VisualScriptCommitRequest request, std::vector<ComponentEdit> edits);
		uint64_t GetNudgeKey();
		// The run the commit of a comment's title belongs to; null when it starts no run.
		NameRun const* FindNameRun();
		void CommitTextEdit();
		void CloseTextEdit();
		void OpenTextEdit(VisualScriptTextEditRequest const& request);
		void AddToast(std::string text, bool failure = false);
		// The actions the window handles itself (find, the shortcut sheet); false for the others.
		bool HandlePanelAction(VisualScriptEditorAction action);

		AssetHandle m_Graph;
		EditorOperations& m_Operations;
		// What the panes share from frame to frame (the My Script row shown, a setting to focus, a rename).
		VisualScriptPaneShared m_PaneShared;
		VisualScriptSemantics& m_Semantics;
		VisualScriptViewSettings& m_Settings;
		// The session every graph window shares: its widgets (the node comment edited in place, the panes' fields, the live
		// value widget) commit under its key, so one interaction is one step.
		EditSession& m_Session;
		VisualScriptEditorServices const& m_Services;
		std::string m_Title;
		// The name descriptions end with ("Move 3 Nodes in Player"): the window's title.
		std::string m_ScriptName;
		// What entity drops need of the edited scene.
		VisualScriptSceneContext m_SceneContext;

		VisualScriptEditorModel m_Model;
		VisualScriptCanvasRenderer m_Renderer;
		// Draws the palette's hover previews, so their counts never mix with the canvas's, from the preview's layout.
		VisualScriptCanvasRenderer m_PreviewRenderer;
		std::optional<NodePreview> m_NodePreview;
		VisualScriptBrushes m_Brushes;

		// The panes (spec §11).
		VisualScriptPalettePopup m_Palette;
		VisualScriptSidebar m_Sidebar;
		VisualScriptDetails m_Details;
		VisualScriptDrawer m_Drawer;
		VisualScriptFindBar m_FindBar;
		// The widget commits of the problems popups and of the canvas's live value widget, and its editor.
		VisualScriptWidgetCommits m_ProblemCommits;
		VisualScriptWidgetCommits m_LiveCommits;
		VisualScriptLiteralEditor m_LiveLiteral;
		// The chip whose live widget is in use (the active item), its change this frame, and its popup.
		std::optional<VisualScriptPinReference> m_ActiveChip;
		std::optional<LiveChange> m_LiveChange;
		bool m_LiveActive = false;
		bool m_LiveDeactivated = false;
		std::optional<LivePopup> m_LivePopup;
		// The live widget's chip and its popup's, resolved.
		std::optional<LiveChipData> m_LiveChipData;
		std::optional<LiveChipData> m_LivePopupData;
		// A widget interaction of the panes or the live widget was in progress in the last draw (the model's Sync paces its
		// analysis by it).
		bool m_WidgetActive = false;
		bool m_PanesWidgetActive = false;
		// The find bar's matches of the shown sub-graph, sorted, which the canvas outlines.
		std::vector<uint32_t> m_Outlined;
		// What a payload dragged over the canvas hovers (reused, so a drag allocates nothing from frame to frame).
		VisualScriptHit m_DropHit;
		// A left press on a problem badge (a node's ID, or a link), released where it was pressed to open its popup.
		std::optional<ProblemsPopup> m_BadgePress;
		glm::vec2 m_BadgePressPoint{0.0f};
		std::optional<ProblemsPopup> m_Problems;
		std::vector<VisualScriptDiagnostic> m_PopupProblems;
		std::vector<VisualScriptOpenRequest> m_OtherGraphRequests;
		// Whether the live widget was drawn this frame (the chip it edits may be gone), and where.
		bool m_LiveDrawn = false;
		std::optional<VisualScriptLiveWidgetFrames> m_LiveWidget;
		// The chip the live widget stood on in the last draw: it stays live while the mouse is over the widget, whose item
		// keeps the hover from the canvas's surface (and so from the model).
		std::optional<VisualScriptPinReference> m_LiveChip;
		std::optional<VisualScriptPinReference> m_HoveredChip;
		// What the frame syncs and draws with: the library (kept alive while the window is drawn, so a commit of the frame or
		// between frames resolves with the one the frame drew with; released while hidden), the measurer and the theme.
		Ref<VisualScriptLibrary const> m_Library;
		VisualScriptTextMeasurer m_Measure;
		VisualScriptThemeAnchors m_Theme;
		double m_Time = 0.0;
		// How many of the links the model reports committed this frame the canvas's draw pulsed.
		size_t m_PulsedLinks = 0;

		// Requests applied at the next draw.
		std::optional<VisualScriptOpenRequest> m_PendingOpen;
		std::vector<VisualScriptEditorAction> m_RequestedActions;
		bool m_FocusRequested = false;
		// The actions the frame routes to the canvas model (keys, the toolbar, the Edit menu).
		std::vector<VisualScriptEditorAction> m_FrameActions;

		// What the last draw saw.
		bool m_Visible = false;
		bool m_Focused = false;
		VisualScriptWindowPart m_FocusedPart = VisualScriptWindowPart::None;
		VisualScriptWindowLayout m_Layout;
		// The canvas's key owner, and the IDs of the child windows that can have the keyboard.
		ImGuiID m_CanvasOwner = 0;
		ImGuiID m_CanvasWindow = 0;
		ImGuiID m_SidebarWindow = 0;
		ImGuiID m_DetailsWindow = 0;
		bool m_WindowHovered = false;
		bool m_CanvasHovered = false;
		bool m_HintShown = false;
		bool m_ViewItemsInMenu = false;
		glm::vec2 m_CanvasMin{0.0f};
		glm::vec2 m_CanvasSize{0.0f};
		glm::vec2 m_LastMouse{0.0f};
		// The node whose header the mouse rests on, and since when (its tooltip waits ImGui's normal delay).
		std::optional<uint32_t> m_HoveredHeader;
		double m_HoveredHeaderSince = 0.0;
		std::vector<VisualScriptEditorAction> m_HiddenToolbarActions;

		// Where the last draw put the items tests and automation locate.
		std::array<std::optional<VisualScriptRect>, static_cast<size_t>(VisualScriptPanelItem::Count)> m_ItemRects{};
		VisualScriptPanelActivity m_Activity;

		// The notices, cards and hints of spec §10.4, as the last sync found them. The repaired file's notice is for the asset
		// object that was loaded so.
		std::vector<NoticeKind> m_Notices;
		VisualScriptCard m_Card = VisualScriptCard::None;
		std::weak_ptr<VisualScriptAsset> m_RepairAsset;
		uint32_t m_Repairs = 0;
		bool m_RepairNotice = false;
		// The nodes the next edit rewrites (canonical types and positions), counted for the document and the analysis last
		// seen (the model's analysis count plus one; 0 for none).
		uint32_t m_Normalizations = 0;
		uint64_t m_NormalizedAnalysis = ~uint64_t(0);
		Ref<VisualScriptDocument const> m_NormalizedDocument;
		uint64_t m_SeenRefusals = 0;
		double m_BlinkStart = -1.0e30;
		double m_SaveFlashStart = -1.0e30;
		// Functions whose new-function hint this window showed and left, or that were dismissed.
		std::vector<uint32_t> m_FunctionHintsSeen;
		std::optional<uint32_t> m_FunctionHint;
		bool m_CanvasHintShown = false;
		// The error a refused file's card names.
		std::string m_LoadError;
		// A card's [Close]: the window closes at the end of the draw.
		bool m_CloseRequested = false;
		// The heights the card and the first canvas's hint had at the last draw (they size themselves to their contents).
		float m_CardHeight = 0.0f;
		float m_RibbonHeight = 0.0f;

		// The context entity (spec §10.5): chosen by an open request, the views file or the user, else the only entity that
		// runs the graph while none is chosen.
		UUID m_ContextEntity = UUID::Invalid();
		std::optional<UUID> m_RequestedContext;
		bool m_ContextChosen = false;
		bool m_ContextResolved = false;
		bool m_InstanceShown = false;
		VisualScriptLiveState m_Live = VisualScriptLiveState::None;
		std::optional<ScriptFieldMap> m_LiveValues;
		double m_LiveRead = -1.0e30;

		// The Bookmarks menu: the names typed in its Rename submenu, by slot.
		std::array<std::string, 9> m_BookmarkNames;
		// The shortcut sheet is opened at the next draw; a press on "?" while it was open only closes it.
		bool m_OpenShortcuts = false;
		bool m_ShortcutsKeepClosed = false;

		// The window's state beside the model's.
		OpenMenu m_Menu;
		// The item picked in the menu, run once the menu is drawn.
		std::function<void()> m_MenuPick;
		std::optional<TextEdit> m_TextEdit;
		std::vector<Toast> m_Toasts;
		VisualScriptSaveState m_SaveState = VisualScriptSaveState::Saved;
		VisualScriptCommitTimings m_LastCommitTimings;
		VisualScriptDrawerMode m_DrawerMode = VisualScriptDrawerMode::Closed;
		float m_DrawerHeight = VisualScriptDrawerDefaultHeight;
		// The stacked column's share above its splitter (VisualScriptStackShare).
		float m_StackShare = VisualScriptStackShare;
		bool m_ShortcutsOpen = false;
		// The comment menu's custom colour while it is picked (sRGB), and its comment.
		std::optional<glm::vec4> m_CustomColor;
		uint32_t m_CustomColorComment = 0;
		bool m_SidebarOverlay = false;
		bool m_DetailsOverlay = false;

		// Merge keys: the arrow-key nudge run and the creation-and-name run.
		uint64_t m_NudgeRuns = 0;
		uint64_t m_NudgeKey = 0;
		double m_LastNudge = -1.0e30;
		uint64_t m_NameRuns = 0;
		std::optional<NameRun> m_NameRun;
	};
}
