#pragma once

#include "Editor/VisualScripting/VisualScriptBrushes.h"
#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"
#include "Editor/VisualScripting/VisualScriptCanvasRenderer.h"
#include "Editor/VisualScripting/VisualScriptController.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptEditorTypes.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/UUID.h"
#include "Strada/Script/ScriptField.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <glm/glm.hpp>
#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
		// The panes' widths; 0 for a pane that is not beside the canvas (hidden, or auto-hidden).
		float Sidebar = 0.0f;
		float Details = 0.0f;
		float Canvas = 0.0f;
		// Shown, Hidden (turned off) or AutoHidden (turned on, but the window is too narrow for it); never Overlay, which is
		// the window's state on top of AutoHidden.
		VisualScriptPaneState SidebarState = VisualScriptPaneState::Shown;
		VisualScriptPaneState DetailsState = VisualScriptPaneState::Shown;
	};

	// The layout rule of spec §10.1 rule 2: each shown pane takes its width (font sizes), at least its minimum and at most
	// VisualScriptPaneMaxShare of the window, a splitter of splitter pixels beside it; the canvas keeps at least
	// VisualScriptCanvasMinWidth font sizes, so in a window too narrow for that Details is auto-hidden first, then the sidebar
	// (the user's toggles do not change). Pure.
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
		inline constexpr std::string_view Degraded =
			"Node details need the scripting runtime, which is not running: you can arrange, comment, copy and delete nodes.";
		inline constexpr std::string_view ReadOnlyMemory = "Read only: this visual script has no file";
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

		inline constexpr std::array<std::string_view, 21> All = {
			Degraded,       ReadOnlyMemory, NewFunction,       EmptyGraph,       EmptyGraphHelp, Live,     LastWorking,     Simulating,
			SimulatingFull, LiveTooltip,    SimulatingTooltip, CanvasHint,       CanvasHintPan,  WireHint, MoveSnappedHint, MoveFreeHint,
			PanHint,        NoBookmarks,    ShortcutSheet,     "Instance: None", "Not saved"};
	}

	// The pane sizes of spec §10.1, in font sizes.
	inline constexpr float VisualScriptSidebarMinWidth = 10.0f;
	inline constexpr float VisualScriptDetailsMinWidth = 14.0f;
	inline constexpr float VisualScriptCanvasMinWidth = 40.0f;
	inline constexpr float VisualScriptDrawerDefaultHeight = 10.0f;
	inline constexpr float VisualScriptDrawerMinHeight = 4.0f;
	// A pane beside the canvas takes at most this share of the window; an overlay pane at most this share of the canvas.
	inline constexpr float VisualScriptPaneMaxShare = 0.4f;
	inline constexpr float VisualScriptOverlayMaxShare = 0.8f;
	// The splitters between the panes and the canvas, in logical pixels.
	inline constexpr float VisualScriptSplitterLogicalPixels = 4.0f;
	// The toolbar's breadcrumb keeps at least this many font sizes when the toolbar is too narrow for it.
	inline constexpr float VisualScriptBreadcrumbMinWidth = 8.0f;

	// The window of one open visual script (spec §10): a toolbar with the breadcrumb, the canvas between the My Script sidebar
	// and Details, the drawer under it, and the canvas's minimap. Each frame it syncs its canvas model with the asset, turns
	// the user's input into the model's input, has its controller resolve what the model asks to edit, executes each edit
	// as one step of the editor's history (EditorOperations::EditVisualScript) and draws the canvas. Only OnImGuiRender calls
	// ImGui; every other member records or reads state, so automation can drive a window between frames. Main thread only.
	class VisualScriptEditorPanel
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
		~VisualScriptEditorPanel();
		VisualScriptEditorPanel(VisualScriptEditorPanel const&) = delete;
		VisualScriptEditorPanel& operator=(VisualScriptEditorPanel const&) = delete;

		// Draws the window: SetNextWindowDockID(*dockTarget, ImGuiCond_FirstUseEver) when given, then Begin with the flags of
		// spec §8.1. A hidden window (a tab not in front) neither updates its model nor claims keys.
		void OnImGuiRender(bool& open, std::optional<ImGuiID> dockTarget);
		// Applied at the next draw: the sub-graph, selection and view (the model's), and the focus.
		void Apply(VisualScriptOpenRequest const& request);
		// An action of the Edit menu while this window is the edit target; applied at the next draw, as its key would be.
		void RequestAction(VisualScriptEditorAction action);
		// After an undo or redo while this window is the edit target: "Undone: <description>" or "Redone: <description>", with
		// [Show] when the step edited the scene (an invalid editedAsset) or another visual script.
		void NotifyHistoryStep(bool undone, std::string const& description, AssetHandle editedAsset);
		// Whether a widget of the window holds an edit it has not committed (the in-place text editor).
		bool HasPendingWidgetEdit() const;
		// Commits that edit now (spec §4.4 rule 2): the window manager calls it, before any window draws, for windows whose
		// widget is no longer active (hidden, closed, a project change).
		void FlushWidgetEdit();
		// What graph.editors reports of the window, as its last draw saw it.
		VisualScriptEditorState GetState() const;
		VisualScriptEditorModel const& GetModel() const { return m_Model; }
		AssetHandle GetGraph() const { return m_Graph; }
		// The title the window shows; empty shows the file's stem (the window manager names two graphs with one stem by their
		// folders). The window keeps its identity across titles.
		void SetTitle(std::string title) { m_Title = std::move(title); }
		// The palette the canvas asked for and the window has not closed: where its node goes, the pin of a wire released on
		// empty canvas, and the first characters typed.
		std::optional<VisualScriptPaletteIntent> const& GetPaletteRequest() const { return m_PaletteRequest; }
		// The canvas's renderer, as the last frame left it (its counters).
		VisualScriptCanvasRenderer const& GetRenderer() const { return m_Renderer; }
		// The part of the window that had the keyboard in the last draw.
		VisualScriptWindowPart GetFocusedPart() const { return m_FocusedPart; }
		// Screen pixels: where the last draw put the item; none when it drew none (a toolbar item the overflow moved into the
		// view menu, a notice not shown).
		std::optional<VisualScriptRect> GetItemRect(VisualScriptPanelItem item) const;
		VisualScriptPanelActivity const& GetActivity() const { return m_Activity; }
		// The context entity (spec §10.5): where entity drops write values and whose live values the window shows; invalid
		// when none.
		UUID GetContextEntity() const { return m_ContextEntity; }
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
			// [Show]: focuses the scene's view (an invalid handle) or the window of another visual script; with ShowElements,
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
			AnchorPins
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
			// Canvas units: where the menu was opened.
			glm::vec2 Position{0.0f};
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
		void DrawPane(char const* id, VisualScriptWindowPart part, ImVec2 size);
		bool DrawSplitter(char const* id, ImVec2 size, bool vertical, float& delta);
		void DrawCanvas(ImVec2 size);
		void UpdateCanvas(VisualScriptCanvasInput& input);
		void AcceptDrops(VisualScriptCanvasInput& input);
		void DrawCanvasOverlays(ImVec2 canvasMin, ImVec2 canvasMax);
		void DrawHeaderTooltip();
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
		void DrawViewMenuItems();
		void DrawSubGraphMenu();

		// Intents, edits and their commits.
		VisualScriptEditContext MakeContext(uint64_t mergeKey) const;
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
		VisualScriptBrushes m_Brushes;
		// What the frame syncs and draws with: the library (kept alive through the frame), the measurer and the theme.
		Ref<VisualScriptLibrary const> m_Library;
		VisualScriptTextMeasurer m_Measure;
		VisualScriptThemeAnchors m_Theme;
		double m_Time = 0.0;

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
		Ref<VisualScriptAsset> m_RepairAsset;
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
		std::optional<VisualScriptPaletteIntent> m_PaletteRequest;
		OpenMenu m_Menu;
		// The item picked in the menu, run once the menu is drawn.
		std::function<void()> m_MenuPick;
		std::optional<TextEdit> m_TextEdit;
		std::vector<Toast> m_Toasts;
		VisualScriptSaveState m_SaveState = VisualScriptSaveState::Saved;
		VisualScriptDrawerMode m_DrawerMode = VisualScriptDrawerMode::Closed;
		float m_DrawerHeight = VisualScriptDrawerDefaultHeight;
		bool m_FindOpen = false;
		std::string m_FindText;
		bool m_ShortcutsOpen = false;
		// The comment menu's custom colour while it is picked (sRGB), and its comment.
		std::optional<glm::vec4> m_CustomColor;
		uint32_t m_CustomColorComment = 0;
		bool m_SidebarOverlay = false;
		bool m_DetailsOverlay = false;
		std::optional<VisualScriptReference> m_FindReferences;
		std::optional<VisualScriptReveal> m_Reveal;

		// Merge keys: the arrow-key nudge run and the creation-and-name run.
		uint64_t m_NudgeRuns = 0;
		uint64_t m_NudgeKey = 0;
		double m_LastNudge = -1.0e30;
		uint64_t m_NameRuns = 0;
		std::optional<NameRun> m_NameRun;
	};
}
