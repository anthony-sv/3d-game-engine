#include "Fuzzing.h"

#include "Strada/Asset/FontAsset.h"
#include "Strada/Asset/TrueTypeSanitizer.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Renderer/FontAtlas.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	// Permissively licensed fonts of the pinned Dear ImGui sources, with TrueType outlines.
	constexpr std::array<char const*, 4> FontFiles = {"Karla-Regular.ttf", "Roboto-Medium.ttf", "Cousine-Regular.ttf", "DroidSans.ttf"};

	using Font = std::vector<uint8_t>;

	Font ReadFont(char const* name)
	{
		Result<Buffer> data = FileSystem::ReadBinaryFile(FileSystem::PathFromUtf8(STRADA_TEST_FONTS_DIR) / name);
		REQUIRE_MESSAGE(data.IsOk(), name);
		std::span<uint8_t const> const bytes = data.GetValue().GetSpan();
		return Font(bytes.begin(), bytes.end());
	}

	uint16_t Get16(Font const& font, size_t offset)
	{
		return static_cast<uint16_t>((font[offset] << 8) | font[offset + 1]);
	}

	uint32_t Get32(Font const& font, size_t offset)
	{
		return (static_cast<uint32_t>(Get16(font, offset)) << 16) | Get16(font, offset + 2);
	}

	void Set16(Font& font, size_t offset, uint16_t value)
	{
		font[offset] = static_cast<uint8_t>(value >> 8);
		font[offset + 1] = static_cast<uint8_t>(value);
	}

	void Set32(Font& font, size_t offset, uint32_t value)
	{
		Set16(font, offset, static_cast<uint16_t>(value >> 16));
		Set16(font, offset + 2, static_cast<uint16_t>(value));
	}

	// A table's directory record (the test fonts are single fonts, not collections).
	struct TableRecord
	{
		size_t Record = 0;
		size_t Offset = 0;
		size_t Length = 0;
	};

	std::optional<TableRecord> FindTable(Font const& font, std::string_view tag)
	{
		uint16_t const tableCount = Get16(font, 4);
		for (size_t index = 0; index < tableCount; index++)
		{
			size_t const record = 12 + 16 * index;
			if (std::string_view(reinterpret_cast<char const*>(font.data() + record), 4) == tag)
			{
				return TableRecord{record, Get32(font, record + 8), Get32(font, record + 12)};
			}
		}
		return std::nullopt;
	}

	TableRecord RequireTable(Font const& font, std::string_view tag)
	{
		std::optional<TableRecord> const table = FindTable(font, tag);
		REQUIRE_MESSAGE(table.has_value(), tag);
		return *table;
	}

	// Where a glyph's data starts and ends in the file.
	std::pair<size_t, size_t> GetGlyphRange(Font const& font, uint32_t glyph)
	{
		TableRecord const loca = RequireTable(font, "loca");
		TableRecord const glyf = RequireTable(font, "glyf");
		bool const longLocations = Get16(font, RequireTable(font, "head").Offset + 50) == 1;
		auto const location = [&](uint32_t index) -> size_t
		{
			return longLocations ? Get32(font, loca.Offset + 4 * index) : 2 * size_t(Get16(font, loca.Offset + 2 * index));
		};
		return {glyf.Offset + location(glyph), glyf.Offset + location(glyph + 1)};
	}

	uint16_t GetGlyphCount(Font const& font)
	{
		return Get16(font, RequireTable(font, "maxp").Offset + 4);
	}

	// The character map subtables stb_truetype may use (Unicode encodings).
	std::vector<size_t> GetUnicodeSubtables(Font const& font)
	{
		TableRecord const cmap = RequireTable(font, "cmap");
		std::vector<size_t> subtables;
		for (size_t index = 0; index < Get16(font, cmap.Offset + 2); index++)
		{
			size_t const record = cmap.Offset + 4 + 8 * index;
			uint16_t const platform = Get16(font, record);
			uint16_t const encoding = Get16(font, record + 2);
			if ((platform == 3 && (encoding == 1 || encoding == 10)) || platform == 0)
			{
				subtables.push_back(cmap.Offset + Get32(font, record + 4));
			}
		}
		return subtables;
	}

	Result<TrueTypeRepairs> Sanitize(Font& font)
	{
		return SanitizeTrueTypeFont(std::span<uint8_t>(font.data(), font.size()));
	}

	Result<Scope<FontAtlas>> CreateAtlas(Font const& font)
	{
		Result<Ref<FontAsset>> asset = FontAsset::Create(Buffer::Copy(font.data(), font.size()));
		if (!asset)
		{
			return Error{asset.GetError()};
		}
		return FontAtlas::Create(asset.TakeValue());
	}

	// Characters the tests draw: printable ASCII and a few beyond.
	std::vector<char32_t> GetTestCharacters()
	{
		std::vector<char32_t> characters;
		for (char32_t character = U' '; character <= U'~'; character++)
		{
			characters.push_back(character);
		}
		for (char32_t const character : {U'é', U'Å', U'ß', U'€', U'中', U'�'})
		{
			characters.push_back(character);
		}
		return characters;
	}

	bool SameGlyph(FontGlyph const& a, FontGlyph const& b)
	{
		return a.Advance == b.Advance && a.PlaneMin == b.PlaneMin && a.PlaneMax == b.PlaneMax && a.HasQuad() == b.HasQuad();
	}
}

