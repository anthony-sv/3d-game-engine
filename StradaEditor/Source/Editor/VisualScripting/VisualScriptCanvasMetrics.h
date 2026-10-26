#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>

namespace Strada
{
	// Measures text for the canvas's layout: the width, in canvas units, of text drawn at a size in canvas units. The editor
	// measures once per model sync with its UI font at ImGui::GetFontSize() and scales the width linearly to the size, which is
	// exact for its fonts (their advances are linear in size); tests pass a fixed-width measurer.
	struct VisualScriptTextMeasurer
	{
		std::function<float(std::string_view text, float sizeCu)> Measure;
		// Identifies what Measure measures with (the editor: its font, base size and UI scale). The canvas model measures every
		// element again when it changes.
		uint64_t Signature = 0;

		// 0 without a function.
		float operator()(std::string_view text, float sizeCu) const { return Measure ? Measure(text, sizeCu) : 0.0f; }
	};

	// The pixel sizes the canvas draws text at: every integer from 6 to 20, then steps of at most 10 % up to 82. Whole sizes
	// render crisp (ImGui draws a baked size at scale 1), and the few sizes bound what ImGui bakes into its font atlas.
	inline constexpr std::array<float, 33> VisualScriptTextLadder = {
		6.0f,  7.0f,  8.0f,  9.0f,  10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f, 17.0f, 18.0f, 19.0f, 20.0f, 22.0f, 24.0f,
		26.0f, 28.0f, 30.0f, 32.0f, 35.0f, 38.0f, 41.0f, 44.0f, 48.0f, 52.0f, 56.0f, 60.0f, 65.0f, 70.0f, 76.0f, 82.0f};

	// The sizes of the canvas's elements, in canvas units (the units of positions in documents; on the screen they are
	// multiplied by the view's zoom and the UI scale) unless a name ends in LogicalPixels: pixels at a UI scale of 1, which the
	// UI scale multiplies but the zoom does not.
	namespace VisualScriptCanvasMetrics
	{
		// Nodes. The corner radius is the editor theme's WindowRounding.
		inline constexpr float NodeCornerRadius = 6.0f;
		inline constexpr float HeaderHeight = 28.0f;
		inline constexpr float HeaderHeightWithSubtitle = 44.0f;
		inline constexpr float TitleTextSize = 15.0f;
		inline constexpr float SubtitleTextSize = 12.0f;
		inline constexpr float PinLabelTextSize = 13.0f;
		inline constexpr float ChipTextSize = 13.0f;
		inline constexpr float NodeCommentTextSize = 13.0f;
		inline constexpr float HorizontalPadding = 10.0f;
		// The slot left of the title that holds the kind's glyph (an event's diamond, a function's "ƒ"). The latent clock at
		// the header's right end and the collapsed marker after the title take slots of the same width. Each glyph is drawn in
		// a HeaderGlyphSize box centred in its slot.
		inline constexpr float KindGlyphSlot = 16.0f;
		inline constexpr float HeaderGlyphSize = 12.0f;
		// With a subtitle, the subtitle's text starts this far below the node's top (the title stays centred in the first
		// HeaderHeight).
		inline constexpr float SubtitleTop = 26.0f;
		// Nodes are measured to fit their title and pins within these widths; longer titles are ellipsized.
		inline constexpr float MinNodeWidth = 120.0f;
		inline constexpr float MaxNodeWidth = 480.0f;
		// Problem badges sit in slots of BadgeSlot at the right end of the header's first HeaderHeight, the error badge
		// rightmost. They are drawn with a radius of BadgeRadius × zoom logical pixels, never below BadgeMinLogicalPixels.
		inline constexpr float BadgeSlot = 16.0f;
		inline constexpr float BadgeRadius = 8.0f;
		inline constexpr float BadgeMinLogicalPixels = 6.0f;
		// Disabled and development-only nodes have a strip of this height under their body, which is part of the node.
		inline constexpr float EnabledStripHeight = 20.0f;
		// A node's shadow reaches this far beyond its edges, offset down.
		inline constexpr float ShadowReach = 12.0f;
		inline constexpr float ShadowOffsetY = 3.0f;

