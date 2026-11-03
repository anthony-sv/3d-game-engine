#include "Fuzzing.h"

#include "Strada/Serialization/JsonScanner.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	std::string_view Slice(std::string_view text, std::pair<uint32_t, uint32_t> range)
	{
		REQUIRE(static_cast<size_t>(range.first) + range.second <= text.size());
		return text.substr(range.first, range.second);
	}

	bool IsJsonWhitespace(char character)
	{
		return character == ' ' || character == '\t' || character == '\n' || character == '\r';
	}

	// Writes random JSON texts: strings with every escape and raw UTF-8, numbers of every form, containers to MaxDepth, and
	// whitespace of every kind between tokens.
	class TextGenerator
	{
	public:
		static constexpr int MaxDepth = 16;

		explicit TextGenerator(uint64_t seed)
			: m_Random(seed)
		{
		}

		// An object with up to 6 members (keys made unique by their index), whose values are often arrays.
		std::string MakeObjectText()
		{
			std::string text;
			Space(text);
			text += '{';
			size_t const members = m_Random.Pick(7);
			for (size_t member = 0; member < members; member++)
			{
				if (member > 0)
				{
					text += ',';
				}
				Space(text);
				String(text, std::to_string(member));
				Space(text);
				text += ':';
				Space(text);
				if (m_Random.Pick(3) != 0)
				{
					Array(text, 1);
				}
				else
				{
					Value(text, 1);
				}
				Space(text);
			}
			Space(text);
			text += '}';
			Space(text);
			return text;
		}

		size_t Pick(size_t count) { return m_Random.Pick(count); }

	private:
		void Space(std::string& text)
		{
			static constexpr std::array<char, 4> Characters = {' ', '\t', '\n', '\r'};
			for (size_t count = m_Random.Pick(4) == 0 ? m_Random.Pick(4) : 0; count > 0; count--)
			{
				text += Characters[m_Random.Pick(Characters.size())];
			}
		}

		// A string with a unique suffix, after random characters: plain, every escape, raw UTF-8 of two to four bytes.
		void String(std::string& text, std::string_view suffix)
		{
			static constexpr std::array<std::string_view, 15> Pieces = {
				"a",        "Nodes",           " ", "\\\"", "\\\\", "\\/", "\\b", "\\f", "\\n", "\\r", "\\t", "\\u00e9", "\\uD83D\\uDE00",
				"\xC3\xA9", "\xF0\x9F\x98\x80"};
			text += '"';
			for (size_t count = m_Random.Pick(6); count > 0; count--)
			{
				text += Pieces[m_Random.Pick(Pieces.size())];
			}
			text += suffix;
			text += '"';
		}

		void Number(std::string& text)
		{
			static constexpr std::array<std::string_view, 16> Forms = {"0",
			                                                           "-0",
			                                                           "7",
			                                                           "-12",
			                                                           "18446744073709551615",
			                                                           "-9223372036854775808",
			                                                           "1.5",
			                                                           "-0.25",
			                                                           "1e5",
			                                                           "1E-5",
			                                                           "-2.5e+10",
			                                                           "0.0",
			                                                           "3E0",
			                                                           "123456789012345678901234567890",
			                                                           "1e-300",
			                                                           "6.02214076e23"};
			text += Forms[m_Random.Pick(Forms.size())];
		}

		void Array(std::string& text, int depth)
		{
			text += '[';
			size_t const elements = m_Random.Pick(5);
			for (size_t element = 0; element < elements; element++)
			{
				if (element > 0)
				{
					text += ',';
				}
				Space(text);
				Value(text, depth + 1);
				Space(text);
			}
			Space(text);
			text += ']';
		}

		void Value(std::string& text, int depth)
		{
			size_t const kind = m_Random.Pick(depth < MaxDepth ? 8 : 6);
			switch (kind)
			{
				case 0:
					text += "true";
					break;
				case 1:
					text += "false";
					break;
				case 2:
					text += "null";
					break;
				case 3:
				case 4:
					Number(text);
					break;
				case 5:
					String(text, {});
					break;
				case 6:
					Array(text, depth);
					break;
				default:
				{
					text += '{';
					size_t const members = m_Random.Pick(4);
					for (size_t member = 0; member < members; member++)
					{
						if (member > 0)
						{
							text += ',';
						}
						Space(text);
						String(text, std::to_string(member));
						Space(text);
						text += ':';
						Space(text);
						Value(text, depth + 1);
						Space(text);
					}
					text += '}';
					break;
				}
			}
		}

		Testing::FuzzRandom m_Random;
	};

	// The scan of a valid object text names its members as nlohmann iterates them: the same keys (decoded), values and
	// elements, each range a whole value without the whitespace around it.
	void CheckScanMatchesParse(std::string_view text)
	{
		CAPTURE(text);
		Result<Json> parsed = ParseJson(text);
		REQUIRE_MESSAGE(parsed.IsOk(), parsed.GetError());
		Json const& document = parsed.GetValue();
		REQUIRE(document.is_object());
		std::optional<std::vector<JsonScanMember>> const scanned = ScanJsonTopLevel(text);
		REQUIRE(scanned.has_value());
		REQUIRE(scanned->size() == document.size());
		size_t index = 0;
		for (auto const& [key, value] : document.items())
		{
			JsonScanMember const& member = (*scanned)[index++];
			Result<Json> const decodedKey = ParseJson("\"" + std::string(member.Key) + "\"");
			REQUIRE(decodedKey.IsOk());
			CHECK(decodedKey.GetValue() == key);
			std::string_view const valueText = Slice(text, member.Value);
			REQUIRE_FALSE(valueText.empty());
			CHECK_FALSE(IsJsonWhitespace(valueText.front()));
			CHECK_FALSE(IsJsonWhitespace(valueText.back()));
			Result<Json> const readValue = ParseJson(valueText);
			REQUIRE(readValue.IsOk());
			CHECK(readValue.GetValue() == value);
			if (!value.is_array())
			{
				CHECK(member.Elements.empty());
				continue;
			}
			REQUIRE(member.Elements.size() == value.size());
			for (size_t element = 0; element < value.size(); element++)
			{
				std::string_view const elementText = Slice(text, member.Elements[element]);
				REQUIRE_FALSE(elementText.empty());
				CHECK_FALSE(IsJsonWhitespace(elementText.front()));
				CHECK_FALSE(IsJsonWhitespace(elementText.back()));
				// Inside the array's range.
				CHECK(member.Elements[element].first > member.Value.first);
				CHECK(member.Elements[element].first + member.Elements[element].second < member.Value.first + member.Value.second);
				Result<Json> const readElement = ParseJson(elementText);
				REQUIRE(readElement.IsOk());
				CHECK(readElement.GetValue() == value[element]);
			}
		}
	}

	std::string Nest(size_t depth, std::string_view inner)
	{
		return std::string(depth, '[') + std::string(inner) + std::string(depth, ']');
	}
}

