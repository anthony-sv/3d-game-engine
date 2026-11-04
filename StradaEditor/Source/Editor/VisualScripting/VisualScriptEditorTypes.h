#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/VisualScriptAsset.h"
#include "Strada/Asset/VisualScriptReferences.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/UUID.h"
#include "Strada/Script/VisualScriptLibrary.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The editor services hand out ImGui fonts without the header needing ImGui.
struct ImFont;

// The vocabulary the parts of the visual script editor share: document editing, the canvas model, the panes, the graph
// windows and automation. Plain data, no ImGui. Main thread only, as every editor panel.
namespace Strada
{
	// The type of a node's pin as the canvas knows it: VisualScriptExecType, a type ID, or VisualScriptWildcardType; nothing
	// when the node has no such pin.
	using VisualScriptPinTypeLookup =
		std::function<std::optional<std::string>(uint32_t node, std::string_view pin, VisualScriptPinDirection direction)>;
	// The size of a node in canvas units (the units of positions in documents) as the canvas measures it.
	using VisualScriptNodeExtents = std::function<glm::vec2(uint32_t node)>;

	// What one visual script of the project offers other graphs, read from its document without the scripting runtime: an entry of
	// VisualScriptProjectIndex (L3), which the palette's index (L2a) and the panes read too.
	struct VisualScriptProjectGraph
	{
		AssetHandle Graph;
		// The asset path ("Scripts/Door.sgraph"); empty for memory assets.
		std::string Path;
		// The file stem ("Door"); a memory asset's name.
		std::string DisplayName;
		VisualScriptDocumentKind Kind = VisualScriptDocumentKind::Script;
		// Null when it does not load (its file is missing or broken).
		Ref<VisualScriptDocument const> Document;
		// Why it does not load; empty when it does.
		std::string LoadError;
		// CollectVisualScriptReferences of the document: the graphs its nodes name.
		std::vector<VisualScriptGraphReference> References;
	};

	// What a view frames when a graph opens.
	enum class VisualScriptFrame : uint8_t
	{
		// The view the sub-graph had.
		Keep = 0,
		Selection,
		All
	};

	// A variable, function or custom event whose references find looks for, or that an open request reveals.
	struct VisualScriptReference
	{
		enum class Kind : uint8_t
		{
			// A variable or a local.
			Variable = 0,
			Function,
			CustomEvent
		};

		Kind ReferenceKind = Kind::Variable;
		// Variable and Function; an open request's custom event: its Event.Custom node.
		uint32_t ID = 0;
		// CustomEvent (find).
		std::string Name;
		// The graph that declares the element: invalid is the searched document. A custom event of an invalid graph is any
		// graph's: every event and sender of its name matches.
		AssetHandle Graph;

		VisualScriptReference() = default;
		// So that a reference is written {kind, id, name} without an aggregate's missing initializer
		// (-Wmissing-field-initializers).
		VisualScriptReference(Kind kind, uint32_t id, std::string name = {}, AssetHandle graph = {})
			: ReferenceKind(kind),
			  ID(id),
			  Name(std::move(name)),
			  Graph(graph)
		{
		}

		bool operator==(VisualScriptReference const& other) const = default;
	};

	// How an open request for another graph moves through the windows' navigation histories (spec-slice3 §10.7).
	enum class VisualScriptHistoryStep : uint8_t
	{
		// A jump (an Open of a node, a quick fix, a find result): the window opened puts the origin on its Back history.
		Jump = 0,
		// Back from a window's cross-graph entry: the window opened puts the origin on its Forward history.
		Back,
		// Forward from a cross-graph entry: the window opened puts the origin on its Back history, keeping its Forward one.
		Forward
	};

	// The view of the window an open request for another graph came from, which Back in the window opened returns to.
	struct VisualScriptOrigin
	{
		AssetHandle Graph;
		// 0 is the event graph, else a function's ID.
		uint32_t Function = 0;
		glm::vec2 Center{0.0f};
		float Zoom = 1.0f;
		VisualScriptHistoryStep Step = VisualScriptHistoryStep::Jump;
		// The requesting graph's display name ("Door"), which a toast names when the open fails (spec-slice4 §13.10); empty
		// when unknown.
		std::string Name;

