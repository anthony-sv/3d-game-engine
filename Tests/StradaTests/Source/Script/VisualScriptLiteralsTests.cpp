#include "Fuzzing.h"

#include "Strada/Script/VisualScriptLiterals.h"

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	// Enums as the scripting runtime describes them: of every underlying type, [Flags] and not, with a combined enumerator,
	// a negative one and an alias; and two descriptions no runtime gives.
	std::optional<VisualScriptEnumInfo> LookUpEnum(std::string_view typeId)
	{
		if (typeId == "Strada.ForceMode")
		{
			return VisualScriptEnumInfo{ScriptFieldType::Int32,
			                            {{"Force", ScriptFieldValue::FromInt32(0)},
			                             {"Impulse", ScriptFieldValue::FromInt32(1)},
			                             {"Acceleration", ScriptFieldValue::FromInt32(2)},
			                             {"VelocityChange", ScriptFieldValue::FromInt32(3)}},
			                            false};
		}
		if (typeId == "Game.Layers")
		{
			return VisualScriptEnumInfo{ScriptFieldType::UInt32,
			                            {{"None", ScriptFieldValue::FromUInt32(0)},
			                             {"Walls", ScriptFieldValue::FromUInt32(1)},
			                             {"Water", ScriptFieldValue::FromUInt32(2)},
			                             {"Air", ScriptFieldValue::FromUInt32(4)},
			                             {"Solid", ScriptFieldValue::FromUInt32(3)}},
			                            true};
		}
		if (typeId == "Game.Distance")
		{
			return VisualScriptEnumInfo{ScriptFieldType::Int64,
			                            {{"Near", ScriptFieldValue::FromInt64(-5)}, {"Far", ScriptFieldValue::FromInt64(9000000000)}},
			                            false};
		}
		if (typeId == "Game.Mask")
		{
			return VisualScriptEnumInfo{
				ScriptFieldType::UInt64,
				{{"Low", ScriptFieldValue::FromUInt64(1)}, {"High", ScriptFieldValue::FromUInt64(0x8000000000000000ull)}},
				true};
		}
		if (typeId == "Game.Signs")
		{
			return VisualScriptEnumInfo{
				ScriptFieldType::Int32,
				{{"Low", ScriptFieldValue::FromInt32(1)}, {"Sign", ScriptFieldValue::FromInt32(std::numeric_limits<int32_t>::min())}},
				true};
		}
		if (typeId == "Game.Grade")
		{
			return VisualScriptEnumInfo{ScriptFieldType::Int32,
			                            {{"First", ScriptFieldValue::FromInt32(1)},
			                             {"Best", ScriptFieldValue::FromInt32(1)},
			                             {"Second", ScriptFieldValue::FromInt32(2)}},
			                            false};
		}
		if (typeId == "Game.Fractional")
		{
			return VisualScriptEnumInfo{ScriptFieldType::Float, {{"Half", ScriptFieldValue::FromFloat(0.5f)}}, false};
		}
		if (typeId == "Game.Mixed")
		{
			return VisualScriptEnumInfo{ScriptFieldType::Int32, {{"Big", ScriptFieldValue::FromInt64(1)}}, false};
		}
		return std::nullopt;
	}

	// The text of a literal; invalid UTF-8 in strings is replaced rather than thrown about.
	std::string GetText(Json const& literal)
	{
		return literal.dump(-1, ' ', false, Json::error_handler_t::replace);
	}

	uint32_t GetBits(float value)
	{
		return std::bit_cast<uint32_t>(value);
	}

	// Every exposable type, the enums above, and types without such literals: an unusable enum, Strada.Matrix4, a struct, exec.
	constexpr std::array<std::string_view, 33> TypeIds = {"bool",
	                                                      "int",
	                                                      "uint",
	                                                      "long",
	                                                      "ulong",
	                                                      "float",
	                                                      "double",
	                                                      "string",
	                                                      "Strada.Vector2",
	                                                      "Strada.Vector3",
	                                                      "Strada.Vector4",
	                                                      "Strada.Quaternion",
	                                                      "Strada.Color",
	                                                      "Strada.Entity",
	                                                      "Strada.Prefab",
	                                                      "Strada.AssetHandle",
	                                                      "Strada.Mesh",
	                                                      "Strada.Material",
	                                                      "Strada.Texture",
	                                                      "Strada.AudioClip",
	                                                      "Strada.Font",
	                                                      "Strada.EnvironmentMap",
	                                                      "Strada.VisualScript",
	                                                      "Strada.ForceMode",
	                                                      "Game.Layers",
	                                                      "Game.Distance",
	                                                      "Game.Mask",
	                                                      "Game.Signs",
	                                                      "Game.Grade",
	                                                      "Game.Fractional",
	                                                      "Strada.Matrix4",
	                                                      "Strada.RaycastHit",
	                                                      "exec"};

	// Literals that look like those of the type, and some that do not, chosen at random.
	class LiteralGenerator
	{
	public:
		explicit LiteralGenerator(uint64_t seed)
			: m_Random(seed)
		{
		}

		Json Make(std::string_view typeId)
		{
			if (typeId == "bool")
			{
				return Pick(2) == 0;
			}
			if (typeId == "int")
			{
				return static_cast<int32_t>(m_Random.Next());
			}
			if (typeId == "uint")
			{
				return static_cast<uint32_t>(m_Random.Next());
			}
			if (typeId == "long" || typeId == "Game.Distance")
			{
				return Pick(4) == 0 ? Json(MakeName()) : Json(std::to_string(static_cast<int64_t>(m_Random.Next())));
			}
			if (typeId == "ulong")
			{
				return std::to_string(m_Random.Next());
			}
			if (typeId == "float")
			{
				return MakeFloat();
			}
			if (typeId == "double")
			{
				double const number = std::bit_cast<double>(m_Random.Next());
				return Pick(2) == 0 ? Json(number) : Json(static_cast<double>(static_cast<int32_t>(m_Random.Next())) / 1024.0);
			}
			if (typeId == "string")
			{
				std::string text;
				for (size_t length = Pick(8); length > 0; length--)
				{
					text.push_back(static_cast<char>(m_Random.Next()));
				}
				return text;
			}
			if (typeId == "Strada.Vector2")
			{
				return MakeFloats(2);
			}
			if (typeId == "Strada.Vector3")
			{
				return MakeFloats(3);
			}
			if (typeId == "Strada.Vector4" || typeId == "Strada.Quaternion" || typeId == "Strada.Color")
			{
				return MakeFloats(4);
			}
			if (typeId == "Strada.Matrix4")
			{
				return MakeFloats(16);
			}
			if (typeId == "Strada.Entity")
			{
				return Pick(2) == 0 ? Json("0") : Json(std::to_string(m_Random.Next()));
			}
			if (typeId == "Strada.ForceMode" || typeId == "Game.Grade" || typeId == "Game.Fractional")
			{
				return Pick(3) == 0 ? Json(static_cast<int32_t>(Pick(9)) - 4) : Json(MakeName());
			}
			if (typeId == "Game.Layers" || typeId == "Game.Mask" || typeId == "Game.Signs")
			{
				switch (Pick(3))
				{
					case 0:
						return static_cast<uint32_t>(Pick(16));
					case 1:
						return std::to_string(m_Random.Next());
					default:
					{
						Json names = Json::array();
						for (size_t count = Pick(4); count > 0; count--)
						{
							names.push_back(MakeName());
						}
						return names;
					}
				}
			}
			// Asset handles, and what other types are given.
			return Pick(4) == 0 ? Json("asset://Textures/Oak.png") : Json(std::to_string(m_Random.Next()));
		}

	private:
		size_t Pick(size_t count) { return m_Random.Pick(count); }

		// A float as documents store it: exact, short, or past the largest float literal.
		Json MakeFloat()
		{
			switch (Pick(3))
			{
				case 0:
				{
					float const value = std::bit_cast<float>(static_cast<uint32_t>(m_Random.Next()));
					return std::isfinite(value) ? Json(static_cast<double>(value)) : Json(0.25);
				}
				case 1:
					return static_cast<double>(static_cast<int32_t>(Pick(2000001)) - 1000000) / 1000.0;
				default:
					return (static_cast<double>(Pick(2001)) - 1000.0) * 4.0e35;
			}
		}

		Json MakeFloats(size_t count)
		{
			Json floats = Json::array();
			for (size_t i = 0; i < count; i++)
			{
				floats.push_back(MakeFloat());
			}
			return floats;
		}

		std::string MakeName()
		{
			constexpr std::array<std::string_view, 15> Names = {"Force", "Impulse", "VelocityChange", "None",  "Walls",
			                                                    "Water", "Air",     "Solid",          "Near",  "Far",
			                                                    "Low",   "High",    "Sign",           "First", "Best"};
			return std::string(Names[Pick(Names.size())]);
		}

		Testing::FuzzRandom m_Random;
	};
}

