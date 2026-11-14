using System;

namespace Strada;

// How the segment from a curve key to the next one interpolates: the engine's CurveInterpolation (Strada/Math/Curve.h), with its
// names in its order (timeline settings and curve assets name them).
internal enum CurveInterpolation : byte
{
	Cubic,
	Linear,
	Constant,
}

// What a curve does before its first key and after its last: the engine's CurveExtrapolation, with its names in its order.
internal enum CurveExtrapolation : byte
{
	Hold,
	Loop,
	PingPong,
	Linear,
}

// The scripting runtime's curve evaluator: the engine's curve semantics (Strada/Math/Curve.h, Docs/Architecture.md section 6.5)
// in managed code, so that graphs evaluate curves per sample without crossing into native code or allocating. Timeline tracks
// (TimelineTrack) and curve assets (CurveDefinition) evaluate through it; Tests/Data/Curves/Differential.json holds it and the
// engine's evaluator together (StradaTests and ScriptCoreTests evaluate its cases to the same values). Keys are in time order
// and given as arrays: their times, and per key Components numbers of values and of slopes, which ResolveSlopes resolves once.
internal static class CurveMath
{
	// The largest key index whose time is at most time; 0 when the first key's time is not (a time before every key, or NaN).
	internal static int LastKeyAtOrBefore(ReadOnlySpan<double> times, double time)
	{
		int low = 0;
		int high = times.Length - 1;
		while (low < high)
		{
			int middle = low + ((high - low + 1) / 2);
			if (times[middle] <= time)
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

	// The slopes with those given as NaN made automatic: 0 at the first and last keys; at an interior key the slope from the key
	// before to the key after (Catmull-Rom on uneven times), or 0 when they are at one time.
	internal static double[] ResolveSlopes(ReadOnlySpan<double> times, ReadOnlySpan<double> values, int components, ReadOnlySpan<double> slopes)
	{
		int count = times.Length;
		double[] resolved = new double[slopes.Length];
		for (int key = 0; key < count; key++)
		{
			for (int component = 0; component < components; component++)
			{
				int index = (key * components) + component;
				if (!double.IsNaN(slopes[index]))
				{
					resolved[index] = slopes[index];
				}
				else if (key > 0 && key < count - 1 && times[key + 1] - times[key - 1] > 0.0)
				{
					resolved[index] = (values[index + components] - values[index - components]) / (times[key + 1] - times[key - 1]);
				}
			}
		}
		return resolved;
	}

	// The keys' value at a time, value.Length components per key (the keys are not empty): before the first key its value,
	// from the last key's time on the last's; between keys the segment's interpolation, per component. Keys at one time make a
	// jump: the segment before them ends at the first, the one after starts at the last. Allocates nothing.
	internal static void Evaluate(ReadOnlySpan<double> times, ReadOnlySpan<double> values, ReadOnlySpan<double> inSlopes,
		ReadOnlySpan<double> outSlopes, ReadOnlySpan<CurveInterpolation> interpolations, double time, Span<double> value)
	{
		int count = times.Length;
		int components = value.Length;
		if (count == 1 || time < times[0])
		{
			values[..components].CopyTo(value);
			return;
		}
		int key = LastKeyAtOrBefore(times, time);
		if (key >= count - 1)
		{
			values.Slice((count - 1) * components, components).CopyTo(value);
			return;
		}
		double span = times[key + 1] - times[key];
		double s = (time - times[key]) / span;
		int from = key * components;
		int to = from + components;
		switch (interpolations[key])
		{
			case CurveInterpolation.Constant:
				values.Slice(from, components).CopyTo(value);
				break;
			case CurveInterpolation.Linear:
				for (int component = 0; component < components; component++)
				{
					value[component] = values[from + component] + ((values[to + component] - values[from + component]) * s);
				}
				break;
			default:
				double s2 = s * s;
				double s3 = s2 * s;
				double h00 = (2.0 * s3) - (3.0 * s2) + 1.0;
				double h10 = s3 - (2.0 * s2) + s;
				double h01 = (-2.0 * s3) + (3.0 * s2);
				double h11 = s3 - s2;
				for (int component = 0; component < components; component++)
				{
					value[component] = (h00 * values[from + component]) + (h10 * span * outSlopes[from + component]) +
						(h01 * values[to + component]) + (h11 * span * inSlopes[to + component]);
				}
				break;
		}
	}
}

// A scalar curve: the data of a curve asset (the engine's Curve, Strada/Math/Curve.h), as the runtime evaluates it. Immutable.
internal sealed class CurveDefinition
{
	// Slopes given as NaN are automatic.
	internal CurveDefinition(double[] times, double[] values, double[] inSlopes, double[] outSlopes, CurveInterpolation[] interpolations,
		CurveExtrapolation preExtrapolation, CurveExtrapolation postExtrapolation)
	{
		Times = times;
		Values = values;
		Interpolations = interpolations;
		InSlopes = CurveMath.ResolveSlopes(times, values, 1, inSlopes);
		OutSlopes = CurveMath.ResolveSlopes(times, values, 1, outSlopes);
		PreExtrapolation = preExtrapolation;
		PostExtrapolation = postExtrapolation;
	}

	// A curve without keys, which evaluates to 0.
	internal static CurveDefinition Empty { get; } = new([], [], [], [], [], CurveExtrapolation.Hold, CurveExtrapolation.Hold);

	internal int KeyCount => Times.Length;

	// Non-decreasing.
	internal double[] Times { get; }

	internal double[] Values { get; }

	// Resolved.
	internal double[] InSlopes { get; }

	internal double[] OutSlopes { get; }

	internal CurveInterpolation[] Interpolations { get; }

	internal CurveExtrapolation PreExtrapolation { get; }

	internal CurveExtrapolation PostExtrapolation { get; }

	// The curve's value at a time, as the engine's Curve::Evaluate gives it (its comment states the rules): no key, 0; the keys'
	// segments from the first key's time to the last's; the extrapolations outside; NaN for a NaN time; computed in double and
	// rounded to float once. Allocates nothing.
	internal float Evaluate(double time)
	{
		int count = KeyCount;
		if (count == 0)
		{
			return 0.0f;
		}
		if (double.IsNaN(time))
		{
			return float.NaN;
		}
		double first = Times[0];
		double last = Times[count - 1];
		if (time < first || time > last)
		{
			bool before = time < first;
			int end = before ? 0 : count - 1;
			double period = last - first;
			CurveExtrapolation extrapolation = before ? PreExtrapolation : PostExtrapolation;
			switch (extrapolation)
			{
				case CurveExtrapolation.Hold:
					return (float)Values[end];
				case CurveExtrapolation.Loop:
				case CurveExtrapolation.PingPong:
					if (!(period > 0.0))
					{
						return (float)Values[end];
					}
					if (!double.IsFinite(time))
					{
						return float.NaN;
					}
					time = MapIntoKeys(time, first, period, extrapolation);
					break;
				default:
					double slope = before ? StartSlope() : EndSlope();
					if (slope == 0.0)
					{
						return (float)Values[end];
					}
					return (float)(Values[end] + (slope * (time - Times[end])));
			}
		}
		Span<double> value = stackalloc double[1];
		CurveMath.Evaluate(Times, Values, InSlopes, OutSlopes, Interpolations, time, value);
		return (float)value[0];
	}

	// A finite time outside the keys mapped into them by Loop or PingPong (period > 0); % on doubles is the exact fmod the
	// engine uses.
	private static double MapIntoKeys(double time, double first, double period, CurveExtrapolation extrapolation)
	{
		double offset = time - first;
		if (extrapolation == CurveExtrapolation.Loop)
		{
			double loopPhase = offset % period;
			if (loopPhase < 0.0)
			{
				loopPhase += period;
			}
			return first + loopPhase;
		}
		double cycle = 2.0 * period;
		double phase = offset % cycle;
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

	// The slope the curve leaves its first time with: that of the segment from the last key at that time.
	private double StartSlope()
	{
		int count = KeyCount;
		int start = 0;
		while (start + 1 < count && Times[start + 1] == Times[0])
		{
			start++;
		}
		return start + 1 >= count ? 0.0 : SegmentSlope(start, atStart: true);
	}

	// The slope the curve reaches its last time with: that of the segment to the first key at that time.
	private double EndSlope()
	{
		int end = KeyCount - 1;
		while (end > 0 && Times[end - 1] == Times[KeyCount - 1])
		{
			end--;
		}
		return end == 0 ? 0.0 : SegmentSlope(end - 1, atStart: false);
	}

	// The slope of the segment from a key to the next at its start or its end.
	private double SegmentSlope(int key, bool atStart) => Interpolations[key] switch
	{
		CurveInterpolation.Constant => 0.0,
		CurveInterpolation.Linear => (Values[key + 1] - Values[key]) / (Times[key + 1] - Times[key]),
		_ => atStart ? OutSlopes[key] : InSlopes[key + 1],
	};
}