		// Pin rows: the first starts PinRowsTop below the header, the last ends PinRowsBottom above the node's bottom edge. A
		// pin's glyph is centred PinGlyphInset inside the node's edge (wires end there) and takes PinGlyphRadius on each side;
		// its label starts PinLabelGap after the glyph, and an input's value chip PinLabelGap after its label. The input and
		// output columns stay at least MinPinColumnGap apart.
		inline constexpr float PinRowHeight = 24.0f;
		inline constexpr float PinRowsTop = 6.0f;
		inline constexpr float PinRowsBottom = 8.0f;
		inline constexpr float PinGlyphInset = 10.0f;
		inline constexpr float PinGlyphRadius = 5.0f;
		inline constexpr float PinLabelGap = 6.0f;
		inline constexpr float MinPinColumnGap = 24.0f;

		// Compact shapes: Variable.Get and Self are pills with the name centred; binary Operator nodes are boxes with their
		// symbol and unlabelled pins; a Reroute is a dot, hit within a radius in logical pixels so it stays easy to grab.
		inline constexpr float PillHeight = 28.0f;
		inline constexpr float PillMinWidth = 48.0f;
		inline constexpr float OperatorWidth = 64.0f;
		inline constexpr float OperatorHeight = 48.0f;
		inline constexpr float OperatorSymbolTextSize = 22.0f;
		inline constexpr float RerouteRadius = 6.0f;
		inline constexpr float RerouteHitRadiusLogicalPixels = 10.0f;

		// Value chips: the drawn twins of the live widget's compact form, one frame per cell, text ChipTextSize centred
		// vertically between ChipPaddingY above and below.
		inline constexpr float ChipHeight = 19.0f;
		inline constexpr float ChipPaddingX = 5.0f;
		inline constexpr float ChipPaddingY = 3.0f;
		// A bool's check box and a colour's swatch are square.
		inline constexpr float ChipSquareSize = ChipHeight;
		// int and uint; long and ulong; float and double.
		inline constexpr float ChipIntegerWidth = 64.0f;
		inline constexpr float ChipLongWidth = 96.0f;
		inline constexpr float ChipFloatWidth = 64.0f;
		inline constexpr float ChipStringWidth = 96.0f;
		// Vectors (2 to 4 cells) and quaternions (3 cells, Euler degrees): one cell per component, ChipCellGap apart, each
		// with a stripe of its axis's colour at its left edge.
		inline constexpr float ChipVectorCellWidth = 44.0f;
		inline constexpr float ChipCellGap = 2.0f;
		inline constexpr float ChipAxisStripeWidth = 2.0f;
		// Enums and [Flags] enums (the name or "A | B" and the arrow box), assets (the file name), and Matrix4 (the text
		// "Matrix": matrices are edited in Details).
		inline constexpr float ChipEnumWidth = 96.0f;
		inline constexpr float ChipAssetWidth = 112.0f;
		inline constexpr float ChipMatrixWidth = 40.0f;
		// The arrow box at the right end of an enum's chip, as a combo's arrow button is a square of its frame's height.
		inline constexpr float ChipArrowBoxWidth = ChipHeight;

		// A node's comment is a bubble above its header, its text wrapped at NodeCommentWrapWidth inside NodeCommentPadding,
		// with a tail toward the node. A longer comment shows its first NodeCommentMaxLines lines (Details shows all of it).
		inline constexpr float NodeCommentWrapWidth = 240.0f;
		inline constexpr float NodeCommentPadding = 6.0f;
		inline constexpr float NodeCommentTailWidth = 8.0f;
		inline constexpr float NodeCommentTailHeight = 6.0f;
		inline constexpr int NodeCommentMaxLines = 16;

		// Comment boxes: a title bar with the first line of the text, the further lines below it wrapped CommentBodyInset
		// inside the box's width. Boxes the canvas makes or resizes are at least CommentMinWidth by CommentMinHeight.
		inline constexpr float CommentTitleBarHeight = 40.0f;
		inline constexpr float CommentTitleTextSize = 18.0f;
		inline constexpr float CommentBodyTextSize = 13.0f;
		inline constexpr float CommentBodyInset = 16.0f;
		inline constexpr float CommentMinWidth = 96.0f;
		inline constexpr float CommentMinHeight = 64.0f;

