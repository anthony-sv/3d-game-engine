using System;
using System.Globalization;
using System.Runtime.InteropServices;

namespace Strada;

/// <summary>A 2D vector of single-precision components, laid out like the engine's vectors.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct Vector2 : IEquatable<Vector2>
{
	/// <summary>The X component.</summary>
	public float X;
	/// <summary>The Y component.</summary>
	public float Y;

	/// <summary>Creates a vector from its components.</summary>
	public Vector2(float x, float y)
	{
		X = x;
		Y = y;
	}

	/// <summary>Creates a vector with both components set to <paramref name="scalar"/>.</summary>
	public Vector2(float scalar)
		: this(scalar, scalar)
	{
	}

	/// <summary>(0, 0).</summary>
	public static Vector2 Zero => new(0.0f);
	/// <summary>(1, 1).</summary>
	public static Vector2 One => new(1.0f);

	/// <summary>The Euclidean length.</summary>
	public readonly float Length => MathF.Sqrt(LengthSquared);

	/// <summary>The squared length (cheaper than <see cref="Length"/> for comparisons).</summary>
	public readonly float LengthSquared => (X * X) + (Y * Y);

	/// <summary>This vector scaled to length 1, or zero when its length is zero.</summary>
	public readonly Vector2 Normalized
	{
		get
		{
			float length = Length;
			return length > 0.0f ? this / length : Zero;
		}
	}

	/// <summary>The dot product of two vectors.</summary>
	public static float Dot(Vector2 a, Vector2 b) => (a.X * b.X) + (a.Y * b.Y);

	/// <summary>The distance between two points.</summary>
	public static float Distance(Vector2 a, Vector2 b) => (a - b).Length;

	/// <summary>Linear interpolation from <paramref name="a"/> (t = 0) to <paramref name="b"/> (t = 1); t is not clamped.</summary>
	public static Vector2 Lerp(Vector2 a, Vector2 b, float t) => a + ((b - a) * t);

	/// <summary>Component-wise sum.</summary>
	public static Vector2 operator +(Vector2 a, Vector2 b) => new(a.X + b.X, a.Y + b.Y);

	/// <summary>Component-wise difference.</summary>
	public static Vector2 operator -(Vector2 a, Vector2 b) => new(a.X - b.X, a.Y - b.Y);

	/// <summary>Negation.</summary>
	public static Vector2 operator -(Vector2 value) => new(-value.X, -value.Y);

	/// <summary>Component-wise product.</summary>
	public static Vector2 operator *(Vector2 a, Vector2 b) => new(a.X * b.X, a.Y * b.Y);

	/// <summary>Scales both components.</summary>
	public static Vector2 operator *(Vector2 value, float scalar) => new(value.X * scalar, value.Y * scalar);

	/// <summary>Scales both components.</summary>
	public static Vector2 operator *(float scalar, Vector2 value) => value * scalar;

	/// <summary>Divides both components.</summary>
	public static Vector2 operator /(Vector2 value, float scalar) => new(value.X / scalar, value.Y / scalar);

	/// <summary>Exact component-wise equality.</summary>
	public static bool operator ==(Vector2 a, Vector2 b) => a.Equals(b);

	/// <summary>Exact component-wise inequality.</summary>
	public static bool operator !=(Vector2 a, Vector2 b) => !a.Equals(b);

	/// <summary>Exact component-wise equality.</summary>
	public readonly bool Equals(Vector2 other) => X.Equals(other.X) && Y.Equals(other.Y);

	/// <inheritdoc/>
	public override readonly bool Equals(object? obj) => obj is Vector2 other && Equals(other);

	/// <inheritdoc/>
	public override readonly int GetHashCode() => HashCode.Combine(X, Y);

	/// <summary>"(X, Y)" in the invariant culture.</summary>
	public override readonly string ToString() => string.Create(CultureInfo.InvariantCulture, $"({X}, {Y})");
}
