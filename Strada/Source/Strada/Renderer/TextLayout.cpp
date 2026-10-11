#include "stpch.h"
#include "Strada/Renderer/TextLayout.h"

#include "Strada/Core/Utf8.h"
#include "Strada/Renderer/FontAtlas.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Strada
{
	TextLayout LayoutText(FontAtlas& atlas, std::string_view text, TextLayoutSettings const& settings)
	{
		TextLayout layout;
		if (text.empty())
		{
			return layout;
		}

		float const lineSpacing = std::max(settings.LineSpacing, 0.0f);
		// Widths that are not positive and finite never wrap.
		float const wrapWidth = std::isfinite(settings.WrapWidth) ? std::max(settings.WrapWidth, 0.0f) : 0.0f;
		float const tabWidth = 4.0f * atlas.GetGlyph(U' ').Advance;
		float const ascent = atlas.GetAscent();
		float minX = std::numeric_limits<float>::max();
		float maxX = std::numeric_limits<float>::lowest();

		uint32_t line = 0;
		size_t lineStart = 0;
		float penX = 0.0f;
		char32_t previous = 0;
		// The line's last run of spaces: when a word does not fit, the glyphs from breakGlyph on (the words after the spaces)
		// move to the next line, and the line ends where the spaces start.
		bool hasBreak = false;
		size_t breakGlyph = 0;
		float breakLineWidth = 0.0f;
		float breakWordStart = 0.0f;

		// Aligns the glyphs [lineStart, end) of a finished line that is width wide.
		auto const finishLine = [&](size_t end, float width)
		{
			float const offset = settings.Alignment == TextAlignment::Center  ? -0.5f * width
			                     : settings.Alignment == TextAlignment::Right ? -width
			                                                                  : 0.0f;
			for (size_t i = lineStart; i < end; i++)
			{
				layout.Glyphs[i].Min.x += offset;
				layout.Glyphs[i].Max.x += offset;
			}
			minX = std::min(minX, offset);
			maxX = std::max(maxX, offset + width);
		};
		auto const startLine = [&](size_t start)
		{
			line++;
			lineStart = start;
			hasBreak = false;
		};

		size_t offset = 0;
		while (offset < text.size())
		{
			char32_t const codepoint = Utf8::DecodeNext(text, offset);
			if (codepoint == U'\n')
			{
				finishLine(layout.Glyphs.size(), penX);
				startLine(layout.Glyphs.size());
				penX = 0.0f;
				previous = 0;
				continue;
			}
			if (codepoint == U'\t')
			{
				penX = tabWidth > 0.0f ? (std::floor(penX / tabWidth + 1e-4f) + 1.0f) * tabWidth : penX;
				previous = 0;
				continue;
			}
			if (codepoint < 0x20 || codepoint == 0x7F)
			{
				continue;
			}

			float kerning = previous != 0 ? atlas.GetKerning(previous, codepoint) : 0.0f;
			FontGlyph const& glyph = atlas.GetGlyph(codepoint);
			if (codepoint == U' ')
			{
				// Spaces never wrap: they hang at the end of the line they follow.
				if (!hasBreak || previous != U' ')
				{
					breakLineWidth = penX;
				}
				hasBreak = true;
				breakGlyph = layout.Glyphs.size();
				penX += kerning + glyph.Advance;
				breakWordStart = penX;
				previous = codepoint;
				continue;
			}
			if (wrapWidth > 0.0f && penX + kerning + glyph.Advance > wrapWidth)
			{
				if (hasBreak)
				{
					finishLine(breakGlyph, breakLineWidth);
					startLine(breakGlyph);
					glm::vec2 const shift(-breakWordStart, lineSpacing);
					for (size_t i = lineStart; i < layout.Glyphs.size(); i++)
					{
						layout.Glyphs[i].Min += shift;
						layout.Glyphs[i].Max += shift;
					}
					penX -= breakWordStart;
				}
				// A word longer than a line breaks before the character that does not fit (a line keeps at least one).
				if (penX > 0.0f && penX + kerning + glyph.Advance > wrapWidth)
				{
					finishLine(layout.Glyphs.size(), penX);
					startLine(layout.Glyphs.size());
					penX = 0.0f;
					kerning = 0.0f;
				}
			}

			penX += kerning;
			if (glyph.HasQuad())
			{
				float const baseline = ascent + static_cast<float>(line) * lineSpacing;
				TextGlyphQuad quad;
				quad.Min = glm::vec2(penX + glyph.PlaneMin.x, baseline - glyph.PlaneMax.y);
				quad.Max = glm::vec2(penX + glyph.PlaneMax.x, baseline - glyph.PlaneMin.y);
				quad.AtlasMin = glyph.AtlasMin;
				quad.AtlasMax = glyph.AtlasMax;
				layout.Glyphs.push_back(quad);
			}
			penX += glyph.Advance;
			previous = codepoint;
		}
		finishLine(layout.Glyphs.size(), penX);

		layout.LineCount = line + 1;
		layout.Min = glm::vec2(minX, 0.0f);
		layout.Max = glm::vec2(maxX, static_cast<float>(line) * lineSpacing + 1.0f);
		return layout;
	}
}
