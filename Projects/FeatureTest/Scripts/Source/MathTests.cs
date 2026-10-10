using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>Vectors, quaternions, matrices, colors, Mathf and Random.</summary>
public sealed class MathTests : FeatureTestScript
{
	protected override void Start()
	{
		Check("Vector2", Vector2Checks);
		Check("Vector3", Vector3Checks);
		Check("Vector4", Vector4Checks);
		Check("Quaternion", QuaternionChecks);
		Check("Matrix4", MatrixChecks);
		Check("Color", ColorChecks);
		Check("Mathf", MathfChecks);
		Check("Random", RandomChecks);
	}

	private static void Vector2Checks()
	{
		Vector2 a = new(3.0f, 4.0f);
		Vector2 b = new(2.0f);
		Assert.AreEqual(3.0f, a.X);
		Assert.AreEqual(4.0f, a.Y);
		Assert.AreEqual(new Vector2(0.0f, 0.0f), Vector2.Zero);
		Assert.AreEqual(new Vector2(1.0f, 1.0f), Vector2.One);
		Assert.AreApproximatelyEqual(5.0f, a.Length);
		Assert.AreApproximatelyEqual(25.0f, a.LengthSquared);
		Assert.AreApproximatelyEqual(1.0f, a.Normalized.Length);
		Assert.AreApproximatelyEqual(14.0f, Vector2.Dot(a, b));
		Assert.AreApproximatelyEqual(5.0f, Vector2.Distance(Vector2.Zero, a));
		Assert.AreEqual(new Vector2(1.5f, 2.0f), Vector2.Lerp(Vector2.Zero, a, 0.5f));
		Assert.AreEqual(new Vector2(5.0f, 6.0f), a + b);
		Assert.AreEqual(new Vector2(1.0f, 2.0f), a - b);
		Assert.AreEqual(new Vector2(-3.0f, -4.0f), -a);
		Assert.AreEqual(new Vector2(6.0f, 8.0f), a * b);
		Assert.AreEqual(new Vector2(6.0f, 8.0f), a * 2.0f);
		Assert.AreEqual(new Vector2(6.0f, 8.0f), 2.0f * a);
		Assert.AreEqual(new Vector2(1.5f, 2.0f), a / 2.0f);
		Assert.IsTrue(a == new Vector2(3.0f, 4.0f));
		Assert.IsTrue(a != b);
		Assert.IsTrue(a.Equals(new Vector2(3.0f, 4.0f)));
		Vector2 changed = a;
		changed.X = 1.0f;
		changed.Y = 2.0f;
		Assert.AreEqual(new Vector2(1.0f, 2.0f), changed);
	}

	private static void Vector3Checks()
	{
		Vector3 a = new(1.0f, 2.0f, 2.0f);
		Vector3 b = new(2.0f);
		Assert.AreEqual(new Vector3(1.0f, 2.0f, 5.0f), new Vector3(new Vector2(1.0f, 2.0f), 5.0f));
		Assert.AreEqual(new Vector2(1.0f, 2.0f), a.XY);
		Assert.AreEqual(new Vector3(0.0f, 0.0f, 0.0f), Vector3.Zero);
		Assert.AreEqual(new Vector3(1.0f, 1.0f, 1.0f), Vector3.One);
		Assert.AreEqual(-Vector3.Left, Vector3.Right);
		Assert.AreEqual(-Vector3.Down, Vector3.Up);
		Assert.AreEqual(-Vector3.Back, Vector3.Forward);
		Assert.AreEqual(new Vector3(0.0f, 0.0f, -1.0f), Vector3.Forward);
		Assert.AreApproximatelyEqual(3.0f, a.Length);
		Assert.AreApproximatelyEqual(9.0f, a.LengthSquared);
		Assert.AreApproximatelyEqual(1.0f, a.Normalized.Length);
		Assert.AreApproximatelyEqual(10.0f, Vector3.Dot(a, b));
		Assert.AreEqual(Vector3.Forward, Vector3.Cross(Vector3.Up, Vector3.Right));
		Assert.AreApproximatelyEqual(3.0f, Vector3.Distance(Vector3.Zero, a));
		Assert.AreApproximatelyEqual(new Vector3(0.5f, 1.0f, 1.0f), Vector3.Lerp(Vector3.Zero, a, 0.5f));
		Assert.AreEqual(new Vector3(1.0f, 2.0f, 2.0f), Vector3.Min(a, new Vector3(3.0f)));
		Assert.AreEqual(new Vector3(2.0f, 2.0f, 2.0f), Vector3.Max(a, b));
		Assert.AreEqual(new Vector3(3.0f, 4.0f, 4.0f), a + b);
		Assert.AreEqual(new Vector3(-1.0f, 0.0f, 0.0f), a - b);
		Assert.AreEqual(new Vector3(-1.0f, -2.0f, -2.0f), -a);
		Assert.AreEqual(new Vector3(2.0f, 4.0f, 4.0f), a * b);
		Assert.AreEqual(new Vector3(3.0f, 6.0f, 6.0f), a * 3.0f);
		Assert.AreEqual(new Vector3(3.0f, 6.0f, 6.0f), 3.0f * a);
		Assert.AreEqual(new Vector3(0.5f, 1.0f, 1.0f), a / b);
		Assert.AreEqual(new Vector3(0.5f, 1.0f, 1.0f), a / 2.0f);
		Assert.IsTrue(a == new Vector3(1.0f, 2.0f, 2.0f));
		Assert.IsTrue(a != b);
		Assert.IsTrue(a.Equals(new Vector3(1.0f, 2.0f, 2.0f)));
		Vector3 changed = a;
		changed.X = 4.0f;
		changed.Y = 5.0f;
		changed.Z = 6.0f;
		Assert.AreEqual(new Vector3(4.0f, 5.0f, 6.0f), changed);
	}