TEST_CASE("VisualScript: canonical literals of every exposable type and enum decode and encode back to themselves")
{
	VisualScriptEnumLookup const lookup = LookUpEnum;
	struct Row
	{
		std::string_view TypeId;
		Json Literal;
	};
	std::vector<Row> const rows = {
		{"bool", true},
		{"bool", false},
		{"int", 0},
		{"int", std::numeric_limits<int32_t>::min()},
		{"int", std::numeric_limits<int32_t>::max()},
		{"uint", 0},
		{"uint", std::numeric_limits<uint32_t>::max()},
		{"long", "-9223372036854775808"},
		{"long", "9223372036854775807"},
		{"long", "-9000000000"},
		{"ulong", "18446744073709551615"},
		{"ulong", "0"},
		// Floats as their shortest texts read: 0.2f is 0.2, 4.0f is 4, the largest float is 3.4028235e38.
		{"float", 0.2},
		{"float", -1.5},
		{"float", 4},
		{"float", 0},
		{"float", 3.4028235e38},
		{"float", -3.4028235e38},
		{"float", 1e-45},
		{"float", 1.1754944e-38},
		{"double", 0.1},
		{"double", -9.75},
		{"double", 1e300},
		{"string", ""},
		{"string", "Hello, ünïcødé"},
		{"Strada.Vector2", Json::array({0.5, -0.5})},
		{"Strada.Vector3", Json::array({1, 2, 3})},
		{"Strada.Vector4", Json::array({0.25, 0.1, -3.4028235e38, 0})},
		{"Strada.Quaternion", Json::array({0, 0.6, 0, 0.8})},
		{"Strada.Quaternion", Json::array({0, 0, 0, 1})},
		// Colors are linear and not clamped.
		{"Strada.Color", Json::array({2, -1, 0.5, 1})},
		{"Strada.Entity", "0"},
		{"Strada.Prefab", "17000004240335447843"},
		{"Strada.AssetHandle", "0"},
		{"Strada.Mesh", "4242"},
		{"Strada.Material", "18446744073709551615"},
		{"Strada.Texture", "1"},
		{"Strada.AudioClip", "9906031496213900225"},
		{"Strada.Font", "1024"},
		{"Strada.EnvironmentMap", "77"},
		{"Strada.VisualScript", "12777872994629461483"},
		// Enums by name; values without an enumerator as integers, decimal strings for 64-bit enums.
		{"Strada.ForceMode", "Impulse"},
		{"Strada.ForceMode", 7},
		{"Strada.ForceMode", -1},
		{"Game.Grade", "First"},
		{"Game.Distance", "Far"},
		{"Game.Distance", "-7000000000"},
		// [Flags] enums as arrays: [] for 0, the enumerator of exactly a value's bits, else the enumerators that make it up.
		{"Game.Layers", Json::array()},
		{"Game.Layers", Json::array({"Water"})},
		{"Game.Layers", Json::array({"Solid"})},
		{"Game.Layers", Json::array({"Walls", "Air"})},
		{"Game.Layers", Json::array({"Walls", "Water", "Air", "Solid"})},
		{"Game.Layers", 8},
		{"Game.Mask", Json::array({"Low", "High"})},
		{"Game.Mask", "4"},
		{"Game.Signs", Json::array({"Sign"})},
		{"Game.Signs", Json::array({"Low", "Sign"})},
		{"Game.Signs", -2},
	};
	for (Row const& row : rows)
	{
		CAPTURE(row.TypeId);
		std::string const text = GetText(row.Literal);
		CAPTURE(text);
		std::optional<ScriptFieldValue> const value = DecodeVisualScriptLiteral(row.Literal, row.TypeId, lookup);
		REQUIRE(value.has_value());
		std::optional<Json> const encoded = EncodeVisualScriptLiteral(*value, row.TypeId, lookup);
		REQUIRE(encoded.has_value());
		CHECK(*encoded == row.Literal);
		// The same text: what the scripting runtime writes for the value.
		CHECK(GetText(*encoded) == text);
	}

	// The values literals stand for.
	CHECK(DecodeVisualScriptLiteral(0.2, "float", lookup) == ScriptFieldValue::FromFloat(0.2f));
	CHECK(DecodeVisualScriptLiteral(3.4028235e38, "float", lookup) == ScriptFieldValue::FromFloat(std::numeric_limits<float>::max()));
	CHECK(DecodeVisualScriptLiteral("-9223372036854775808", "long", lookup) ==
	      ScriptFieldValue::FromInt64(std::numeric_limits<int64_t>::min()));
	CHECK(DecodeVisualScriptLiteral("18446744073709551615", "ulong", lookup) ==
	      ScriptFieldValue::FromUInt64(std::numeric_limits<uint64_t>::max()));
	CHECK(DecodeVisualScriptLiteral("17000004240335447843", "Strada.Prefab", lookup) ==
	      ScriptFieldValue::FromPrefab(AssetHandle(UUID(17000004240335447843ull))));
	CHECK(DecodeVisualScriptLiteral("4242", "Strada.Mesh", lookup) == ScriptFieldValue::FromAsset(AssetHandle(UUID(4242))));
	CHECK(DecodeVisualScriptLiteral(Json::array({"Walls", "Air"}), "Game.Layers", lookup) == ScriptFieldValue::FromUInt32(5));
	CHECK(DecodeVisualScriptLiteral(Json::array({"Low", "Sign"}), "Game.Signs", lookup) ==
	      ScriptFieldValue::FromInt32(std::numeric_limits<int32_t>::min() + 1));
	CHECK(DecodeVisualScriptLiteral("Far", "Game.Distance", lookup) == ScriptFieldValue::FromInt64(9000000000));
}

