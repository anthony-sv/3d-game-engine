#pragma once

#include "Editor/VisualScripting/VisualScriptCanvasMetrics.h"

#include <array>
#include <cstdint>
#include <string_view>

// Text widths without a font: what a canvas measures with when no window draws it (graph.arrange, graph.edit's measured
// operations). Layer L3: pure, no ImGui; any thread.
namespace Strada
{
	// The advance of each code point from 32 (space) to 126 ('~') in the editor's UI font (the embedded Roboto Medium) at a
	// font size of 1, in 1/64 of the size, rounded up: ImGui's advance (stb_truetype's advance in font units times the
	// font's scale for a pixel height of 1) of the font at size 64, rounded up. A headless test regenerates it from the font
	// and compares.
	extern std::array<uint16_t, 95> const VisualScriptTextAdvances;

	// The margin every advance of VisualScriptTextAdvances is multiplied by: it absorbs what the table's rounding does not
	// (the window's measurer baking a pixel size, its UI scale), so an estimate is never narrower than a window's measure.
	inline constexpr float VisualScriptTextEstimateMargin = 1.04f;

	// The width of a text at sizeCu as the editor's UI font draws it, never narrower: each code point 32..126 counts its
	// advance (VisualScriptTextAdvances) times VisualScriptTextEstimateMargin; every other code point, and each byte of a
	// sequence that is not UTF-8, counts 1 (the font size, wider than any of the font's glyphs).
	[[nodiscard]] float EstimateVisualScriptTextWidth(std::string_view text, float sizeCu);

	// A VisualScriptTextMeasurer over EstimateVisualScriptTextWidth, Signature Hash::FNV1a("VisualScriptEstimatedText v1").
	[[nodiscard]] VisualScriptTextMeasurer MakeEstimatingVisualScriptTextMeasurer();
}
