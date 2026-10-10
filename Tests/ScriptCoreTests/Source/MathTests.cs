using System;
using System.Globalization;
using System.Threading;
using Xunit;

namespace Strada.ScriptCore.Tests;

public sealed class MathTests
{
	private const float Tolerance = 1e-5f;

	private static void AssertNear(Vector3 expected, Vector3 actual)
	{
		Assert.True(Vector3.Distance(expected, actual) < Tolerance, $"expected {expected}, got {actual}");
	}

	[Fact]
	public void VectorsCombineComponentWise()
	{
		Vector3 a = new(1.0f, 2.0f, 3.0f);
		Vector3 b = new(4.0f, 5.0f, 6.0f);
		Assert.Equal(new Vector3(5.0f, 7.0f, 9.0f), a + b);
		Assert.Equal(new Vector3(-3.0f, -3.0f, -3.0f), a - b);
		Assert.Equal(new Vector3(4.0f, 10.0f, 18.0f), a * b);
		Assert.Equal(new Vector3(2.0f, 4.0f, 6.0f), 2.0f * a);
		Assert.Equal(new Vector3(0.5f, 1.0f, 1.5f), a / 2.0f);
		Assert.Equal(32.0f, Vector3.Dot(a, b));
		Assert.Equal(new Vector3(-3.0f, 6.0f, -3.0f), Vector3.Cross(a, b));
		Assert.Equal(new Vector3(0.0f, 0.0f, 1.0f), Vector3.Cross(Vector3.Right, Vector3.Up));
		Assert.Equal(Vector3.Back, Vector3.Cross(Vector3.Right, Vector3.Up));
		Assert.Equal(Vector3.Zero, Vector3.Left + Vector3.Right + Vector3.Down + Vector3.Up + Vector3.Back + Vector3.Forward);
		Assert.Equal(new Vector3(2.5f, 3.5f, 4.5f), Vector3.Lerp(a, b, 0.5f));
		Assert.Equal(new Vector3(1.0f, 2.0f, 3.0f), Vector3.Min(a, b));
		Assert.Equal(5.0f, new Vector3(3.0f, 4.0f, 0.0f).Length);
		Assert.Equal(new Vector3(0.6f, 0.8f, 0.0f), new Vector3(3.0f, 4.0f, 0.0f).Normalized);
		Assert.Equal(Vector3.Zero, Vector3.Zero.Normalized);
		Assert.Equal(new Vector2(1.0f, 2.0f), new Vector3(1.0f, 2.0f, 3.0f).XY);
		Assert.Equal(new Vector3(1.0f, 2.0f, 3.0f), new Vector4(new Vector3(1.0f, 2.0f, 3.0f), 4.0f).XYZ);
		Assert.Equal(5.0f, Vector2.Distance(Vector2.Zero, new Vector2(3.0f, 4.0f)));
	}

	[Fact]
	public void TextUsesTheInvariantCulture()
	{
		CultureInfo culture = Thread.CurrentThread.CurrentCulture;
		try
		{
			Thread.CurrentThread.CurrentCulture = new CultureInfo("de-DE");
			Assert.Equal("(1.5, -2, 3.25)", new Vector3(1.5f, -2.0f, 3.25f).ToString());
			Assert.Equal("(0.5, 0.25, 1, 1)", new Color(0.5f, 0.25f, 1.0f).ToString());
			Assert.Equal("(0, 0, 0, 1)", Quaternion.Identity.ToString());
		}
		finally
		{
			Thread.CurrentThread.CurrentCulture = culture;
		}
	}

	[Fact]
	public void QuaternionsRotateLikeTheEngine()
	{
		Quaternion turn = Quaternion.AngleAxis(90.0f, Vector3.Up);
		// Counterclockwise looking down the axis: +X turns to -Z.
		AssertNear(new Vector3(0.0f, 0.0f, -1.0f), turn * Vector3.Right);
		AssertNear(Vector3.Right, Quaternion.Identity * Vector3.Right);
		AssertNear(Vector3.Right, turn.Inverse * (turn * Vector3.Right));
		Assert.Equal(Quaternion.Identity, Quaternion.AngleAxis(45.0f, Vector3.Zero));
		Assert.Equal(Quaternion.Identity, new Quaternion(0.0f, 0.0f, 0.0f, 0.0f).Normalized);

		// a * b applies b first.
		Quaternion tilt = Quaternion.AngleAxis(90.0f, Vector3.Right);
		AssertNear(turn * (tilt * Vector3.Up), (turn * tilt) * Vector3.Up);

		Quaternion half = Quaternion.Slerp(Quaternion.Identity, turn, 0.5f);
		AssertNear(Quaternion.AngleAxis(45.0f, Vector3.Up) * Vector3.Right, half * Vector3.Right);
		// t is clamped: past the end is the end.
		Assert.True(Quaternion.Dot(turn, Quaternion.Slerp(Quaternion.Identity, turn, 2.0f)) > 1.0f - Tolerance);
	}