TEST_CASE("VisualScript: a float literal is the shortest text that reads back as the same float")
{
	VisualScriptEnumLookup const none;
	auto const encode = [&none](float value)
	{
		return EncodeVisualScriptLiteral(ScriptFieldValue::FromFloat(value), "float", none);
	};
	auto const decode = [&none](Json const& literal)
	{
		std::optional<ScriptFieldValue> const value = DecodeVisualScriptLiteral(literal, "float", none);
		REQUIRE(value.has_value());
		return value->GetFloat();
	};

	// Never the value widened to a double, which is how JSON writes floats otherwise.
	REQUIRE(encode(0.2f).has_value());
	CHECK(GetText(*encode(0.2f)) == "0.2");
	CHECK(GetText(Json(0.2f)) == "0.20000000298023224");
	CHECK(GetText(*encode(4.0f)) == "4");
	CHECK(GetText(*encode(-1e-10f)) == "-1e-10");
	CHECK(GetText(*encode(std::numeric_limits<float>::max())) == "3.4028235e+38");
	CHECK(GetText(*EncodeVisualScriptLiteral(ScriptFieldValue::FromVector3({0.1f, 0.2f, 0.3f}), "Strada.Vector3", none)) ==
	      "[0.1,0.2,0.3]");
	CHECK(GetText(*EncodeVisualScriptLiteral(ScriptFieldValue::FromColor({1.0f, 0.5f, 0.25f, 1.0f}), "Strada.Color", none)) ==
	      "[1,0.5,0.25,1]");

	// A float its shortest text does not give back bit for bit is written as its exact value: -0 (the text "-0" reads as
	// the integer 0), and ±7.038531e-26, which a reader through a double rounds to the neighbouring float. Every float
	// comes back.
	for (uint32_t const bits : {0x80000000u, 0x15AE43FDu, 0x95AE43FDu, 0x00000001u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x3E4CCCCDu})
	{
		CAPTURE(bits);
		float const value = std::bit_cast<float>(bits);
		std::optional<Json> const encoded = encode(value);
		REQUIRE(encoded.has_value());
		CHECK(GetBits(decode(*encoded)) == bits);
	}
	CHECK(GetText(*encode(-0.0f)) == "-0.0");
	CHECK(GetText(*encode(std::bit_cast<float>(0x15AE43FDu))) != "7.038531e-26");

	// Numbers that are not finite have no literal.
	for (float const value :
	     {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
	{
		CHECK_FALSE(encode(value).has_value());
		CHECK_FALSE(EncodeVisualScriptLiteral(ScriptFieldValue::FromVector2({0.0f, value}), "Strada.Vector2", none).has_value());
		CHECK_FALSE(EncodeVisualScriptLiteral(ScriptFieldValue::FromColor({value, 0.0f, 0.0f, 1.0f}), "Strada.Color", none).has_value());
		CHECK_FALSE(
			EncodeVisualScriptLiteral(ScriptFieldValue::FromQuaternion(glm::quat::wxyz(1.0f, value, 0.0f, 0.0f)), "Strada.Quaternion", none)
				.has_value());
		CHECK_FALSE(EncodeVisualScriptLiteral(ScriptFieldValue::FromDouble(static_cast<double>(value)), "double", none).has_value());
	}

	// Quaternions are written normalized; one of zero length is the identity, as it reads.
	std::optional<Json> const half =
		EncodeVisualScriptLiteral(ScriptFieldValue::FromQuaternion(glm::quat::wxyz(4.0f, 0.0f, 3.0f, 0.0f)), "Strada.Quaternion", none);
	REQUIRE(half.has_value());
	CHECK(GetText(*half) == "[0,0.6,0,0.8]");
	CHECK(EncodeVisualScriptLiteral(ScriptFieldValue::FromQuaternion(glm::quat::wxyz(0.0f, 0.0f, 0.0f, 0.0f)), "Strada.Quaternion", none) ==
	      std::optional<Json>(Json::array({0, 0, 0, 1})));
}

TEST_CASE("VisualScript: enum values are written as the scripting runtime writes them")
{
	VisualScriptEnumLookup const lookup = LookUpEnum;
	auto const encode = [&lookup](ScriptFieldValue const& value, std::string_view typeId)
	{
		std::optional<Json> encoded = EncodeVisualScriptLiteral(value, typeId, lookup);
		REQUIRE(encoded.has_value());
		// What is written reads back as the value.
		CHECK(DecodeVisualScriptLiteral(*encoded, typeId, lookup) == std::optional<ScriptFieldValue>(value));
		return *encoded;
	};

	CHECK(encode(ScriptFieldValue::FromInt32(2), "Strada.ForceMode") == "Acceleration");
	CHECK(encode(ScriptFieldValue::FromInt32(7), "Strada.ForceMode") == 7);
	// Of enumerators of one value, the first is its name.
	CHECK(encode(ScriptFieldValue::FromInt32(1), "Game.Grade") == "First");
	CHECK(DecodeVisualScriptLiteral("Best", "Game.Grade", lookup) == ScriptFieldValue::FromInt32(1));
	CHECK(encode(ScriptFieldValue::FromInt64(-5), "Game.Distance") == "Near");
	CHECK(encode(ScriptFieldValue::FromInt64(-7000000000), "Game.Distance") == "-7000000000");

	// [Flags]: none for 0 (even with an enumerator of 0), the enumerator of exactly the bits, else every enumerator whose bits
	// are among them, in declaration order; the integer when they do not make it up.
	CHECK(encode(ScriptFieldValue::FromUInt32(0), "Game.Layers") == Json::array());
	CHECK(encode(ScriptFieldValue::FromUInt32(3), "Game.Layers") == Json::array({"Solid"}));
	CHECK(encode(ScriptFieldValue::FromUInt32(5), "Game.Layers") == Json::array({"Walls", "Air"}));
	CHECK(encode(ScriptFieldValue::FromUInt32(7), "Game.Layers") == Json::array({"Walls", "Water", "Air", "Solid"}));
	CHECK(encode(ScriptFieldValue::FromUInt32(9), "Game.Layers") == 9);
	CHECK(encode(ScriptFieldValue::FromUInt64(0x8000000000000001ull), "Game.Mask") == Json::array({"Low", "High"}));
	CHECK(encode(ScriptFieldValue::FromUInt64(4), "Game.Mask") == "4");
	CHECK(encode(ScriptFieldValue::FromInt32(std::numeric_limits<int32_t>::min()), "Game.Signs") == Json::array({"Sign"}));
	CHECK(encode(ScriptFieldValue::FromInt32(-1), "Game.Signs") == -1);

	// The description in hand works alike.
	std::optional<VisualScriptEnumInfo> const layers = LookUpEnum("Game.Layers");
	REQUIRE(layers.has_value());
	CHECK(EncodeVisualScriptEnumLiteral(ScriptFieldValue::FromUInt32(6), *layers) == std::optional<Json>(Json::array({"Water", "Air"})));
	CHECK(DecodeVisualScriptEnumLiteral(Json::array({"Water", "Air"}), *layers) == ScriptFieldValue::FromUInt32(6));
	// A single name is no [Flags] literal, and a name of another enum none at all.
	CHECK_FALSE(DecodeVisualScriptEnumLiteral("Water", *layers).has_value());
	CHECK_FALSE(DecodeVisualScriptEnumLiteral(Json::array({"Water", "Lava"}), *layers).has_value());
}

TEST_CASE("VisualScript: values without a literal of their type encode to nothing, and only enums are looked up")
{
	std::set<std::string, std::less<>> asked;
	VisualScriptEnumLookup const lookup = [&asked](std::string_view typeId)
	{
		asked.emplace(typeId);
		return LookUpEnum(typeId);
	};

	// An entity other than none, and values of another field type than the type's.
	CHECK(EncodeVisualScriptLiteral(ScriptFieldValue::FromEntity(UUID::Invalid()), "Strada.Entity", lookup) == std::optional<Json>("0"));
	CHECK_FALSE(EncodeVisualScriptLiteral(ScriptFieldValue::FromEntity(UUID(42)), "Strada.Entity", lookup).has_value());
	CHECK_FALSE(EncodeVisualScriptLiteral(ScriptFieldValue::FromInt32(1), "float", lookup).has_value());
	CHECK_FALSE(EncodeVisualScriptLiteral(ScriptFieldValue::FromVector4(glm::vec4(1.0f)), "Strada.Color", lookup).has_value());
	CHECK_FALSE(EncodeVisualScriptLiteral(ScriptFieldValue::FromColor(glm::vec4(1.0f)), "Strada.Vector4", lookup).has_value());
	CHECK_FALSE(EncodeVisualScriptLiteral(ScriptFieldValue::FromPrefab(AssetHandle(UUID(5))), "Strada.Material", lookup).has_value());
	CHECK_FALSE(EncodeVisualScriptLiteral(ScriptFieldValue::FromAsset(AssetHandle(UUID(5))), "Strada.Prefab", lookup).has_value());
	CHECK_FALSE(EncodeVisualScriptLiteral(ScriptFieldValue(), "bool", lookup).has_value());
	CHECK_FALSE(EncodeVisualScriptLiteral(ScriptFieldValue::FromUInt32(1), "Strada.ForceMode", lookup).has_value());

	// Strada.Matrix4 has a literal form but no script field type; other structs and classes have neither.
	Json const identity = Json::array({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
	for (std::string_view const typeId : {"Strada.Matrix4", "Strada.RaycastHit", "System.Object", "Strada.Entity[]", "exec", "*", ""})
	{
		CAPTURE(typeId);
		CHECK_FALSE(DecodeVisualScriptLiteral(identity, typeId, lookup).has_value());
		CHECK_FALSE(DecodeVisualScriptLiteral(0, typeId, lookup).has_value());
		CHECK_FALSE(EncodeVisualScriptLiteral(ScriptFieldValue::FromInt32(0), typeId, lookup).has_value());
	}

	// Enums need the lookup, and one that describes them as the scripting runtime does.
	VisualScriptEnumLookup const none;
	CHECK_FALSE(DecodeVisualScriptLiteral("Impulse", "Strada.ForceMode", none).has_value());
	CHECK_FALSE(EncodeVisualScriptLiteral(ScriptFieldValue::FromInt32(1), "Strada.ForceMode", none).has_value());
	for (std::string_view const typeId : {"Game.Fractional", "Game.Mixed"})
	{
		CAPTURE(typeId);
		std::optional<VisualScriptEnumInfo> const info = LookUpEnum(typeId);
		REQUIRE(info.has_value());
		CHECK_FALSE(info->IsUsable());
		CHECK_FALSE(DecodeVisualScriptLiteral(0.5, typeId, lookup).has_value());
		CHECK_FALSE(DecodeVisualScriptLiteral(1, typeId, lookup).has_value());
		CHECK_FALSE(DecodeVisualScriptLiteral("Half", typeId, lookup).has_value());
		CHECK_FALSE(EncodeVisualScriptLiteral(ScriptFieldValue::FromInt32(1), typeId, lookup).has_value());
		CHECK_FALSE(EncodeVisualScriptLiteral(ScriptFieldValue::FromFloat(0.5f), typeId, lookup).has_value());
	}
	CHECK(LookUpEnum("Game.Layers")->IsUsable());

	// The lookup is asked about the types that may be enums only.
	CHECK(asked == std::set<std::string, std::less<>>{"Strada.ForceMode", "Strada.RaycastHit", "Game.Fractional", "Game.Mixed"});
	CHECK(CanBeVisualScriptEnum("Strada.KeyCode"));
	CHECK(CanBeVisualScriptEnum("Game.Player"));
	for (std::string_view const typeId : {"exec", "*", "Strada.Matrix4", "System.Object", "int[]", ""})
	{
		CAPTURE(typeId);
		CHECK_FALSE(CanBeVisualScriptEnum(typeId));
	}
}

TEST_CASE("VisualScript: decoding a literal of any JSON kind as any type never throws")
{
	VisualScriptEnumLookup const lookup = LookUpEnum;
	std::vector<Json> const kinds = {
		Json(),
		true,
		false,
		0,
		-1,
		std::numeric_limits<int64_t>::min(),
		std::numeric_limits<uint64_t>::max(),
		0.5,
		-1.0e30,
		1.0e300,
		std::numeric_limits<double>::infinity(),
		std::numeric_limits<double>::quiet_NaN(),
		"",
		"0",
		"-1",
		"Water",
		"18446744073709551616",
		"asset://Textures/Oak.png",
		Json::array(),
		Json::array({1}),
		Json::array({1, 2, 3, 4}),
		Json::array({1, "2", 3}),
		Json::array({"Water", "Air"}),
		Json::array({Json::array({1})}),
		Json::object(),
		Json::object({{"X", 1}}),
		Json::binary({1, 2, 3}),
	};
	for (std::string_view const typeId : TypeIds)
	{
		CAPTURE(typeId);
		for (Json const& literal : kinds)
		{
			std::string const text = GetText(literal);
			CAPTURE(text);
			std::optional<ScriptFieldValue> value;
			CHECK_NOTHROW(value = DecodeVisualScriptLiteral(literal, typeId, lookup));
			if (value)
			{
				std::optional<Json> const encoded = EncodeVisualScriptLiteral(*value, typeId, lookup);
				REQUIRE(encoded.has_value());
				CHECK(DecodeVisualScriptLiteral(*encoded, typeId, lookup) == value);
			}
		}
	}
}

TEST_CASE("VisualScript: literals that decode encode back to themselves")
{
	VisualScriptEnumLookup const lookup = LookUpEnum;
	// The values come from a stream of their own, the damage from the mutator.
	LiteralGenerator generator(10401);
	Testing::DocumentMutator mutator(10400);
	int decoded = 0;
	int refused = 0;
	int const rounds = Testing::GetFuzzRounds(500);
	for (int round = 0; round < rounds; round++)
	{
		for (std::string_view const typeId : TypeIds)
		{
			Json literal = generator.Make(typeId);
			if (round % 2 == 1)
			{
				literal = mutator.Damage(literal, round);
			}
			std::string const text = GetText(literal);
			INFO("round ", round, ", ", typeId, ": ", text);
			std::optional<ScriptFieldValue> value;
			REQUIRE_NOTHROW(value = DecodeVisualScriptLiteral(literal, typeId, lookup));
			if (!value)
			{
				refused++;
				continue;
			}
			decoded++;
			// What decodes encodes to a literal that decodes to the same value, and is written alike again: floats come back
			// bit for bit (the text of a zero keeps its sign).
			std::optional<Json> const encoded = EncodeVisualScriptLiteral(*value, typeId, lookup);
			REQUIRE(encoded.has_value());
			std::optional<ScriptFieldValue> const again = DecodeVisualScriptLiteral(*encoded, typeId, lookup);
			REQUIRE(again.has_value());
			CHECK(*again == *value);
			std::optional<Json> const reencoded = EncodeVisualScriptLiteral(*again, typeId, lookup);
			REQUIRE(reencoded.has_value());
			CHECK(GetText(*reencoded) == GetText(*encoded));
		}
	}
	CHECK(decoded > 0);
	CHECK(refused > 0);
}
