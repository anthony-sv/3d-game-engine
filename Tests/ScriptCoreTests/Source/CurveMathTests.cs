using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The scripting runtime's curve evaluator (CurveMath, CurveDefinition): the engine's curve semantics in managed code, held to
// the engine's evaluator by Tests/Data/Curves/Differential.json, which StradaTests evaluates too (Math/CurveTests.cpp).
public sealed class CurveMathTests
{
	private static double ReadNumber(JsonElement element) => element.ValueKind switch
	{
		JsonValueKind.Number => element.GetDouble(),
		JsonValueKind.String => element.GetString() switch
		{
			"NaN" => double.NaN,
			"Infinity" => double.PositiveInfinity,
			"-Infinity" => double.NegativeInfinity,
			{ } text => throw new InvalidDataException($"not a number: {text}"),
			null => throw new InvalidDataException("not a number"),
		},
		_ => throw new InvalidDataException($"not a number: {element}"),
	};

	private static bool ReadBool(JsonElement key, string name) => key.TryGetProperty(name, out JsonElement value) && value.GetBoolean();

	// A curve as a .scurve file's content holds it, read the way the engine's field table reads it (missing fields take their
	// defaults; numbers become floats).
	private static CurveDefinition ReadCurve(JsonElement curve)
	{
		List<double> times = [];
		List<double> values = [];
		List<double> inSlopes = [];
		List<double> outSlopes = [];
		List<CurveInterpolation> interpolations = [];
		foreach (JsonElement key in curve.GetProperty("Keys").EnumerateArray())
		{
			times.Add((float)key.GetProperty("Time").GetDouble());
			values.Add((float)key.GetProperty("Value").GetDouble());
			interpolations.Add(key.TryGetProperty("Interpolation", out JsonElement interpolation)
				? Enum.Parse<CurveInterpolation>(interpolation.GetString()!)
				: CurveInterpolation.Cubic);
			inSlopes.Add(ReadBool(key, "HasIn") ? (float)key.GetProperty("In").GetDouble() : double.NaN);
			outSlopes.Add(ReadBool(key, "HasOut") ? (float)key.GetProperty("Out").GetDouble() : double.NaN);
		}
		return new CurveDefinition([.. times], [.. values], [.. inSlopes], [.. outSlopes], [.. interpolations],
			Enum.Parse<CurveExtrapolation>(curve.GetProperty("PreExtrapolation").GetString()!),
			Enum.Parse<CurveExtrapolation>(curve.GetProperty("PostExtrapolation").GetString()!));
	}

	private static CurveDefinition MakeCurve(CurveExtrapolation before, CurveExtrapolation after, params (double Time, double Value, CurveInterpolation Interpolation)[] keys)
	{
		double[] times = new double[keys.Length];
		double[] values = new double[keys.Length];
		double[] automatic = new double[keys.Length];
		CurveInterpolation[] interpolations = new CurveInterpolation[keys.Length];
		for (int index = 0; index < keys.Length; index++)
		{
			times[index] = keys[index].Time;
			values[index] = keys[index].Value;
			automatic[index] = double.NaN;
			interpolations[index] = keys[index].Interpolation;
		}
		return new CurveDefinition(times, values, automatic, [.. automatic], interpolations, before, after);
	}