	private static void Vector4Checks()
	{
		Vector4 a = new(1.0f, 2.0f, 3.0f, 4.0f);
		Vector4 b = new(2.0f);
		Assert.AreEqual(a, new Vector4(new Vector3(1.0f, 2.0f, 3.0f), 4.0f));
		Assert.AreEqual(new Vector3(1.0f, 2.0f, 3.0f), a.XYZ);
		Assert.AreEqual(new Vector4(0.0f), Vector4.Zero);
		Assert.AreEqual(new Vector4(1.0f), Vector4.One);
		Assert.AreApproximatelyEqual(Mathf.Sqrt(30.0f), a.Length);
		Assert.AreApproximatelyEqual(20.0f, Vector4.Dot(a, b));
		Assert.AreEqual(new Vector4(0.5f, 1.0f, 1.5f, 2.0f), Vector4.Lerp(Vector4.Zero, a, 0.5f));
		Assert.AreEqual(new Vector4(3.0f, 4.0f, 5.0f, 6.0f), a + b);
		Assert.AreEqual(new Vector4(-1.0f, 0.0f, 1.0f, 2.0f), a - b);
		Assert.AreEqual(new Vector4(-1.0f, -2.0f, -3.0f, -4.0f), -a);
		Assert.AreEqual(new Vector4(2.0f, 4.0f, 6.0f, 8.0f), a * b);
		Assert.AreEqual(new Vector4(2.0f, 4.0f, 6.0f, 8.0f), a * 2.0f);
		Assert.AreEqual(new Vector4(2.0f, 4.0f, 6.0f, 8.0f), 2.0f * a);
		Assert.AreEqual(new Vector4(0.5f, 1.0f, 1.5f, 2.0f), a / 2.0f);
		Assert.IsTrue(a == new Vector4(1.0f, 2.0f, 3.0f, 4.0f));
		Assert.IsTrue(a != b);
		Assert.IsTrue(a.Equals(new Vector4(1.0f, 2.0f, 3.0f, 4.0f)));
		Vector4 changed = a;
		changed.X = 5.0f;
		changed.Y = 6.0f;
		changed.Z = 7.0f;
		changed.W = 8.0f;
		Assert.AreEqual(new Vector4(5.0f, 6.0f, 7.0f, 8.0f), changed);
	}

	private static void QuaternionChecks()
	{
		Quaternion quarter = Quaternion.AngleAxis(90.0f, Vector3.Up);
		Assert.AreApproximatelyEqual(Vector3.Forward, quarter * Vector3.Right, 1e-5f);
		Assert.AreApproximatelyEqual(1.0f, quarter.LengthSquared);
		Assert.AreEqual(new Quaternion(0.0f, 0.0f, 0.0f, 1.0f), Quaternion.Identity);
		Quaternion scaled = new(quarter.X * 2.0f, quarter.Y * 2.0f, quarter.Z * 2.0f, quarter.W * 2.0f);
		Assert.AreApproximatelyEqual(1.0f, scaled.Normalized.LengthSquared);
		Assert.AreApproximatelyEqual(Vector3.Right, quarter.Inverse * (quarter * Vector3.Right), 1e-5f);
		Assert.AreApproximatelyEqual(1.0f, Mathf.Abs(Quaternion.Dot(quarter * quarter.Inverse, Quaternion.Identity)), 1e-5f);
		Quaternion eighth = Quaternion.Slerp(Quaternion.Identity, quarter, 0.5f);
		Assert.AreApproximatelyEqual(1.0f, Mathf.Abs(Quaternion.Dot(eighth, Quaternion.AngleAxis(45.0f, Vector3.Up))), 1e-5f);
		Quaternion same = Quaternion.AngleAxis(90.0f, Vector3.Up);
		Assert.IsTrue(quarter == same);
		Assert.IsTrue(quarter != Quaternion.Identity);
		Assert.IsTrue(quarter.Equals(same));
		Quaternion fields = Quaternion.Identity;
		fields.X = 0.0f;
		fields.Y = 1.0f;
		fields.Z = 0.0f;
		fields.W = 0.0f;
		Assert.AreApproximatelyEqual(Vector3.Left, fields * Vector3.Right, 1e-5f);
	}

