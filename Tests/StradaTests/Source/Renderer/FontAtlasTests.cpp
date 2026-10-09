#include "Renderer/FontTestUtilities.h"

#include "Strada/Core/Utf8.h"

#include <doctest/doctest.h>

#include <vector>

using namespace Strada;

namespace
{
	uint8_t PixelAt(FontAtlas const& atlas, glm::uvec2 const& pixel)
	{
		return atlas.GetPixels()[static_cast<size_t>(pixel.y) * atlas.GetWidth() + pixel.x];
	}

	std::vector<uint8_t> CopyPixels(FontAtlas const& atlas, FontGlyph const& glyph)
	{
		std::vector<uint8_t> pixels;
		for (uint32_t y = glyph.AtlasMin.y; y < glyph.AtlasMax.y; y++)
		{
			for (uint32_t x = glyph.AtlasMin.x; x < glyph.AtlasMax.x; x++)
			{
				pixels.push_back(PixelAt(atlas, glm::uvec2(x, y)));
			}
		}
		return pixels;
	}
}

TEST_CASE("FontAtlas: glyphs are distance fields with metrics in lines")
{
	Scope<FontAtlas> const atlas = Testing::CreateDefaultFontAtlas();
	CHECK(atlas->GetAscent() > 0.5f);
	CHECK(atlas->GetAscent() < 1.0f);
	CHECK(atlas->GetPixels().size() == static_cast<size_t>(atlas->GetWidth()) * atlas->GetHeight());

	FontGlyph const& space = atlas->GetGlyph(U' ');
	CHECK_FALSE(space.HasQuad());
	CHECK(space.Advance > 0.1f);

	FontGlyph const& bar = atlas->GetGlyph(U'I');
	REQUIRE(bar.HasQuad());
	CHECK(bar.Advance > 0.0f);
	// Rendered at PixelsPerLine: the quad covers exactly its atlas pixels.
	CHECK((bar.PlaneMax.x - bar.PlaneMin.x) * FontAtlas::PixelsPerLine ==
	      doctest::Approx(static_cast<float>(bar.AtlasMax.x - bar.AtlasMin.x)));
	CHECK((bar.PlaneMax.y - bar.PlaneMin.y) * FontAtlas::PixelsPerLine ==
	      doctest::Approx(static_cast<float>(bar.AtlasMax.y - bar.AtlasMin.y)));
	// 'I' stands on the baseline (the margin reaches below it) and rises well above it.
	CHECK(bar.PlaneMin.y < 0.0f);
	CHECK(bar.PlaneMax.y > 0.5f);
	// The middle of the bar is inside the outline, the corner of the margin outside.
	CHECK(PixelAt(*atlas, (bar.AtlasMin + bar.AtlasMax) / 2u) > FontAtlas::EdgeValue);
	CHECK(PixelAt(*atlas, bar.AtlasMin) < FontAtlas::EdgeValue);

	// Kerning tightens pairs like "AV" in this font.
	CHECK(atlas->GetKerning(U'A', U'V') < 0.0f);
	CHECK(atlas->GetKerning(U'A', U'V') == atlas->GetKerning(U'A', U'V'));
}

TEST_CASE("FontAtlas: characters render on first use and missing ones share the replacement glyph")
{
	Scope<FontAtlas> const atlas = Testing::CreateDefaultFontAtlas();
	uint64_t const version = atlas->GetVersion();
	FontGlyph const& accented = atlas->GetGlyph(U'é');
	CHECK(accented.HasQuad());
	CHECK(atlas->GetVersion() > version);
	uint64_t const rendered = atlas->GetVersion();
	CHECK(atlas->GetGlyph(U'é').AtlasMin == accented.AtlasMin);
	CHECK(atlas->GetVersion() == rendered);

	// The default font has no CJK characters.
	CHECK_FALSE(atlas->HasGlyph(U'中'));
	char32_t const fallback = atlas->HasGlyph(Utf8::ReplacementCharacter) ? Utf8::ReplacementCharacter : U'?';
	FontGlyph const missing = atlas->GetGlyph(U'中');
	FontGlyph const& expected = atlas->GetGlyph(fallback);
	CHECK(missing.HasQuad());
	CHECK(missing.AtlasMin == expected.AtlasMin);
	CHECK(missing.Advance == expected.Advance);
	CHECK_FALSE(atlas->HasGlyph(0x110000));
}

TEST_CASE("FontAtlas: the image grows when it is full and keeps glyphs in place")
{
	Scope<FontAtlas> const atlas = Testing::CreateDefaultFontAtlas();
	FontGlyph const glyph = atlas->GetGlyph(U'W');
	std::vector<uint8_t> const pixels = CopyPixels(*atlas, glyph);
	uint32_t const width = atlas->GetWidth();

	// Latin-1, Latin Extended and Greek: more glyphs than the image holds.
	for (char32_t codepoint = 0xA1; codepoint < 0x3FF && atlas->GetWidth() == width; codepoint++)
	{
		(void)atlas->GetGlyph(codepoint);
	}
	CHECK(atlas->GetWidth() > width);
	CHECK(atlas->GetWidth() <= FontAtlas::MaxSize);
	CHECK(atlas->GetPixels().size() == static_cast<size_t>(atlas->GetWidth()) * atlas->GetHeight());
	CHECK(atlas->GetGlyph(U'W').AtlasMin == glyph.AtlasMin);
	CHECK(CopyPixels(*atlas, glyph) == pixels);
}

TEST_CASE("FontAtlas: invalid fonts are refused")
{
	CHECK(FontAtlas::Create(nullptr).IsError());
}