		bool operator==(VisualScriptOrigin const& other) const = default;
	};

	// What opening a graph asks for (the editor's Open entries and graph.open).
	struct VisualScriptOpenRequest
	{
		AssetHandle Graph;
		// The sub-graph to show: 0 is the event graph, else a function's ID; none shows the sub-graph shown last.
		std::optional<uint32_t> Function;
		// Node and comment IDs of that sub-graph; when not empty they replace its selection.
		std::vector<uint32_t> Select;
		VisualScriptFrame Frame = VisualScriptFrame::Keep;
		// Clamped to [0.10, 2.00].
		std::optional<float> Zoom;
		// The canvas point at the centre of the view.
		std::optional<glm::vec2> Center;
		// An entity that runs the graph, whose values the window shows while playing.
		UUID ContextEntity = UUID::Invalid();
		// Opens the problems drawer.
		bool ShowProblems = false;
		// Opens the find bar with this text when not empty.
		std::string Find;
		// Opens the drawer in Timeline mode on this Flow.Timeline node of the sub-graph shown.
		std::optional<uint32_t> Timeline;
		// A row of My Script to select (shown in Details): a variable or local, a function, or a custom event (its ID the
		// Event.Custom node); its Graph and Name are not read.
		std::optional<VisualScriptReference> Reveal;
		// The window that asked, for a request of another graph (spec-slice3 §10.7).
		std::optional<VisualScriptOrigin> Origin;
		// A node of the sub-graph shown to bring into view (spec-slice4 §13.7): the view pans the least distance that shows it
		// with a margin of 2 font sizes, keeping the zoom unless the node is larger than the view (then it is centred and
		// zoomed to fit), and keeps the selection. Applied after the request's framing, zoom and centre.
		std::optional<uint32_t> ShowNode;
		// The debugger opened it (following a stop or a call-stack row, spec-slice4 §13.8): it pushes at most one Back entry
		// per stop session, so following steps does not bury the user's own navigation.
		bool FromDebugger = false;
		// A row of Details, by its label ("Formula", "Length", "Settings", a setting's or an input's name), to scroll to the top
		// of the pane once Details draws it (S4-R5.2): with Select, the selected node's. Empty scrolls nothing.
		std::string DetailsRow;
		bool Focus = true;

		bool operator==(VisualScriptOpenRequest const& other) const = default;
	};

	// Keyboard and menu actions, which the graph window routes to its canvas model or to the controller that edits.
	enum class VisualScriptEditorAction : uint8_t
	{
		Delete = 0,
		DeleteWithoutRelink,
		Cut,
		Copy,
		Paste,
		Duplicate,
		SelectAll,
		DeselectAll,
		Cancel,
		Rename,
		FrameSelection,
		FrameAll,
		ZoomIn,
		ZoomOut,
		ZoomReset,
		ToggleMinimap,
		ToggleSnapToGrid,
		OpenPalette,
		AddCommentBox,
		Straighten,
		AlignLeft,
		AlignRight,
		AlignTop,
		AlignBottom,
		AlignCenters,
		AlignMiddles,
		DistributeHorizontally,
		DistributeVertically,
		Arrange,
		// The node menu and its keys.
		ToggleCollapsed,
		ToggleDisabled,
		SetEnabled,
		SetDisabled,
		SetDevelopmentOnly,
		BreakNodeLinks,
		AddNodeComment,
		EditNodeComment,
		RemoveNodeComment,
		FindReferences,
		OpenNode,
		UpdateNode,
		NudgeLeft,
		NudgeRight,
		NudgeUp,
		NudgeDown,
		NudgeLeftFine,
		NudgeRightFine,
		NudgeUpFine,
		NudgeDownFine,
		Back,
		Forward,
		Find,
		FindInProject,
		ShowShortcuts,
		// Bookmark slots 1 to 9, each run of nine in slot order.
		StoreBookmark1,
		StoreBookmark2,
		StoreBookmark3,
		StoreBookmark4,
		StoreBookmark5,
		StoreBookmark6,
		StoreBookmark7,
		StoreBookmark8,
		StoreBookmark9,
		GoToBookmark1,
		GoToBookmark2,
		GoToBookmark3,
		GoToBookmark4,
		GoToBookmark5,
		GoToBookmark6,
		GoToBookmark7,
		GoToBookmark8,
		GoToBookmark9,
		// The Timeline drawer's Shift+Left and Shift+Right: its selected keys nudged ten time steps.
		NudgeLeftCoarse,
		NudgeRightCoarse,
		// The debugger's (spec-slice4 §13.5, §13.6): the selection's breakpoints added or removed (F9), enabled or disabled
		// (Ctrl+F9), the primary node's breakpoint popup, and Run to Here (Ctrl+F10). Not edits: the window hands them to the
		// debug session.
		ToggleBreakpoint,
		ToggleBreakpointEnabled,
		BreakpointSettings,
		RunToNode,
		// The editor's global debug keys while an execution is stopped (spec-slice4 §14.4), which the shortcut sheet lists:
		// the editor routes them (VisualScriptDebugControls), never a graph window.
		Continue,
		StepOver,
		StepInto,
		StepOut
	};

