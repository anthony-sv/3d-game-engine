#pragma once

#include "Strada/Core/Result.h"

#include <cstdint>
#include <span>

namespace Strada
{
	// What SanitizeTrueTypeFont changed in a font.
	struct TrueTypeRepairs
	{
		// Glyphs whose outlines were damaged; they are empty now.
		uint32_t EmptiedGlyphs = 0;
		// Damaged optional tables (kern, GPOS) that are ignored now.
		uint32_t DroppedTables = 0;
		// Whether the binary-search hints of the character map were recomputed.
		bool RecomputedCharacterMap = false;

		bool IsEmpty() const { return EmptiedGlyphs == 0 && DroppedTables == 0 && !RecomputedCharacterMap; }
	};

	// stb_truetype trusts every offset and count in a font, so a damaged font (a truncated download, a corrupted file)
	// makes it read outside the font's data. This checks, in place, everything the engine's use of stb_truetype reads
	// (FontAsset, FontAtlas): the first font of the file, its required tables, its Unicode character map, metrics,
	// outlines and kerning. Damage in required structures fails. Damaged glyphs become empty (zero-length locations),
	// damaged kerning tables are dropped (their directory tag changes, so lookups miss them) and the character map's
	// binary-search hints are recomputed. Fonts with CFF outlines fail: stb_truetype bounds those by 512 MB rather than
	// by their table. Glyph indices must stay below the font's glyph count (stbtt_fontinfo::numGlyphs), which
	// stb_truetype does not check when reading metrics.
	[[nodiscard]] Result<TrueTypeRepairs> SanitizeTrueTypeFont(std::span<uint8_t> font);
}
