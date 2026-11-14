#include "Strada/Asset/CurveAsset.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Math/Curve.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/StructSerialization.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace Strada;

namespace
{
	CurveKey MakeKey(float time, float value, CurveInterpolation interpolation = CurveInterpolation::Cubic)
	{
		CurveKey key;
		key.Time = time;
		key.Value = value;
		key.Interpolation = interpolation;
		return key;
	}

	Curve MakeCurve(std::vector<CurveKey> keys, CurveExtrapolation before = CurveExtrapolation::Hold,
	                CurveExtrapolation after = CurveExtrapolation::Hold)
	{
		Curve curve;
		curve.Keys = std::move(keys);
		curve.PreExtrapolation = before;
		curve.PostExtrapolation = after;
		return curve;
	}

	// A key of two components, read through CurveKeyTraits as the timeline's keys are.
	struct PairKey
	{
		float Time = 0.0f;
		std::array<float, 2> Value{};
		std::optional<float> Out;
	};

	// A number of the differential file: a number, or NaN, Infinity or -Infinity as strings.
	double ReadDifferentialNumber(Json const& value)
	{
		if (value.is_string())
		{
			std::string const& text = value.get_ref<std::string const&>();
			if (text == "NaN")
			{
				return std::numeric_limits<double>::quiet_NaN();
			}
			REQUIRE_MESSAGE((text == "Infinity" || text == "-Infinity"), text);
			return text == "Infinity" ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
		}
		REQUIRE(value.is_number());
		return value.get<double>();
	}
}

namespace Strada
{
	template<>
	struct CurveKeyTraits<PairKey>
	{
		static float GetTime(PairKey const& key) { return key.Time; }
		static float GetValue(PairKey const& key, uint32_t component) { return key.Value[component]; }
		static CurveInterpolation GetInterpolation(PairKey const&) { return CurveInterpolation::Cubic; }
		static std::optional<float> GetIn(PairKey const&, uint32_t) { return std::nullopt; }
		static std::optional<float> GetOut(PairKey const& key, uint32_t component) { return component == 0 ? key.Out : std::nullopt; }
	};
}

