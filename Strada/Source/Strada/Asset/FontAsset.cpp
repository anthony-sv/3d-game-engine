#include "stpch.h"
#include "Strada/Asset/FontAsset.h"

#include <stb_truetype.h>

#include <limits>

namespace Strada
{
	Result<Ref<FontAsset>> FontAsset::Create(Buffer data)
	{
		// stb_truetype reads table offsets without bounds checks, so reject data too small to hold the headers it needs.
		constexpr uint64_t MinimumFontSize = 64;
		if (data.GetSize() < MinimumFontSize || data.GetSize() > static_cast<uint64_t>(std::numeric_limits<int>::max()))
		{
			return Error{"not a TrueType/OpenType font"};
		}

		unsigned char const* bytes = data.GetData();
		int const offset = stbtt_GetFontOffsetForIndex(bytes, 0);
		stbtt_fontinfo info;
		if (offset < 0 || stbtt_InitFont(&info, bytes, offset) == 0)
		{
			return Error{"not a TrueType/OpenType font"};
		}
		return CreateRef<FontAsset>(PrivateTag{}, std::move(data));
	}

	FontAsset::FontAsset(PrivateTag, Buffer data)
		: m_Data(std::move(data))
	{
	}
}
