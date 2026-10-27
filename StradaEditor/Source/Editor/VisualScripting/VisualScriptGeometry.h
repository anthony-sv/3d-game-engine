#pragma once

#include "Editor/VisualScripting/VisualScriptCanvasModel.h"
#include "Editor/VisualScripting/VisualScriptView.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The one source of the canvas's geometry: every rectangle, anchor, curve and tolerance the renderer draws with comes from
// these functions, and hit testing calls the same ones, so what the mouse hits is what is drawn under it. Canvas units unless
// a name or a comment says screen pixels. Pure; any thread.
namespace Strada
{
	// How a wire meets its ends. It leaves an output to the right and reaches an input from the left, except at a reroute that
	// faces left (its first target lies to its left), so a wire can loop back cleanly through reroutes.
	struct VisualScriptWireEnds
	{
		// The wire leaves its start toward the left.
		bool FromReversed = false;
		// The wire reaches its end from the right.
		bool ToReversed = false;

		bool operator==(VisualScriptWireEnds const& other) const = default;
	};

	// Where nodes are drawn this frame other than at their stored places: moved by Offset during a move (a preview), and a
	// collapsed node drawn expanded under a dragged wire. Indexed by the canvas model's node indices; empty means none.
	struct VisualScriptNodePreview
	{
		// Non-zero for each moved node.
		std::span<uint8_t const> Moved;
		glm::vec2 Offset{0.0f};
		std::optional<uint32_t> Expanded;

		bool IsMoved(uint32_t node) const { return node < Moved.size() && Moved[node] != 0; }
		glm::vec2 GetOffset(uint32_t node) const { return IsMoved(node) ? Offset : glm::vec2(0.0f); }
	};

	// Where the minimap is on the screen and how it maps the canvas: the area it shows, centred in its rectangle at one scale
	// for both axes.
	struct VisualScriptMinimapTransform
	{
		// Screen pixels.
		VisualScriptRect Rect;
		// Canvas units: the bounds of the nodes and comments, grown on every side by VisualScriptCanvasMetrics::
		// MinimapContentMargin of their larger side. Empty when there is nothing to show.
		VisualScriptRect Bounds;
		// Screen pixels per canvas unit; 0 when there is nothing to show.
		float Scale = 0.0f;

		glm::vec2 ToScreen(glm::vec2 canvasPoint) const { return Rect.GetCenter() + (canvasPoint - Bounds.GetCenter()) * Scale; }
		// The canvas point a screen point of the minimap stands for; the middle of Bounds when nothing is shown.
		glm::vec2 ToCanvas(glm::vec2 screenPoint) const
		{
			return Scale > 0.0f ? Bounds.GetCenter() + (screenPoint - Rect.GetCenter()) / Scale : Bounds.GetCenter();
		}

		bool operator==(VisualScriptMinimapTransform const& other) const = default;
	};

	namespace VisualScriptGeometry
	{
		// A wire's tangents: forward (its input right of its output) between MinTangent and MaxTangent long; backward from
		// BackwardMinTangent, and a backward wire between ends closer in height than LoopDrop loops LoopDrop below them.
		inline constexpr float MinTangent = 40.0f;
		inline constexpr float MaxTangent = 400.0f;
		inline constexpr float BackwardMinTangent = 100.0f;
		inline constexpr float LoopDrop = 120.0f;
		// Flattening wires: at most MaxDepth subdivisions, and at most MaxSegments segments at Detail and Compact,
		// OverviewMaxSegments at Overview; at Far, FarSegments segments of equal steps.
		inline constexpr int MaxDepth = 10;
		inline constexpr int MaxSegments = 64;
		inline constexpr int OverviewMaxSegments = 12;
		inline constexpr int FarSegments = 3;
		// What the spatial index holds for a wire: its control points' box, this much larger on every side (the stroke).
		inline constexpr float WireBoundsMargin = 3.0f;

		// Comment titles: drawn at least CommentTitleMinGlyphs glyphs wide or not at all; at Far and Overview on a plate of the
		// comment's bar colour, CommentLabelPaddingLogicalPixels around the title and rounded by CommentLabelRoundingLogicalPixels
		// (both × UI scale).
		inline constexpr uint32_t CommentTitleMinGlyphs = 3;
		inline constexpr float CommentLabelPaddingLogicalPixels = 4.0f;
		inline constexpr float CommentLabelRoundingLogicalPixels = 4.0f;