TEST_CASE("Serialization: JsonScanner's members and element ranges match nlohmann's iteration over random values")
{
	TextGenerator generator(9100);
	for (int round = 0, rounds = Testing::GetFuzzRounds(400); round < rounds; round++)
	{
		CAPTURE(round);
		std::string const text = generator.MakeObjectText();
		CheckScanMatchesParse(text);
		// nlohmann's own forms of the same document, compact and indented.
		Json const document = ParseJson(text).TakeValue();
		CheckScanMatchesParse(document.dump(-1, ' ', false, Json::error_handler_t::replace));
		CheckScanMatchesParse(DumpJson(document));
	}

	// The empty object and arrays, and members of every kind.
	CheckScanMatchesParse("{}");
	CheckScanMatchesParse(" \t\r\n{ \t\r\n} \t\r\n");
	CheckScanMatchesParse(R"({"Nodes":[],"Diagnostics":[ ]})");
	CheckScanMatchesParse(R"({"a":1,"b":-2.5e+3,"c":"x","d":true,"e":false,"f":null,"g":{"h":[1,{"i":"]"}]},"j":[[],{},"[",-0]})");
	// Brackets and quotes inside strings delimit nothing.
	CheckScanMatchesParse(R"({"a":["]}\"[{,",{"b":"\\"}]})");
}