TEST_CASE("Curve: keys evaluate as the visual script timeline's tracks did (spec-slice3 §3.8.3)")
{
	// The cases of the timeline's own tests, on the engine's curve: the results are the ones they pin.
	Curve track = MakeCurve({MakeKey(0.0f, 0.0f), MakeKey(1.0f, 1.0f)});
	std::array<double, 4> const times = {0.25, 0.5, 0.75, 1.0};
	std::array<float, 4> const cubic = {0.15625f, 0.5f, 0.84375f, 1.0f};
	for (size_t i = 0; i < times.size(); i++)
	{
		CHECK(track.Evaluate(times[i]) == doctest::Approx(cubic[i]).epsilon(1e-6));
	}
	track.Keys[0].Interpolation = CurveInterpolation::Linear;
	std::array<float, 4> const linear = {0.25f, 0.5f, 0.75f, 1.0f};
	for (size_t i = 0; i < times.size(); i++)
	{
		CHECK(track.Evaluate(times[i]) == doctest::Approx(linear[i]).epsilon(1e-6));
	}
	track.Keys[0].Interpolation = CurveInterpolation::Constant;
	std::array<float, 4> const constant = {0.0f, 0.0f, 0.0f, 1.0f};
	for (size_t i = 0; i < times.size(); i++)
	{
		CHECK(track.Evaluate(times[i]) == doctest::Approx(constant[i]).epsilon(1e-6));
	}

	// Given slopes: (0, 0, Out 2), (1, 1, In 0).
	Curve slopes = MakeCurve({MakeKey(0.0f, 0.0f), MakeKey(1.0f, 1.0f)});
	slopes.Keys[0].HasOut = true;
	slopes.Keys[0].Out = 2.0f;
	slopes.Keys[1].HasIn = true;
	slopes.Keys[1].In = 0.0f;
	CHECK(slopes.Evaluate(0.5) == doctest::Approx(0.75).epsilon(1e-6));

	// Automatic slopes on uneven values: (0, 0), (1, 1), (2, 4).
	Curve const three = MakeCurve({MakeKey(0.0f, 0.0f), MakeKey(1.0f, 1.0f), MakeKey(2.0f, 4.0f)});
	CHECK(three.Evaluate(0.5) == doctest::Approx(0.25).epsilon(1e-6));
	CHECK(three.Evaluate(1.5) == doctest::Approx(2.75).epsilon(1e-6));
	CHECK(three.GetKeySlopes(1) == std::pair(2.0f, 2.0f));
	CHECK(three.GetKeySlopes(0) == std::pair(0.0f, 0.0f));
	CHECK(three.GetKeySlopes(7) == std::pair(0.0f, 0.0f));
	// Before the first key and after the last, their values (Hold); no key, 0.
	CHECK(three.Evaluate(-1.0) == 0.0f);
	CHECK(three.Evaluate(3.0) == 4.0f);
	CHECK(MakeCurve({}).Evaluate(0.5) == 0.0f);

	// Keys at one time make a jump: the left segment ends at the first, the right one starts at the last.
	Curve jump = MakeCurve({MakeKey(0.0f, 0.0f), MakeKey(1.0f, 1.0f), MakeKey(1.0f, 5.0f), MakeKey(2.0f, 5.0f)});
	for (CurveKey& key : jump.Keys)
	{
		key.Interpolation = CurveInterpolation::Linear;
	}
	CHECK(jump.Evaluate(0.5) == doctest::Approx(0.5));
	CHECK(jump.Evaluate(1.0) == 5.0f);
	CHECK(jump.Evaluate(1.5) == 5.0f);

	// The algorithms read other key types per component, as the timeline's vectors: here two components, the first with a
	// given Out slope, each evaluated on its own.
	std::array<PairKey, 2> pairs{};
	pairs[1].Time = 1.0f;
	pairs[1].Value = {1.0f, 2.0f};
	pairs[0].Out = 2.0f;
	std::array<double, 2> values{};
	EvaluateCurveKeys(std::span<PairKey const>(pairs), 0.5, std::span<double>(values));
	CHECK(values[0] == doctest::Approx(0.75));
	CHECK(values[1] == doctest::Approx(1.0));
	CHECK(GetCurveKeySlopes(std::span<PairKey const>(pairs), 0, 0) == std::pair(0.0, 2.0));
	CHECK(FindCurveSegment(std::span<PairKey const>(pairs), 0.99) == 0u);
	CHECK(FindCurveSegment(std::span<PairKey const>(pairs), 1.0) == 1u);
}