		// The bits of VisualScriptHit::Edges: which edges of a comment box a point is on.
		inline constexpr uint8_t LeftEdge = 1u << 0;
		inline constexpr uint8_t TopEdge = 1u << 1;
		inline constexpr uint8_t RightEdge = 1u << 2;
		inline constexpr uint8_t BottomEdge = 1u << 3;

		// Nodes. Rectangles are absolute; offset moves the node, as a move preview does.
		//
		// Its rectangle as drawn (Position, Size).
		VisualScriptRect GetNodeRect(VisualScriptNodeVisual const& node, glm::vec2 offset = {});
		// The node as drawn expanded: a collapsed node under a dragged wire takes its ExpandedSize and its pin rows. Others are
		// returned unchanged.
		VisualScriptNodeVisual GetExpandedNode(VisualScriptNodeVisual node);
		// The header: a Standard node's top HeaderHeight (HeaderHeightWithSubtitle with a subtitle); a pill or an operator box
		// is all header; a reroute has none.
		VisualScriptRect GetHeaderRect(VisualScriptNodeVisual const& node, glm::vec2 offset = {});
		// Where the title text is drawn: left-aligned after the kind glyph's slot in a Standard header, centred in a pill (left
		// of its output) and in an operator box; as wide as the measured title and as high as its text size.
		VisualScriptRect GetTitleRect(VisualScriptNodeVisual const& node, glm::vec2 offset = {});
		// Under the title, in a header with a subtitle; empty without one.
		VisualScriptRect GetSubtitleRect(VisualScriptNodeVisual const& node, glm::vec2 offset = {});
		// HeaderGlyphSize boxes in a Standard header: the kind's glyph left of the title (events, function nodes), the latent
		// clock at the header's right end before the badges, and the collapsed marker after the title. Empty when the node
		// has no such glyph.
		VisualScriptRect GetKindGlyphRect(VisualScriptNodeVisual const& node, glm::vec2 offset = {});
		VisualScriptRect GetLatentGlyphRect(VisualScriptNodeVisual const& node, glm::vec2 offset = {});
		VisualScriptRect GetCollapsedMarkerRect(VisualScriptNodeVisual const& node, glm::vec2 offset = {});
		// The radius badges are drawn with: BadgeRadius × zoom logical pixels, at least BadgeMinLogicalPixels, in canvas units.
		float GetBadgeRadius(float pixelsPerUnit, float uiScale);
		// The badges' centres: the error badge's in the rightmost badge slot of a Standard header, else on the top-right corner;
		// a warning badge left of it (in the rightmost slot without errors). Nothing for a node without problems.
		std::optional<glm::vec2> GetErrorBadgeCenter(VisualScriptNodeVisual const& node, glm::vec2 offset = {});
		std::optional<glm::vec2> GetWarningBadgeCenter(VisualScriptNodeVisual const& node, float pixelsPerUnit, float uiScale,
		                                               glm::vec2 offset = {});
		// The box around the node's badges (circles of GetBadgeRadius around their centres); empty without problems.
		VisualScriptRect GetBadgeRect(VisualScriptNodeVisual const& node, float pixelsPerUnit, float uiScale, glm::vec2 offset = {});
		// The node comment's bubble above the node, its tail included; empty without a comment.
		VisualScriptRect GetBubbleRect(VisualScriptNodeVisual const& node, glm::vec2 offset = {});
		// The strip under a disabled or development-only node; empty for other nodes and for reroutes.
		VisualScriptRect GetEnabledStripRect(VisualScriptNodeVisual const& node, glm::vec2 offset = {});
		// A reroute's dot (and any node's middle).
		glm::vec2 GetRerouteCenter(VisualScriptNodeVisual const& node, glm::vec2 offset = {});

