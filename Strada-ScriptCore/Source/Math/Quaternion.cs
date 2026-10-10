using System;
using System.Globalization;
using System.Runtime.InteropServices;

namespace Strada;

/// <summary>A rotation stored as a unit quaternion (X, Y, Z, W), laid out like the engine's quaternions.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct Quaternion : IEquatable<Quaternion>
{
	/// <summary>The X component of the vector part.</summary>
	public float X;
	/// <summary>The Y component of the vector part.</summary>
	public float Y;
	/// <summary>The Z component of the vector part.</summary>
	public float Z;
	/// <summary>The scalar part.</summary>
	public float W;

	/// <summary>Creates a quaternion from its components.</summary>
	public Quaternion(float x, float y, float z, float w)
	{
		X = x;
		Y = y;
		Z = z;
		W = w;
	}

	/// <summary>The rotation that changes nothing.</summary>
	public static Quaternion Identity => new(0.0f, 0.0f, 0.0f, 1.0f);

	/// <summary>A rotation of <paramref name="degrees"/> around <paramref name="axis"/> (counterclockwise looking down the
	/// axis towards the origin). A zero axis gives the identity.</summary>
	public static Quaternion AngleAxis(float degrees, Vector3 axis)
	{
		Vector3 direction = axis.Normalized;
		if (direction == Vector3.Zero)
		{
			return Identity;
		}
		float halfAngle = degrees * (MathF.PI / 180.0f) * 0.5f;
		float sine = MathF.Sin(halfAngle);
		return new Quaternion(direction.X * sine, direction.Y * sine, direction.Z * sine, MathF.Cos(halfAngle));
	}

	/// <summary>The squared length; 1 for rotations.</summary>
	public readonly float LengthSquared => Dot(this, this);

	/// <summary>This quaternion scaled to length 1, or the identity when its length is zero.</summary>
	public readonly Quaternion Normalized
	{
		get
		{
			float length = MathF.Sqrt(LengthSquared);
			return length > 0.0f ? new Quaternion(X / length, Y / length, Z / length, W / length) : Identity;
		}
	}

	/// <summary>The opposite rotation (the conjugate scaled by the inverse squared length).</summary>
	public readonly Quaternion Inverse
	{
		get
		{
			float lengthSquared = LengthSquared;
			return lengthSquared > 0.0f
				? new Quaternion(-X / lengthSquared, -Y / lengthSquared, -Z / lengthSquared, W / lengthSquared)
				: Identity;
		}
	}

	/// <summary>The four-dimensional dot product of two quaternions.</summary>
	public static float Dot(Quaternion a, Quaternion b) => (a.X * b.X) + (a.Y * b.Y) + (a.Z * b.Z) + (a.W * b.W);

	/// <summary>Spherical interpolation along the shortest arc from <paramref name="a"/> (t = 0) to <paramref name="b"/>
	/// (t = 1); t is clamped to [0, 1].</summary>
	public static Quaternion Slerp(Quaternion a, Quaternion b, float t)
	{
		t = Math.Clamp(t, 0.0f, 1.0f);
		float cosine = Dot(a, b);
		if (cosine < 0.0f)
		{
			b = new Quaternion(-b.X, -b.Y, -b.Z, -b.W);
			cosine = -cosine;
		}
		float weightA;
		float weightB;
		if (cosine > 0.9995f)
		{
			// Nearly parallel: linear interpolation avoids dividing by a vanishing sine.
			weightA = 1.0f - t;
			weightB = t;
		}
		else
		{
			float angle = MathF.Acos(cosine);
			float sine = MathF.Sin(angle);
			weightA = MathF.Sin((1.0f - t) * angle) / sine;
			weightB = MathF.Sin(t * angle) / sine;
		}
		Quaternion result = new((weightA * a.X) + (weightB * b.X), (weightA * a.Y) + (weightB * b.Y), (weightA * a.Z) + (weightB * b.Z),
			(weightA * a.W) + (weightB * b.W));
		return result.Normalized;
	}

	/// <summary>The rotation <paramref name="b"/> followed by <paramref name="a"/>.</summary>
	public static Quaternion operator *(Quaternion a, Quaternion b)
	{
		return new Quaternion(
			(a.W * b.X) + (a.X * b.W) + (a.Y * b.Z) - (a.Z * b.Y),
			(a.W * b.Y) + (a.Y * b.W) + (a.Z * b.X) - (a.X * b.Z),
			(a.W * b.Z) + (a.Z * b.W) + (a.X * b.Y) - (a.Y * b.X),
			(a.W * b.W) - (a.X * b.X) - (a.Y * b.Y) - (a.Z * b.Z));
	}

	/// <summary>Rotates a vector.</summary>
	public static Vector3 operator *(Quaternion rotation, Vector3 vector)
	{
		Vector3 axis = new(rotation.X, rotation.Y, rotation.Z);
		Vector3 cross = Vector3.Cross(axis, vector);
		return vector + (2.0f * rotation.W * cross) + (2.0f * Vector3.Cross(axis, cross));
	}

	/// <summary>Exact component-wise equality.</summary>
	public static bool operator ==(Quaternion a, Quaternion b) => a.Equals(b);

	/// <summary>Exact component-wise inequality.</summary>
	public static bool operator !=(Quaternion a, Quaternion b) => !a.Equals(b);

	/// <summary>Exact component-wise equality (a rotation and its negation compare unequal).</summary>
	public readonly bool Equals(Quaternion other) => X.Equals(other.X) && Y.Equals(other.Y) && Z.Equals(other.Z) && W.Equals(other.W);

	/// <inheritdoc/>
	public override readonly bool Equals(object? obj) => obj is Quaternion other && Equals(other);

	/// <inheritdoc/>
	public override readonly int GetHashCode() => HashCode.Combine(X, Y, Z, W);

	/// <summary>"(X, Y, Z, W)" in the invariant culture.</summary>
	public override readonly string ToString() => string.Create(CultureInfo.InvariantCulture, $"({X}, {Y}, {Z}, {W})");
}
