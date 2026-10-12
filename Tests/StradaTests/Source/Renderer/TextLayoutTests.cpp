#include "Renderer/FontTestUtilities.h"

#include "Strada/Renderer/TextLayout.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
#include <string_view>

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

TEST_CASE("TextLayout: lines wrap at spaces, and long words between characters")
{
	Scope<FontAtlas> const atlas = Testing::CreateDefaultFontAtlas();
	auto const width = [&atlas](std::string_view text)
	{
		return LayoutText(*atlas, text).Max.x;
	};
	float const space = atlas->GetGlyph(U' ').Advance;
	float const hello = width("hello");

	// "hello world" does not fit in one and a half words: "world" moves to the second line, without the space.
	TextLayoutSettings settings;
	settings.WrapWidth = 1.5f * hello;
	TextLayout const wrapped = LayoutText(*atlas, "hello world", settings);
	CHECK(wrapped.LineCount == 2);
	REQUIRE(wrapped.Glyphs.size() == 10);
	CHECK(wrapped.Glyphs[6].Min.y - wrapped.Glyphs[4].Min.y == doctest::Approx(1.0f));
	CHECK(wrapped.Glyphs[5].Min.x == doctest::Approx(atlas->GetGlyph(U'w').PlaneMin.x));
	CHECK(wrapped.Max.x == doctest::Approx(std::max(hello, width("world"))));
	CHECK(wrapped.Max.y == doctest::Approx(2.0f));

	// Wide enough: one line, as without wrapping.
	settings.WrapWidth = width("hello world") + 0.01f;
	TextLayout const fits = LayoutText(*atlas, "hello world", settings);
	CHECK(fits.LineCount == 1);
	CHECK(fits.Max.x == doctest::Approx(width("hello world")));

	// Several words per line: "aa aa" fits, the third word wraps; spaces at the end of a line do not count.
	settings.WrapWidth = width("aa aa") + 0.5f * space;
	TextLayout const words = LayoutText(*atlas, "aa aa aa", settings);
	CHECK(words.LineCount == 2);
	CHECK(words.Max.x == doctest::Approx(width("aa aa")));
	// Nor do runs of spaces before a wrapped word: the line ends at its last word.
	TextLayout const spaced = LayoutText(*atlas, "aa   aa", settings);
	CHECK(spaced.LineCount == 2);
	CHECK(spaced.Max.x == doctest::Approx(width("aa")));
	REQUIRE(spaced.Glyphs.size() == 4);
	CHECK(spaced.Glyphs[2].Min.x == doctest::Approx(spaced.Glyphs[0].Min.x));

	// Lines are aligned after wrapping: centered lines are centered on their own width.
	settings.WrapWidth = 1.5f * hello;
	settings.Alignment = TextAlignment::Center;
	TextLayout const centered = LayoutText(*atlas, "hello world", settings);
	REQUIRE(centered.Glyphs.size() == 10);
	CHECK(centered.Glyphs[0].Min.x == doctest::Approx(-0.5f * hello + atlas->GetGlyph(U'h').PlaneMin.x));
	CHECK(centered.Glyphs[5].Min.x == doctest::Approx(-0.5f * width("world") + atlas->GetGlyph(U'w').PlaneMin.x));

	// A word longer than a line breaks before the character that does not fit, keeping at least one per line.
	settings = TextLayoutSettings();
	settings.WrapWidth = 2.5f * atlas->GetGlyph(U'W').Advance;
	TextLayout const broken = LayoutText(*atlas, "WWWWW", settings);
	CHECK(broken.LineCount == 3);
	CHECK(broken.Max.x <= settings.WrapWidth);
	settings.WrapWidth = 0.01f;
	CHECK(LayoutText(*atlas, "WWW", settings).LineCount == 3);

	// Newlines still break, and widths that are not positive and finite never wrap.
	settings.WrapWidth = 100.0f;
	CHECK(LayoutText(*atlas, "a\nb", settings).LineCount == 2);
	settings.WrapWidth = -1.0f;
	CHECK(LayoutText(*atlas, "hello world", settings).LineCount == 1);
	settings.WrapWidth = std::numeric_limits<float>::quiet_NaN();
	CHECK(LayoutText(*atlas, "hello world", settings).LineCount == 1);
}

TEST_CASE("TextLayout: spaces before a line's first word indent it and are no place to break it")
{
	Scope<FontAtlas> const atlas = Testing::CreateDefaultFontAtlas();
	auto const width = [&atlas](std::string_view text)
	{
		return LayoutText(*atlas, text).Max.x;
	};
	// Where a glyph lies alone at the start of a line: the first glyph of the text, moved down by lines.
	auto const checkGlyph = [&atlas](TextGlyphQuad const& glyph, std::string_view reference, float lines)
	{
		TextGlyphQuad const expected = LayoutText(*atlas, reference).Glyphs.front();
		CHECK(glyph.Min.x == doctest::Approx(expected.Min.x));
		CHECK(glyph.Min.y == doctest::Approx(expected.Min.y + lines));
	};

	// A first word that does not fit after the indentation breaks between characters on the indented line: no empty line
	// comes first, and the word keeps its indentation.
	TextLayoutSettings settings;
	settings.WrapWidth = width("   Extra");
	TextLayout const indented = LayoutText(*atlas, "   Extraordinarily", settings);
	CHECK(indented.LineCount >= 2);
	REQUIRE(indented.Glyphs.size() == 15);
	checkGlyph(indented.Glyphs[0], "   E", 0.0f);
	checkGlyph(indented.Glyphs[5], "o", 1.0f);
	CHECK(indented.Max.x <= settings.WrapWidth + 1e-4f);

	// An indented paragraph after a newline keeps its indentation the same way.
	settings.WrapWidth = width("    Extra");
	TextLayout const paragraph = LayoutText(*atlas, "Title\n    Extraordinarily", settings);
	CHECK(paragraph.LineCount >= 3);
	REQUIRE(paragraph.Glyphs.size() == 20);
	checkGlyph(paragraph.Glyphs[0], "T", 0.0f);
	checkGlyph(paragraph.Glyphs[5], "    E", 1.0f);
	checkGlyph(paragraph.Glyphs[10], "o", 2.0f);

	// Indented words that fit wrap at the spaces after them as usual.
	settings.WrapWidth = width("   hello") + 0.5f * atlas->GetGlyph(U' ').Advance;
	TextLayout const words = LayoutText(*atlas, "   hello world", settings);
	CHECK(words.LineCount == 2);
	REQUIRE(words.Glyphs.size() == 10);
	checkGlyph(words.Glyphs[0], "   h", 0.0f);
	checkGlyph(words.Glyphs[5], "w", 1.0f);

	// Spaces wider than the line stay on it with the character after them: a line keeps at least one character.
	settings.WrapWidth = 2.0f * atlas->GetGlyph(U' ').Advance;
	TextLayout const wide = LayoutText(*atlas, "      x", settings);
	CHECK(wide.LineCount == 1);
	REQUIRE(wide.Glyphs.size() == 1);
	checkGlyph(wide.Glyphs[0], "      x", 0.0f);
}