		// Pins.
		//
		// Where the pin's wires end: its glyph's centre; for a collapsed node, the middle of its header's left edge (inputs) or
		// right edge (outputs).
		glm::vec2 GetPinAnchor(VisualScriptNodeVisual const& node, VisualScriptPinVisual const& pin, glm::vec2 offset = {});
		// Whether pins are drawn with their labels at a level and zoom: at Detail, and at Compact while PinLabelTextSize text
		// is drawn (13 z >= 7). Elsewhere pins are dots without labels, hit only around their glyphs, so that pressing a node's
		// body there selects and moves it. The renderer draws pin labels by the same rule.
		bool ArePinLabelsDrawn(VisualScriptDetailLevel level, float zoom);
		// Where pressing starts a wire from the pin: its row up to the end of its label when labelRows (ArePinLabelsDrawn), and
		// at least minimumCu around its anchor (a collapsed node's pins, and every pin without labelRows: around the anchor
		// only).
		VisualScriptRect GetPinHitRect(VisualScriptNodeVisual const& node, VisualScriptPinVisual const& pin, float minimumCu,
		                               glm::vec2 offset = {}, bool labelRows = true);
		// The label's box (its measured width, PinLabelTextSize high, centred on the row): after the glyph for inputs, before it
		// for outputs. Empty for unlabelled pins (LabelHidden, compact shapes).
		VisualScriptRect GetPinLabelRect(VisualScriptNodeVisual const& node, VisualScriptPinVisual const& pin, glm::vec2 offset = {});
		// The value chip; empty when the pin draws none.
		VisualScriptRect GetChipRect(VisualScriptNodeVisual const& node, VisualScriptPinVisual const& pin, glm::vec2 offset = {});
		// A cell of a Cells chip (vector and quaternion components), ChipVectorCellWidth wide and ChipCellGap apart.
		VisualScriptRect GetChipCellRect(VisualScriptRect const& chip, uint32_t cell);

		// Wires. Points and bounds are canvas units unless named screen.
		//
		// The cubic Bézier of a wire from an output's anchor to an input's anchor. Forward (the input right of the output, as
		// the ends face): tangents of clamp(0.5 |dx| + 0.25 |dy|, MinTangent, MaxTangent). Backward: clamp(0.5 |dx| + 0.5 |dy|,
		// BackwardMinTangent, MaxTangent), and with both ends facing right and heights closer than LoopDrop the wire loops
		// below both nodes.
		std::array<glm::vec2, 4> GetWireControlPoints(glm::vec2 from, glm::vec2 to, VisualScriptWireEnds ends);
		// The box of the control points, which holds the whole curve.
		VisualScriptRect GetWireBounds(std::array<glm::vec2, 4> const& points);
		// A link's wire as drawn this frame: its pins' anchors (moved and expanded nodes as the preview says) and the facing of
		// reroutes at its ends.
		std::array<glm::vec2, 4> GetLinkControlPoints(VisualScriptCanvasModel const& model, uint32_t link,
		                                              VisualScriptNodePreview const& preview = {});
		// What the spatial index holds for a link: the box of its stored control points, WireBoundsMargin larger; with a
		// preview, of the control points it is drawn with.
		VisualScriptRect GetLinkBounds(VisualScriptCanvasModel const& model, uint32_t link, VisualScriptNodePreview const& preview = {});
		// How finely wires are flattened at a level of detail, given ImGuiStyle::CurveTessellationTol.
		struct WireFlattening
		{
			// Screen pixels; 0 flattens into SegmentCap equal steps.
			float Tolerance = 0.0f;
			// The most segments.
			int SegmentCap = MaxSegments;
		};
		WireFlattening GetWireFlattening(VisualScriptDetailLevel level, float curveTessellationTolerance);
		// The polyline the renderer strokes, in screen pixels, its first point included: ImGui's recursive subdivision with its
		// flatness test (to MaxDepth), or, when that gives more than maxSegments segments or toleranceScreen is 0, maxSegments
		// equal steps of the curve's parameter. Replaces out's contents.
		void FlattenWire(std::array<glm::vec2, 4> const& screenPoints, float toleranceScreen, int maxSegments, std::vector<glm::vec2>& out);

