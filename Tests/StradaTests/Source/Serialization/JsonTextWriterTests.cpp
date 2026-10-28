#include "Fuzzing.h"

#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/JsonTextWriter.h"

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Strada;

namespace
{
	constexpr JsonTextWriter::Style Pretty{true, '\t', 1};
	constexpr JsonTextWriter::Style Compact{false, ' ', 0};
	// Four spaces per level, and 40 (past the 512 characters nlohmann's indent string starts with, from depth 13).
	constexpr JsonTextWriter::Style Spaces{true, ' ', 4};
	constexpr JsonTextWriter::Style Wide{true, '\t', 40};
	constexpr std::array<JsonTextWriter::Style, 4> Styles = {Pretty, Compact, Spaces, Wide};

	// What nlohmann writes for a value in a style, with ensure_ascii off and invalid UTF-8 replaced.
	std::string Dump(Json const& value, JsonTextWriter::Style style)
	{
		return style.Pretty ? value.dump(static_cast<int>(style.IndentStep), style.IndentChar, false, Json::error_handler_t::replace)
		                    : value.dump(-1, ' ', false, Json::error_handler_t::replace);
	}

	std::string Write(Json const& value, JsonTextWriter::Style style)
	{
		JsonTextWriter writer(style);
		writer.Value(value);
		return writer.TakeText();
	}

	std::string DescribeStyle(JsonTextWriter::Style style)
	{
		return style.Pretty ? "pretty, " + std::to_string(style.IndentStep) + " of " + std::to_string(static_cast<int>(style.IndentChar))
		                    : "compact";
	}

	// The value written by Value, and nested two levels deep (inside an object member and an array element the writer
	// opened itself), equals nlohmann's dump in every style.
	void CheckValue(Json const& value)
	{
		for (JsonTextWriter::Style const style : Styles)
		{
			CAPTURE(DescribeStyle(style));
			CHECK(Write(value, style) == Dump(value, style));

			JsonTextWriter writer(style);
			writer.BeginObject();
			writer.Key("Outer");
			writer.BeginArray();
			writer.Integer(1);
			writer.Value(value);
			writer.EndArray();
			writer.EndObject();
			CHECK(writer.TakeText() == Dump(Json::object({{"Outer", Json::array({1, value})}}), style));
		}
	}

	// Doubles written one by one with Double, as the elements of an array, against nlohmann's dump of the same array; on a
	// difference (reported without the texts, which can be megabytes), the doubles that differ alone are named by their bits.
	void CheckDoubles(std::span<double const> values)
	{
		JsonTextWriter writer(Compact);
		writer.BeginArray();
		Json expected = Json::array();
		for (double const value : values)
		{
			writer.Double(value);
			expected.push_back(value);
		}
		writer.EndArray();
		bool const same = writer.TakeText() == expected.dump();
		CHECK_MESSAGE(same, "the array of " << values.size() << " doubles is not written as nlohmann's dump writes it");
		if (same)
		{
			return;
		}
		for (double const value : values)
		{
			JsonTextWriter single(Compact);
			single.Double(value);
			CAPTURE(std::bit_cast<uint64_t>(value));
			CHECK(single.TakeText() == Json(value).dump());
		}
	}

	// Short byte strings that are often valid UTF-8 and often not: escaped characters, sequences of every length, and the
	// bytes that make them invalid (stray continuations, overlong leads, surrogates, leads above U+10FFFF, 0xF8 and above).
	std::string MakeBytes(Testing::FuzzRandom& random)
	{
		constexpr std::array<uint8_t, 34> Interesting = {0x00, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x1F, 0x20, 0x22, 0x2F, 0x41,
		                                                 0x5C, 0x7E, 0x7F, 0x80, 0x8F, 0x9F, 0xA0, 0xBF, 0xC0, 0xC1, 0xC2, 0xDF,
		                                                 0xE0, 0xED, 0xEF, 0xF0, 0xF4, 0xF5, 0xF8, 0xFC, 0xFE, 0xFF};
		constexpr std::array<std::string_view, 5> Sequences = {"\xC3\xA9", "\xE2\x82\xAC", "\xEF\xBF\xBD", "\xF0\x9F\x98\x80",
		                                                       "\xF4\x8F\xBF\xBF"};
		std::string bytes;
		size_t const length = random.Pick(13);
		while (bytes.size() < length)
		{
			switch (random.Pick(3))
			{
				case 0:
					bytes.push_back(static_cast<char>(Interesting[random.Pick(Interesting.size())]));
					break;
				case 1:
					bytes.push_back(static_cast<char>(random.Next() & 0xFF));
					break;
				default:
					bytes.append(Sequences[random.Pick(Sequences.size())]);
					break;
			}
		}
		return bytes;
	}
}

