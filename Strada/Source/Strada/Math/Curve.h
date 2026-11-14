#pragma once

#include "Strada/Core/Result.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// The engine's curve (Docs/Architecture.md §6.5): keys in time order, each with a value, the interpolation of the segment
// to the next key and optional slopes, and what the curve does before its first key and after its last. One semantics with
// two evaluators: this one, which the engine and the editor use (the visual script timeline's tracks evaluate through it,
// CurveKeyTraits), and the managed one of the scripting runtime (Strada-ScriptCore's CurveMath), which graphs evaluate
// without crossing into native code. Tests/Data/Curves/Differential.json holds the two together: StradaTests and
// ScriptCoreTests evaluate its cases to the same values. Pure functions of values: any thread.
namespace Strada
{
	// How the segment from a key to the next one interpolates.
	enum class CurveInterpolation : uint8_t
	{
		// Cubic Hermite through the two keys, with the first key's Out slope and the second key's In slope.
		Cubic = 0,
		Linear,
		// The first key's value until the next key.
		Constant
	};

	// What a curve does before its first key (PreExtrapolation) or after its last (PostExtrapolation).
	enum class CurveExtrapolation : uint8_t
	{
		// The end key's value.
		Hold = 0,
		// The keys repeat with the period from the first key's time to the last's.
		Loop,
		// The keys repeat forwards, then backwards, with that period.
		PingPong,
		// The curve goes on along a line, with the slope it has at the end key.
		Linear
	};

	// The most keys a curve holds.
	inline constexpr size_t MaxCurveKeys = 1024;

	struct CurveKey
	{
		// Where on the curve's input the key lies (seconds, revolutions per minute, a distance): any finite number.
		float Time = 0.0f;
		float Value = 0.0f;
		// The segment to the next key; the last key's is unused.
		CurveInterpolation Interpolation = CurveInterpolation::Cubic;
		// The slopes (value units per input unit) the curve enters and leaves the key with, each automatic unless set.
		bool HasIn = false;
		float In = 0.0f;
		bool HasOut = false;
		float Out = 0.0f;

		bool operator==(CurveKey const& other) const = default;
	};

	// A scalar curve, the data of a .scurve asset (Asset/CurveAsset.h) and of curves embedded in other data. The value-
	// initialized curve has no key and evaluates to 0.
	struct Curve
	{
		CurveExtrapolation PreExtrapolation = CurveExtrapolation::Hold;
		CurveExtrapolation PostExtrapolation = CurveExtrapolation::Hold;
		// In time order (non-decreasing); several keys at one time make a jump. At most MaxCurveKeys.
		std::vector<CurveKey> Keys;

		// The curve's value at a time:
		// - no key: 0, at every time;
		// - from the first key's time to the last's: the keys' segments (EvaluateCurveKeys);
		// - before the first key and after the last: PreExtrapolation and PostExtrapolation. Hold gives the first key's value
		//   before and the last key's after. Loop and PingPong map the time into the keys' span (Hold when every key lies at
		//   one time). Linear goes on from the end key's value along the curve's slope at that end: 0 after a Constant
		//   segment and where every key lies at one time, a Linear segment's slope, a Cubic segment's slope there (the
		//   first key's Out slope, the last key's In slope); where keys at the end's time make a jump, the segment is the one
		//   that reaches that time.
		// - A NaN time gives NaN (except on a curve without keys). Infinite times lie beyond every key: Hold gives the end
		//   value, Linear the end value or an infinity along a slope that is not 0, and Loop and PingPong, which have no
		//   phase there, NaN.
		// Computed in double from the keys' floats; the result is rounded to float once.
		[[nodiscard]] float Evaluate(double time) const;

		// A key's slopes with automatic ones resolved (GetCurveKeySlopes, as floats): (In, Out). (0, 0) for a key the curve does
		// not have.
		[[nodiscard]] std::pair<float, float> GetKeySlopes(uint32_t key) const;

		bool operator==(Curve const& other) const = default;
	};

	// Whether a curve is one a .scurve file holds: at most MaxCurveKeys keys, every number finite, keys in time order. The
	// data asset's Validate (the order is what its field table cannot state); it checks curves made in C++ as well. Error
	// messages name the key ("Keys[2].Time: keys are in time order: 0.25 comes after 0.5").
	[[nodiscard]] Result<void> ValidateCurve(Curve const& curve);

	// How the curve algorithms below read a key type of up to four components: the engine's CurveKey (one component, below)
	// and the visual script timeline's keys (VisualScriptTimeline.h) specialize it. Each specialization gives
	//   static float GetTime(TKey const& key);
	//   static float GetValue(TKey const& key, uint32_t component);
	//   static CurveInterpolation GetInterpolation(TKey const& key);
	//   static std::optional<float> GetIn(TKey const& key, uint32_t component);   // nothing: automatic
	//   static std::optional<float> GetOut(TKey const& key, uint32_t component);
	template<typename TKey>
	struct CurveKeyTraits;

	template<typename TKey>
	concept CurveKeyType = requires(TKey const& key, uint32_t component) {
		{ CurveKeyTraits<TKey>::GetTime(key) } -> std::same_as<float>;
		{ CurveKeyTraits<TKey>::GetValue(key, component) } -> std::same_as<float>;
		{ CurveKeyTraits<TKey>::GetInterpolation(key) } -> std::same_as<CurveInterpolation>;
		{ CurveKeyTraits<TKey>::GetIn(key, component) } -> std::same_as<std::optional<float>>;
		{ CurveKeyTraits<TKey>::GetOut(key, component) } -> std::same_as<std::optional<float>>;
	};

