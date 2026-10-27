#pragma once

#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptGeometry.h"
#include "Editor/VisualScripting/VisualScriptSpatialIndex.h"
#include "Editor/VisualScripting/VisualScriptTheme.h"
#include "Editor/VisualScripting/VisualScriptThemeStyle.h"
#include "Editor/VisualScripting/VisualScriptView.h"

#include "Strada/Script/VisualScriptLibrary.h"

#include <glm/glm.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The canvas's renderer: draws a graph window's canvas from its editor model into an ImDrawList, layer by layer in draw order
// (no channel splitter), touching only what is visible. It reads no input and changes nothing in the model or the document;
// what it keeps between frames is its own (animations, buffers and caches). Main thread only.
namespace Strada
{
	class VisualScriptBrushes;

	// Every fixed text the canvas renderer draws, besides the document's own texts (titles, labels, chip values) and the
	// library's type titles: the editor's UI font must have each of their characters (VisualScriptTextTests checks it).
	// "\xC2\xB7" is the middle dot, "\xC6\x92" the function glyph ƒ; arrows are drawn as shapes (UI::DrawArrowText).
	namespace VisualScriptCanvasTexts
	{
		inline constexpr std::string_view Disabled = "Disabled";
		inline constexpr std::string_view DevelopmentOnly = "Development Only";
		inline constexpr std::string_view Self = "Self";
		inline constexpr std::string_view None = "None";
		inline constexpr std::string_view FunctionGlyph = "\xC6\x92";
		inline constexpr std::string_view Separator = " \xC2\xB7 ";
		inline constexpr std::string_view Default = "Default: ";
		inline constexpr std::string_view ResolvedTo = " resolved to ";
		inline constexpr std::string_view Unresolved = "unresolved";
		inline constexpr std::string_view ConvertedFrom = "converted from ";
		inline constexpr std::string_view Converted = ", converted";
		inline constexpr std::string_view Arrow = " \xE2\x86\x92 ";
		inline constexpr std::string_view Cannot = "Cannot connect ";
		inline constexpr std::string_view Exec = "exec";
		inline constexpr std::string_view Ellipsis = "\xE2\x80\xA6";

		// All of them, for the glyph test.
		inline constexpr std::array<std::string_view, 15> All = {Disabled,  DevelopmentOnly, Self,       None,       FunctionGlyph,
		                                                         Separator, Default,         ResolvedTo, Unresolved, ConvertedFrom,
		                                                         Converted, Arrow,           Cannot,     Exec,       Ellipsis};
	}

	// The layers of the canvas, in the order they are drawn: every item of a layer is emitted before the next layer starts.
	enum class VisualScriptDrawLayer : uint8_t
	{
		// The canvas's fill and its grid.
		Canvas = 0,
		Comments,
		Wires,
		// The wires of selected or hovered nodes, the hovered wire, pulsing wires, the splice target.
		HighlightedWires,
		// Unselected nodes (reroutes included), in draw order (GetVisualScriptNodeDrawKey).
		Nodes,
		SelectedNodes,
		// The ghost wire, the marquee, the cut line, link pulses, snap glows, the hover pill and pin tooltips.
		Overlay,
		Minimap
	};

	// What an item of the draw record is.
	enum class VisualScriptDrawItemKind : uint8_t
	{
		Grid = 0,
		Comment,
		Wire,
		Node,
		Reroute,
		GhostWire,
		// A link picked up with Ctrl+drag, drawn from its far end to the mouse.
		PickedWire,
		Marquee,
		CutLine,
		// A new link's dot moving from its output to its input.
		Pulse,
		// The wire hover label, a pin tooltip or the invalid-wire tooltip (DrawValuePill).
		Pill,
		Minimap
	};