		// How far a point is from a polyline, and where on it the nearest point is (as a fraction of its length from its first
		// point). Screen pixels where the renderer and hit testing use it. A polyline of one point is that point; none is
		// infinitely far.
		struct WireDistance
		{
			float Distance = 0.0f;
			float ArcFraction = 0.0f;
		};
		WireDistance MeasureDistance(std::span<glm::vec2 const> polyline, glm::vec2 point);
		// The point at a fraction of a polyline's length from its first point (MeasureDistance's ArcFraction), the fraction
		// clamped to [0, 1]; the origin for an empty polyline.
		glm::vec2 GetPolylinePoint(std::span<glm::vec2 const> polyline, float arcFraction);
		// Whether the segment from a to b crosses or touches the polyline: the cut line.
		bool SegmentCrossesPolyline(glm::vec2 a, glm::vec2 b, std::span<glm::vec2 const> polyline);

		// Comment boxes.
		//
		// The title bar: CommentTitleBarHeight, but never lower on the screen than the title's pixel size plus
		// CommentTitleBarPaddingLogicalPixels, nor higher than the comment.
		VisualScriptRect GetCommentBarRect(VisualScriptCommentVisual const& comment, float pixelsPerUnit, float uiScale);
		// The text size of a comment's title on the screen: CommentTitleTextSize × zoom, at least CommentTitleMinLogicalPixels,
		// times the UI scale, floored to the text ladder.
		float GetCommentTitlePixels(float pixelsPerUnit, float uiScale);
		// Where a comment's title is drawn, in screen pixels (spec §11.1 rule 3): GetCommentTitlePixels high, centred in the
		// title bar, from max(the box's left edge, the canvas's) + an inset of max(HorizontalPadding cu, 4 logical pixels), and
		// as wide as the title at that size (titleWidthCu is its width at CommentTitleTextSize, VisualScriptTextRef::Width) but no
		// wider than the box and the canvas leave before the same inset on the right. Empty when the bar is above or below the
		// canvas or lower than the text, and when less than CommentTitleMinGlyphs of the title's average glyph width fits (the
		// average of the title's drawn bytes, its ellipsis counted as one; a title that is shorter needs its whole width).
		VisualScriptRect GetCommentTitleRect(VisualScriptCommentVisual const& comment, VisualScriptView const& view, float titleWidthCu);
		// The plate a comment's title is drawn on at Far and Overview, above the nodes (spec §11.1 rules 4 and 5): the title's
		// rectangle grown by CommentLabelPaddingLogicalPixels × UI scale, which keeps it inside the box and the canvas. Empty when
		// the title is. Screen pixels; the renderer draws it and hit testing tests it, so what is hit is what is drawn.
		VisualScriptRect GetCommentLabelRect(VisualScriptCommentVisual const& comment, VisualScriptView const& view, float titleWidthCu);
		// Which edges of the comment a point is within tolerance of (LeftEdge | TopEdge | ...): a corner gives two. 0 when the
		// point is farther from every edge, or outside the comment's box grown by the tolerance.
		uint8_t GetCommentEdges(VisualScriptCommentVisual const& comment, glm::vec2 point, float tolerance);

		// The minimap.
		//
		// Its rectangle in screen pixels for a canvas rectangle and UI scale (VisualScriptCanvasMetrics' Minimap metrics).
		VisualScriptRect GetMinimapRect(glm::vec2 canvasMin, glm::vec2 canvasSize, float uiScale);
		// How a minimap rectangle shows content of these bounds (nodes and comments, canvas units); nothing to show without
		// bounds or with an empty rectangle.
		VisualScriptMinimapTransform GetMinimapTransform(VisualScriptRect const& minimapRect,
		                                                 std::optional<VisualScriptRect> const& contentBounds);

		// Rectangles.
		bool Contains(VisualScriptRect const& rect, glm::vec2 point);
		bool Intersects(VisualScriptRect const& a, VisualScriptRect const& b);
		bool IsEmpty(VisualScriptRect const& rect);
		// The smallest rectangle holding both; an empty one counts as nothing.
		VisualScriptRect Unite(VisualScriptRect const& a, VisualScriptRect const& b);
		VisualScriptRect Expand(VisualScriptRect const& rect, float amount);
		VisualScriptRect Translate(VisualScriptRect const& rect, glm::vec2 offset);
	}
}
