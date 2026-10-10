using System;
using System.Globalization;
using System.Runtime.InteropServices;

namespace Strada;

/// <summary>A 4D vector of single-precision components, laid out like the engine's vectors.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct Vector4 : IEquatable<Vector4>
{
	/// <summary>The X component.</summary>
	public float X;
	/// <summary>The Y component.</summary>
	public float Y;
	/// <summary>The Z component.</summary>
	public float Z;
	/// <summary>The W component.</summary>
	public float W;

	/// <summary>Creates a vector from its components.</summary>
	public Vector4(float x, float y, float z, float w)
	{
		X = x;
		Y = y;
		Z = z;
		W = w;
	}

	/// <summary>Creates a vector with every component set to <paramref name="scalar"/>.</summary>
	public Vector4(float scalar)
		: this(scalar, scalar, scalar, scalar)
	{
	}

	/// <summary>Creates a vector from a 3D vector and a W component.</summary>
	public Vector4(Vector3 xyz, float w)
		: this(xyz.X, xyz.Y, xyz.Z, w)
	{
	}

	/// <summary>(0, 0, 0, 0).</summary>
	public static Vector4 Zero => new(0.0f);
	/// <summary>(1, 1, 1, 1).</summary>
	public static Vector4 One => new(1.0f);

	/// <summary>The X, Y and Z components.</summary>
	public readonly Vector3 XYZ => new(X, Y, Z);

	/// <summary>The Euclidean length.</summary>
	public readonly float Length => MathF.Sqrt(Dot(this, this));

	/// <summary>The dot product of two vectors.</summary>
	public static float Dot(Vector4 a, Vector4 b) => (a.X * b.X) + (a.Y * b.Y) + (a.Z * b.Z) + (a.W * b.W);

	/// <summary>Linear interpolation from <paramref name="a"/> (t = 0) to <paramref name="b"/> (t = 1); t is not clamped.</summary>
	public static Vector4 Lerp(Vector4 a, Vector4 b, float t) => a + ((b - a) * t);

	/// <summary>Component-wise sum.</summary>
	public static Vector4 operator +(Vector4 a, Vector4 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z, a.W + b.W);

	/// <summary>Component-wise difference.</summary>
	public static Vector4 operator -(Vector4 a, Vector4 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z, a.W - b.W);

	/// <summary>Negation.</summary>
	public static Vector4 operator -(Vector4 value) => new(-value.X, -value.Y, -value.Z, -value.W);

	/// <summary>Component-wise product.</summary>
	public static Vector4 operator *(Vector4 a, Vector4 b) => new(a.X * b.X, a.Y * b.Y, a.Z * b.Z, a.W * b.W);

	/// <summary>Scales every component.</summary>
	public static Vector4 operator *(Vector4 value, float scalar) => new(value.X * scalar, value.Y * scalar, value.Z * scalar, value.W * scalar);

	/// <summary>Scales every component.</summary>
	public static Vector4 operator *(float scalar, Vector4 value) => value * scalar;

	/// <summary>Divides every component.</summary>
	public static Vector4 operator /(Vector4 value, float scalar) => new(value.X / scalar, value.Y / scalar, value.Z / scalar, value.W / scalar);

	/// <summary>Exact component-wise equality.</summary>
	public static bool operator ==(Vector4 a, Vector4 b) => a.Equals(b);

	/// <summary>Exact component-wise inequality.</summary>
	public static bool operator !=(Vector4 a, Vector4 b) => !a.Equals(b);

	/// <summary>Exact component-wise equality.</summary>
	public readonly bool Equals(Vector4 other) => X.Equals(other.X) && Y.Equals(other.Y) && Z.Equals(other.Z) && W.Equals(other.W);

	/// <inheritdoc/>
	public override readonly bool Equals(object? obj) => obj is Vector4 other && Equals(other);

	/// <inheritdoc/>
	public override readonly int GetHashCode() => HashCode.Combine(X, Y, Z, W);

	/// <summary>"(X, Y, Z, W)" in the invariant culture.</summary>
	public override readonly string ToString() => string.Create(CultureInfo.InvariantCulture, $"({X}, {Y}, {Z}, {W})");
}