	// VisualScriptDrawRecordItem::Elements: what was drawn of an item.
	namespace VisualScriptDrawElements
	{
		inline constexpr uint32_t Shadow = 1u << 0;
		inline constexpr uint32_t Body = 1u << 1;
		inline constexpr uint32_t Header = 1u << 2;
		inline constexpr uint32_t Title = 1u << 3;
		inline constexpr uint32_t Subtitle = 1u << 4;
		// The kind's glyph: an event's diamond, a function's "ƒ", the latent clock.
		inline constexpr uint32_t KindGlyph = 1u << 5;
		inline constexpr uint32_t Pins = 1u << 6;
		inline constexpr uint32_t PinLabels = 1u << 7;
		// Value chips with frames (Detail), or as text (Compact).
		inline constexpr uint32_t Chips = 1u << 8;
		inline constexpr uint32_t ChipText = 1u << 9;
		// The live widget was drawn in place of a chip.
		inline constexpr uint32_t LiveWidget = 1u << 10;
		inline constexpr uint32_t Badges = 1u << 11;
		// The node comment's bubble (at Overview: its small marker).
		inline constexpr uint32_t Bubble = 1u << 12;
		// The disabled or development-only strip, and its label on its pill.
		inline constexpr uint32_t Strip = 1u << 13;
		inline constexpr uint32_t StripLabel = 1u << 14;
		// A disabled node's dashed line from its exec input to its pass-through exit.
		inline constexpr uint32_t Bypass = 1u << 15;
		// The hatching of a node of an unknown type, the stripes of a development-only strip.
		inline constexpr uint32_t Hatching = 1u << 16;
		// A state outline (hovered, selected, error, flash) instead of the plain border.
		inline constexpr uint32_t Outline = 1u << 17;
		inline constexpr uint32_t Border = 1u << 18;
		// The selection glow, a flash's glow (soft-rectangle brush).
		inline constexpr uint32_t Glow = 1u << 19;
		inline constexpr uint32_t Flash = 1u << 20;
		inline constexpr uint32_t CollapsedMarker = 1u << 21;
		// Wires: drawn in dashes; with an underlay halo (the splice target); faded (cut, disabled); recoloured along their
		// length (an implicit conversion); recoloured by ImGui's linear gradient instead (an unexpected vertex layout).
		inline constexpr uint32_t Dashed = 1u << 22;
		inline constexpr uint32_t Halo = 1u << 23;
		inline constexpr uint32_t Faded = 1u << 24;
		inline constexpr uint32_t Gradient = 1u << 25;
		inline constexpr uint32_t GradientFallback = 1u << 26;
		// An error link's badge at half its length.
		inline constexpr uint32_t ErrorBadge = 1u << 27;
		// Comments: their title bar text, their body text, their resize handles.
		inline constexpr uint32_t BodyText = 1u << 28;
		inline constexpr uint32_t ResizeHandles = 1u << 29;
		// A pulse's moving dot (Reduce Motion draws the highlight alone); a wire mixed toward white by a pulse.
		inline constexpr uint32_t PulseDot = 1u << 30;
		inline constexpr uint32_t PulseHighlight = 1u << 31;
	}

	// One item the renderer drew: a test's view of the draw order and of what each item showed.
	struct VisualScriptDrawRecordItem
	{
		VisualScriptDrawLayer Layer = VisualScriptDrawLayer::Canvas;
		VisualScriptDrawItemKind Kind = VisualScriptDrawItemKind::Grid;
		// Nodes, reroutes and comments: the ID; wires and pulses: the link's index in the visual model; pills: the hovered
		// node's ID (0 for a wire's).
		uint32_t Element = 0;
		VisualScriptDetailLevel Level = VisualScriptDetailLevel::Detail;
		// VisualScriptDrawElements.
		uint32_t Elements = 0;
		// The item's place in the emission order.
		uint32_t Order = 0;
		// Screen pixels: the item's rectangle (a node's body, a comment's box, a wire's control-point box).
		VisualScriptRect Screen;
		// The main colour: a wire's (its output end's), an outline's, a reroute's dot; a ring's and the gap's around it.
		uint32_t Color = 0;
		uint32_t RingColor = 0;
		uint32_t GapColor = 0;
		// The vertices the item wrote (ImDrawList::VtxBuffer indices; a node's include its live widget's); wires: the points
		// of their polyline.
		uint32_t VertexBegin = 0;
		uint32_t VertexEnd = 0;
		uint32_t PointCount = 0;
		// Wires: the thickness of their stroke in pixels.
		float Thickness = 0.0f;
		// Pills: the text shown.
		std::string Text;

