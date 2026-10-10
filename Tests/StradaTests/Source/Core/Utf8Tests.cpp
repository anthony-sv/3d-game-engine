#include "Strada/Core/Utf8.h"

#include <doctest/doctest.h>

#include <string>

using namespace Strada;

TEST_CASE("Utf8: code points of every sequence length decode")
{
	CHECK(Utf8::Decode("").empty());
	CHECK(Utf8::Decode("Az") == U"Az");
	CHECK(Utf8::Decode("\xC3\xA9") == U"é");
	CHECK(Utf8::Decode("\xE2\x82\xAC") == U"€");
	CHECK(Utf8::Decode("\xF0\x9F\x98\x80") == U"\U0001F600");
	CHECK(Utf8::Decode("a\xC3\xA9z") == U"aéz");
	CHECK(Utf8::Decode("\xF4\x8F\xBF\xBF") == U"\U0010FFFF");
}

TEST_CASE("Utf8: invalid input decodes to replacement characters and always advances")
{
	char32_t const replacement = Utf8::ReplacementCharacter;
	// Bytes that cannot start a sequence.
	CHECK(Utf8::Decode("\xFF") == std::u32string{replacement});
	CHECK(Utf8::Decode("\x80"
	                   "A") == std::u32string{replacement, U'A'});
	// Truncated sequences, at the end and before an ASCII character.
	CHECK(Utf8::Decode("\xC3") == std::u32string{replacement});
	CHECK(Utf8::Decode("\xE2\x82"
	                   "A") == std::u32string{replacement, replacement, U'A'});
	// Overlong encodings, surrogates and values above U+10FFFF.
	CHECK(Utf8::Decode("\xC0\xAF") == std::u32string{replacement, replacement});
	CHECK(Utf8::Decode("\xED\xA0\x80") == std::u32string{replacement, replacement, replacement});
	CHECK(Utf8::Decode("\xF4\x90\x80\x80") == std::u32string(4, replacement));

	std::string_view const truncated("\xF0\x9F");
	size_t offset = 0;
	CHECK(Utf8::DecodeNext(truncated, offset) == replacement);
	CHECK(offset == 1);
}

TEST_CASE("Utf8: validation and sanitizing replace only invalid bytes")
{
	CHECK(Utf8::IsValid(""));
	CHECK(Utf8::IsValid("Caf\xC3\xA9 \xF0\x9F\x98\x80"));
	// An encoded replacement character is valid text.
	CHECK(Utf8::IsValid("\xEF\xBF\xBD"));
	CHECK(Utf8::Sanitize("Caf\xC3\xA9") == "Caf\xC3\xA9");

	CHECK_FALSE(Utf8::IsValid("\xFF"));
	CHECK_FALSE(Utf8::IsValid("a\xC3"));
	CHECK_FALSE(Utf8::IsValid("\xED\xA0\x80"));
	CHECK(Utf8::Sanitize("a\xFF"
	                     "b") == "a\xEF\xBF\xBD"
	                             "b");
	CHECK(Utf8::Sanitize("\xC0\xAF") == "\xEF\xBF\xBD\xEF\xBF\xBD");
	CHECK(Utf8::IsValid(Utf8::Sanitize("\xF4\x90\x80\x80 \xE2\x82")));
}