TEST_CASE("Serialization: JsonScanner refuses malformed texts at the first character the grammar does not allow")
{
	std::vector<std::string> const refused = {
		"",
		"   ",
		"[]",
		"1",
		"\"a\"",
		"{",
		"}",
		R"({"a"})",
		R"({"a":})",
		R"({"a" 1})",
		R"({"a":1,})",
		R"({"a":1 "b":2})",
		R"({a:1})",
		R"({'a':1})",
		R"({"a":[1,]})",
		R"({"a":[,1]})",
		R"({"a":[1 2]})",
		R"({"a":[1)",
		R"({"a":[1]})"
		" x",
		"{}{}",
		"{} x",
		R"({"a":"\x"})",
		R"({"a":"\u12g4"})",
		R"({"a":"\u12"})",
		"{\"a\":\"tab\tinside\"}",
		"{\"a\":\"\x01\"}",
		R"({"a":"unterminated})",
		"{\"a\":\"ends in a backslash\\",
		R"({"a":[{]}]})",
		R"({"a":{"b":[}]}})",
		R"({"a":[}]})",
		R"({"a":[True]})",
		R"({"a":[#]})",
		R"({"a":[/* comment */1]})",
		R"({"a":[1]} // comment)",
		// A byte order mark before the object, and before an element (which nlohmann would skip only at the start).
		"\xEF\xBB\xBF{}",
		"{\"a\":[\xEF\xBB\xBF{}]}",
		// Bytes no JSON token has, outside strings.
		"{\"a\":[{\"b\":1\x7F}]}",
		"{\"a\":[[1,\xC3\xA9]]}",
		std::string("{\"a\":[1\0]}", 11),
	};
	for (std::string const& text : refused)
	{
		CAPTURE(text);
		CHECK_FALSE(ScanJsonTopLevel(text).has_value());
	}

	// Every proper prefix of an object text is refused.
	std::string const whole = R"({"Nodes":[{"ID":1,"Pins":[{"Name":"A\"]"}]}],"Diagnostics":[]})";
	REQUIRE(ScanJsonTopLevel(whole).has_value());
	for (size_t length = 0; length < whole.size(); length++)
	{
		CAPTURE(length);
		CHECK_FALSE(ScanJsonTopLevel(std::string_view(whole).substr(0, length)).has_value());
	}

	// Nesting as deep as ParseJson allows is delimited; one level deeper is refused by both.
	std::string const deepest = "{\"a\":" + Nest(MaxJsonScanDepth - 1, "1") + "}";
	std::string const tooDeep = "{\"a\":" + Nest(MaxJsonScanDepth, "1") + "}";
	CHECK(ParseJson(deepest).IsOk());
	CHECK(ScanJsonTopLevel(deepest).has_value());
	CHECK(ParseJson(tooDeep).IsError());
	CHECK_FALSE(ScanJsonTopLevel(tooDeep).has_value());
	static_assert(MaxJsonScanDepth == MaxJsonDepth);
}

TEST_CASE("Serialization: a text JsonScanner delimits whose every part parses is a text ParseJson reads to the same members")
{
	// What the analysis reader relies on: damaging valid texts byte by byte, whenever the scan succeeds and every key, element
	// and other member value parses alone, the whole text parses, to the values the parts parse to.
	TextGenerator generator(9200);
	static constexpr std::array<char, 12> Bytes = {'{', '}', '[', ']', '"', '\\', ',', ':', ' ', '1', 'x', '\xEF'};
	int accepted = 0;
	for (int round = 0, rounds = Testing::GetFuzzRounds(600); round < rounds; round++)
	{
		CAPTURE(round);
		std::string text = generator.MakeObjectText();
		if (text.size() < 3)
		{
			continue;
		}
		size_t const at = generator.Pick(text.size());
		switch (generator.Pick(3))
		{
			case 0:
				text.erase(at, 1);
				break;
			case 1:
				text.insert(text.begin() + static_cast<std::ptrdiff_t>(at), Bytes[generator.Pick(Bytes.size())]);
				break;
			default:
				text[at] = Bytes[generator.Pick(Bytes.size())];
				break;
		}
		CAPTURE(text);
		std::optional<std::vector<JsonScanMember>> const scanned = ScanJsonTopLevel(text);
		if (!scanned)
		{
			continue;
		}
		std::vector<Json> parts;
		bool partsParse = true;
		for (JsonScanMember const& member : *scanned)
		{
			// Keys are delimited, not validated (their UTF-8 is not checked): a reader compares them with the keys it takes.
			partsParse = partsParse && ParseJson("\"" + std::string(member.Key) + "\"").IsOk();
			if (member.Elements.empty())
			{
				Result<Json> value = ParseJson(Slice(text, member.Value));
				partsParse = partsParse && value.IsOk();
				if (value)
				{
					parts.push_back(value.TakeValue());
				}
				continue;
			}
			for (std::pair<uint32_t, uint32_t> const& element : member.Elements)
			{
				Result<Json> value = ParseJson(Slice(text, element));
				partsParse = partsParse && value.IsOk();
				if (value)
				{
					parts.push_back(value.TakeValue());
				}
			}
		}
		if (!partsParse)
		{
			continue;
		}
		Result<Json> const whole = ParseJson(text);
		REQUIRE_MESSAGE(whole.IsOk(), whole.GetError());
		if (whole.GetValue().size() != scanned->size())
		{
			// A damaged key repeats another: nlohmann keeps one member of the two (the analysis reader takes its full path
			// for anything but two distinct keys).
			continue;
		}
		accepted++;
		// The parts, in order, are the whole's member values and array elements.
		std::vector<Json> expected;
		for (auto const& [key, value] : whole.GetValue().items())
		{
			if (value.is_array() && !value.empty())
			{
				expected.insert(expected.end(), value.begin(), value.end());
			}
			else
			{
				expected.push_back(value);
			}
		}
		CHECK(parts == expected);
	}
	// Many damaged texts stay valid (a space inserted, a digit changed), so the property is exercised.
	CHECK(accepted > 0);
}