		bool Has(uint32_t elements) const { return (Elements & elements) == elements; }
	};

	// VisualScriptDrawRecordPin::States.
	namespace VisualScriptPinDrawStates
	{
		inline constexpr uint8_t Hovered = 1u << 0;
		inline constexpr uint8_t Compatible = 1u << 1;
		inline constexpr uint8_t Converting = 1u << 2;
		inline constexpr uint8_t Incompatible = 1u << 3;
		inline constexpr uint8_t Snapped = 1u << 4;
		// The analysis reports a problem on the pin.
		inline constexpr uint8_t Problem = 1u << 5;
	}

	// A pin drawn with a state (hovered, classed while wiring, a problem on it): its ring and the gap around its glyph.
	struct VisualScriptDrawRecordPin
	{
		uint32_t Node = 0;
		// The pin's index in the visual model's pins.
		uint32_t Pin = 0;
		// VisualScriptPinDrawStates.
		uint8_t States = 0;
		// The glyph's scale (1.3 hovered, 1.15 compatible, 1.4 snapped).
		float Scale = 1.0f;
		// 0 when no ring is drawn.
		uint32_t RingColor = 0;
		uint32_t GapColor = 0;
		// The alpha the pin's row is drawn at (0.3 for an incompatible pin while wiring).
		float Alpha = 1.0f;
	};

	// What a frame drew, item by item: tests pass one to the renderer; without one it records nothing.
	struct VisualScriptDrawRecord
	{
		std::vector<VisualScriptDrawRecordItem> Items;
		std::vector<VisualScriptDrawRecordPin> Pins;

		void Clear()
		{
			Items.clear();
			Pins.clear();
		}
		// The first item of a kind for an element; null when there is none.
		VisualScriptDrawRecordItem const* Find(VisualScriptDrawItemKind kind, uint32_t element) const;
		size_t Count(VisualScriptDrawItemKind kind) const;
	};

	// What a frame of the canvas cost, counted every frame (VisualScriptCanvasRenderer::GetStats).
	struct VisualScriptRenderStats
	{
		uint32_t VisibleNodes = 0;
		uint32_t VisibleWires = 0;
		uint32_t VisibleComments = 0;
		// The culling query's grid cells and candidates.
		VisualScriptIndexStats Index;
		// What the canvas added to the draw list: vertices and indices (VtxBuffer and IdxBuffer growth) and draw commands.
		uint32_t Vertices = 0;
		uint32_t Indices = 0;
		uint32_t DrawCommands = 0;
		// Distinct text sizes requested (pixels, from VisualScriptTextLadder).
		uint32_t TextSizes = 0;
		uint32_t LiveWidgets = 0;
		// Soft rectangles and dots drawn with the brushes.
		uint32_t BrushDraws = 0;
		// Wires drawn, and wires left out at Far (shorter than FarMinWireLength, or over FarWireLimit).
		uint32_t DrawnWires = 0;
		uint32_t SkippedWires = 0;
		// Phase timings in milliseconds, measured only with VisualScriptCanvasDrawInput::MeasurePhases.
		double CullMilliseconds = 0.0;
		double CommentsMilliseconds = 0.0;
		double WiresMilliseconds = 0.0;
		double NodesMilliseconds = 0.0;
		double OverlayMilliseconds = 0.0;
		double MinimapMilliseconds = 0.0;
		double TotalMilliseconds = 0.0;
	};

