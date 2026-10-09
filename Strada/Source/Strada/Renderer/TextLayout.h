#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace Strada
{
	class FontAtlas;

	// Horizontal placement of text lines relative to the text's origin.
	enum class TextAlignment : uint8_t
	{
		// Lines start at the origin.
		Left = 0,
		// Lines are centered on it.
		Center,
		// Lines end at it.
		Right
	};

	struct TextLayoutSettings
	{
		TextAlignment Alignment = TextAlignment::Left;
		// Distance between consecutive baselines, in lines (1 is the font's line height).
		float LineSpacing = 1.0f;
	};

	// A glyph of laid-out text.
	struct TextGlyphQuad
	{
		// Corners in lines: x to the right and y downwards from the top of the first line, with the alignment anchor at
		// x = 0.
		glm::vec2 Min = glm::vec2(0.0f);
		glm::vec2 Max = glm::vec2(0.0f);
		// The glyph's pixels in the font atlas (see FontGlyph).
		glm::uvec2 AtlasMin = glm::uvec2(0);
		glm::uvec2 AtlasMax = glm::uvec2(0);
	};

	struct TextLayout
	{
		std::vector<TextGlyphQuad> Glyphs;
		// The text block (every line box, including spaces) in the glyphs' coordinates; empty for empty text.
		glm::vec2 Min = glm::vec2(0.0f);
		glm::vec2 Max = glm::vec2(0.0f);
		uint32_t LineCount = 0;
	};

	// Lays out UTF-8 text with the atlas's glyphs (adding missing ones): '\n' starts a line, '\t' advances to the next
	// multiple of four spaces, other control characters are skipped, and kerning applies between neighboring glyphs.
	TextLayout LayoutText(FontAtlas& atlas, std::string_view text, TextLayoutSettings const& settings = {});
}