TEST_CASE("TrueTypeSanitizer: fonts without damage are left as they are")
{
	for (char const* const name : FontFiles)
	{
		CAPTURE(name);
		Font const original = ReadFont(name);
		Font font = original;
		Result<TrueTypeRepairs> repairs = Sanitize(font);
		REQUIRE_MESSAGE(repairs.IsOk(), (repairs ? std::string() : repairs.GetError()));
		CHECK(repairs.GetValue().IsEmpty());
		CHECK(font == original);
	}
}

TEST_CASE("TrueTypeSanitizer: fonts with damaged structures are refused")
{
	Font const original = ReadFont("Karla-Regular.ttf");
	auto const refused = [](Font font, std::string_view reason)
	{
		CAPTURE(reason);
		Result<TrueTypeRepairs> repairs = Sanitize(font);
		REQUIRE(repairs.IsError());
		CHECK_MESSAGE(repairs.GetError().find(reason) != std::string::npos, repairs.GetError());
		CHECK(FontAsset::Create(Buffer::Copy(font.data(), font.size())).IsError());
	};

	refused(Font(), "too short");
	refused(Font(256, 0x5A), "not a TrueType font");
	refused(Font(original.begin(), original.begin() + 100), "cut off");
	refused(Font(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(original.size() / 2)), "cut off");

	Font damaged = original;
	Set16(damaged, 4, 0xFFFF);
	refused(damaged, "table directory is cut off");

	damaged = original;
	Set32(damaged, RequireTable(damaged, "hmtx").Record + 8, static_cast<uint32_t>(damaged.size()));
	refused(damaged, "'hmtx' table is cut off");

	damaged = original;
	Set32(damaged, RequireTable(damaged, "hmtx").Record + 8, static_cast<uint32_t>(RequireTable(damaged, "loca").Offset));
	refused(damaged, "tables overlap");

	damaged = original;
	damaged[RequireTable(damaged, "glyf").Record] = 'x';
	refused(damaged, "CFF outlines");

	damaged = original;
	Set16(damaged, RequireTable(damaged, "maxp").Offset + 4, 0);
	refused(damaged, "no glyphs");

	damaged = original;
	Set16(damaged, RequireTable(damaged, "hhea").Offset + 34, 0);
	refused(damaged, "no horizontal metrics");

	damaged = original;
	Set16(damaged, RequireTable(damaged, "head").Offset + 50, 7);
	refused(damaged, "location format 7");

	// stb_truetype asserts on character map formats it does not know.
	damaged = original;
	for (size_t const subtable : GetUnicodeSubtables(damaged))
	{
		Set16(damaged, subtable, 2);
	}
	refused(damaged, "format 2 is not supported");
}