	// The value chip whose live widget the renderer asks its host to draw: the chip's place and the metrics that make the
	// widget's frames equal the chip's cells (taken from the chip, never from the theme's style, so the widget does not pop).
	struct VisualScriptLiveChip
	{
		// The node's ID, its index in the visual model and the pin's index there.
		uint32_t Node = 0;
		uint32_t NodeIndex = 0;
		uint32_t Pin = 0;
		VisualScriptChipKind Kind = VisualScriptChipKind::None;
		// Screen pixels: the chip as the visual model lays it out; its cells (one per component of a vector or quaternion, else
		// one) are CellWidth wide and CellGap apart (GetCell). Cell widths are whole pixels, as ImGui gives items
		// (ImGui::CalcItemWidth truncates widths), except the squares of check boxes and colour buttons (the frame's height).
		VisualScriptRect Rect;
		uint32_t CellCount = 1;
		float CellWidth = 0.0f;
		float CellGap = 0.0f;
		// The text size in pixels (ImGui::GetFontSize() inside VisualScriptLiveChipScope).
		float FontPixels = 13.0f;
		// The size passed to ImGui::PushFont: FontPixels before the style's global font scales.
		float FontSizeBase = 13.0f;
		// The widget's frame padding, item spacing (both ItemSpacing and ItemInnerSpacing), frame rounding and grab size.
		ImVec2 FramePadding{0.0f, 0.0f};
		ImVec2 ItemSpacing{0.0f, 0.0f};
		float FrameRounding = 0.0f;
		float GrabMinSize = 0.0f;

		// A cell's frame on the screen: the chip draws its cells there, and the widget's items lie there.
		VisualScriptRect GetCell(uint32_t cell) const
		{
			float const left = Rect.Min.x + static_cast<float>(cell) * (CellWidth + CellGap);
			return {glm::vec2(left, Rect.Min.y), glm::vec2(left + CellWidth, Rect.Max.y)};
		}
	};

	// Pushes a live chip's font size and metrics, and puts the cursor on the chip, for the widget its host draws; pops them
	// when it ends: one ImGui::PushFont(nullptr, chip.FontSizeBase) and five style variables (FramePadding, ItemSpacing,
	// ItemInnerSpacing, FrameRounding, GrabMinSize), so the host must pop whatever it pushes inside the scope before it ends.
	// Each cell's widget takes SetNextItemWidth(chip.CellWidth); cells follow one another with SameLine(). A popup the widget
	// asks for is opened after the scope ended, at the window's normal size.
	class VisualScriptLiveChipScope
	{
	public:
		explicit VisualScriptLiveChipScope(VisualScriptLiveChip const& chip);
		~VisualScriptLiveChipScope();
		VisualScriptLiveChipScope(VisualScriptLiveChipScope const&) = delete;
		VisualScriptLiveChipScope& operator=(VisualScriptLiveChipScope const&) = delete;
	};

	// What draws the live widget of a value chip: the graph window, with the literal editor's compact form inside a
	// VisualScriptLiveChipScope. The renderer calls it at most once per frame, at the chip's node's place in its layer.
	class VisualScriptLiveChipHost
	{
	public:
		virtual ~VisualScriptLiveChipHost() = default;
		virtual void DrawLiveChip(VisualScriptLiveChip const& chip) = 0;

	protected:
		VisualScriptLiveChipHost() = default;
		VisualScriptLiveChipHost(VisualScriptLiveChipHost const&) = default;
		VisualScriptLiveChipHost& operator=(VisualScriptLiveChipHost const&) = default;
	};

	// What the renderer needs to draw a frame besides the editor model.
	struct VisualScriptCanvasDrawInput
	{
		// The canvas's draw list, whose clip rectangle is the canvas and whose texture is the UI font atlas: the current
		// window's (ImGui::GetWindowDrawList() of the canvas's child window), so the live widget's items land in draw order.
		ImDrawList* DrawList = nullptr;
		// The UI font text is drawn with; null for ImGui::GetFont() (outside any PushFont).
		ImFont* Font = nullptr;
		VisualScriptCanvasStyle Style;
		// The node library, for the type titles of pin tooltips; may be null.
		VisualScriptLibrary const* Library = nullptr;
		// Seconds (ImGui::GetTime() in the editor): animations and hover delays.
		double Time = 0.0;
		// The mouse in screen pixels, and whether the canvas itself is under it (not a popup or another window): the live
		// widget and the hover pills appear only then.
		glm::vec2 Mouse{0.0f};
		bool Hovered = false;
		// Moves are instant and pulses and flashes a static highlight of the same duration (the per-user view setting).
		bool ReduceMotion = false;
		// The chip (node ID, input pin name) whose live widget is being used (ImGui's active item): it stays live while the
		// mouse is elsewhere. Null when none is.
		VisualScriptPinReference const* ActiveChip = nullptr;
		// Draws the live widget; none draws chips only.
		VisualScriptLiveChipHost* LiveChips = nullptr;
		// Shadows, glows and pulse dots; none leaves them out.
		VisualScriptBrushes* Brushes = nullptr;
		// Filled with every item drawn when given (tests).
		VisualScriptDrawRecord* Record = nullptr;
		// Times the phases of the frame (VisualScriptRenderStats).
		bool MeasurePhases = false;
	};

