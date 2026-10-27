#pragma once

#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"
#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptEditorTypes.h"
#include "Editor/VisualScripting/VisualScriptGeometry.h"
#include "Editor/VisualScripting/VisualScriptHitTest.h"
#include "Editor/VisualScripting/VisualScriptSemantics.h"
#include "Editor/VisualScripting/VisualScriptSpatialIndex.h"
#include "Editor/VisualScripting/VisualScriptTheme.h"
#include "Editor/VisualScripting/VisualScriptView.h"
#include "Editor/VisualScripting/VisualScriptViewSettings.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/UUID.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Strada
{
	// The state of a graph window's document, as each Sync finds its asset.
	enum class VisualScriptDocumentState : uint8_t
	{
		Editable = 0,
		// A memory asset: no file to write.
		ReadOnly,
		// A file without write permission.
		ReadOnlyFile,
		// The asset's file is missing.
		Missing,
		// The file does not load.
		Unloadable,
		// The handle is no longer registered (the asset was deleted): the window closes.
		Gone
	};

	// What a Sync found.
	struct VisualScriptSyncResult
	{
		VisualScriptDocumentState State = VisualScriptDocumentState::Editable;
		// The document drawn changed: an own commit, an outside change, a widget preview.
		bool DocumentChanged = false;
		// The analysis changed: a new one, or the last one kept without its problems.
		bool AnalysisChanged = false;
		// The visual model's geometry changed and the spatial index was rebuilt.
		bool GeometryChanged = false;
	};

	// --- Input ---

	// What a drag-and-drop payload dropped on the canvas holds.
	enum class VisualScriptDropKind : uint8_t
	{
		// DragDropPayload::Asset: an asset of the project.
		Asset = 0,
		// DragDropPayload::Entities: entities of the scene.
		Entities,
		// DragDropPayload::VisualScriptElement: a row of a script's My Script pane.
		Element
	};

	// A payload the panel accepted over the canvas this frame (its drop target is the canvas's surface item).
	struct VisualScriptCanvasDrop
	{
		VisualScriptDropKind Kind = VisualScriptDropKind::Asset;
		// Asset: the asset's handle; Element: the row, as the pane that made the payload encodes it.
		uint64_t Payload = 0;
		// Entities: the entities' UUIDs.
		std::vector<uint64_t> Entities;
	};

	// VisualScriptDropIntent::Modifiers: the keys held at the drop.
	namespace VisualScriptDropModifiers
	{
		inline constexpr uint8_t Ctrl = 1u << 0;
		inline constexpr uint8_t Shift = 1u << 1;
		inline constexpr uint8_t Alt = 1u << 2;
	}

	// What the canvas sees of the user's input in a frame, as the panel reads it from ImGui's IO and the canvas's items. Mouse
	// buttons are indexed left, right, middle. "Ctrl" is Cmd on macOS (ImGui swaps them).
	struct VisualScriptCanvasInput
	{
		// Screen pixels.
		glm::vec2 Mouse{0.0f};
		// Where the canvas is on the screen, in pixels.
		glm::vec2 CanvasMin{0.0f};
		glm::vec2 CanvasSize{0.0f};
		// ImGuiStyle::FontScaleDpi.
		float UIScale = 1.0f;
		// ImGui::GetFontSize(): edge auto-pan's band (2 font sizes) and its top speed (20 font sizes a second) are measured in
		// it.
		float FontSize = 15.0f;
		// ImGuiStyle::CurveTessellationTol: wires are hit and cut where they are drawn.
		float CurveTessellationTolerance = 1.25f;
		// The canvas itself is under the mouse: not a popup, the live widget or another window over it. A press starts nothing
		// on the canvas while it is not.
		bool Hovered = false;
		// The canvas has the keyboard: Space and typed characters count only then.
		bool Focused = false;
		std::array<bool, 3> Down{};
		// Presses on the canvas this frame (on its surface item).
		std::array<bool, 3> Pressed{};
		std::array<bool, 3> Released{};
		// Second presses of a double-click (each comes with its Pressed).
		std::array<bool, 3> DoubleClicked{};
		// The mouse buttons X1 and X2 (back and forward) pressed over the window.
		std::array<bool, 2> ExtraPressed{};
		// ImGuiIO::MouseWheelH and MouseWheel over the canvas: positive scrolls left and up.
		glm::vec2 Wheel{0.0f};
		bool Ctrl = false;
		bool Shift = false;
		bool Alt = false;
		bool SpaceDown = false;
		bool SpacePressed = false;
		bool SpaceReleased = false;
		// UTF-8 typed this frame, as ImGuiIO::InputQueueCharacters holds it (the Space key's own space included): read while
		// Space is down, and as the palette's first query in the frame OpenPalette is routed. Spaces and control characters
		// never start a query.
		std::string Characters;
		// The live widget owns the mouse: no gesture starts.
		bool WidgetActive = false;
		// ImGuiIO::WantTextInput: a text field has the keyboard.
		bool TextInput = false;
		// Seconds, the clock of Sync's time (animations, the analysis' pace, the Space tap, hover delays).
		double Time = 0.0;
		// Shortcuts and menu picks routed to the canvas, in the order they came.
		std::vector<VisualScriptEditorAction> Actions;
		// A payload dropped on the canvas this frame.
		std::optional<VisualScriptCanvasDrop> Drop;
	};

	// --- Intents: the model's only output that changes documents ---
	//
	// The panel hands each to its controller, which turns it into at most one commit. Node, comment and link references name
	// the shown sub-graph's elements as the display document stores them, with pin names as the canvas knows them (an
	// analysis' new name of a renamed pin; the commit's canonical rewrite renames the rest of the node with it).

	// Moves nodes and comments (one drag, nudges come as actions).
	struct VisualScriptMoveIntent
	{
		// Node and comment IDs: the selection, and the nodes and comments inside its moved comments when the drag started.
		std::vector<uint32_t> Elements;
		// Canvas units. With Snap the primary element's top-left lands on the grid (VisualScriptCanvasMetrics::GridSize); the
		// others keep their offsets to it.
		glm::vec2 Offset{0.0f};
		bool Snap = true;
	};

	// Connects an output to an input.
	struct VisualScriptConnectIntent
	{
		VisualScriptLink Link;
		// The pins cannot connect, but a Convert node to ConvertTo bridges them: it is added at the wire's middle and linked
		// to both.
		bool InsertConvert = false;
		std::string ConvertTo;
	};

	// Links picked up from a pin with Ctrl+drag: Remove are the links picked, Add their moved forms (none when they were
	// dropped on empty canvas).
	struct VisualScriptRelinkIntent
	{
		std::vector<VisualScriptLink> Remove;
		std::vector<VisualScriptLink> Add;
	};

	struct VisualScriptDisconnectIntent
	{
		std::vector<VisualScriptLink> Links;
	};

	struct VisualScriptResizeCommentIntent
	{
		uint32_t Comment = 0;
		// Canvas units: the comment's new position (Min) and size.
		VisualScriptRect Rect;
	};

	struct VisualScriptInsertRerouteIntent
	{
		VisualScriptLink Link;
		// The reroute's position (its top-left): its dot's centre on the wire's point nearest the click, snapped as moves are.
		glm::vec2 Position{0.0f};
	};

	// A dragged node dropped on a wire: Link becomes Link.From -> Node.InputPin and Node.OutputPin -> Link.To, and the node
	// moves by Offset, in one step.
	struct VisualScriptSpliceIntent
	{
		uint32_t Node = 0;
		VisualScriptLink Link;
		std::string InputPin;
		std::string OutputPin;
		glm::vec2 Offset{0.0f};
	};

	// Opens the palette.
	struct VisualScriptPaletteIntent
	{
		// Canvas units: where the node goes.
		glm::vec2 Position{0.0f};
		// Context mode: the pin a dragged wire was released from, of Direction.
		std::optional<VisualScriptPinReference> From;
		VisualScriptPinDirection Direction = VisualScriptPinDirection::Out;
		// The search field's first text: characters typed while Space was down, or in the frame Tab opened it.
		std::string Query;
	};

	// A right click on an element (a press and release within the click threshold): the panel opens that element's menu.
	struct VisualScriptContextMenuIntent
	{
		// What was under the mouse at the press. A pin's Target.Pin is its index in the visual model as laid out in the frame
		// the intent is emitted (found again by name at the release); a menu that stays open over later frames keeps Pin below.
		VisualScriptHit Target;
		glm::vec2 ScreenPosition{0.0f};
		// Pin: the pin by its name as the canvas knows it (as intents name pins), of PinDirection. WholeSide: it is a collapsed
		// node's anchor, which stands for every pin of its side (Pin names the side's first pin).
		std::optional<VisualScriptPinReference> Pin;
		VisualScriptPinDirection PinDirection = VisualScriptPinDirection::In;
		bool WholeSide = false;
	};

	// A keyboard or menu action that edits, or that the panel handles (find, the shortcut sheet).
	struct VisualScriptActionIntent
	{
		VisualScriptEditorAction Action = VisualScriptEditorAction::Delete;
		// The shown sub-graph's selection, the primary element first.
		std::vector<uint32_t> Selection;
		// Canvas units: where pasted and created elements go (the mouse over the canvas, else the view's centre).
		glm::vec2 Position{0.0f};
	};

	// A double-click on a call of a function of the document: show that function's graph.
	struct VisualScriptOpenFunctionIntent
	{
		uint32_t Function = 0;
	};

	struct VisualScriptDropIntent
	{
		VisualScriptDropKind Kind = VisualScriptDropKind::Asset;
		uint64_t Payload = 0;
		std::vector<uint64_t> Entities;
		// Canvas units.
		glm::vec2 Position{0.0f};
		// The pin it was dropped on, of Direction; none for the canvas.
		std::optional<VisualScriptPinReference> Pin;
		VisualScriptPinDirection Direction = VisualScriptPinDirection::In;
		// VisualScriptDropModifiers.
		uint8_t Modifiers = 0;
	};

	// A press on a collapsed node's anchor that stands for several pins of its side: the panel lists those pins and starts
	// the wire from the one chosen (VisualScriptEditorModel::StartWire).
	struct VisualScriptPinMenuIntent
	{
		uint32_t Node = 0;
		VisualScriptPinDirection Direction = VisualScriptPinDirection::In;
		glm::vec2 ScreenPosition{0.0f};
	};

	using VisualScriptEditorIntent =
		std::variant<VisualScriptMoveIntent, VisualScriptConnectIntent, VisualScriptRelinkIntent, VisualScriptDisconnectIntent,
	                 VisualScriptResizeCommentIntent, VisualScriptInsertRerouteIntent, VisualScriptSpliceIntent, VisualScriptPaletteIntent,
	                 VisualScriptContextMenuIntent, VisualScriptActionIntent, VisualScriptOpenFunctionIntent, VisualScriptDropIntent,
	                 VisualScriptPinMenuIntent>;

	// Whether an action edits the document: the model emits none of these in read-only states. Copy, find, the shortcut
	// sheet, opening a node and the actions the model applies itself (view, selection, navigation, bookmarks) do not.
	[[nodiscard]] bool IsVisualScriptEditAction(VisualScriptEditorAction action);

	// --- What the model reports ---

	enum class VisualScriptSelectMode : uint8_t
	{
		// The elements become the selection, the first one primary.
		Replace = 0,
		// The elements join it; a single element given becomes primary.
		Add,
		// Each element joins it or leaves it; a single element that joins becomes primary.
		Toggle,
		Remove
	};

	enum class VisualScriptGesture : uint8_t
	{
		Idle = 0,
		Panning,
		Marquee,
		MovingElements,
		ResizingComment,
		// A new wire from a pin.
		Wiring,
		// The links of a pin picked up with Ctrl+drag.
		Relinking,
		// Ctrl+right drag: the cut line.
		Cutting,
		MinimapDrag
	};

	// How a pin is classed against the pin a wire is dragged from (VisualScriptGestureView::PinClasses).
	enum class VisualScriptPinClass : uint8_t
	{
		// Not classed (no wire is dragged, or the pin was not on the screen yet).
		None = 0,
		// Connects with the same type (or a wildcard).
		Compatible,
		// Connects through an implicit conversion.
		Converting,
		// Does not connect (a Convert node may still bridge it when the wire is released on it).
		Incompatible,
		// The pin the wire snaps to.
		Snapped
	};

	// What the renderer draws of the gesture in progress (all canvas units). Elements are named by ID; the moved and expanded
	// nodes by index are VisualScriptEditorModel::GetNodePreview's. A view of the model's own state, made without allocating:
	// its spans and pointers stay valid until the model's next Sync, Update, CancelGesture or StartWire (copy what is kept
	// longer).
	struct VisualScriptGestureView
	{
		// Also picks the gesture's hint at the canvas's bottom.
		VisualScriptGesture Kind = VisualScriptGesture::Idle;
		// Space is held over the focused canvas: the hand cursor, and a left drag pans.
		bool SpaceHeld = false;
		// The ID of a collapsed node drawn expanded under the dragged wire, once the wire rested on it.
		std::optional<uint32_t> ExpandedNode;
		// The dragged wire's control points, from its output end to its input end.
		std::optional<std::array<glm::vec2, 4>> GhostWire;
		// The colours of its output and input ends (different when it snaps onto a converting pin or a Convert bridges it).
		uint32_t GhostColorFrom = 0;
		uint32_t GhostColorTo = 0;
		// Over a pin it cannot connect to, which no Convert bridges: drawn in the Error colour, dashed, with the not-allowed
		// cursor and a tooltip naming both types (the pin under the mouse is GetHover()'s, the wire's own is WireSource).
		bool GhostInvalid = false;
		// Wiring: the pin the wire is dragged from, by its name as the canvas knows it (as intents name pins), of
		// WireDirection; null for other gestures.
		VisualScriptPinReference const* WireSource = nullptr;
		VisualScriptPinDirection WireDirection = VisualScriptPinDirection::Out;
		std::optional<VisualScriptRect> Marquee;
		// From the press to the mouse.
		std::optional<std::pair<glm::vec2, glm::vec2>> CutLine;
		// While a wire is dragged: per pin of the visual model (by index, as VisualScriptCanvasModel::GetPins), a
		// VisualScriptPinClass. Every pin on the screen is classed; pins never on the screen during the drag stay None. Empty
		// for other gestures.
		std::span<uint8_t const> PinClasses;
		// The move preview: node and comment IDs, drawn moved by MoveOffset.
		std::span<uint32_t const> MovedElements;
		glm::vec2 MoveOffset{0.0f};
		// The wire a dragged node rests on, highlighted: releasing splices the node into it. Null when there is none.
		VisualScriptLink const* SpliceTarget = nullptr;
		// The wires the cut line crosses, drawn dashed.
		std::span<VisualScriptLink const> CutLinks;
		// The nodes and comments the marquee would apply to, ascending IDs: drawn with hover outlines.
		std::span<uint32_t const> MarqueeElements;
		// A comment being resized: drawn at ResizedRect; ResizeEdges are VisualScriptGeometry::LeftEdge... (the resize cursor).
		std::optional<uint32_t> ResizedComment;
		VisualScriptRect ResizedRect;
		uint8_t ResizeEdges = 0;
		// Relinking: the links picked up (their wires are not drawn) and their ghosts, from each far end to the mouse or the
		// pin it snaps to, in the same order.
		std::span<VisualScriptLink const> PickedLinks;
		std::span<std::array<glm::vec2, 4> const> PickedWires;
	};

	// The minimap as the model hit-tests it and the renderer draws it.
	struct VisualScriptMinimapState
	{
		VisualScriptMinimapTransform Transform;
		// The fade: 0 hidden (not hit), 1 shown. It is shown while turned on and some node or comment is out of view.
		float Alpha = 0.0f;
	};

	// What the window shows besides the canvas, so tests and graph.editors observe it without screenshots.
	enum class VisualScriptNotice : uint8_t
	{
		Degraded = 0,
		Normalization,
		Repaired,
		ReadOnlyMemory,
		ReadOnlyFile,
		NewFunction,
		FirstCanvas,
		GestureHint
	};

	enum class VisualScriptCard : uint8_t
	{
		None = 0,
		Missing,
		Unloadable,
		EmptyGraph
	};

	enum class VisualScriptLiveState : uint8_t
	{
		None = 0,
		Live,
		LastWorking,
		Simulating
	};

	enum class VisualScriptSaveState : uint8_t
	{
		Saved = 0,
		NotSaved
	};

	enum class VisualScriptDrawerMode : uint8_t
	{
		Closed = 0,
		Problems,
		Find
	};

	enum class VisualScriptPaneState : uint8_t
	{
		Hidden = 0,
		Shown,
		AutoHidden,
		Overlay
	};

	// What graph.editors reports of a window. The model fills the first block (to Warnings) and the members it keeps
	// (Bookmarks, MinimapVisible); the panel fills the rest. Both are what the last draw saw, so reading them needs no ImGui.
	struct VisualScriptEditorState
	{
		AssetHandle Graph;
		// The asset's path (a memory asset's name).
		std::string Path;
		uint32_t Function = 0;
		VisualScriptViewState View;
		VisualScriptDetailLevel Level = VisualScriptDetailLevel::Detail;
		// In selection order, the primary element first.
		std::vector<uint32_t> SelectedNodes;
		std::vector<uint32_t> SelectedComments;
		VisualScriptDocumentState Document = VisualScriptDocumentState::Editable;
		bool Analyzed = false;
		// The analysis' problems in the whole document.
		uint32_t Errors = 0;
		uint32_t Warnings = 0;

		bool Focused = false;
		bool Visible = false;
		UUID ContextEntity = UUID::Invalid();
		bool PaletteOpen = false;
		std::string PaletteQuery;
		// Notices and banners shown, top to bottom.
		std::vector<VisualScriptNotice> Notices;
		// The card over the canvas.
		VisualScriptCard Card = VisualScriptCard::None;
		// The toasts shown, oldest first.
		std::vector<std::string> Toasts;
		// The toolbar's pill.
		VisualScriptLiveState Live = VisualScriptLiveState::None;
		// The toolbar's save status.
		VisualScriptSaveState Save = VisualScriptSaveState::Saved;
		VisualScriptDrawerMode Drawer = VisualScriptDrawerMode::Closed;
		// The find bar.
		bool FindOpen = false;
		std::string FindText;
		uint32_t FindIndex = 0;
		uint32_t FindCount = 0;
		// The bookmark slots stored (1 to 9), ascending.
		std::vector<uint8_t> Bookmarks;
		// The minimap is drawn (not faded out).
		bool MinimapVisible = false;
		VisualScriptPaneState Sidebar = VisualScriptPaneState::Shown;
		VisualScriptPaneState Details = VisualScriptPaneState::Shown;
		// Both panes are shown in one column beside the canvas, My Script above Details (the window is too narrow for them
		// side by side).
		bool PanesStacked = false;
		// The shortcut sheet.
		bool ShortcutsOpen = false;
		// Toolbar actions the overflow rule moved into the view menu.
		std::vector<VisualScriptEditorAction> HiddenToolbarActions;
	};

	// The model of one graph window: which document it shows and how (the asset's document, with its unplaced nodes placed in
	// memory), the analysis policy, the visual model and its spatial index, and the interaction: the view, the selection,
	// the gestures, and the intents they end in. Pure (no ImGui): each frame the panel calls Sync, then Update with its input,
	// hands the intents to its controller, and draws from the queries. Main thread only.
	//
	// Documents are told apart by identity. The window's own commits come back as the very object the commit wrote
	// (OnCommitted); any other new document is an outside change (undo, redo, automation, another window, a reload), which
	// cancels the gesture in progress, drops a widget preview and records the elements it changed for the renderer to flash.
	// After every change the selection and the raised nodes keep only the elements that still exist.
	//
	// Analysis is a synchronous round trip to the scripting runtime, so it runs only when the program changes
	// (HaveSameVisualScriptProgram: moves, comments and collapsing never analyze) or the library changes, at most once per
	// frame, at most every AnalysisInterval while a widget is being used, never during a gesture, and never without the
	// runtime; and at once, whatever the pace, when something needs the pins: a wire starts, the palette opens, or a delete,
	// cut, paste or Update Node is emitted. Nodes changed since the last analysis take their library template's pins until the
	// next one.
	//
	// Each sub-graph keeps its view and selection while the window shows another, and the window keeps a navigation history of
	// views (sub-graph, centre, zoom): every sub-graph switch and every jump (open requests, bookmarks, Go to Event) pushes the
	// view it leaves. Views are kept in the per-user settings as they settle; a sub-graph without a view kept is framed whole.
	class VisualScriptEditorModel
	{
	public:
		// While a widget is being used, the analysis runs at most this often (the previous one keeps drawing).
		static constexpr double AnalysisInterval = 0.25;
		// The navigation history keeps this many views back.
		static constexpr size_t MaxHistory = 64;

		// settings is the per-user view settings the window's views are kept in; it must outlive the model.
		VisualScriptEditorModel(AssetHandle graph, VisualScriptViewSettings& settings);

		// Looks the asset up (never keeping it across frames as the document's source), adopts a changed document, analyzes
		// as the policy says, and brings the visual model and the index up to date. time is in seconds (ImGui's time): two
		// Syncs given the same time are one frame. semantics must stay valid until the next Sync (the model analyzes and lays
		// out in Update when a gesture needs pins). The measurer is copied at every Sync and that copy is what lays out until
		// the next Sync, so what its function captures must stay valid until then; a measurer whose captures fit
		// std::function's small buffer (a font pointer and two floats) copies without allocating. A new Signature lays every
		// node out anew; the same Signature promises the same widths.
		VisualScriptSyncResult Sync(VisualScriptSemantics& semantics, VisualScriptTextMeasurer const& measure,
		                            VisualScriptThemeAnchors const& theme, bool widgetActive, double time);
		// Handles a frame of input after Sync: hover, view moves, the Space rule, routed actions, gestures and drops. Returns
		// what the frame asks to edit (and the menus and palette it opens), in order. Selection, view, navigation and bookmark
		// actions apply here and are not undoable. While a gesture or a mouse press is in progress, routed actions other than
		// Cancel (which ends it) and the mouse's back and forward buttons are ignored.
		std::vector<VisualScriptEditorIntent> Update(VisualScriptCanvasInput const& input);
		// The document the panel's commit just wrote (the request's document, the object the asset now holds). Call Sync again
		// in the same frame: it then adopts it as the window's own change.
		void OnCommitted(Ref<VisualScriptDocument const> committed);
		// Ends the gesture in progress without an intent: its preview is dropped (a failed commit, the window hidden).
		void CancelGesture();
		// Applies an open request: its sub-graph at once (when it has one), its selection and view at the next Update, once the
		// canvas shows that sub-graph: framing first, then its zoom about the framed centre, then its centre. Animated, except
		// for the window's first view and with Reduce Motion. The request's other members are the panel's.
		void ApplyOpenRequest(VisualScriptOpenRequest const& request);
		// Shows a sub-graph (0 for the event graph, else a function's ID; an ID the document does not have is ignored), keeping
		// the view and selection of the one left, and pushing its view on the history. The next Sync lays it out.
		void ShowFunction(uint32_t function);
		// Changes the shown sub-graph's selection; IDs of elements not on the canvas are ignored.
		void Select(std::span<uint32_t const> elements, VisualScriptSelectMode mode = VisualScriptSelectMode::Replace);
		// The document as a widget's uncommitted value makes it, drawn (and analyzed at the policy's pace) until the widget
		// commits or ClearWidgetPreview; edits are still computed on GetDisplayDocument.
		void SetWidgetPreview(VisualScriptDocument preview);
		void ClearWidgetPreview();
		// Starts a wire from a pin (a pin chosen in the menu of a VisualScriptPinMenuIntent): it follows the mouse with the
		// button up and ends at the next left press on the canvas, as a dragged wire ends at its release; a right press or
		// Cancel drops it. Nothing in read-only states, without the scripting runtime, or for a pin the node does not have.
		void StartWire(uint32_t node, std::string_view pin, VisualScriptPinDirection direction);
		// Checks again, at the next Sync, whether the graph's file can be written: when the window gains focus, after a write
		// failed and on the read-only banner's Retry. (Sync checks by itself when the window opens and when the version
		// changes.)
		void CheckWritable();

		// Queries (the renderer, the panes, the panel, tests and graph.editors).
		//
		// The first block of graph.editors' state (VisualScriptEditorState).
		VisualScriptEditorState GetState() const;
		VisualScriptViewState const& GetViewState() const { return m_View.GetState(); }
		VisualScriptView const& GetView() const { return m_View; }
		VisualScriptCanvasModel const& GetCanvas() const { return m_Canvas; }
		VisualScriptSpatialIndex const& GetIndex() const { return m_Index; }
		// The asset's document with its unplaced nodes placed: what edits are computed on. Empty before the first document.
		VisualScriptDocument const& GetDisplayDocument() const;
		// The asset's own document object, as last seen; null before the first.
		Ref<VisualScriptDocument const> const& GetStoredDocument() const { return m_Document; }
		// Null without the scripting runtime (the last analysis is then kept for its pins only) and before the first analysis.
		VisualScriptAnalysis const* GetAnalysis() const;
		// The type of a pin of any node of the display document, as the canvas knows it (the analysis while the node is what
		// it analyzed, else the library's template, else "*" for the pins its links and literals name); nothing for a pin the
		// node does not have (an orphan included). Valid until the next Sync.
		VisualScriptPinTypeLookup MakePinTypeLookup() const;
		// The shown sub-graph's selected node and comment IDs, the primary element first.
		std::span<uint32_t const> GetSelection() const { return m_Selection; }
		// What the mouse was over at the last Update: Kind None when the canvas was not hovered and no gesture ran, and while a
		// gesture carries the mouse beyond the canvas's edges (nothing is drawn there).
		VisualScriptHit const& GetHover() const { return m_Hover; }
		VisualScriptGestureView GetGesture() const;
		// In every state but Editable the model emits no edit intents; viewing, selecting and copying still work.
		bool IsReadOnly() const { return m_State != VisualScriptDocumentState::Editable; }
		// The nodes and comments of the shown graph that the last Sync's outside change added or changed (their stored data).
		std::span<uint32_t const> ChangedElements() const { return m_ChangedElements; }
		// How many analyses ran: for tests of the policy.
		uint64_t GetAnalysisCount() const { return m_AnalysisCount; }
		// Counts the visual model's Syncs: what is cached by its node, pin and link indices (and their texts) is valid while it
		// stays the same.
		uint64_t GetCanvasRevision() const { return m_CanvasRevision; }
		// Bytes the visual model, the index and the model's own buffers have reserved: an idle frame does not grow it.
		size_t GetCapacityBytes() const;

		AssetHandle GetGraph() const { return m_Graph; }
		// The sub-graph shown: 0 for the event graph, else a function's ID. A function deleted while shown shows the event graph.
		uint32_t GetFunction() const { return m_Function; }
		VisualScriptDocumentState GetDocumentState() const { return m_State; }
		// Per node of the visual model (by index, as GetCanvas().GetNodes()): non-zero for the selected ones; and each node's
		// raise counter. The renderer sorts the nodes it draws by GetVisualScriptNodeDrawKey with them, as hit testing does.
		std::span<uint8_t const> GetNodeSelection() const { return m_NodeSelected; }
		std::span<uint32_t const> GetNodeRaise() const { return m_NodeRaise; }
		// Per comment of the visual model (by index, as GetCanvas().GetComments()): non-zero for the selected ones.
		std::span<uint8_t const> GetCommentSelection() const { return m_CommentSelected; }
		// The nodes drawn moved (the move preview) and expanded (under a dragged wire) this frame, by node index: what the
		// renderer passes to VisualScriptGeometry. Valid until the next Sync or Update.
		VisualScriptNodePreview GetNodePreview() const;
		// The spatial index's dynamic items this frame, by index of the visual model: the moved nodes and comments, the comment
		// being resized, a collapsed node drawn expanded under a dragged wire, and the wires whose shape follows them (their
		// own, and every wire of a reroute whose first target is one of those nodes, since the reroute may turn). Index
		// queries leave them out, as their stored bounds are not where they are drawn. Valid until the next Sync or Update.
		std::span<uint32_t const> GetDynamicNodes() const { return m_DynamicNodes; }
		std::span<uint32_t const> GetDynamicComments() const { return m_DynamicComments; }
		std::span<uint32_t const> GetDynamicLinks() const { return m_DynamicLinks; }
		// What is drawn within a canvas rectangle this frame, as VisualScriptSpatialIndex items of the kinds in kindMask: the
		// index's query, and the dynamic items whose bounds where they are drawn (GetNodePreview, the gesture's move offset and
		// resized rectangle) intersect it. Replaces out's contents. The renderer culls with it.
		void QueryDrawn(VisualScriptRect const& rect, uint8_t kindMask, std::vector<uint32_t>& out,
		                VisualScriptIndexStats* stats = nullptr) const;
		VisualScriptMinimapState const& GetMinimap() const { return m_Minimap; }
		// The links this window's own commit added to the shown graph, reported for the frame of that commit's Sync (the
		// renderer pulses them).
		std::span<VisualScriptLink const> CommittedLinks() const { return m_CommittedLinks; }
		// Whether Back and Forward have somewhere to go.
		bool CanGoBack() const { return !m_Back.empty(); }
		bool CanGoForward() const { return !m_Forward.empty(); }
		// How many gestures and keys that need the scripting runtime were refused without it (a wire, the palette, a reroute
		// on a wire): the panel blinks the degraded notice when it grows.
		uint64_t GetRefusedCount() const { return m_RefusedCount; }

	private:
		using IdIndex = std::pair<uint32_t, uint32_t>;

		// What a press on the canvas does when it is released as a click, or becomes once it is dragged.
		enum class PressKind : uint8_t
		{
			// Nothing more.
			Inert = 0,
			// Click: clears the selection; drag: the marquee.
			Background,
			// A node, a reroute, a badge or a comment's title bar. Click: the selection decided on release; drag: the move.
			Element,
			// Click: nothing more; drag: the resize.
			CommentEdge,
			// Drag: a wire, or with Ctrl on a linked pin the relink.
			Pin,
			// Alt+click: breaks the pin's links, or the link.
			AltPin,
			AltLink,
			// Click: the element's menu or the palette; drag: the pan, or with Ctrl the cut line.
			Right
		};

		struct PressState
		{
			bool Active = false;
			int Button = 0;
			PressKind Kind = PressKind::Inert;
			glm::vec2 Screen{0.0f};
			glm::vec2 Canvas{0.0f};
			// The farthest the mouse has been from the press, in screen pixels.
			float MaxDistance = 0.0f;
			VisualScriptHit Hit;
			bool Ctrl = false;
			bool Shift = false;
			bool Alt = false;
			// Element: the click (no drag) replaces the selection with it, or with Ctrl removes it.
			bool DecideOnRelease = false;
			// Pin, and a right press on a pin: the pin's node, name as the canvas knows it, and direction, and whether it is
			// linked. Kept by name, since a layout during the press (an analysis, a commit) may move pin indices.
			uint32_t PinNode = 0;
			std::string Pin;
			VisualScriptPinDirection Direction = VisualScriptPinDirection::In;
			bool PinLinked = false;
		};

		// A pin of the shown graph by the names intents use; resolved to the visual model's indices each frame.
		struct PinKey
		{
			uint32_t Node = 0;
			std::string Pin;
			VisualScriptPinDirection Direction = VisualScriptPinDirection::In;
		};

		struct MoveState
		{
			// Node and comment IDs; the node IDs among them.
			std::vector<uint32_t> Elements;
			std::vector<uint32_t> Nodes;
			uint32_t Comments = 0;
			// The primary element's position when the drag started.
			glm::vec2 PrimaryOrigin{0.0f};
			glm::vec2 Offset{0.0f};
			bool Snap = true;
			// A single unlinked node over a wire: the wire it rests on and since when, and the wire it splices into.
			std::optional<VisualScriptLink> SpliceCandidate;
			double SpliceSince = 0.0;
			std::optional<VisualScriptLink> SpliceTarget;
			std::string SpliceInput;
			std::string SpliceOutput;
		};

		struct WireState
		{
			PinKey Source;
			// The same pin, as the gesture view names it.
			VisualScriptPinReference SourceReference;
			// Its library pin (the analysis' or the template's), which stays valid during the gesture: no analysis runs then.
			VisualScriptPin const* SourceInfo = nullptr;
			// Started by StartWire: ends at the next press, not at a release.
			bool EndsOnPress = false;
			// The visual model's pins the classes are for (a layout during the gesture classes them anew).
			uint64_t CanvasRevision = 0;
			// Per pin of the visual model: a VisualScriptPinClass, or Unclassified (not classed yet); and what the gesture view
			// shows of them, None for those not classed and Snapped for the one the wire snaps to.
			std::vector<uint8_t> Classes;
			std::vector<uint8_t> Shown;
			// The pin index the wire snaps to.
			std::optional<uint32_t> Snapped;
			// A collapsed node the wire rests over (ID) and since when; the one drawn expanded.
			std::optional<uint32_t> Resting;
			double RestingSince = 0.0;
			std::optional<uint32_t> Expanded;
			std::array<glm::vec2, 4> Ghost{};
			uint32_t ColorFrom = 0;
			uint32_t ColorTo = 0;
			bool Invalid = false;
		};

		struct RelinkState
		{
			PinKey Pin;
			// The links picked up, and the far end of each.
			std::vector<VisualScriptLink> Links;
			std::vector<PinKey> FarEnds;
			std::vector<std::array<glm::vec2, 4>> Wires;
			std::optional<uint32_t> Snapped;
		};

		struct ResizeState
		{
			uint32_t Comment = 0;
			VisualScriptRect Original;
			VisualScriptRect Rect;
			uint8_t Edges = 0;
		};

		// A sub-graph's view and selection while the window shows another.
		struct SubGraphState
		{
			uint32_t Function = 0;
			VisualScriptViewState View;
			std::vector<uint32_t> Selection;
		};

		struct PendingOpen
		{
			VisualScriptOpenRequest Request;
			// The function switch pushed the history already.
			bool Pushed = false;
		};

		// Sync's parts.
		void LookUpAsset(VisualScriptSyncResult& result);
		void AdoptDocument(Ref<VisualScriptDocument const> document, VisualScriptSyncResult& result);
		void RecordChangedElements(VisualScriptDocument const& before, VisualScriptDocument const& after);
		void RecordCommittedLinks(VisualScriptDocument const& before, VisualScriptDocument const& after);
		void RefreshAnalysis(VisualScriptSemantics& semantics, bool widgetActive, double time, bool force, VisualScriptSyncResult& result);
		void SyncCanvas(VisualScriptThemeAnchors const& theme, VisualScriptSyncResult& result);
		// After the visual model changed: what is kept by ID mapped to its indices again.
		void OnCanvasChanged();
		// Analyzes now when the analysis is behind the document, whatever the pace, and lays the canvas out with it.
		void ForceAnalysis();
		void IndexAnalysis();
		VisualScriptDocument const& GetDrawnDocument() const;
		Ref<VisualScriptDocument const> const& GetDrawnDocumentRef() const;
		VisualScriptNode const* FindDrawnNode(uint32_t node, VisualScriptGraph const** graph = nullptr) const;
		std::optional<std::string> FindPinType(uint32_t node, std::string_view pin, VisualScriptPinDirection direction) const;
		// The library pin a pin of the shown graph has (the analysis' or the template's); null for inferred and orphan pins.
		VisualScriptPin const* FindPinInfo(uint32_t node, std::string_view pin, VisualScriptPinDirection direction) const;
		bool HasFunction(uint32_t function) const;
		bool IsInteractive() const;

		// Update's parts.
		bool IsInCanvas(glm::vec2 screenPoint) const;
		// What a screen point is over: nothing outside the canvas, where nothing is drawn.
		VisualScriptHit HitTest(glm::vec2 mouse, bool skipMoved = false);
		// The same into m_Hover, reusing the strings it holds: hovering a wire allocates nothing from frame to frame.
		void UpdateHover(glm::vec2 mouse);
		VisualScriptHitContext MakeHitContext(bool skipMoved);
		void UpdateMinimap(double deltaTime, bool first);
		void ApplyPendingView(double time);
		// Whether Space was held during the frame (its keys then type).
		bool UpdateSpace(VisualScriptCanvasInput const& input, std::vector<VisualScriptEditorIntent>& intents);
		void ApplyAction(VisualScriptEditorAction action, VisualScriptCanvasInput const& input,
		                 std::vector<VisualScriptEditorIntent>& intents);
		void OpenPalette(glm::vec2 position, std::string query, std::vector<VisualScriptEditorIntent>& intents);
		void UpdateMouse(VisualScriptCanvasInput const& input, double deltaTime, std::vector<VisualScriptEditorIntent>& intents);
		void UpdateDrop(VisualScriptCanvasInput const& input, std::vector<VisualScriptEditorIntent>& intents);
		glm::vec2 GetActionPosition(VisualScriptCanvasInput const& input) const;
		void AutoPan(VisualScriptCanvasInput const& input, double deltaTime);
		double GetMoveDuration() const;
		void StoreView();

		// Presses and clicks.
		void BeginPress(VisualScriptCanvasInput const& input, std::vector<VisualScriptEditorIntent>& intents);
		bool HandleDoubleClick(VisualScriptCanvasInput const& input, std::vector<VisualScriptEditorIntent>& intents);
		void PressElement(uint32_t element, VisualScriptCanvasInput const& input);
		void PressPin(VisualScriptCanvasInput const& input, std::vector<VisualScriptEditorIntent>& intents);
		// Keeps the hovered pin in the press by name (PinNode, Pin, Direction, PinLinked); false when it is not on the canvas.
		bool KeepPressedPin();
		void ContinuePress(VisualScriptCanvasInput const& input, double deltaTime, std::vector<VisualScriptEditorIntent>& intents);
		void StartDrag(VisualScriptCanvasInput const& input);
		void Click(VisualScriptCanvasInput const& input, std::vector<VisualScriptEditorIntent>& intents);
		void RightClick(VisualScriptCanvasInput const& input, std::vector<VisualScriptEditorIntent>& intents);

		// Gestures.
		void ContinueGesture(VisualScriptCanvasInput const& input, double deltaTime, std::vector<VisualScriptEditorIntent>& intents);
		void EndGesture();
		void StartPan(int button, glm::vec2 from);
		void StartMove(VisualScriptCanvasInput const& input);
		void ContinueMove(VisualScriptCanvasInput const& input, double deltaTime, std::vector<VisualScriptEditorIntent>& intents);
		void FindSpliceTarget(VisualScriptCanvasInput const& input);
		void StartResize();
		void ContinueResize(VisualScriptCanvasInput const& input, double deltaTime, std::vector<VisualScriptEditorIntent>& intents);
		void ContinueMarquee(VisualScriptCanvasInput const& input, double deltaTime);
		bool BeginWire(PinKey const& source, bool endsOnPress);
		void ContinueWire(VisualScriptCanvasInput const& input, double deltaTime, std::vector<VisualScriptEditorIntent>& intents);
		void UpdateExpandedNode();
		void UpdateGhostWire(VisualScriptCanvasInput const& input);
		void FinishWire(VisualScriptCanvasInput const& input, std::vector<VisualScriptEditorIntent>& intents);
		uint8_t ClassifyPin(uint32_t nodeIndex, uint32_t pinIndex);
		// The pin the wire snaps to (an index of the visual model's pins), kept in the shown classes too.
		void SetWireSnap(std::optional<uint32_t> pinIndex);
		void ClassifyVisiblePins();
		// Whether a wire (relink: the picked links) snaps to and connects with a pin, and how much it prefers it (lower first).
		bool Accepts(bool relink, uint32_t nodeIndex, uint32_t pinIndex);
		int RankPin(bool relink, uint32_t nodeIndex, uint32_t pinIndex);
		// The pin a wire snaps to: the nearest accepted glyph within WireSnapLogicalPixels, else an accepted pin whose row the
		// mouse is over. Pin indices of the visual model.
		std::optional<uint32_t> FindSnapPin(glm::vec2 mouse, bool relink);
		// A node's accepted pin a release over its body connects: the best ranked, then the first.
		std::optional<uint32_t> FindFirstPin(uint32_t nodeIndex, bool relink);
		void StartRelink();
		void ContinueRelink(VisualScriptCanvasInput const& input, double deltaTime, std::vector<VisualScriptEditorIntent>& intents);
		void FinishRelink(std::vector<VisualScriptEditorIntent>& intents);
		bool CanRelinkTo(size_t link, uint32_t nodeIndex, uint32_t pinIndex) const;
		void ContinueCut(VisualScriptCanvasInput const& input, std::vector<VisualScriptEditorIntent>& intents);
		void StartMinimapDrag(glm::vec2 mouse);
		void CenterOnMinimap(glm::vec2 mouse);

		// Selection, raising, sub-graphs, navigation.
		bool IsSelected(uint32_t element) const;
		void SetSelection(std::vector<uint32_t> selection);
		void MakePrimary(uint32_t element);
		void Raise(uint32_t node);
		void RebuildSelectionMasks();
		void RebuildRaise();
		void RebuildDynamic();
		void PruneToCanvas();
		std::vector<uint32_t> GetAllElements() const;
		void Frame(std::span<uint32_t const> elements, double time);
		void SwitchFunction(uint32_t function);
		void PushHistory(VisualScriptViewState const& view);
		void GoBack(double time);
		void GoForward(double time);
		void GoTo(VisualScriptViewState const& view, double time);
		void StoreBookmark(uint8_t slot);
		void GoToBookmark(uint8_t slot, double time);
		bool GoToCustomEvent(VisualScriptNode const& sender);

		// Pins and links of the visual model by the names intents use.
		std::optional<std::pair<uint32_t, uint32_t>> FindCanvasPin(PinKey const& key) const;
		// The index of the node a pin of the visual model belongs to.
		std::optional<uint32_t> FindPinNode(uint32_t pinIndex) const;
		std::optional<uint32_t> FindCanvasLink(VisualScriptLink const& link) const;
		PinKey MakePinKey(uint32_t nodeIndex, uint32_t pinIndex) const;
		VisualScriptPin const* FindPinInfo(PinKey const& key) const;
		// The links of a pin (a collapsed node's anchor: of every pin of its side), as the document stores them.
		std::vector<VisualScriptLink> GetPinLinks(uint32_t nodeIndex, uint32_t pinIndex, bool wholeSide) const;

		AssetHandle m_Graph;
		VisualScriptViewSettings& m_Settings;
		uint32_t m_Function = 0;
		VisualScriptDocumentState m_State = VisualScriptDocumentState::Editable;

		// What the last Sync saw: the asset object and its version, its shared document, the display copy, and what the
		// panel's last commit wrote until Sync sees it.
		Ref<VisualScriptAsset> m_SeenAsset;
		uint64_t m_SeenVersion = 0;
		Ref<VisualScriptDocument const> m_Document;
		Ref<VisualScriptDocument const> m_Display;
		Ref<VisualScriptDocument const> m_OwnDocument;
		Ref<VisualScriptDocument const> m_Preview;
		bool m_PreviewChanged = false;
		// The last Sync adopted an outside change: the gesture in progress is cancelled.
		bool m_OutsideChange = false;
		// The time of the frame whose Syncs report m_ChangedElements and m_CommittedLinks.
		double m_LastSyncTime = -1.0e30;
		bool m_IsFileAsset = false;
		bool m_CheckWritable = true;
		bool m_Writable = true;

		// The analysis: the library of the last Sync, the analysis with the document and library it was made from, and a
		// document the runtime failed on with that library.
		VisualScriptSemantics* m_Semantics = nullptr;
		Ref<VisualScriptLibrary const> m_Library;
		Ref<VisualScriptAnalysis const> m_Analysis;
		Ref<VisualScriptDocument const> m_AnalyzedDocument;
		Ref<VisualScriptLibrary const> m_AnalyzedLibrary;
		Ref<VisualScriptDocument const> m_FailedDocument;
		Ref<VisualScriptLibrary const> m_FailedLibrary;
		// The runtime stopped: the analysis is kept for its pins, without its problems.
		bool m_AnalysisStale = false;
		bool m_AnalysisPending = true;
		double m_LastAnalysisTime = -1.0e30;
		bool m_HasAnalyzed = false;
		uint64_t m_AnalysisCount = 0;

		// What the visual model was last synced with.
		bool m_CanvasStale = true;
		VisualScriptThemeAnchors m_Theme;
		VisualScriptTextMeasurer m_Measure;
		bool m_HasMeasure = false;
		uint64_t m_AssetVersion = 0;
		uint32_t m_CanvasFunction = 0;
		// Counts the visual model's layouts, so classes kept by pin index know when they are stale.
		uint64_t m_CanvasRevision = 0;
		std::optional<VisualScriptRect> m_ContentBounds;

		VisualScriptCanvasModel m_Canvas;
		VisualScriptSpatialIndex m_Index;
		std::vector<uint32_t> m_ChangedElements;
		std::vector<VisualScriptLink> m_CommittedLinks;
		// By ID: the analysis' nodes; the analyzed document's nodes and the drawn document's nodes, as (function, index) packed
		// in 64 bits.
		std::vector<IdIndex> m_AnalysisNodes;
		std::vector<std::pair<uint32_t, uint64_t>> m_AnalyzedNodes;
		std::vector<std::pair<uint32_t, uint64_t>> m_DrawnNodes;

		// The view, the minimap, the history, the sub-graphs' views and selections.
		VisualScriptView m_View;
		VisualScriptMinimapState m_Minimap;
		std::optional<VisualScriptViewState> m_StoredView;
		// The shown sub-graph has no view to show yet: it is framed whole at the next Update with a canvas.
		bool m_NeedsFrame = false;
		// An Update with a canvas has shown a view: later moves animate.
		bool m_HasShownView = false;
		std::optional<PendingOpen> m_PendingOpen;
		std::vector<VisualScriptViewState> m_Back;
		std::vector<VisualScriptViewState> m_Forward;
		std::vector<SubGraphState> m_SubGraphs;

		// The selection (primary first) and the raised nodes (ID, raise counter), and their masks by index.
		std::vector<uint32_t> m_Selection;
		std::vector<IdIndex> m_Raised;
		uint32_t m_RaiseCounter = 0;
		std::vector<uint8_t> m_NodeSelected;
		std::vector<uint8_t> m_CommentSelected;
		std::vector<uint32_t> m_NodeRaise;
		std::vector<uint8_t> m_NodeMoved;

		// Input and gestures.
		VisualScriptHit m_Hover;
		// Where the mouse was at the last Update.
		glm::vec2 m_LastMouse{0.0f};
		VisualScriptHitScratch m_HitScratch;
		std::vector<uint32_t> m_Query;
		std::vector<glm::vec2> m_Polyline;
		float m_CurveTolerance = 1.25f;
		double m_Time = 0.0;
		double m_InputTime = 0.0;
		bool m_HasUpdated = false;
		VisualScriptGesture m_Gesture = VisualScriptGesture::Idle;
		PressState m_Press;
		// The button the gesture in progress ends with.
		int m_GestureButton = 0;
		glm::vec2 m_PanLast{0.0f};
		// Canvas units: where the marquee and the cut line end.
		glm::vec2 m_GestureCanvas{0.0f};
		// The index's dynamic items (SetDynamic's input), by model index.
		std::vector<uint32_t> m_DynamicNodes;
		std::vector<uint32_t> m_DynamicComments;
		std::vector<uint32_t> m_DynamicLinks;
		MoveState m_Move;
		WireState m_Wire;
		RelinkState m_Relink;
		ResizeState m_Resize;
		std::vector<uint32_t> m_MarqueeElements;
		std::vector<VisualScriptLink> m_CutLinks;
		bool m_SpaceHeld = false;
		bool m_SpaceTap = false;
		bool m_SpacePaletteOpened = false;
		double m_SpaceDownTime = 0.0;
		uint64_t m_RefusedCount = 0;
	};
}
