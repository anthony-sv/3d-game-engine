#include "stpch.h"
#include "Strada/Asset/FontAsset.h"

#include "Strada/Asset/TrueTypeSanitizer.h"

#include <stb_truetype.h>

#include <limits>

namespace Strada
{
	Result<Ref<FontAsset>> FontAsset::Create(Buffer data)
	{
		// stb_truetype addresses fonts with int offsets.
		if (data.GetSize() > static_cast<uint64_t>(std::numeric_limits<int>::max()))
		{
			return Error{"the font is too large"};
		}
		Result<TrueTypeRepairs> repairs = SanitizeTrueTypeFont(data.GetSpan());
		if (!repairs)
		{
			return Error{repairs.GetError()};
		}
		if (TrueTypeRepairs const& repaired = repairs.GetValue(); repaired.EmptiedGlyphs > 0 || repaired.DroppedTables > 0)
		{
			ST_CORE_WARN("A damaged font was repaired: {} glyphs are left empty, {} kerning tables are ignored", repaired.EmptiedGlyphs,
			             repaired.DroppedTables);
		}

		unsigned char const* bytes = data.GetData();
		int const offset = stbtt_GetFontOffsetForIndex(bytes, 0);
		stbtt_fontinfo info;
		if (offset < 0 || stbtt_InitFont(&info, bytes, offset) == 0)
		{
			return Error{"the data is not a TrueType font"};
		}
		return CreateRef<FontAsset>(PrivateTag{}, std::move(data));
	}

	FontAsset::FontAsset(PrivateTag, Buffer data)
		: m_Data(std::move(data))
	{
	}
}