	[Fact]
	public void DifferentialCasesEvaluateAsTheEngineDoes()
	{
		string path = Path.Combine(AppContext.BaseDirectory, "Curves", "Differential.json");
		using JsonDocument document = JsonDocument.Parse(File.ReadAllText(path));
		double tolerance = document.RootElement.GetProperty("Tolerance").GetDouble();
		int samples = 0;
		List<string> failures = [];
		foreach (JsonElement differential in document.RootElement.GetProperty("Cases").EnumerateArray())
		{
			string name = differential.GetProperty("Name").GetString()!;
			CurveDefinition curve = ReadCurve(differential.GetProperty("Curve"));
			foreach (JsonElement sample in differential.GetProperty("Samples").EnumerateArray())
			{
				double time = ReadNumber(sample.GetProperty("Time"));
				float expected = (float)ReadNumber(sample.GetProperty("Value"));
				float value = curve.Evaluate(time);
				bool same = float.IsNaN(expected) ? float.IsNaN(value)
					: float.IsInfinity(expected) ? value == expected
					: Math.Abs((double)value - expected) <= tolerance * Math.Max(1.0, Math.Abs((double)expected));
				if (!same)
				{
					failures.Add(FormattableString.Invariant($"{name} at {time}: {value}, expected {expected}"));
				}
				samples++;
			}
		}
		Assert.Empty(failures);
		Assert.True(samples >= 100, $"only {samples} samples");
	}

	[Fact]
	public void ExtrapolationsHoldLoopPingPongAndGoOnAlongTheSlope()
	{
		CurveDefinition triangle = MakeCurve(CurveExtrapolation.Loop, CurveExtrapolation.Loop, (0, 0, CurveInterpolation.Linear),
			(1, 1, CurveInterpolation.Linear), (2, 0, CurveInterpolation.Cubic));
		Assert.Equal(0.5f, triangle.Evaluate(2.5));
		Assert.Equal(0.5f, triangle.Evaluate(-0.5));
		Assert.Equal(0.25f, triangle.Evaluate(-3.75));
		CurveDefinition ramp = MakeCurve(CurveExtrapolation.PingPong, CurveExtrapolation.Linear, (1, 10, CurveInterpolation.Linear),
			(3, 20, CurveInterpolation.Cubic));
		Assert.Equal(15.0f, ramp.Evaluate(0.0));
		Assert.Equal(20.0f, ramp.Evaluate(-1.0));
		Assert.Equal(25.0f, ramp.Evaluate(4.0));
		Assert.Equal(float.PositiveInfinity, ramp.Evaluate(double.PositiveInfinity));
		Assert.True(float.IsNaN(ramp.Evaluate(double.NegativeInfinity)));
		Assert.True(float.IsNaN(ramp.Evaluate(double.NaN)));
		// Hold is the end keys' values; no key is 0 everywhere, NaN included.
		CurveDefinition held = MakeCurve(CurveExtrapolation.Hold, CurveExtrapolation.Hold, (0, 1, CurveInterpolation.Constant),
			(1, 3, CurveInterpolation.Cubic));
		Assert.Equal(1.0f, held.Evaluate(double.NegativeInfinity));
		Assert.Equal(3.0f, held.Evaluate(5.0));
		Assert.Equal(0.0f, CurveDefinition.Empty.Evaluate(double.NaN));
	}

	[Fact]
	public void TheEnumsNameTheEnginesValuesInItsOrder()
	{
		// Strada/Asset/CurveAsset.h's EnumTraits: timeline settings and curve files carry these names.
		Assert.Equal(["Cubic", "Linear", "Constant"], Enum.GetNames<CurveInterpolation>());
		Assert.Equal(["Hold", "Loop", "PingPong", "Linear"], Enum.GetNames<CurveExtrapolation>());
	}

	[Fact]
	public void EvaluatingAllocatesNothing()
	{
		CurveDefinition curve = MakeCurve(CurveExtrapolation.PingPong, CurveExtrapolation.Loop, (0, 0, CurveInterpolation.Cubic),
			(0.5, 2, CurveInterpolation.Linear), (2, 1, CurveInterpolation.Cubic));
		float sum = 0.0f;
		sum += curve.Evaluate(0.25);
		long before = GC.GetAllocatedBytesForCurrentThread();
		for (int sample = -100; sample < 100; sample++)
		{
			sum += curve.Evaluate(sample * 0.037);
		}
		Assert.Equal(before, GC.GetAllocatedBytesForCurrentThread());
		Assert.True(float.IsFinite(sum));
	}
}
