#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace Strada
{
	namespace Utf8
	{
		// Stands in for invalid input.
		inline constexpr char32_t ReplacementCharacter = 0xFFFD;

		// Decodes the code point starting at `offset` and moves `offset` past it. Invalid, truncated and overlong sequences,
		// surrogates and values above U+10FFFF decode to ReplacementCharacter and consume one byte, so decoding always
		// advances. `offset` must be below text.size().
		char32_t DecodeNext(std::string_view text, size_t& offset);

		// Every code point of the text.
		std::u32string Decode(std::string_view text);
	}
}