	private static void MatrixChecks()
	{
		Vector3 translation = new(1.0f, 2.0f, 3.0f);
		Quaternion rotation = Quaternion.AngleAxis(90.0f, Vector3.Up);
		Vector3 scale = new(2.0f);
		Matrix4 trs = Matrix4.TRS(translation, rotation, scale);
		Matrix4 product = Matrix4.Translation(translation) * Matrix4.Rotation(rotation) * Matrix4.Scaling(scale);
		Vector3 point = new(0.5f, -1.0f, 2.0f);
		Assert.AreApproximatelyEqual(trs.TransformPoint(point), product.TransformPoint(point), 1e-5f);
		Assert.AreApproximatelyEqual(new Vector3(1.0f, 2.0f, 1.0f), trs.TransformPoint(Vector3.Right), 1e-5f);
		Assert.AreApproximatelyEqual(new Vector3(0.0f, 0.0f, -2.0f), trs.TransformDirection(Vector3.Right), 1e-5f);
		Assert.AreApproximatelyEqual(new Vector3(2.0f, 2.0f, 3.0f), (Matrix4.Translation(translation) * new Vector4(Vector3.Right, 1.0f)).XYZ);
		Matrix4? inverse = trs.Inverse;
		Assert.IsTrue(inverse.HasValue);
		Assert.AreApproximatelyEqual(point, inverse.Value.TransformPoint(trs.TransformPoint(point)), 1e-5f);
		Assert.IsFalse(Matrix4.Scaling(Vector3.Zero).Inverse.HasValue);
		Matrix4 identity = new(new Vector4(1.0f, 0.0f, 0.0f, 0.0f), new Vector4(0.0f, 1.0f, 0.0f, 0.0f), new Vector4(0.0f, 0.0f, 1.0f, 0.0f),
			new Vector4(0.0f, 0.0f, 0.0f, 1.0f));
		Assert.IsTrue(identity == Matrix4.Identity);
		Assert.IsTrue(identity != trs);
		Assert.IsTrue(identity.Equals(Matrix4.Identity));
		identity[3, 0] = 5.0f;
		Assert.AreEqual(5.0f, identity[3, 0]);
		Assert.AreEqual(new Vector4(5.0f, 0.0f, 0.0f, 1.0f), identity.Column3);
		identity.Column0 = Vector4.Zero;
		identity.Column1 = Vector4.Zero;
		identity.Column2 = Vector4.Zero;
		identity.Column3 = Vector4.One;
		Assert.AreEqual(Vector4.Zero, identity.Column0 + identity.Column1 + identity.Column2);
		Assert.AreEqual(Vector4.One, identity.Column3);
	}

	private static void ColorChecks()
	{
		Color orange = new(1.0f, 0.5f, 0.0f);
		Assert.AreEqual(1.0f, orange.A);
		Assert.AreEqual(new Color(1.0f, 1.0f, 1.0f), Color.White);
		Assert.AreEqual(new Color(0.0f, 0.0f, 0.0f), Color.Black);
		Assert.AreEqual(new Color(1.0f, 0.0f, 0.0f), Color.Red);
		Assert.AreEqual(new Color(0.0f, 1.0f, 0.0f), Color.Green);
		Assert.AreEqual(new Color(0.0f, 0.0f, 1.0f), Color.Blue);
		Assert.AreEqual(0.0f, Color.Clear.A);
		Assert.AreEqual(new Color(0.5f, 0.5f, 0.5f), Color.Lerp(Color.Black, Color.White, 0.5f));
		Vector4 vector = (Vector4)orange;
		Assert.AreEqual(new Vector4(1.0f, 0.5f, 0.0f, 1.0f), vector);
		Assert.IsTrue((Color)vector == orange);
		Assert.IsTrue(orange != Color.Red);
		Assert.IsTrue(orange.Equals(new Color(1.0f, 0.5f, 0.0f, 1.0f)));
		Color changed = orange;
		changed.R = 0.1f;
		changed.G = 0.2f;
		changed.B = 0.3f;
		changed.A = 0.4f;
		Assert.AreEqual(new Color(0.1f, 0.2f, 0.3f, 0.4f), changed);
	}