	// Draws a rounded label of one or more lines (split by '\n', arrows drawn by UI::DrawArrowText) whose top-left corner is
	// near anchor and which stays within bounds when it fits: the wire hover label, pin tooltips, and later the values of
	// wires while debugging. Returns its rectangle in screen pixels.
	VisualScriptRect DrawVisualScriptValuePill(ImDrawList* drawList, ImFont* font, float fontSize, glm::vec2 anchor,
	                                           VisualScriptRect const& bounds, std::string_view text, VisualScriptThemeAnchors const& theme,
	                                           uint32_t textColor);

	// Draws one graph window's canvas. Holds the animations started by commits (link pulses, flashes of elements changed
	// elsewhere, the glide of an Arrange) and buffers reused from frame to frame, so a frame in steady state allocates nothing.
	class VisualScriptCanvasRenderer
	{
	public:
		// The Far level's wire rules: wires shorter than this on the screen (pixels × UI scale) are skipped, and with more
		// than FarWireLimit wires in view only exec wires and the wires of selected nodes are drawn.
		static constexpr float FarMinWireLength = 6.0f;
		static constexpr size_t FarWireLimit = 4000;
		// Animations (seconds).
		static constexpr double PulseSeconds = 0.35;
		static constexpr double PulseHighlightSeconds = 0.6;
		static constexpr double FlashSeconds = 0.8;
		static constexpr double GlideSeconds = 0.25;

		VisualScriptCanvasRenderer();
		~VisualScriptCanvasRenderer();
		VisualScriptCanvasRenderer(VisualScriptCanvasRenderer const&) = delete;
		VisualScriptCanvasRenderer& operator=(VisualScriptCanvasRenderer const&) = delete;

		// Draws the model's canvas: what its view shows of its visual model, the gesture in progress, the selection and hover,
		// the minimap, and the animations under way. Starts the pulses of the links the model reports as committed this frame
		// and the flashes of the elements it reports as changed elsewhere.
		void Draw(VisualScriptEditorModel const& model, VisualScriptCanvasDrawInput const& input);

		// Lets the nodes an Arrange moved glide from their old places (node IDs and positions in canvas units) to their new
		// ones over GlideSeconds from time; drawing only, the document changed at once. Instant with Reduce Motion (nothing
		// to do then).
		void StartGlide(std::span<std::pair<uint32_t, glm::vec2> const> oldPositions, double time);
		// Ends every animation (a sub-graph switch, the window hidden).
		void StopAnimations();

		VisualScriptRenderStats const& GetStats() const { return m_Stats; }
		// The distinct text sizes (pixels) the last frame drew text at, in the order first used (at most 16 listed;
		// GetStats().TextSizes counts them all).
		std::span<float const> GetTextSizes() const { return {m_TextSizes.data(), std::min<size_t>(m_TextSizeCount, m_TextSizes.size())}; }
		// Bytes its reused buffers have reserved (capacity, not size): an idle frame does not grow it.
		size_t GetCapacityBytes() const;