TEST_CASE("Curve: before the first key and after the last it holds, loops, ping-pongs or goes on along its slope")
{
	// A triangle from 0 up to 1 and back, over 2 units.
	Curve triangle =
		MakeCurve({MakeKey(0.0f, 0.0f, CurveInterpolation::Linear), MakeKey(1.0f, 1.0f, CurveInterpolation::Linear), MakeKey(2.0f, 0.0f)},
	              CurveExtrapolation::Loop, CurveExtrapolation::Loop);
	CHECK(triangle.Evaluate(2.5) == 0.5f);
	CHECK(triangle.Evaluate(4.0) == 0.0f);
	CHECK(triangle.Evaluate(-0.5) == 0.5f);
	CHECK(triangle.Evaluate(-3.75) == 0.25f);
	CHECK(triangle.Evaluate(1000000.5) == 0.5f);

	// A ramp from 10 to 20 over [1, 3], reflected at each end.
	Curve ramp = MakeCurve({MakeKey(1.0f, 10.0f, CurveInterpolation::Linear), MakeKey(3.0f, 20.0f)}, CurveExtrapolation::PingPong,
	                       CurveExtrapolation::PingPong);
	CHECK(ramp.Evaluate(4.0) == 15.0f);
	CHECK(ramp.Evaluate(5.0) == 10.0f);
	CHECK(ramp.Evaluate(0.0) == 15.0f);
	CHECK(ramp.Evaluate(-1.0) == 20.0f);

	// Linear along a linear segment's slope, and along given cubic slopes; flat along automatic ones and after a step.
	ramp.PreExtrapolation = CurveExtrapolation::Linear;
	ramp.PostExtrapolation = CurveExtrapolation::Linear;
	CHECK(ramp.Evaluate(0.0) == 5.0f);
	CHECK(ramp.Evaluate(4.0) == 25.0f);
	Curve sloped = MakeCurve({MakeKey(0.0f, 0.0f), MakeKey(1.0f, 1.0f)}, CurveExtrapolation::Linear, CurveExtrapolation::Linear);
	CHECK(sloped.Evaluate(-1.0) == 0.0f);
	CHECK(sloped.Evaluate(3.0) == 1.0f);
	sloped.Keys[0].HasOut = true;
	sloped.Keys[0].Out = 3.0f;
	sloped.Keys[1].HasIn = true;
	sloped.Keys[1].In = -1.0f;
	CHECK(sloped.Evaluate(-2.0) == -6.0f);
	CHECK(sloped.Evaluate(3.0) == -1.0f);
	sloped.Keys[0].Interpolation = CurveInterpolation::Constant;
	CHECK(sloped.Evaluate(-2.0) == 0.0f);
	CHECK(sloped.Evaluate(3.0) == 1.0f);

	// Where keys at an end's time make a jump, Linear goes on from the end key along the segment that reaches that time.
	Curve jumps = MakeCurve({MakeKey(0.0f, 0.0f), MakeKey(0.0f, 2.0f, CurveInterpolation::Linear),
	                         MakeKey(1.0f, 3.0f, CurveInterpolation::Linear), MakeKey(1.0f, 9.0f)},
	                        CurveExtrapolation::Linear, CurveExtrapolation::Linear);
	CHECK(jumps.Evaluate(-1.0) == -1.0f);
	CHECK(jumps.Evaluate(0.0) == 2.0f);
	CHECK(jumps.Evaluate(1.0) == 9.0f);
	CHECK(jumps.Evaluate(2.0) == 10.0f);

	// Every key at one time, or a single key: every extrapolation holds.
	for (CurveExtrapolation const extrapolation :
	     {CurveExtrapolation::Hold, CurveExtrapolation::Loop, CurveExtrapolation::PingPong, CurveExtrapolation::Linear})
	{
		CAPTURE(static_cast<int>(extrapolation));
		Curve const single = MakeCurve({MakeKey(2.0f, 3.0f)}, extrapolation, extrapolation);
		CHECK(single.Evaluate(-10.0) == 3.0f);
		CHECK(single.Evaluate(10.0) == 3.0f);
		Curve const step = MakeCurve({MakeKey(1.0f, 2.0f, CurveInterpolation::Linear), MakeKey(1.0f, 6.0f)}, extrapolation, extrapolation);
		CHECK(step.Evaluate(0.0) == 2.0f);
		CHECK(step.Evaluate(4.0) == 6.0f);
	}
}

TEST_CASE("Curve: NaN gives NaN, and infinite times lie beyond every key")
{
	double const infinity = std::numeric_limits<double>::infinity();
	double const nan = std::numeric_limits<double>::quiet_NaN();
	// No key: 0 everywhere.
	CHECK(MakeCurve({}, CurveExtrapolation::Loop, CurveExtrapolation::Linear).Evaluate(nan) == 0.0f);
	Curve curve = MakeCurve({MakeKey(0.0f, 1.0f, CurveInterpolation::Constant), MakeKey(1.0f, 3.0f)});
	CHECK(std::isnan(curve.Evaluate(nan)));
	CHECK(std::isnan(MakeCurve({MakeKey(0.0f, 1.0f)}).Evaluate(nan)));
	CHECK(curve.Evaluate(-infinity) == 1.0f);
	CHECK(curve.Evaluate(infinity) == 3.0f);
	curve.PreExtrapolation = CurveExtrapolation::Loop;
	curve.PostExtrapolation = CurveExtrapolation::PingPong;
	CHECK(std::isnan(curve.Evaluate(-infinity)));
	CHECK(std::isnan(curve.Evaluate(infinity)));
	curve.Keys[0].Interpolation = CurveInterpolation::Linear;
	curve.PreExtrapolation = CurveExtrapolation::Linear;
	curve.PostExtrapolation = CurveExtrapolation::Linear;
	CHECK(curve.Evaluate(-infinity) == -std::numeric_limits<float>::infinity());
	CHECK(curve.Evaluate(infinity) == std::numeric_limits<float>::infinity());
}