	private static void MathfChecks()
	{
		Assert.AreApproximatelyEqual(180.0f, Mathf.PI * Mathf.Rad2Deg);
		Assert.AreApproximatelyEqual(Mathf.PI, 180.0f * Mathf.Deg2Rad);
		Assert.IsTrue(Mathf.Epsilon > 0.0f);
		Assert.AreEqual(2.0f, Mathf.Abs(-2.0f));
		Assert.AreEqual(1.0f, Mathf.Min(1.0f, 2.0f));
		Assert.AreEqual(2.0f, Mathf.Max(1.0f, 2.0f));
		Assert.AreEqual(1.0f, Mathf.Clamp(5.0f, 0.0f, 1.0f));
		Assert.AreEqual(0.0f, Mathf.Clamp01(-1.0f));
		Assert.AreEqual(1.0f, Mathf.Lerp(0.0f, 1.0f, 2.0f));
		Assert.AreEqual(2.0f, Mathf.LerpUnclamped(0.0f, 1.0f, 2.0f));
		Assert.AreEqual(0.25f, Mathf.InverseLerp(0.0f, 4.0f, 1.0f));
		Assert.AreApproximatelyEqual(0.5f, Mathf.SmoothStep(0.0f, 1.0f, 0.5f));
		Assert.AreEqual(1.5f, Mathf.MoveTowards(1.0f, 3.0f, 0.5f));
		Assert.AreApproximatelyEqual(-20.0f, Mathf.DeltaAngle(10.0f, 350.0f));
		Assert.AreApproximatelyEqual(1.0f, Mathf.Repeat(5.0f, 2.0f));
		Assert.AreApproximatelyEqual(1.0f, Mathf.PingPong(3.0f, 2.0f));
		Assert.IsTrue(Mathf.Approximately(0.1f + 0.2f, 0.3f));
		Assert.AreEqual(-1.0f, Mathf.Sign(-3.0f));
		Assert.AreEqual(3.0f, Mathf.Sqrt(9.0f));
		Assert.AreEqual(8.0f, Mathf.Pow(2.0f, 3.0f));
		Assert.AreApproximatelyEqual(1.0f, Mathf.Log(Mathf.Exp(1.0f)));
		Assert.AreEqual(1.0f, Mathf.Floor(1.5f));
		Assert.AreEqual(2.0f, Mathf.Ceil(1.5f));
		Assert.AreEqual(2.0f, Mathf.Round(1.6f));
		Assert.AreApproximatelyEqual(1.0f, Mathf.Sin(Mathf.PI * 0.5f));
		Assert.AreApproximatelyEqual(1.0f, Mathf.Cos(0.0f));
		Assert.AreApproximatelyEqual(1.0f, Mathf.Tan(Mathf.PI * 0.25f));
		Assert.AreApproximatelyEqual(Mathf.PI * 0.5f, Mathf.Asin(1.0f));
		Assert.AreApproximatelyEqual(0.0f, Mathf.Acos(1.0f));
		Assert.AreApproximatelyEqual(Mathf.PI * 0.25f, Mathf.Atan(1.0f));
		Assert.AreApproximatelyEqual(Mathf.PI * 0.5f, Mathf.Atan2(1.0f, 0.0f));
	}

	private static void RandomChecks()
	{
		Random.Seed(42);
		float first = Random.Value;
		Random.Seed(42);
		Assert.AreEqual(first, Random.Value);
		for (int i = 0; i < 100; i++)
		{
			float value = Random.Range(-2.0f, 3.0f);
			Assert.IsTrue(value >= -2.0f && value <= 3.0f, $"{value} is outside [-2, 3]");
			int whole = Random.Range(1, 4);
			Assert.IsTrue(whole >= 1 && whole < 4, $"{whole} is outside [1, 4)");
			Assert.IsTrue(Random.InsideUnitSphere.Length <= 1.0f + 1e-5f);
			Assert.AreApproximatelyEqual(1.0f, Random.OnUnitSphere.Length, 1e-4f);
			Assert.AreApproximatelyEqual(1.0f, Random.Rotation.LengthSquared, 1e-4f);
		}
	}
}
