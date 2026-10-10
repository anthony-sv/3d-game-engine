using System;

namespace Strada;

/// <summary>Single-precision math helpers for games.</summary>
public static class Mathf
{
	/// <summary>The ratio of a circle's circumference to its diameter.</summary>
	public const float PI = MathF.PI;

	/// <summary>Multiply degrees by this to get radians.</summary>
	public const float Deg2Rad = MathF.PI / 180.0f;

	/// <summary>Multiply radians by this to get degrees.</summary>
	public const float Rad2Deg = 180.0f / MathF.PI;

	/// <summary>The smallest positive float.</summary>
	public const float Epsilon = float.Epsilon;

	/// <summary>The absolute value.</summary>
	public static float Abs(float value) => MathF.Abs(value);

	/// <summary>The smaller value.</summary>
	public static float Min(float a, float b) => MathF.Min(a, b);

	/// <summary>The larger value.</summary>
	public static float Max(float a, float b) => MathF.Max(a, b);

	/// <summary>The value limited to [min, max].</summary>
	public static float Clamp(float value, float min, float max) => value < min ? min : value > max ? max : value;

	/// <summary>The value limited to [0, 1].</summary>
	public static float Clamp01(float value) => Clamp(value, 0.0f, 1.0f);

	/// <summary>Linear interpolation from <paramref name="a"/> to <paramref name="b"/>, with t clamped to [0, 1].</summary>
	public static float Lerp(float a, float b, float t) => a + ((b - a) * Clamp01(t));

	/// <summary>Linear interpolation from <paramref name="a"/> to <paramref name="b"/> without clamping t.</summary>
	public static float LerpUnclamped(float a, float b, float t) => a + ((b - a) * t);

	/// <summary>Where <paramref name="value"/> lies between <paramref name="a"/> (0) and <paramref name="b"/> (1),
	/// clamped; 0 when they are equal.</summary>
	public static float InverseLerp(float a, float b, float value) => a != b ? Clamp01((value - a) / (b - a)) : 0.0f;

	/// <summary>Hermite interpolation: eases in and out between <paramref name="from"/> and <paramref name="to"/>.</summary>
	public static float SmoothStep(float from, float to, float t)
	{
		t = Clamp01(t);
		return LerpUnclamped(from, to, t * t * (3.0f - (2.0f * t)));
	}

	/// <summary>Moves <paramref name="current"/> towards <paramref name="target"/> by at most
	/// <paramref name="maxDelta"/>.</summary>
	public static float MoveTowards(float current, float target, float maxDelta) =>
		MathF.Abs(target - current) <= maxDelta ? target : current + (MathF.Sign(target - current) * maxDelta);

	/// <summary>The shortest difference between two angles in degrees, in [-180, 180).</summary>
	public static float DeltaAngle(float current, float target)
	{
		float delta = Repeat(target - current, 360.0f);
		return delta >= 180.0f ? delta - 360.0f : delta;
	}

	/// <summary>Wraps the value into [0, length).</summary>
	public static float Repeat(float value, float length) => Clamp(value - (MathF.Floor(value / length) * length), 0.0f, length);

	/// <summary>Bounces the value between 0 and <paramref name="length"/>.</summary>
	public static float PingPong(float value, float length)
	{
		value = Repeat(value, length * 2.0f);
		return length - MathF.Abs(value - length);
	}

	/// <summary>Whether two values are equal within a small tolerance relative to their size.</summary>
	public static bool Approximately(float a, float b) =>
		MathF.Abs(b - a) < MathF.Max(1e-6f * MathF.Max(MathF.Abs(a), MathF.Abs(b)), Epsilon * 8.0f);

	/// <summary>-1, 0 or 1.</summary>
	public static float Sign(float value) => MathF.Sign(value);

	/// <summary>The square root.</summary>
	public static float Sqrt(float value) => MathF.Sqrt(value);

	/// <summary><paramref name="value"/> raised to <paramref name="power"/>.</summary>
	public static float Pow(float value, float power) => MathF.Pow(value, power);

	/// <summary>e raised to the value.</summary>
	public static float Exp(float value) => MathF.Exp(value);

	/// <summary>The natural logarithm.</summary>
	public static float Log(float value) => MathF.Log(value);

	/// <summary>The largest whole number not above the value.</summary>
	public static float Floor(float value) => MathF.Floor(value);

	/// <summary>The smallest whole number not below the value.</summary>
	public static float Ceil(float value) => MathF.Ceiling(value);

	/// <summary>The nearest whole number (halves to even).</summary>
	public static float Round(float value) => MathF.Round(value);

	/// <summary>The sine of an angle in radians.</summary>
	public static float Sin(float radians) => MathF.Sin(radians);

	/// <summary>The cosine of an angle in radians.</summary>
	public static float Cos(float radians) => MathF.Cos(radians);

	/// <summary>The tangent of an angle in radians.</summary>
	public static float Tan(float radians) => MathF.Tan(radians);

	/// <summary>The angle in radians whose sine is the value.</summary>
	public static float Asin(float value) => MathF.Asin(value);

	/// <summary>The angle in radians whose cosine is the value.</summary>
	public static float Acos(float value) => MathF.Acos(value);

	/// <summary>The angle in radians whose tangent is the value.</summary>
	public static float Atan(float value) => MathF.Atan(value);

	/// <summary>The angle in radians of the point (x, y) from the X axis.</summary>
	public static float Atan2(float y, float x) => MathF.Atan2(y, x);
}
