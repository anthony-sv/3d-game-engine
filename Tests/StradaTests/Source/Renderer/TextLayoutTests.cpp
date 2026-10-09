#include "Renderer/FontTestUtilities.h"

#include "Strada/Renderer/TextLayout.h"

#include <doctest/doctest.h>

using namespace Strada;

TEST_CASE("TextLayout: glyphs follow each other on the baseline with kerning")
{
	Scope<FontAtlas> const atlas = Testing::CreateDefaultFontAtlas();
	FontGlyph const a = atlas->GetGlyph(U'A');
	FontGlyph const v = atlas->GetGlyph(U'V');
	float const kerning = atlas->GetKerning(U'A', U'V');

	TextLayout const layout = LayoutText(*atlas, "AV");
	REQUIRE(layout.Glyphs.size() == 2);
	CHECK(layout.LineCount == 1);
	CHECK(layout.Glyphs[0].Min.x == doctest::Approx(a.PlaneMin.x));
	CHECK(layout.Glyphs[1].Min.x == doctest::Approx(a.Advance + kerning + v.PlaneMin.x));
	// y grows downwards from the top of the line; the baseline is at the ascent.
	CHECK(layout.Glyphs[0].Max.y == doctest::Approx(atlas->GetAscent() - a.PlaneMin.y));
	CHECK(layout.Glyphs[0].Min.y == doctest::Approx(atlas->GetAscent() - a.PlaneMax.y));
	CHECK(layout.Glyphs[1].AtlasMin == v.AtlasMin);
	CHECK(layout.Min.x == doctest::Approx(0.0f));
	CHECK(layout.Max.x == doctest::Approx(a.Advance + kerning + v.Advance));
	CHECK(layout.Min.y == doctest::Approx(0.0f));
	CHECK(layout.Max.y == doctest::Approx(1.0f));
}

TEST_CASE("TextLayout: alignment places lines relative to the origin")
{
	Scope<FontAtlas> const atlas = Testing::CreateDefaultFontAtlas();
	TextLayout const left = LayoutText(*atlas, "WWW\nI", {TextAlignment::Left, 1.0f});
	TextLayout const center = LayoutText(*atlas, "WWW\nI", {TextAlignment::Center, 1.0f});
	TextLayout const right = LayoutText(*atlas, "WWW\nI", {TextAlignment::Right, 1.0f});
	float const width = left.Max.x - left.Min.x;
	REQUIRE(width > 0.0f);

	CHECK(left.Min.x == doctest::Approx(0.0f));
	CHECK(center.Min.x == doctest::Approx(-0.5f * width));
	CHECK(center.Max.x == doctest::Approx(0.5f * width));
	CHECK(right.Min.x == doctest::Approx(-width));
	CHECK(right.Max.x == doctest::Approx(0.0f));

	// Each line is aligned on its own: the short second line ends at the origin when right-aligned.
	float const barAdvance = atlas->GetGlyph(U'I').Advance;
	REQUIRE(right.Glyphs.size() == 4);
	CHECK(right.Glyphs[3].Min.x == doctest::Approx(-barAdvance + atlas->GetGlyph(U'I').PlaneMin.x));
	CHECK(center.Glyphs[3].Min.x == doctest::Approx(-0.5f * barAdvance + atlas->GetGlyph(U'I').PlaneMin.x));
}

TEST_CASE("TextLayout: lines, spacing, tabs and control characters")
{
	Scope<FontAtlas> const atlas = Testing::CreateDefaultFontAtlas();
	float const space = atlas->GetGlyph(U' ').Advance;

	TextLayout const spaced = LayoutText(*atlas, "A\nA", {TextAlignment::Left, 1.5f});
	REQUIRE(spaced.Glyphs.size() == 2);
	CHECK(spaced.LineCount == 2);
	CHECK(spaced.Glyphs[1].Min.y - spaced.Glyphs[0].Min.y == doctest::Approx(1.5f));
	CHECK(spaced.Glyphs[1].Min.x == doctest::Approx(spaced.Glyphs[0].Min.x));
	CHECK(spaced.Max.y == doctest::Approx(2.5f));

	TextLayout const tabbed = LayoutText(*atlas, "\tA");
	REQUIRE(tabbed.Glyphs.size() == 1);
	CHECK(tabbed.Glyphs[0].Min.x == doctest::Approx(4.0f * space + atlas->GetGlyph(U'A').PlaneMin.x));

	// Control characters are skipped without breaking kerning.
	TextLayout const plain = LayoutText(*atlas, "AV");
	TextLayout const withReturn = LayoutText(*atlas, "A\rV");
	REQUIRE(withReturn.Glyphs.size() == 2);
	CHECK(withReturn.Glyphs[1].Min.x == doctest::Approx(plain.Glyphs[1].Min.x));

	TextLayout const empty = LayoutText(*atlas, "");
	CHECK(empty.Glyphs.empty());
	CHECK(empty.LineCount == 0);
	CHECK(empty.Max == glm::vec2(0.0f));

	TextLayout const spaces = LayoutText(*atlas, "   ");
	CHECK(spaces.Glyphs.empty());
	CHECK(spaces.Max.x == doctest::Approx(3.0f * space));

	// Invalid UTF-8 draws the replacement glyph.
	CHECK(LayoutText(*atlas, "\xFF").Glyphs.size() == 1);
}