	private:
		struct Frame;
		// A node as this frame draws it: its index, its offset from its stored place, its states.
		struct NodeState
		{
			uint32_t Index = 0;
			glm::vec2 Offset{0.0f};
			// Screen pixels.
			VisualScriptRect Screen;
			bool Selected = false;
			bool Hovered = false;
			// When it started flashing (changed elsewhere).
			std::optional<double> Flash;
			// Disabled nodes draw every primitive but their text at VisualScriptDisabledAlpha.
			float Fade = 1.0f;
		};
		struct Pulse
		{
			VisualScriptLink Link;
			double Start = 0.0;
		};
		struct Flash
		{
			uint32_t Element = 0;
			double Start = 0.0;
		};
		struct Glide
		{
			uint32_t Node = 0;
			glm::vec2 From{0.0f};
		};
		// A pulse's link found in the visual model this frame: how far its dot is (negative: none) and how much its wire is
		// mixed toward white.
		struct ActivePulse
		{
			uint32_t Link = 0;
			float Progress = -1.0f;
			float Highlight = 0.0f;
		};

		// Per-link marks of a frame.
		static constexpr uint8_t CutMark = 1u << 0;
		static constexpr uint8_t PickedMark = 1u << 1;
		static constexpr uint8_t HighlightMark = 1u << 2;

		void TakeEvents(VisualScriptEditorModel const& model, Frame& frame);
		void Cull(Frame& frame);
		void DrawGrid(Frame& frame);
		void DrawComments(Frame& frame);
		void DrawComment(Frame& frame, uint32_t index);
		void DrawWires(Frame& frame, bool highlighted);
		void DrawWire(Frame& frame, uint32_t link, bool highlighted);
		void DrawNodes(Frame& frame, bool selected);
		void DrawNode(Frame& frame, uint32_t index, bool selected);
		void DrawNodeBox(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state, VisualScriptDrawRecordItem& item);
		void DrawStandardNode(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state, VisualScriptDrawRecordItem& item);
		void DrawCompactShape(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state, VisualScriptDrawRecordItem& item);
		void DrawReroute(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state, VisualScriptDrawRecordItem& item);
		void DrawShadowAndGlow(Frame& frame, NodeState const& state, VisualScriptDrawRecordItem& item);
		void DrawNodeOutline(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state, float rounding,
		                     VisualScriptDrawRecordItem& item);
		void DrawBadges(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state, VisualScriptDrawRecordItem& item);
		void DrawStrip(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state, VisualScriptRect const& strip,
		               VisualScriptDrawRecordItem& item);
		void DrawBubble(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state, VisualScriptDrawRecordItem& item);
		void DrawPins(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state, VisualScriptDrawRecordItem& item);
		void DrawPin(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state, uint32_t pinIndex,
		             VisualScriptDrawRecordItem& item);
		void DrawPinGlyph(Frame& frame, glm::vec2 anchor, VisualScriptPinVisual const& pin, float scale, uint32_t color);
		void DrawBypass(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state, VisualScriptDrawRecordItem& item);
		void DrawChip(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state, VisualScriptPinVisual const& pin,
		              float alpha, VisualScriptDrawRecordItem& item);
		void DrawLiveChip(Frame& frame, VisualScriptNodeVisual const& node, NodeState const& state, uint32_t pinIndex,
		                  VisualScriptDrawRecordItem& item);
		// A chip's layout on the screen, with the live widget's metrics.
		VisualScriptLiveChip MakeChip(Frame const& frame, VisualScriptNodeVisual const& node, NodeState const& state,
		                              uint32_t pinIndex) const;
		void DrawExclamation(Frame& frame, glm::vec2 center, float radius, uint32_t color);
		void DrawOverlay(Frame& frame);
		void DrawPills(Frame& frame);
		void DrawMinimap(Frame& frame);
		void RebuildMinimapCells(Frame& frame, VisualScriptMinimapTransform const& transform, uint64_t selectionHash);

		// The pill texts, built into m_PillText.
		void AppendTypeName(Frame const& frame, std::string_view type, bool withID);
		void BuildPinText(Frame const& frame, uint32_t nodeIndex, uint32_t pinIndex);
		void BuildWireText(Frame const& frame, uint32_t link);

