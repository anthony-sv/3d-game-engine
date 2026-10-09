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
		float const tabWidth = 4.0f * atlas.GetGlyph(U' ').Advance;
		float const ascent = atlas.GetAscent();
		float minX = std::numeric_limits<float>::max();
		float maxX = std::numeric_limits<float>::lowest();

		uint32_t line = 0;
		size_t lineStart = 0;
		float penX = 0.0f;
		char32_t previous = 0;
		// Aligns the glyphs of the finished line; penX is its width.
		auto const finishLine = [&]
		{
			float const offset = settings.Alignment == TextAlignment::Center  ? -0.5f * penX
			                     : settings.Alignment == TextAlignment::Right ? -penX
			                                                                  : 0.0f;
			for (size_t i = lineStart; i < layout.Glyphs.size(); i++)
			{
				layout.Glyphs[i].Min.x += offset;
				layout.Glyphs[i].Max.x += offset;
			}
			minX = std::min(minX, offset);
			maxX = std::max(maxX, offset + penX);
		};

		size_t offset = 0;
		while (offset < text.size())
		{
			char32_t const codepoint = Utf8::DecodeNext(text, offset);
			if (codepoint == U'\n')
			{
				finishLine();
				line++;
				lineStart = layout.Glyphs.size();
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

			if (previous != 0)
			{
				penX += atlas.GetKerning(previous, codepoint);
			}
			FontGlyph const& glyph = atlas.GetGlyph(codepoint);
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
		finishLine();

		layout.LineCount = line + 1;
		layout.Min = glm::vec2(minX, 0.0f);
		layout.Max = glm::vec2(maxX, static_cast<float>(line) * lineSpacing + 1.0f);
		return layout;
	}
}
