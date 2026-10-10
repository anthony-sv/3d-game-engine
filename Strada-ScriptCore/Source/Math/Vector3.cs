using System;
using System.Globalization;
using System.Runtime.InteropServices;

namespace Strada;

/// <summary>A 3D vector of single-precision components, laid out like the engine's vectors.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct Vector3 : IEquatable<Vector3>
{
	/// <summary>The X component.</summary>
	public float X;
	/// <summary>The Y component.</summary>
	public float Y;
	/// <summary>The Z component.</summary>
	public float Z;

	/// <summary>Creates a vector from its components.</summary>
	public Vector3(float x, float y, float z)
	{
		X = x;
		Y = y;
		Z = z;
	}

	/// <summary>Creates a vector with every component set to <paramref name="scalar"/>.</summary>
	public Vector3(float scalar)
		: this(scalar, scalar, scalar)
	{
	}

	/// <summary>Creates a vector from a 2D vector and a Z component.</summary>
	public Vector3(Vector2 xy, float z)
		: this(xy.X, xy.Y, z)
	{
	}

	/// <summary>(0, 0, 0).</summary>
	public static Vector3 Zero => new(0.0f);
	/// <summary>(1, 1, 1).</summary>
	public static Vector3 One => new(1.0f);
	/// <summary>(1, 0, 0), the engine's right direction.</summary>
	public static Vector3 Right => new(1.0f, 0.0f, 0.0f);
	/// <summary>(0, 1, 0), the engine's up direction.</summary>
	public static Vector3 Up => new(0.0f, 1.0f, 0.0f);
	/// <summary>(0, 0, -1), the engine's forward direction (cameras look down -Z).</summary>
	public static Vector3 Forward => new(0.0f, 0.0f, -1.0f);
	/// <summary>(-1, 0, 0).</summary>
	public static Vector3 Left => new(-1.0f, 0.0f, 0.0f);
	/// <summary>(0, -1, 0).</summary>
	public static Vector3 Down => new(0.0f, -1.0f, 0.0f);
	/// <summary>(0, 0, 1).</summary>
	public static Vector3 Back => new(0.0f, 0.0f, 1.0f);

	/// <summary>The X and Y components.</summary>
	public readonly Vector2 XY => new(X, Y);

	/// <summary>The Euclidean length.</summary>
	public readonly float Length => MathF.Sqrt(LengthSquared);

	/// <summary>The squared length (cheaper than <see cref="Length"/> for comparisons).</summary>
	public readonly float LengthSquared => (X * X) + (Y * Y) + (Z * Z);

	/// <summary>This vector scaled to length 1, or zero when its length is zero.</summary>
	public readonly Vector3 Normalized
	{
		get
		{
			float length = Length;
			return length > 0.0f ? this / length : Zero;
		}
	}

	/// <summary>The dot product of two vectors.</summary>
	public static float Dot(Vector3 a, Vector3 b) => (a.X * b.X) + (a.Y * b.Y) + (a.Z * b.Z);

	/// <summary>The cross product of two vectors (right-handed).</summary>
	public static Vector3 Cross(Vector3 a, Vector3 b) => new((a.Y * b.Z) - (a.Z * b.Y), (a.Z * b.X) - (a.X * b.Z), (a.X * b.Y) - (a.Y * b.X));

	/// <summary>The distance between two points.</summary>
	public static float Distance(Vector3 a, Vector3 b) => (a - b).Length;

	/// <summary>Linear interpolation from <paramref name="a"/> (t = 0) to <paramref name="b"/> (t = 1); t is not clamped.</summary>
	public static Vector3 Lerp(Vector3 a, Vector3 b, float t) => a + ((b - a) * t);

	/// <summary>The component-wise minimum.</summary>
	public static Vector3 Min(Vector3 a, Vector3 b) => new(MathF.Min(a.X, b.X), MathF.Min(a.Y, b.Y), MathF.Min(a.Z, b.Z));

	/// <summary>The component-wise maximum.</summary>
	public static Vector3 Max(Vector3 a, Vector3 b) => new(MathF.Max(a.X, b.X), MathF.Max(a.Y, b.Y), MathF.Max(a.Z, b.Z));

	/// <summary>Component-wise sum.</summary>
	public static Vector3 operator +(Vector3 a, Vector3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);

	/// <summary>Component-wise difference.</summary>
	public static Vector3 operator -(Vector3 a, Vector3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);

	/// <summary>Negation.</summary>
	public static Vector3 operator -(Vector3 value) => new(-value.X, -value.Y, -value.Z);

	/// <summary>Component-wise product.</summary>
	public static Vector3 operator *(Vector3 a, Vector3 b) => new(a.X * b.X, a.Y * b.Y, a.Z * b.Z);

	/// <summary>Scales every component.</summary>
	public static Vector3 operator *(Vector3 value, float scalar) => new(value.X * scalar, value.Y * scalar, value.Z * scalar);

	/// <summary>Scales every component.</summary>
	public static Vector3 operator *(float scalar, Vector3 value) => value * scalar;

	/// <summary>Component-wise quotient.</summary>
	public static Vector3 operator /(Vector3 a, Vector3 b) => new(a.X / b.X, a.Y / b.Y, a.Z / b.Z);

	/// <summary>Divides every component.</summary>
	public static Vector3 operator /(Vector3 value, float scalar) => new(value.X / scalar, value.Y / scalar, value.Z / scalar);

	/// <summary>Exact component-wise equality.</summary>
	public static bool operator ==(Vector3 a, Vector3 b) => a.Equals(b);

	/// <summary>Exact component-wise inequality.</summary>
	public static bool operator !=(Vector3 a, Vector3 b) => !a.Equals(b);

	/// <summary>Exact component-wise equality.</summary>
	public readonly bool Equals(Vector3 other) => X.Equals(other.X) && Y.Equals(other.Y) && Z.Equals(other.Z);

	/// <inheritdoc/>
	public override readonly bool Equals(object? obj) => obj is Vector3 other && Equals(other);

	/// <inheritdoc/>
	public override readonly int GetHashCode() => HashCode.Combine(X, Y, Z);

	/// <summary>"(X, Y, Z)" in the invariant culture.</summary>
	public override readonly string ToString() => string.Create(CultureInfo.InvariantCulture, $"({X}, {Y}, {Z})");
}