		// Strokes a polyline (screen pixels); with a dash length, cut into dashes along it. Returns the VtxBuffer range written.
		std::pair<uint32_t, uint32_t> Stroke(Frame& frame, std::span<glm::vec2 const> points, uint32_t color, float thickness,
		                                     float dash = 0.0f, float gap = 0.0f);
		// Recolours a stroke from one colour to another along its length, keeping each vertex's alpha. Returns false when the
		// stroke's vertices do not have AddPolyline's per-point layout, and ImGui's linear gradient between its ends was used.
		bool RecolorAlongLength(Frame& frame, std::pair<uint32_t, uint32_t> vertices, std::span<glm::vec2 const> points, float thickness,
		                        uint32_t from, uint32_t to);
		// The brushes' shapes, counted; false when they were not drawn.
		bool DrawSoftRect(Frame& frame, ImVec2 lower, ImVec2 upper, float reach, uint32_t color);
		bool DrawSoftDot(Frame& frame, glm::vec2 center, float radius, uint32_t color);
		// Draws text at a size of the text ladder (counted); the ellipsis is drawn after an ellipsized text.
		void DrawText(Frame& frame, float pixels, glm::vec2 position, uint32_t color, std::string_view text, bool ellipsis = false);
		void CountTextSize(float pixels);

		// The node's offset from its stored place: the move preview and an Arrange's glide.
		glm::vec2 GetNodeOffset(Frame const& frame, uint32_t index) const;
		// A link's control points as drawn this frame (canvas units).
		std::array<glm::vec2, 4> GetWirePoints(Frame const& frame, uint32_t link) const;
		// When the element started its latest flash; nothing when it is not flashing.
		std::optional<double> FindFlash(uint32_t element) const;

		VisualScriptRenderStats m_Stats;

		// Per frame, reused.
		std::vector<uint32_t> m_Query;
		std::vector<uint32_t> m_VisibleNodes;
		std::vector<uint64_t> m_NodeOrder;
		std::vector<uint32_t> m_VisibleComments;
		std::vector<uint32_t> m_VisibleLinks;
		std::vector<uint32_t> m_HighlightedLinks;
		// Per link of the visual model: CutMark, PickedMark, HighlightMark; m_MarkedLinks lists those set, to clear them.
		std::vector<uint8_t> m_LinkMarks;
		std::vector<uint32_t> m_MarkedLinks;
		// Per node: the frame that found it visible (m_Stamp), so the visible list holds each node once.
		std::vector<uint32_t> m_NodeStamps;
		uint32_t m_Stamp = 0;
		std::vector<glm::vec2> m_Polyline;
		std::vector<glm::vec2> m_DashPolyline;
		std::vector<float> m_Lengths;
		std::vector<ActivePulse> m_ActivePulses;
		// The distinct text sizes of the frame (the count goes on past the array).
		std::array<float, 16> m_TextSizes{};
		uint32_t m_TextSizeCount = 0;
		std::string m_Text;
		std::string m_PillText;

		// Animations.
		std::vector<Pulse> m_Pulses;
		std::vector<Flash> m_Flashes;
		std::vector<Glide> m_Glides;
		double m_GlideStart = 0.0;
		// (node index, glide) sorted by index, valid for m_GlideRevision of the canvas.
		std::vector<std::pair<uint32_t, uint32_t>> m_GlideIndices;
		uint64_t m_GlideRevision = ~0ull;
		double m_LastEventTime = -1.0;

		// Hover delays: what the mouse rests on (a hash of names, 0 for nothing) and since when.
		uint64_t m_HoverKey = 0;
		double m_HoverSince = 0.0;

		// The minimap's grid of nodes (a colour per cell, 0 for empty), rebuilt when the visual model, the minimap's transform
		// or the selection changes.
		std::vector<uint32_t> m_MinimapCells;
		uint32_t m_MinimapColumns = 0;
		uint32_t m_MinimapRows = 0;
		uint64_t m_MinimapRevision = ~0ull;
		uint64_t m_MinimapSelectionHash = 0;
		VisualScriptMinimapTransform m_MinimapTransform;
	};
}
