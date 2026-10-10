using System;
using System.Globalization;
using System.Runtime.InteropServices;

namespace Strada;

/// <summary>A linear RGBA color with single-precision components (1 is full intensity; lights may exceed it).</summary>
[StructLayout(LayoutKind.Sequential)]
public struct Color : IEquatable<Color>
{
	/// <summary>The red component.</summary>
	public float R;
	/// <summary>The green component.</summary>
	public float G;
	/// <summary>The blue component.</summary>
	public float B;
	/// <summary>The alpha (opacity) component.</summary>
	public float A;

	/// <summary>Creates a color from its components.</summary>
	public Color(float r, float g, float b, float a = 1.0f)
	{
		R = r;
		G = g;
		B = b;
		A = a;
	}

	/// <summary>Opaque white.</summary>
	public static Color White => new(1.0f, 1.0f, 1.0f);
	/// <summary>Opaque black.</summary>
	public static Color Black => new(0.0f, 0.0f, 0.0f);
	/// <summary>Opaque red.</summary>
	public static Color Red => new(1.0f, 0.0f, 0.0f);
	/// <summary>Opaque green.</summary>
	public static Color Green => new(0.0f, 1.0f, 0.0f);
	/// <summary>Opaque blue.</summary>
	public static Color Blue => new(0.0f, 0.0f, 1.0f);
	/// <summary>Fully transparent black.</summary>
	public static Color Clear => new(0.0f, 0.0f, 0.0f, 0.0f);

	/// <summary>Linear interpolation from <paramref name="a"/> (t = 0) to <paramref name="b"/> (t = 1); t is not clamped.</summary>
	public static Color Lerp(Color a, Color b, float t) =>
		new(a.R + ((b.R - a.R) * t), a.G + ((b.G - a.G) * t), a.B + ((b.B - a.B) * t), a.A + ((b.A - a.A) * t));

	/// <summary>The components as a vector (R, G, B, A).</summary>
	public static explicit operator Vector4(Color color) => new(color.R, color.G, color.B, color.A);

	/// <summary>A color from a vector (X, Y, Z, W as R, G, B, A).</summary>
	public static explicit operator Color(Vector4 vector) => new(vector.X, vector.Y, vector.Z, vector.W);

	/// <summary>Exact component-wise equality.</summary>
	public static bool operator ==(Color a, Color b) => a.Equals(b);

	/// <summary>Exact component-wise inequality.</summary>
	public static bool operator !=(Color a, Color b) => !a.Equals(b);

	/// <summary>Exact component-wise equality.</summary>
	public readonly bool Equals(Color other) => R.Equals(other.R) && G.Equals(other.G) && B.Equals(other.B) && A.Equals(other.A);

	/// <inheritdoc/>
	public override readonly bool Equals(object? obj) => obj is Color other && Equals(other);

	/// <inheritdoc/>
	public override readonly int GetHashCode() => HashCode.Combine(R, G, B, A);

	/// <summary>"(R, G, B, A)" in the invariant culture.</summary>
	public override readonly string ToString() => string.Create(CultureInfo.InvariantCulture, $"({R}, {G}, {B}, {A})");
}
