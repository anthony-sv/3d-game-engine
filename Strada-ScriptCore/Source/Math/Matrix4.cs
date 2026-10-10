using System;
using System.Globalization;
using System.Runtime.InteropServices;

namespace Strada;

/// <summary>A 4x4 matrix of single-precision values stored as four columns, like the engine's matrices. Matrices
/// transform column vectors: <c>matrix * vector</c>, and <c>a * b</c> applies <c>b</c> first.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct Matrix4 : IEquatable<Matrix4>
{
	/// <summary>The first column (the X axis of a transform).</summary>
	public Vector4 Column0;
	/// <summary>The second column (the Y axis of a transform).</summary>
	public Vector4 Column1;
	/// <summary>The third column (the Z axis of a transform).</summary>
	public Vector4 Column2;
	/// <summary>The fourth column (the translation of a transform).</summary>
	public Vector4 Column3;

	/// <summary>Creates a matrix from its columns.</summary>
	public Matrix4(Vector4 column0, Vector4 column1, Vector4 column2, Vector4 column3)
	{
		Column0 = column0;
		Column1 = column1;
		Column2 = column2;
		Column3 = column3;
	}

	/// <summary>The matrix that changes nothing.</summary>
	public static Matrix4 Identity => new(new Vector4(1.0f, 0.0f, 0.0f, 0.0f), new Vector4(0.0f, 1.0f, 0.0f, 0.0f),
		new Vector4(0.0f, 0.0f, 1.0f, 0.0f), new Vector4(0.0f, 0.0f, 0.0f, 1.0f));

	/// <summary>The element at a column and row (each 0 to 3).</summary>
	public float this[int column, int row]
	{
		readonly get
		{
			Vector4 values = GetColumn(column);
			return row switch
			{
				0 => values.X,
				1 => values.Y,
				2 => values.Z,
				3 => values.W,
				_ => throw new ArgumentOutOfRangeException(nameof(row)),
			};
		}
		set
		{
			Vector4 values = GetColumn(column);
			switch (row)
			{
				case 0:
					values.X = value;
					break;
				case 1:
					values.Y = value;
					break;
				case 2:
					values.Z = value;
					break;
				case 3:
					values.W = value;
					break;
				default:
					throw new ArgumentOutOfRangeException(nameof(row));
			}
			SetColumn(column, values);
		}
	}

	/// <summary>A translation.</summary>
	public static Matrix4 Translation(Vector3 translation)
	{
		Matrix4 matrix = Identity;
		matrix.Column3 = new Vector4(translation, 1.0f);
		return matrix;
	}

	/// <summary>A rotation.</summary>
	public static Matrix4 Rotation(Quaternion rotation)
	{
		Quaternion q = rotation.Normalized;
		float xx = q.X * q.X;
		float yy = q.Y * q.Y;
		float zz = q.Z * q.Z;
		float xy = q.X * q.Y;
		float xz = q.X * q.Z;
		float yz = q.Y * q.Z;
		float wx = q.W * q.X;
		float wy = q.W * q.Y;
		float wz = q.W * q.Z;
		return new Matrix4(
			new Vector4(1.0f - (2.0f * (yy + zz)), 2.0f * (xy + wz), 2.0f * (xz - wy), 0.0f),
			new Vector4(2.0f * (xy - wz), 1.0f - (2.0f * (xx + zz)), 2.0f * (yz + wx), 0.0f),
			new Vector4(2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - (2.0f * (xx + yy)), 0.0f),
			new Vector4(0.0f, 0.0f, 0.0f, 1.0f));
	}

	/// <summary>A scale along the axes.</summary>
	public static Matrix4 Scaling(Vector3 scale) => new(new Vector4(scale.X, 0.0f, 0.0f, 0.0f), new Vector4(0.0f, scale.Y, 0.0f, 0.0f),
		new Vector4(0.0f, 0.0f, scale.Z, 0.0f), new Vector4(0.0f, 0.0f, 0.0f, 1.0f));

	/// <summary>Scale, then rotation, then translation: an entity's transform.</summary>
	public static Matrix4 TRS(Vector3 translation, Quaternion rotation, Vector3 scale) =>
		Translation(translation) * Rotation(rotation) * Scaling(scale);

	/// <summary>Transforms a point (w = 1).</summary>
	public readonly Vector3 TransformPoint(Vector3 point) => (this * new Vector4(point, 1.0f)).XYZ;

	/// <summary>Transforms a direction (w = 0: translation does not apply).</summary>
	public readonly Vector3 TransformDirection(Vector3 direction) => (this * new Vector4(direction, 0.0f)).XYZ;

	/// <summary>The matrix that undoes this one, or null when it is singular.</summary>
	public readonly Matrix4? Inverse
	{
		get
		{
			// Cofactor expansion over 2x2 sub-determinants.
			float a00 = Column0.X, a01 = Column0.Y, a02 = Column0.Z, a03 = Column0.W;
			float a10 = Column1.X, a11 = Column1.Y, a12 = Column1.Z, a13 = Column1.W;
			float a20 = Column2.X, a21 = Column2.Y, a22 = Column2.Z, a23 = Column2.W;
			float a30 = Column3.X, a31 = Column3.Y, a32 = Column3.Z, a33 = Column3.W;
			float b00 = (a00 * a11) - (a01 * a10);
			float b01 = (a00 * a12) - (a02 * a10);
			float b02 = (a00 * a13) - (a03 * a10);
			float b03 = (a01 * a12) - (a02 * a11);
			float b04 = (a01 * a13) - (a03 * a11);
			float b05 = (a02 * a13) - (a03 * a12);
			float b06 = (a20 * a31) - (a21 * a30);
			float b07 = (a20 * a32) - (a22 * a30);
			float b08 = (a20 * a33) - (a23 * a30);
			float b09 = (a21 * a32) - (a22 * a31);
			float b10 = (a21 * a33) - (a23 * a31);
			float b11 = (a22 * a33) - (a23 * a32);
			float determinant = (b00 * b11) - (b01 * b10) + (b02 * b09) + (b03 * b08) - (b04 * b07) + (b05 * b06);
			if (MathF.Abs(determinant) < 1e-12f)
			{
				return null;
			}
			float inverse = 1.0f / determinant;
			return new Matrix4(
				new Vector4(((a11 * b11) - (a12 * b10) + (a13 * b09)) * inverse, ((a02 * b10) - (a01 * b11) - (a03 * b09)) * inverse,
					((a31 * b05) - (a32 * b04) + (a33 * b03)) * inverse, ((a22 * b04) - (a21 * b05) - (a23 * b03)) * inverse),
				new Vector4(((a12 * b08) - (a10 * b11) - (a13 * b07)) * inverse, ((a00 * b11) - (a02 * b08) + (a03 * b07)) * inverse,
					((a32 * b02) - (a30 * b05) - (a33 * b01)) * inverse, ((a20 * b05) - (a22 * b02) + (a23 * b01)) * inverse),
				new Vector4(((a10 * b10) - (a11 * b08) + (a13 * b06)) * inverse, ((a01 * b08) - (a00 * b10) - (a03 * b06)) * inverse,
					((a30 * b04) - (a31 * b02) + (a33 * b00)) * inverse, ((a21 * b02) - (a20 * b04) - (a23 * b00)) * inverse),
				new Vector4(((a11 * b07) - (a10 * b09) - (a12 * b06)) * inverse, ((a00 * b09) - (a01 * b07) + (a02 * b06)) * inverse,
					((a31 * b01) - (a30 * b03) - (a32 * b00)) * inverse, ((a20 * b03) - (a21 * b01) + (a22 * b00)) * inverse));
		}
	}

	/// <summary>The product: <paramref name="b"/> applied first, then <paramref name="a"/>.</summary>
	public static Matrix4 operator *(Matrix4 a, Matrix4 b) => new(a * b.Column0, a * b.Column1, a * b.Column2, a * b.Column3);

	/// <summary>Transforms a vector.</summary>
	public static Vector4 operator *(Matrix4 matrix, Vector4 vector) =>
		(matrix.Column0 * vector.X) + (matrix.Column1 * vector.Y) + (matrix.Column2 * vector.Z) + (matrix.Column3 * vector.W);

	/// <summary>Exact element-wise equality.</summary>
	public static bool operator ==(Matrix4 a, Matrix4 b) => a.Equals(b);

	/// <summary>Exact element-wise inequality.</summary>
	public static bool operator !=(Matrix4 a, Matrix4 b) => !a.Equals(b);

	/// <summary>Exact element-wise equality.</summary>
	public readonly bool Equals(Matrix4 other) =>
		Column0 == other.Column0 && Column1 == other.Column1 && Column2 == other.Column2 && Column3 == other.Column3;

	/// <inheritdoc/>
	public override readonly bool Equals(object? obj) => obj is Matrix4 other && Equals(other);

	/// <inheritdoc/>
	public override readonly int GetHashCode() => HashCode.Combine(Column0, Column1, Column2, Column3);

	/// <summary>The columns, in the invariant culture.</summary>
	public override readonly string ToString() => string.Create(CultureInfo.InvariantCulture, $"[{Column0}, {Column1}, {Column2}, {Column3}]");

	private readonly Vector4 GetColumn(int column) => column switch
	{
		0 => Column0,
		1 => Column1,
		2 => Column2,
		3 => Column3,
		_ => throw new ArgumentOutOfRangeException(nameof(column)),
	};

	private void SetColumn(int column, Vector4 values)
	{
		switch (column)
		{
			case 0:
				Column0 = values;
				break;
			case 1:
				Column1 = values;
				break;
			case 2:
				Column2 = values;
				break;
			case 3:
				Column3 = values;
				break;
			default:
				throw new ArgumentOutOfRangeException(nameof(column));
		}
	}
}