TEST_CASE("Serialization: JsonTextWriter writes doubles as nlohmann's dump writes them")
{
	double const smallestSubnormal = std::numeric_limits<double>::denorm_min();
	std::vector<double> special = {0.0,
	                               -0.0,
	                               smallestSubnormal,
	                               -smallestSubnormal,
	                               2.0 * smallestSubnormal,
	                               static_cast<double>(std::numeric_limits<float>::denorm_min()),
	                               std::numeric_limits<double>::min(),
	                               static_cast<double>(FLT_MAX),
	                               -static_cast<double>(FLT_MAX),
	                               static_cast<double>(std::nextafter(FLT_MAX, 0.0f)),
	                               std::numeric_limits<double>::max(),
	                               std::numeric_limits<double>::lowest(),
	                               1.0e-5,
	                               1.0e-4,
	                               9.99e-5,
	                               1.0e15,
	                               -1.0e15,
	                               std::nextafter(1.0e15, 0.0),
	                               999999999999999.0,
	                               1.0e16,
	                               1.0e15 + 2.0,
	                               9007199254740992.0,
	                               0.1,
	                               0.5,
	                               1.5,
	                               -2.25,
	                               3.4028235e38,
	                               static_cast<double>(0.1f),
	                               static_cast<double>(16.3f),
	                               std::numeric_limits<double>::quiet_NaN(),
	                               -std::numeric_limits<double>::quiet_NaN(),
	                               std::numeric_limits<double>::infinity(),
	                               -std::numeric_limits<double>::infinity()};
	CheckDoubles(special);
	for (double const value : special)
	{
		CAPTURE(std::bit_cast<uint64_t>(value));
		CheckValue(Json(value));
	}
	// Not finite: null, as nlohmann writes them.
	JsonTextWriter writer(Compact);
	writer.Double(std::numeric_limits<double>::quiet_NaN());
	CHECK(writer.TakeText() == "null");

	// Every power of two, both signs.
	std::vector<double> powers;
	for (int exponent = -1074; exponent <= 1023; exponent++)
	{
		powers.push_back(std::ldexp(1.0, exponent));
		powers.push_back(-std::ldexp(1.0, exponent));
	}
	CheckDoubles(powers);

	// Integral doubles drawn uniformly over [-1e15, 1e15], and doubles of random bits (NaNs and infinities among them).
	Testing::FuzzRandom random(10400);
	constexpr size_t Chunk = 4096;
	std::vector<double> values;
	values.reserve(Chunk);
	constexpr uint64_t IntegralRange = 2000000000000001;
	for (size_t drawn = 0; drawn < 1000000; drawn++)
	{
		values.push_back(static_cast<double>(static_cast<int64_t>(random.Next() % IntegralRange) - 1000000000000000));
		if (values.size() == Chunk)
		{
			CheckDoubles(values);
			values.clear();
		}
	}
	CheckDoubles(values);
	values.clear();
	for (size_t drawn = 0; drawn < 100000; drawn++)
	{
		values.push_back(std::bit_cast<double>(random.Next()));
		if (values.size() == Chunk)
		{
			CheckDoubles(values);
			values.clear();
		}
	}
	CheckDoubles(values);
}

TEST_CASE("Serialization: JsonTextWriter writes integers, booleans and null as nlohmann's dump writes them")
{
	for (Json const& value : {Json(0), Json(-1), Json(std::numeric_limits<int64_t>::min()), Json(std::numeric_limits<int64_t>::max()),
	                          Json(uint64_t{0}), Json(std::numeric_limits<uint64_t>::max()), Json(true), Json(false), Json()})
	{
		CAPTURE(value.dump());
		CheckValue(value);
	}
	JsonTextWriter writer(Compact);
	writer.BeginArray();
	writer.Integer(std::numeric_limits<int64_t>::min());
	writer.Unsigned(std::numeric_limits<uint64_t>::max());
	writer.Bool(true);
	writer.Null();
	writer.EndArray();
	CHECK(writer.TakeText() == "[-9223372036854775808,18446744073709551615,true,null]");
	// The writer is empty once its text is taken.
	CHECK(writer.TakeText().empty());
}

