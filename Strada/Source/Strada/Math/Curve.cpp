#include "stpch.h"

#include "Strada/Math/Curve.h"

#include <cmath>
#include <limits>

namespace Strada
{
	namespace
	{
		// The slope a curve leaves its first time with: that of the segment from the last key at that time.
		double GetStartSlope(std::span<CurveKey const> keys)
		{
			size_t start = 0;
			while (start + 1 < keys.size() && keys[start + 1].Time == keys.front().Time)
			{
				start++;
			}
			if (start + 1 >= keys.size())
			{
				return 0.0;
			}
			CurveKey const& from = keys[start];
			CurveKey const& to = keys[start + 1];
			switch (from.Interpolation)
			{
				case CurveInterpolation::Constant:
					return 0.0;
				case CurveInterpolation::Linear:
					return (static_cast<double>(to.Value) - static_cast<double>(from.Value)) /
					       (static_cast<double>(to.Time) - static_cast<double>(from.Time));
				case CurveInterpolation::Cubic:
					return GetCurveKeySlopes(keys, static_cast<uint32_t>(start), 0).second;
			}
			return 0.0;
		}

		// The slope a curve reaches its last time with: that of the segment to the first key at that time.
		double GetEndSlope(std::span<CurveKey const> keys)
		{
			size_t end = keys.size() - 1;
			while (end > 0 && keys[end - 1].Time == keys.back().Time)
			{
				end--;
			}
			if (end == 0)
			{
				return 0.0;
			}
			CurveKey const& from = keys[end - 1];
			CurveKey const& to = keys[end];
			switch (from.Interpolation)
			{
				case CurveInterpolation::Constant:
					return 0.0;
				case CurveInterpolation::Linear:
					return (static_cast<double>(to.Value) - static_cast<double>(from.Value)) /
					       (static_cast<double>(to.Time) - static_cast<double>(from.Time));
				case CurveInterpolation::Cubic:
					return GetCurveKeySlopes(keys, static_cast<uint32_t>(end), 0).first;
			}
			return 0.0;
		}

		// A finite time outside [first, first + period] mapped into it by Loop or PingPong (period > 0). fmod is exact, so the
		// managed evaluator (C#'s % on doubles) maps to the same number.
		double MapIntoKeys(double time, double first, double period, CurveExtrapolation extrapolation)
		{
			double const offset = time - first;
			if (extrapolation == CurveExtrapolation::Loop)
			{
				double phase = std::fmod(offset, period);
				if (phase < 0.0)
				{
					phase += period;
				}
				return first + phase;
			}
			double const cycle = 2.0 * period;
			double phase = std::fmod(offset, cycle);
			if (phase < 0.0)
			{
				phase += cycle;
			}
			if (phase > period)
			{
				phase = cycle - phase;
			}
			return first + phase;
		}
	}

	float Curve::Evaluate(double time) const
	{
		if (Keys.empty())
		{
			return 0.0f;
		}
		if (std::isnan(time))
		{
			return std::numeric_limits<float>::quiet_NaN();
		}
		std::span<CurveKey const> const keys(Keys);
		double const first = keys.front().Time;
		double const last = keys.back().Time;
		if (time < first || time > last)
		{
			bool const before = time < first;
			CurveKey const& end = before ? keys.front() : keys.back();
			double const period = last - first;
			switch (before ? PreExtrapolation : PostExtrapolation)
			{
				case CurveExtrapolation::Hold:
					return end.Value;
				case CurveExtrapolation::Loop:
				case CurveExtrapolation::PingPong:
					if (!(period > 0.0))
					{
						return end.Value;
					}
					if (!std::isfinite(time))
					{
						return std::numeric_limits<float>::quiet_NaN();
					}
					time = MapIntoKeys(time, first, period, before ? PreExtrapolation : PostExtrapolation);
					break;
				case CurveExtrapolation::Linear:
				{
					double const slope = before ? GetStartSlope(keys) : GetEndSlope(keys);
					if (slope == 0.0)
					{
						return end.Value;
					}
					return static_cast<float>(static_cast<double>(end.Value) + slope * (time - static_cast<double>(end.Time)));
				}
			}
		}
		double value = 0.0;
		EvaluateCurveKeys(keys, time, std::span<double>(&value, 1));
		return static_cast<float>(value);
	}

	std::pair<float, float> Curve::GetKeySlopes(uint32_t key) const
	{
		std::pair<double, double> const slopes = GetCurveKeySlopes(std::span<CurveKey const>(Keys), key, 0);
		return {static_cast<float>(slopes.first), static_cast<float>(slopes.second)};
	}

	Result<void> ValidateCurve(Curve const& curve)
	{
		if (curve.Keys.size() > MaxCurveKeys)
		{
			return MakeError("Keys: a curve has at most {} keys, not {}", MaxCurveKeys, curve.Keys.size());
		}
		for (size_t index = 0; index < curve.Keys.size(); index++)
		{
			CurveKey const& key = curve.Keys[index];
			if (!std::isfinite(key.Time) || !std::isfinite(key.Value) || !std::isfinite(key.In) || !std::isfinite(key.Out))
			{
				return MakeError("Keys[{}]: a key's time, value and slopes are finite numbers", index);
			}
			if (index > 0 && key.Time < curve.Keys[index - 1].Time)
			{
				return MakeError("Keys[{}].Time: keys are in time order: {} comes after {}", index, key.Time, curve.Keys[index - 1].Time);
			}
		}
		return {};
	}
}