	[Fact]
	public void MatricesComposeTransformsAndInvert()
	{
		Matrix4 transform = Matrix4.TRS(new Vector3(10.0f, 0.0f, 0.0f), Quaternion.AngleAxis(90.0f, Vector3.Up), new Vector3(2.0f));
		AssertNear(new Vector3(10.0f, 0.0f, -2.0f), transform.TransformPoint(Vector3.Right));
		AssertNear(new Vector3(0.0f, 0.0f, -2.0f), transform.TransformDirection(Vector3.Right));
		AssertNear(Quaternion.AngleAxis(90.0f, Vector3.Up) * Vector3.Forward, Matrix4.Rotation(Quaternion.AngleAxis(90.0f, Vector3.Up)).TransformDirection(Vector3.Forward));

		Matrix4? inverse = transform.Inverse;
		Assert.NotNull(inverse);
		AssertNear(Vector3.Right, inverse.Value.TransformPoint(transform.TransformPoint(Vector3.Right)));
		Matrix4 identity = transform * inverse.Value;
		for (int column = 0; column < 4; column++)
		{
			for (int row = 0; row < 4; row++)
			{
				Assert.True(MathF.Abs(identity[column, row] - (column == row ? 1.0f : 0.0f)) < Tolerance);
			}
		}
		Assert.Null(Matrix4.Scaling(new Vector3(1.0f, 0.0f, 1.0f)).Inverse);

		Matrix4 edited = Matrix4.Identity;
		edited[3, 1] = 5.0f;
		Assert.Equal(new Vector4(0.0f, 5.0f, 0.0f, 1.0f), edited.Column3);
		Assert.Throws<ArgumentOutOfRangeException>(() => edited[4, 0]);
	}

	[Fact]
	public void MathfHelpersClampWrapAndInterpolate()
	{
		Assert.Equal(1.0f, Mathf.Clamp(3.0f, -1.0f, 1.0f));
		Assert.Equal(0.0f, Mathf.Clamp01(-0.5f));
		Assert.Equal(10.0f, Mathf.Lerp(0.0f, 10.0f, 2.0f));
		Assert.Equal(20.0f, Mathf.LerpUnclamped(0.0f, 10.0f, 2.0f));
		Assert.Equal(0.25f, Mathf.InverseLerp(0.0f, 4.0f, 1.0f));
		Assert.Equal(0.0f, Mathf.InverseLerp(2.0f, 2.0f, 5.0f));
		Assert.Equal(0.5f, Mathf.SmoothStep(0.0f, 1.0f, 0.5f));
		Assert.Equal(3.0f, Mathf.MoveTowards(0.0f, 10.0f, 3.0f));
		Assert.Equal(10.0f, Mathf.MoveTowards(9.0f, 10.0f, 3.0f));
		Assert.Equal(-20.0f, Mathf.DeltaAngle(10.0f, 350.0f));
		Assert.Equal(1.0f, Mathf.Repeat(-3.0f, 4.0f));
		Assert.Equal(1.0f, Mathf.PingPong(3.0f, 2.0f));
		Assert.True(Mathf.Approximately(1.0f, 1.0f + 1e-7f));
		Assert.False(Mathf.Approximately(1.0f, 1.001f));
		Assert.Equal(180.0f, Mathf.PI * Mathf.Rad2Deg, 3);
	}

	[Fact]
	public void RandomRepeatsAfterSeedingAndStaysInRange()
	{
		Random.Seed(1234);
		float first = Random.Value;
		int whole = Random.Range(0, 10);
		Random.Seed(1234);
		Assert.Equal(first, Random.Value);
		Assert.Equal(whole, Random.Range(0, 10));

		for (int sample = 0; sample < 1000; sample++)
		{
			float value = Random.Range(-2.0f, 3.0f);
			Assert.InRange(value, -2.0f, 3.0f);
			Assert.InRange(Random.Range(5, 8), 5, 7);
			Assert.InRange(Random.OnUnitSphere.Length, 1.0f - Tolerance, 1.0f + Tolerance);
			Assert.True(Random.InsideUnitSphere.Length <= 1.0f + Tolerance);
			Assert.InRange(Random.Rotation.LengthSquared, 1.0f - 1e-4f, 1.0f + 1e-4f);
		}
		Assert.Equal(5, Random.Range(5, 5));
	}

	[Fact]
	public void ColorsAndHandlesCompareByValue()
	{
		Assert.Equal(new Color(0.5f, 0.5f, 0.5f, 1.0f), Color.Lerp(Color.Black, Color.White, 0.5f));
		Assert.Equal(new Vector4(1.0f, 0.0f, 0.0f, 1.0f), (Vector4)Color.Red);
		Assert.Equal(Color.Blue, (Color)new Vector4(0.0f, 0.0f, 1.0f, 1.0f));
		Assert.Equal(new AssetHandle(7), new AssetHandle(7));
		Assert.NotEqual(new AssetHandle(7), new AssetHandle(8));
		Assert.False(AssetHandle.Invalid.IsValid);
		Assert.Equal("7", new AssetHandle(7).ToString());
	}
}