		// Text smaller than this on the screen at a UI scale of 1 is not drawn: it would be unreadable and bake more sizes.
		inline constexpr float MinTextLogicalPixels = 7.0f;
		// Subtitles need a little more (at Detail, from a zoom of 2/3).
		inline constexpr float MinSubtitleLogicalPixels = 8.0f;
		// Readable floors: node titles at Overview are drawn at TitleTextSize × zoom but never below
		// OverviewTitleMinLogicalPixels (and not at all when fewer than OverviewTitleMinGlyphs glyphs fit their box); comment
		// titles at CommentTitleTextSize × zoom but never below CommentTitleMinLogicalPixels, so a zoomed-out graph reads like
		// a map of labelled regions.
		inline constexpr float OverviewTitleMinLogicalPixels = 9.0f;
		inline constexpr int OverviewTitleMinGlyphs = 3;
		inline constexpr float CommentTitleMinLogicalPixels = 12.0f;
		// A comment's title bar is CommentTitleBarHeight high, but never lower on the screen than its title's pixel size plus
		// CommentTitleBarPaddingLogicalPixels (× UI scale), nor higher than the comment.
		inline constexpr float CommentTitleBarPaddingLogicalPixels = 10.0f;

		// How close the mouse must come to an element to hit it, in logical pixels: a pin's glyph (beyond its row), a wire's
		// drawn line, and a comment's edges and corners (its resize handles).
		inline constexpr float PinSlopLogicalPixels = 6.0f;
		inline constexpr float WireHitLogicalPixels = 6.0f;
		inline constexpr float CommentHandleLogicalPixels = 8.0f;

		// Interaction. A press becomes a drag once the mouse moved more than ClickDragThresholdPixels screen pixels from it (the
		// editor's threshold, ViewportPanel.cpp, unscaled as there); a dragged wire snaps to a pin whose glyph is within
		// WireSnapLogicalPixels. Moves and resizes snap to GridSize, and an arrow key nudges by one GridSize (Alt+arrow: 1).
		inline constexpr float ClickDragThresholdPixels = 4.0f;
		inline constexpr float WireSnapLogicalPixels = 16.0f;
		inline constexpr float GridSize = 16.0f;
		// A wire resting this long over a collapsed node draws it expanded, and a node dragged over a wire this long makes the
		// wire its splice target; a Space press released within SpaceTapSeconds is a tap (the palette).
		inline constexpr double HoverExpandSeconds = 0.3;
		inline constexpr double SpaceTapSeconds = 0.25;

		// The minimap: at the canvas's bottom-right corner, MinimapMarginLogicalPixels inside it, MinimapWidthShare of the
		// canvas's width within MinimapMinWidthLogicalPixels and MinimapMaxWidthLogicalPixels, MinimapAspect as high as wide. It
		// shows the bounds of the nodes and comments grown by MinimapContentMargin of their larger side on every side, and fades
		// in or out over MinimapFadeSeconds.
		inline constexpr float MinimapMarginLogicalPixels = 12.0f;
		inline constexpr float MinimapWidthShare = 0.2f;
		inline constexpr float MinimapMinWidthLogicalPixels = 160.0f;
		inline constexpr float MinimapMaxWidthLogicalPixels = 280.0f;
		inline constexpr float MinimapAspect = 0.65f;
		inline constexpr float MinimapContentMargin = 0.05f;
		inline constexpr double MinimapFadeSeconds = 0.2;

		static_assert(ChipHeight == ChipTextSize + 2.0f * ChipPaddingY);

		// Whether text of sizeCu canvas units is drawn at a zoom: when it is at least MinTextLogicalPixels at a UI scale of 1.
		constexpr bool IsTextVisible(float sizeCu, float zoom)
		{
			return sizeCu * zoom >= MinTextLogicalPixels;
		}

		// The pixel size text is drawn at when the layout gives it pixels (its size in canvas units × zoom × UI scale): the
		// largest ladder size that is not larger, so text never overflows a box measured in canvas units and is at most one
		// step smaller. 0 below the smallest size (also for NaN): nothing that small is drawn.
		constexpr float LadderFloor(float pixels)
		{
			float size = 0.0f;
			for (float const step : VisualScriptTextLadder)
			{
				if (!(step <= pixels))
				{
					break;
				}
				size = step;
			}
			return size;
		}
	}
}