TEST_CASE("Curve: keys are in time order and finite, at most MaxCurveKeys of them")
{
	Curve curve = MakeCurve({MakeKey(0.0f, 0.0f), MakeKey(0.5f, 1.0f), MakeKey(0.5f, 2.0f)});
	CHECK(ValidateCurve(curve).IsOk());
	CHECK(ValidateCurve(Curve{}).IsOk());
	curve.Keys[2].Time = 0.25f;
	Result<void> const unordered = ValidateCurve(curve);
	REQUIRE(unordered.IsError());
	CHECK(unordered.GetError() == "Keys[2].Time: keys are in time order: 0.25 comes after 0.5");
	curve.Keys[2].Time = 1.0f;
	curve.Keys[1].Out = std::numeric_limits<float>::infinity();
	Result<void> const infinite = ValidateCurve(curve);
	REQUIRE(infinite.IsError());
	CHECK(infinite.GetError() == "Keys[1]: a key's time, value and slopes are finite numbers");
	curve.Keys[1].Out = 0.0f;
	curve.Keys.resize(MaxCurveKeys + 1, MakeKey(2.0f, 0.0f));
	Result<void> const many = ValidateCurve(curve);
	REQUIRE(many.IsError());
	CHECK(many.GetError() == "Keys: a curve has at most 1024 keys, not 1025");
}

TEST_CASE("Curve: the differential cases evaluate to their values, as the scripting runtime's evaluator does")
{
	// Tests/Data/Curves/Differential.json holds the engine's evaluator and the scripting runtime's together: ScriptCoreTests
	// evaluates the same cases (CurveMathTests.cs) to the same values, within the file's tolerance.
	std::filesystem::path const path = FileSystem::PathFromUtf8(STRADA_TEST_DATA_DIR) / "Curves" / "Differential.json";
	Result<std::string> const text = FileSystem::ReadTextFile(path);
	REQUIRE(text.IsOk());
	Result<Json> const document = ParseJson(text.GetValue());
	REQUIRE(document.IsOk());
	double const tolerance = document.GetValue().at("Tolerance").get<double>();
	Json const& cases = document.GetValue().at("Cases");
	REQUIRE(cases.size() >= 20);
	size_t samples = 0;
	for (Json const& differential : cases)
	{
		CAPTURE(differential.at("Name").get<std::string>());
		Curve curve;
		Result<void> const read =
			DeserializeFields<StructTraits<Curve>>(differential.at("Curve"), curve, DeserializationContext{}, "curve");
		REQUIRE_MESSAGE(read.IsOk(), (read ? std::string() : read.GetError()));
		REQUIRE(ValidateCurve(curve).IsOk());
		for (Json const& sample : differential.at("Samples"))
		{
			double const time = ReadDifferentialNumber(sample.at("Time"));
			auto const expected = static_cast<float>(ReadDifferentialNumber(sample.at("Value")));
			float const value = curve.Evaluate(time);
			CAPTURE(time);
			CAPTURE(expected);
			CAPTURE(value);
			if (std::isnan(expected))
			{
				CHECK(std::isnan(value));
			}
			else if (std::isinf(expected))
			{
				CHECK(value == expected);
			}
			else
			{
				CHECK(std::abs(static_cast<double>(value) - static_cast<double>(expected)) <=
				      tolerance * std::max(1.0, std::abs(static_cast<double>(expected))));
			}
			samples++;
		}
	}
	CHECK(samples >= 100);
}