TEST_CASE("TrueTypeSanitizer: damaged glyphs become empty and the other characters stay")
{
	Font const original = ReadFont("Karla-Regular.ttf");
	Font damaged = original;
	// Contour counts no glyph's data can hold.
	uint32_t damagedGlyphs = 0;
	for (uint32_t glyph = 10; glyph < 40; glyph++)
	{
		auto const [begin, end] = GetGlyphRange(damaged, glyph);
		if (begin < end)
		{
			Set16(damaged, begin, 0x7FFF);
			damagedGlyphs++;
		}
	}
	REQUIRE(damagedGlyphs > 0);
	Font sanitized = damaged;
	Result<TrueTypeRepairs> repairs = Sanitize(sanitized);
	REQUIRE(repairs.IsOk());
	CHECK(repairs.GetValue().EmptiedGlyphs == damagedGlyphs);
	for (uint32_t glyph = 10; glyph < 40; glyph++)
	{
		auto const [begin, end] = GetGlyphRange(sanitized, glyph);
		CHECK(begin == end);
	}

	Result<Scope<FontAtlas>> expected = CreateAtlas(original);
	Result<Scope<FontAtlas>> atlas = CreateAtlas(damaged);
	REQUIRE(expected.IsOk());
	REQUIRE(atlas.IsOk());
	int same = 0;
	int emptied = 0;
	for (char32_t const character : GetTestCharacters())
	{
		FontGlyph const& glyph = atlas.GetValue()->GetGlyph(character);
		if (SameGlyph(glyph, expected.GetValue()->GetGlyph(character)))
		{
			same++;
		}
		else
		{
			CHECK_FALSE(glyph.HasQuad());
			emptied++;
		}
	}
	CHECK(same > 0);
	CHECK(emptied > 0);
}

TEST_CASE("TrueTypeSanitizer: composite glyphs in a cycle become empty")
{
	Font damaged = ReadFont("Roboto-Medium.ttf");
	// The first composite glyph made of itself.
	std::optional<uint32_t> composite;
	for (uint32_t glyph = 0; glyph < GetGlyphCount(damaged) && !composite; glyph++)
	{
		auto const [begin, end] = GetGlyphRange(damaged, glyph);
		if (begin < end && static_cast<int16_t>(Get16(damaged, begin)) < 0)
		{
			Set16(damaged, begin + 12, static_cast<uint16_t>(glyph));
			composite = glyph;
		}
	}
	REQUIRE(composite.has_value());
	Font sanitized = damaged;
	Result<TrueTypeRepairs> repairs = Sanitize(sanitized);
	REQUIRE(repairs.IsOk());
	CHECK(repairs.GetValue().EmptiedGlyphs == 1);
	auto const [begin, end] = GetGlyphRange(sanitized, *composite);
	CHECK(begin == end);

	// Drawing every character finishes (stb_truetype would recurse into the cycle until the stack ran out).
	Result<Scope<FontAtlas>> atlas = CreateAtlas(damaged);
	REQUIRE(atlas.IsOk());
	for (char32_t character = 0xA0; character < 0x250; character++)
	{
		static_cast<void>(atlas.GetValue()->GetGlyph(character));
	}
}