TEST_CASE("Serialization: JsonTextWriter escapes strings and keys, and replaces invalid UTF-8, as nlohmann's dump does it")
{
	std::vector<std::string> strings = {"",     "plain",        "\"quoted\"",       "back\\slash",      "slash/",
	                                    "\x7F", "\xEF\xBF\xBD", "\xF0\x9F\x98\x80", "\xF4\x8F\xBF\xBF", "caf\xC3\xA9"};
	// Every C0 control, alone and between other characters.
	for (int code = 0; code < 0x20; code++)
	{
		strings.push_back(std::string(1, static_cast<char>(code)));
		strings.push_back("a" + std::string(1, static_cast<char>(code)) + "b");
	}
	// Invalid UTF-8: truncated sequences, overlong forms, surrogates, code points above U+10FFFF, stray continuations, bytes
	// that never start a sequence, and invalid bytes followed by valid ones (nlohmann reads such a byte again).
	for (std::string_view const invalid : {"\xC3",
	                                       "\xE2\x82",
	                                       "\xF0\x9F\x98",
	                                       "a\xC3",
	                                       "\xC0\x80",
	                                       "\xC1\xBF",
	                                       "\xE0\x80\x80",
	                                       "\xF0\x80\x80\x80",
	                                       "\xED\xA0\x80",
	                                       "\xED\xBF\xBF",
	                                       "\xF4\x90\x80\x80",
	                                       "\xF5\x80\x80\x80",
	                                       "\x80",
	                                       "\xBF",
	                                       "a\x80"
	                                       "b",
	                                       "\xF8\x88\x80\x80\x80",
	                                       "\xFC",
	                                       "\xFE",
	                                       "\xFF",
	                                       "\xC3\x41",
	                                       "\xE2\x82\x41",
	                                       "\xE2\x28\xA1",
	                                       "\xF0\x9F\x98\x22",
	                                       "\xC3\xA9\xA9"})
	{
		strings.emplace_back(invalid);
	}
	Testing::FuzzRandom random(10401);
	for (int drawn = 0; drawn < 10000; drawn++)
	{
		strings.push_back(MakeBytes(random));
	}

	for (std::string const& text : strings)
	{
		CAPTURE(Json(text).dump(-1, ' ', true, Json::error_handler_t::replace));
		for (JsonTextWriter::Style const style : {Pretty, Compact})
		{
			JsonTextWriter value(style);
			value.String(text);
			CHECK(value.TakeText() == Dump(Json(text), style));

			JsonTextWriter key(style);
			key.BeginObject();
			key.Key(text);
			key.String(text);
			key.EndObject();
			Json object = Json::object();
			object[text] = text;
			CHECK(key.TakeText() == Dump(object, style));
		}
	}
}

TEST_CASE("Serialization: JsonTextWriter lays containers out as nlohmann's dump does, empty and nested to depth 16")
{
	CheckValue(Json::object());
	CheckValue(Json::array());
	CheckValue(Json::array({Json::object(), Json::array(), Json::object({{"A", Json::array()}})}));
	CheckValue(Json::object({{"Name", "Graph"}, {"Count", 3}, {"Scale", 0.25}, {"On", true}, {"Nothing", nullptr}}));

	// Objects and arrays in turn, 16 levels deep, each level with a scalar beside the deeper value.
	Json nested = Json::array({1.5, "leaf"});
	for (int level = 1; level < 16; level++)
	{
		nested = level % 2 == 0 ? Json::array({level, std::move(nested), Json::array()})
		                        : Json::object({{"Level", level}, {"Inner", std::move(nested)}, {"Empty", Json::object()}});
	}
	CheckValue(nested);

	// Members keep their stored order.
	Json ordered = Json::object();
	ordered["Z"] = 1;
	ordered["A"] = 2;
	ordered["M"] = Json::array({3});
	CheckValue(ordered);

	// Values nlohmann writes itself.
	CheckValue(Json::binary({1, 2, 3}));
	CheckValue(Json::array({Json::binary({4}, 7), "after"}));
	CheckValue(Json(Json::value_t::discarded));
}

TEST_CASE("Serialization: JsonTextWriter writes a document member by member as nlohmann dumps it whole")
{
	Json const settings = Json::object({{"Function", 3}, {"Cases", Json::array({"A", "B"})}, {"Offset", Json::array({0.5, -0.0, 1e20})}});
	Json expected = Json::object();
	expected["Strada"] = Json::object({{"Version", 3}, {"Type", "VisualScript"}});
	expected["Nodes"] = Json::array();
	for (uint32_t id = 1; id <= 3; id++)
	{
		Json node = Json::object();
		node["ID"] = id;
		node["Position"] = Json::array({static_cast<double>(16.25f * static_cast<float>(id)), -32.0});
		node["Settings"] = settings;
		node["Pin"] = Json::array({id, "Result"});
		expected["Nodes"].push_back(std::move(node));
	}
	expected["Empty"] = Json::array();

	for (JsonTextWriter::Style const style : Styles)
	{
		CAPTURE(DescribeStyle(style));
		JsonTextWriter writer(style, 64);
		writer.BeginObject();
		writer.Key("Strada");
		writer.BeginObject();
		writer.Key("Version");
		writer.Integer(3);
		writer.Key("Type");
		writer.String("VisualScript");
		writer.EndObject();
		writer.Key("Nodes");
		writer.BeginArray();
		for (uint32_t id = 1; id <= 3; id++)
		{
			writer.BeginObject();
			writer.Key("ID");
			writer.Unsigned(id);
			writer.Key("Position");
			writer.BeginArray();
			writer.Double(static_cast<double>(16.25f * static_cast<float>(id)));
			writer.Double(-32.0);
			writer.EndArray();
			writer.Key("Settings");
			writer.Value(settings);
			writer.Key("Pin");
			writer.BeginArray();
			writer.Unsigned(id);
			writer.String("Result");
			writer.EndArray();
			writer.EndObject();
		}
		writer.EndArray();
		writer.Key("Empty");
		writer.BeginArray();
		writer.EndArray();
		writer.EndObject();
		CHECK(writer.TakeText() == Dump(expected, style));
	}
}