	template<>
	struct CurveKeyTraits<CurveKey>
	{
		static float GetTime(CurveKey const& key) { return key.Time; }
		static float GetValue(CurveKey const& key, uint32_t) { return key.Value; }
		static CurveInterpolation GetInterpolation(CurveKey const& key) { return key.Interpolation; }
		static std::optional<float> GetIn(CurveKey const& key, uint32_t) { return key.HasIn ? std::optional<float>(key.In) : std::nullopt; }
		static std::optional<float> GetOut(CurveKey const& key, uint32_t)
		{
			return key.HasOut ? std::optional<float>(key.Out) : std::nullopt;
		}
	};

	// The largest index of a key whose time is at most time, among keys in time order; 0 when the first key's time is not
	// (a time before every key, or NaN).
	template<CurveKeyType TKey>
	[[nodiscard]] uint32_t FindCurveSegment(std::span<TKey const> keys, double time)
	{
		using Traits = CurveKeyTraits<TKey>;
		uint32_t low = 0;
		uint32_t high = keys.empty() ? 0 : static_cast<uint32_t>(keys.size() - 1);
		while (low < high)
		{
			uint32_t const middle = low + (high - low + 1) / 2;
			if (static_cast<double>(Traits::GetTime(keys[middle])) <= time)
			{
				low = middle;
			}
			else
			{
				high = middle - 1;
			}
		}
		return low;
	}

	// A key's slopes in one component, (In, Out), in double. An automatic slope is 0 at the first and the last key, and at
	// an interior key the slope from the key before to the key after (Catmull-Rom on uneven times), or 0 when those two lie
	// at one time. (0, 0) for a key index past the keys.
	template<CurveKeyType TKey>
	[[nodiscard]] std::pair<double, double> GetCurveKeySlopes(std::span<TKey const> keys, uint32_t key, uint32_t component)
	{
		using Traits = CurveKeyTraits<TKey>;
		if (key >= keys.size())
		{
			return {0.0, 0.0};
		}
		double automatic = 0.0;
		if (key > 0 && key + 1 < keys.size())
		{
			double const span = static_cast<double>(Traits::GetTime(keys[key + 1])) - static_cast<double>(Traits::GetTime(keys[key - 1]));
			if (span > 0.0)
			{
				automatic = (static_cast<double>(Traits::GetValue(keys[key + 1], component)) -
				             static_cast<double>(Traits::GetValue(keys[key - 1], component))) /
				            span;
			}
		}
		std::optional<float> const in = Traits::GetIn(keys[key], component);
		std::optional<float> const out = Traits::GetOut(keys[key], component);
		return {in ? static_cast<double>(*in) : automatic, out ? static_cast<double>(*out) : automatic};
	}

	// The keys' value at a time, per component (values.size() components, at most 4), in double: before the first key its
	// value, from the last key's time on the last key's; between keys the segment's interpolation. Keys at one time make a
	// jump: the segment before them ends at the first, the one after starts at the last. A NaN time evaluates the first
	// segment at NaN. The keys are in time order and not empty. Allocates nothing.
	template<CurveKeyType TKey>
	void EvaluateCurveKeys(std::span<TKey const> keys, double time, std::span<double> values)
	{
		using Traits = CurveKeyTraits<TKey>;
		auto const copy = [&values](TKey const& source)
		{
			for (uint32_t copied = 0; copied < values.size(); copied++)
			{
				values[copied] = static_cast<double>(Traits::GetValue(source, copied));
			}
		};
		if (keys.size() == 1 || time < static_cast<double>(Traits::GetTime(keys.front())))
		{
			copy(keys.front());
			return;
		}
		uint32_t const index = FindCurveSegment(keys, time);
		if (index + 1 >= keys.size())
		{
			copy(keys.back());
			return;
		}
		TKey const& from = keys[index];
		TKey const& to = keys[index + 1];
		double const start = Traits::GetTime(from);
		double const span = static_cast<double>(Traits::GetTime(to)) - start;
		double const s = (time - start) / span;
		switch (Traits::GetInterpolation(from))
		{
			case CurveInterpolation::Constant:
				copy(from);
				break;
			case CurveInterpolation::Linear:
				for (uint32_t component = 0; component < values.size(); component++)
				{
					double const first = Traits::GetValue(from, component);
					values[component] = first + (static_cast<double>(Traits::GetValue(to, component)) - first) * s;
				}
				break;
			case CurveInterpolation::Cubic:
			{
				double const s2 = s * s;
				double const s3 = s2 * s;
				double const h00 = 2.0 * s3 - 3.0 * s2 + 1.0;
				double const h10 = s3 - 2.0 * s2 + s;
				double const h01 = -2.0 * s3 + 3.0 * s2;
				double const h11 = s3 - s2;
				for (uint32_t component = 0; component < values.size(); component++)
				{
					double const outgoing = GetCurveKeySlopes(keys, index, component).second;
					double const incoming = GetCurveKeySlopes(keys, index + 1, component).first;
					values[component] = h00 * static_cast<double>(Traits::GetValue(from, component)) + h10 * span * outgoing +
					                    h01 * static_cast<double>(Traits::GetValue(to, component)) + h11 * span * incoming;
				}
				break;
			}
		}
	}
}