TEST_CASE("TrueTypeSanitizer: wrong character map search hints are recomputed")
{
	Font const original = ReadFont("Karla-Regular.ttf");
	Font damaged = original;
	int segmentMaps = 0;
	for (size_t const subtable : GetUnicodeSubtables(damaged))
	{
		if (Get16(damaged, subtable) == 4)
		{
			Set16(damaged, subtable + 8, 0xFFFE);
			Set16(damaged, subtable + 10, 0x00FF);
			Set16(damaged, subtable + 12, 0xFFFE);
			segmentMaps++;
		}
	}
	REQUIRE(segmentMaps > 0);
	Font sanitized = damaged;
	Result<TrueTypeRepairs> repairs = Sanitize(sanitized);
	REQUIRE(repairs.IsOk());
	CHECK(repairs.GetValue().RecomputedCharacterMap);
	CHECK(sanitized == original);
}

TEST_CASE("TrueTypeSanitizer: damaged kerning is ignored")
{
	Font const original = ReadFont("Roboto-Medium.ttf");
	Font damaged = original;
	// The lookup list beyond the table.
	TableRecord const gpos = RequireTable(damaged, "GPOS");
	Set16(damaged, gpos.Offset + 8, 0xFFFF);
	Font sanitized = damaged;
	Result<TrueTypeRepairs> repairs = Sanitize(sanitized);
	REQUIRE(repairs.IsOk());
	CHECK(repairs.GetValue().DroppedTables == 1);
	CHECK_FALSE(FindTable(sanitized, "GPOS").has_value());

	Result<Scope<FontAtlas>> expected = CreateAtlas(original);
	Result<Scope<FontAtlas>> atlas = CreateAtlas(damaged);
	REQUIRE(expected.IsOk());
	REQUIRE(atlas.IsOk());
	CHECK(expected.GetValue()->GetKerning(U'A', U'V') < 0.0f);
	CHECK(atlas.GetValue()->GetKerning(U'A', U'V') == 0.0f);
}

TEST_CASE("FontAsset: damaged fonts are refused, or load and draw their characters")
{
	std::vector<Font> fonts;
	for (char const* const name : FontFiles)
	{
		fonts.push_back(ReadFont(name));
	}
	std::vector<char32_t> const characters = GetTestCharacters();
	Testing::FuzzRandom random(6000);
	int loaded = 0;
	int refused = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(200); round < rounds; round++)
	{
		CAPTURE(round);
		Font font = fonts[random.Pick(fonts.size())];
		// The damage stb_truetype is most exposed to: truncation, and bytes of the directory, the table records and
		// the tables.
		for (size_t count = 1 + random.Pick(3); count > 0; count--)
		{
			uint16_t const tableCount = Get16(font, 4);
			switch (random.Pick(4))
			{
				case 0:
					font.resize(12 + random.Pick(font.size() - 12));
					break;
				case 1:
					font[random.Pick(font.size())] = static_cast<uint8_t>(random.Next());
					break;
				case 2:
				{
					size_t const at = random.Pick(font.size() - 1);
					std::array<uint16_t, 5> const values = {0, 1, 0x7FFF, 0x8000, 0xFFFF};
					Set16(font, at, values[random.Pick(values.size())]);
					break;
				}
				default:
				{
					if (tableCount == 0 || 12 + 16 * size_t(tableCount) > font.size())
					{
						break;
					}
					size_t const record = 12 + 16 * random.Pick(tableCount);
					std::array<uint32_t, 4> const values = {0, 0xFFFFFFFF, static_cast<uint32_t>(font.size()),
					                                        static_cast<uint32_t>(random.Next())};
					Set32(font, record + 8 + 4 * random.Pick(2), values[random.Pick(values.size())]);
					break;
				}
			}
			if (font.size() < 16)
			{
				break;
			}
		}
		Result<Scope<FontAtlas>> atlas = CreateAtlas(font);
		if (!atlas)
		{
			refused++;
			continue;
		}
		loaded++;
		for (char32_t const character : characters)
		{
			static_cast<void>(atlas.GetValue()->GetGlyph(character));
			static_cast<void>(atlas.GetValue()->GetKerning(U'A', character));
		}
	}
	CHECK(loaded > 0);
	CHECK(refused > 0);
}
