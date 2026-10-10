using System;

namespace Strada;

/// <summary>Random numbers for games. Seeding makes the sequence repeat (main thread use).</summary>
public static class Random
{
	private static System.Random s_Generator = new();

	/// <summary>Restarts the sequence from a seed, so the same calls give the same numbers.</summary>
	public static void Seed(int seed)
	{
		s_Generator = new System.Random(seed);
	}

	/// <summary>A number in [0, 1).</summary>
	public static float Value => s_Generator.NextSingle();

	/// <summary>A number in [min, max).</summary>
	public static float Range(float min, float max) => min + ((max - min) * s_Generator.NextSingle());

	/// <summary>A whole number in [min, max); <paramref name="min"/> when the range is empty.</summary>
	public static int Range(int min, int max) => max > min ? s_Generator.Next(min, max) : min;

	/// <summary>A point inside the sphere of radius 1, evenly distributed.</summary>
	public static Vector3 InsideUnitSphere => OnUnitSphere * MathF.Cbrt(Value);

	/// <summary>A point on the sphere of radius 1, evenly distributed.</summary>
	public static Vector3 OnUnitSphere
	{
		get
		{
			float z = Range(-1.0f, 1.0f);
			float angle = Range(0.0f, 2.0f * MathF.PI);
			float radius = MathF.Sqrt(1.0f - (z * z));
			return new Vector3(radius * MathF.Cos(angle), radius * MathF.Sin(angle), z);
		}
	}

	/// <summary>A rotation, evenly distributed over all orientations.</summary>
	public static Quaternion Rotation
	{
		get
		{
			// Shoemake's method.
			float u1 = Value;
			float u2 = Range(0.0f, 2.0f * MathF.PI);
			float u3 = Range(0.0f, 2.0f * MathF.PI);
			float a = MathF.Sqrt(1.0f - u1);
			float b = MathF.Sqrt(u1);
			return new Quaternion(a * MathF.Sin(u2), a * MathF.Cos(u2), b * MathF.Sin(u3), b * MathF.Cos(u3));
		}
	}
}