	// Game View Beside's split rule (spec-slice4 §15.6, S4-R1.1 Q12), in font sizes F, which the game preview's Beside symbol
	// draws too.
	namespace VisualScriptBeside
	{
		// Up gives the Game View this share of the height, when the graph keeps MinGraphHeight of it.
		inline constexpr float UpShare = 0.45f;
		inline constexpr float MinGraphHeight = 20.0f;
		// Left gives the graph MinGraphWidth (the stacked panes' 14 F and the canvas's minimum of 40 F) and the Game View the
		// rest, at most MaxLeftShare of the width; below MinLeftShare it gives the Game View FallbackLeftShare instead.
		inline constexpr float MinGraphWidth = 54.0f;
		inline constexpr float MaxLeftShare = 0.5f;
		inline constexpr float MinLeftShare = 0.3f;
		inline constexpr float FallbackLeftShare = 0.45f;
		// The game's aspect ratio when it has not rendered yet.
		inline constexpr float DefaultAspect = 16.0f / 9.0f;
	}

	// The game as the graph windows' preview shows it (spec-slice4 §15.6), from the editor's game view in the frame being drawn.
	struct VisualScriptGamePreview
	{
		// The texture of the game's image rendered this frame (an ImTextureID); none when nothing rendered (no GPU, no
		// primary camera, or the game was not rendered for the preview yet).
		std::optional<uint64_t> Texture;
		// The pixels the game renders, whose aspect ratio the preview keeps.
		glm::uvec2 Size{1280, 720};
		// The game rendered in this frame (for the game view or the preview): with no Texture, no camera renders it. Before
		// the frame's render (the first frame the game view is hidden), there is neither.
		bool Rendered = false;
		// The game view is open, and visible this frame (its tab in front): a visible one shows the game, so no preview does.
		bool GameViewOpen = false;
		bool GameViewVisible = false;
		// The dock node the game view was drawn in this frame with its tab in front (an ImGuiID); 0 when it was not, or floats.
		uint32_t GameViewDockNode = 0;
		// Where the game view's window was last drawn (screen pixels), while it is open.
		std::optional<std::pair<glm::vec2, glm::vec2>> GameViewRect;
		// The game has the keyboard and mouse (it takes them only through the game view).
		bool GameHasInput = false;

		bool operator==(VisualScriptGamePreview const& other) const = default;
	};

	// Editor services the graph windows and their panes use, which the editor injects as it injects the console's font and
	// its Show Console callback. Tests leave the defaults (or give fakes).
	struct VisualScriptEditorServices
	{
		// Shortcut keys, type strings and problem codes; null draws them with the current font.
		ImFont* MonospaceFont = nullptr;
		// The [Show Console] button of notices and error toasts; empty leaves the button out.
		std::function<void()> ShowConsole;
		// The game for the windows' preview, asked while they draw; empty, or answering none, shows no preview (no game view).
		std::function<std::optional<VisualScriptGamePreview>()> GamePreview;
		// Brings the game view to the front with the game's input (the preview's expand button and a click on its image).
		std::function<void()> ShowGameView;
		// Asks for Game View Beside for the graph's window (VisualScriptEditors::RequestGameViewBeside), run at the next frame.
		std::function<void(AssetHandle graph)> PlaceGameViewBeside;
	};
}
