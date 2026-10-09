#include "stpch.h"
#include "Strada/Core/Utf8.h"

namespace Strada
{
	namespace Utf8
	{
		char32_t DecodeNext(std::string_view text, size_t& offset)
		{
			auto const byteAt = [&text](size_t index)
			{
				return static_cast<unsigned char>(text[index]);
			};
			unsigned char const lead = byteAt(offset);
			if (lead < 0x80)
			{
				offset++;
				return lead;
			}

			// The sequence length and the smallest code point it may encode (anything below is an overlong encoding).
			size_t length = 0;
			char32_t codepoint = 0;
			char32_t minimum = 0;
			if ((lead & 0xE0) == 0xC0)
			{
				length = 2;
				codepoint = lead & 0x1Fu;
				minimum = 0x80;
			}
			else if ((lead & 0xF0) == 0xE0)
			{
				length = 3;
				codepoint = lead & 0x0Fu;
				minimum = 0x800;
			}
			else if ((lead & 0xF8) == 0xF0)
			{
				length = 4;
				codepoint = lead & 0x07u;
				minimum = 0x10000;
			}
			else
			{
				offset++;
				return ReplacementCharacter;
			}

			if (offset + length > text.size())
			{
				offset++;
				return ReplacementCharacter;
			}
			for (size_t i = 1; i < length; i++)
			{
				unsigned char const continuation = byteAt(offset + i);
				if ((continuation & 0xC0) != 0x80)
				{
					offset++;
					return ReplacementCharacter;
				}
				codepoint = (codepoint << 6) | (continuation & 0x3Fu);
			}
			if (codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
			{
				offset++;
				return ReplacementCharacter;
			}
			offset += length;
			return codepoint;
		}

		std::u32string Decode(std::string_view text)
		{
			std::u32string codepoints;
			codepoints.reserve(text.size());
			size_t offset = 0;
			while (offset < text.size())
			{
				codepoints.push_back(DecodeNext(text, offset));
			}
			return codepoints;
		}
	}
}
