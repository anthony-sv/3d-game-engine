using System;

namespace Strada.VisualScripting;

// Runs a formula's program over the slots of a frame (spec-slice3 section 3.3.8): one loop with one switch of slot
// operations, which allocates nothing. Values are computed as the nodes that compute them in graphs do (Operators,
// Conversions and the vector operators of the scripting API), so a formula and the same computation built from nodes give
// the same bits.
internal static class ExpressionMachine
{
	// An aborting error of an operator (an integer division or modulo by zero, the smallest value divided by -1) is thrown
	// again with the part of the formula that raised it.
	internal static void Run(ExpressionProgram program, Slot[] slots)
	{
		ExpressionInstruction[] code = program.Code;
		int index = 0;
		try
		{
			while (index < code.Length)
			{
				ref readonly ExpressionInstruction instruction = ref code[index];
				switch (instruction.Op)
				{
					case ExpressionOp.Copy:
						slots[instruction.Target] = slots[instruction.A];
						break;
					case ExpressionOp.Arithmetic:
						slots[instruction.Target] = Operators.Arithmetic((OperatorKind)instruction.Detail, instruction.Kind, slots[instruction.A], slots[instruction.B]);
						break;
					case ExpressionOp.Negate:
						slots[instruction.Target] = ExpressionMath.IsVector(instruction.Kind)
							? ExpressionMath.Negate(instruction.Kind, slots[instruction.A])
							: Operators.Negate(instruction.Kind, slots[instruction.A]);
						break;
					case ExpressionOp.Compare:
						slots[instruction.Target] = Slot.FromBool(Operators.Compare((OperatorKind)instruction.Detail, instruction.Kind, slots[instruction.A],
							slots[instruction.B]));
						break;
					case ExpressionOp.Equality:
						slots[instruction.Target] = Slot.FromBool(ExpressionMath.Equal(instruction.Kind, slots[instruction.A], slots[instruction.B]) !=
							(instruction.Detail == 1));
						break;
					case ExpressionOp.Not:
						slots[instruction.Target] = Slot.FromBool(!slots[instruction.A].Bool);
						break;
					case ExpressionOp.Widen:
						Slot widened = slots[instruction.A];
						Conversions.Widen(ref widened, (ConversionOp)instruction.Detail);
						slots[instruction.Target] = widened;
						break;
					case ExpressionOp.Cast:
						slots[instruction.Target] = Conversions.NumericCast(slots[instruction.A], instruction.Kind2, instruction.Kind);
						break;
					case ExpressionOp.Vector:
						slots[instruction.Target] = ExpressionMath.Vector(instruction.Kind, instruction.Detail, slots[instruction.A], slots[instruction.B]);
						break;
					case ExpressionOp.Component:
						slots[instruction.Target] = Slot.FromFloat(ExpressionMath.Component(slots[instruction.A].Vector4, instruction.Detail));
						break;
					case ExpressionOp.Construct:
						slots[instruction.Target] = ExpressionMath.Construct(instruction.Kind, slots[instruction.A].Float, slots[instruction.B].Float,
							instruction.C >= 0 ? slots[instruction.C].Float : 0.0f, instruction.D >= 0 ? slots[instruction.D].Float : 0.0f);
						break;
					case ExpressionOp.Call:
						slots[instruction.Target] = ExpressionMath.Call((ExpressionFunction)instruction.Detail, instruction.Kind, instruction.Kind2,
							slots[instruction.A], instruction.B >= 0 ? slots[instruction.B] : default, instruction.C >= 0 ? slots[instruction.C] : default);
						break;
					case ExpressionOp.JumpIfFalse:
						if (!slots[instruction.A].Bool)
						{
							index = instruction.Jump;
							continue;
						}
						break;
					case ExpressionOp.JumpIfTrue:
						if (slots[instruction.A].Bool)
						{
							index = instruction.Jump;
							continue;
						}
						break;
					case ExpressionOp.Jump:
						index = instruction.Jump;
						continue;
				}
				index++;
			}
		}
		catch (VisualScriptException exception)
		{
			throw new VisualScriptException($"{exception.Message} in {program.TextOf(code[index].Span)}");
		}
	}
}

// The vector operations and functions of formulas (spec-slice3 sections 3.3.5 and 3.3.6). Vectors are read through the
// slot's Vector4, whose first components are a Vector2's or Vector3's, and every result is made of exactly its components;
// sums run in the order the vector types' own members sum.
internal static class ExpressionMath
{
	internal static bool IsVector(SlotKind kind) => kind is SlotKind.Vector2 or SlotKind.Vector3 or SlotKind.Vector4;

	internal static float Component(Vector4 vector, byte index) => index switch
	{
		0 => vector.X,
		1 => vector.Y,
		2 => vector.Z,
		_ => vector.W,
	};

	internal static Slot Construct(SlotKind kind, float x, float y, float z, float w) => kind switch
	{
		SlotKind.Vector2 => Slot.FromVector2(new Vector2(x, y)),
		SlotKind.Vector3 => Slot.FromVector3(new Vector3(x, y, z)),
		_ => Slot.FromVector4(new Vector4(x, y, z, w)),
	};

	// Exact equality per component of vectors (their own equality, as the Equal node compares them), IEEE for numbers.
	internal static bool Equal(SlotKind kind, in Slot a, in Slot b) => kind switch
	{
		SlotKind.Bool => a.Bool == b.Bool,
		SlotKind.Vector2 => a.Vector2 == b.Vector2,
		SlotKind.Vector3 => a.Vector3 == b.Vector3,
		SlotKind.Vector4 => a.Vector4 == b.Vector4,
		_ => Operators.Equal(kind, a, b),
	};

	internal static Slot Negate(SlotKind kind, in Slot a)
	{
		Vector4 v = a.Vector4;
		return Construct(kind, -v.X, -v.Y, -v.Z, -v.W);
	}

	// A + B, A - B, A * B or A / B per component: two vectors, a vector and a float, or a float and a vector.
	internal static Slot Vector(SlotKind kind, byte detail, in Slot a, in Slot b)
	{
		OperatorKind op = ExpressionCode.OperatorOf(detail);
		switch (ExpressionCode.OperandsOf(detail))
		{
			case VectorOperands.VectorVector:
				Vector4 left = a.Vector4;
				Vector4 right = b.Vector4;
				return Construct(kind, Apply(op, left.X, right.X), Apply(op, left.Y, right.Y), Apply(op, left.Z, right.Z), Apply(op, left.W, right.W));
			case VectorOperands.VectorScalar:
				Vector4 vector = a.Vector4;
				float scalar = b.Float;
				return Construct(kind, Apply(op, vector.X, scalar), Apply(op, vector.Y, scalar), Apply(op, vector.Z, scalar), Apply(op, vector.W, scalar));
			default:
				// A number times a vector is the vector times the number, as the vector types define it.
				Vector4 scaled = b.Vector4;
				float factor = a.Float;
				return Construct(kind, Apply(op, scaled.X, factor), Apply(op, scaled.Y, factor), Apply(op, scaled.Z, factor), Apply(op, scaled.W, factor));
		}
	}

	// operands is the kind of the arguments, or Float for clamp(vector, number, number), whose result is the vector.
	internal static Slot Call(ExpressionFunction function, SlotKind result, SlotKind operands, in Slot a, in Slot b, in Slot c)
	{
		if (IsVector(result) || IsVector(operands))
		{
			return VectorCall(function, result, operands, a, b, c);
		}
		switch (operands)
		{
			case SlotKind.Int:
				return Slot.FromInt(Int(function, a.Int, b.Int, c.Int));
			case SlotKind.UInt:
				return Slot.FromUInt(UInt(function, a.UInt, b.UInt, c.UInt));
			case SlotKind.Long:
				return Slot.FromLong(Long(function, a.Long, b.Long, c.Long));
			case SlotKind.ULong:
				return Slot.FromULong(ULong(function, a.ULong, b.ULong, c.ULong));
			case SlotKind.Float:
				return Slot.FromFloat(Float(function, a.Float, b.Float, c.Float));
			default:
				return Slot.FromDouble(Double(function, a.Double, b.Double, c.Double));
		}
	}

	private static float Apply(OperatorKind op, float a, float b) => op switch
	{
		OperatorKind.Add => a + b,
		OperatorKind.Subtract => a - b,
		OperatorKind.Multiply => a * b,
		_ => a / b,
	};

	private static int Int(ExpressionFunction function, int a, int b, int c) => function switch
	{
		ExpressionFunction.Abs => a < 0 ? unchecked(-a) : a,
		ExpressionFunction.Sign => a > 0 ? 1 : a < 0 ? -1 : 0,
		ExpressionFunction.Min => Math.Min(a, b),
		ExpressionFunction.Max => Math.Max(a, b),
		_ => Math.Min(Math.Max(a, b), c),
	};

	private static uint UInt(ExpressionFunction function, uint a, uint b, uint c) => function switch
	{
		ExpressionFunction.Abs => a,
		ExpressionFunction.Sign => a > 0 ? 1u : 0u,
		ExpressionFunction.Min => Math.Min(a, b),
		ExpressionFunction.Max => Math.Max(a, b),
		_ => Math.Min(Math.Max(a, b), c),
	};

	private static long Long(ExpressionFunction function, long a, long b, long c) => function switch
	{
		ExpressionFunction.Abs => a < 0 ? unchecked(-a) : a,
		ExpressionFunction.Sign => a > 0 ? 1L : a < 0 ? -1L : 0L,
		ExpressionFunction.Min => Math.Min(a, b),
		ExpressionFunction.Max => Math.Max(a, b),
		_ => Math.Min(Math.Max(a, b), c),
	};

	private static ulong ULong(ExpressionFunction function, ulong a, ulong b, ulong c) => function switch
	{
		ExpressionFunction.Abs => a,
		ExpressionFunction.Sign => a > 0 ? 1UL : 0UL,
		ExpressionFunction.Min => Math.Min(a, b),
		ExpressionFunction.Max => Math.Max(a, b),
		_ => Math.Min(Math.Max(a, b), c),
	};

	private static float Float(ExpressionFunction function, float a, float b, float c) => function switch
	{
		ExpressionFunction.Abs => MathF.Abs(a),
		// Keeps -0, +0 and NaN.
		ExpressionFunction.Sign => a > 0.0f ? 1.0f : a < 0.0f ? -1.0f : a,
		ExpressionFunction.Min => MathF.Min(a, b),
		ExpressionFunction.Max => MathF.Max(a, b),
		ExpressionFunction.Clamp => MathF.Min(MathF.Max(a, b), c),
		ExpressionFunction.Floor => MathF.Floor(a),
		ExpressionFunction.Ceil => MathF.Ceiling(a),
		ExpressionFunction.Round => MathF.Round(a),
		ExpressionFunction.Trunc => MathF.Truncate(a),
		ExpressionFunction.Frac => a - MathF.Floor(a),
		ExpressionFunction.Sqrt => MathF.Sqrt(a),
		ExpressionFunction.Exp => MathF.Exp(a),
		ExpressionFunction.Log => MathF.Log(a),
		ExpressionFunction.Log2 => MathF.Log2(a),
		ExpressionFunction.Log10 => MathF.Log10(a),
		ExpressionFunction.Pow => MathF.Pow(a, b),
		ExpressionFunction.Atan2 => MathF.Atan2(a, b),
		ExpressionFunction.Sin => MathF.Sin(a),
		ExpressionFunction.Cos => MathF.Cos(a),
		ExpressionFunction.Tan => MathF.Tan(a),
		ExpressionFunction.Asin => MathF.Asin(a),
		ExpressionFunction.Acos => MathF.Acos(a),
		ExpressionFunction.Atan => MathF.Atan(a),
		ExpressionFunction.Radians => a * (MathF.PI / 180.0f),
		ExpressionFunction.Degrees => a * (180.0f / MathF.PI),
		ExpressionFunction.Lerp => a + ((b - a) * c),
		ExpressionFunction.Smoothstep => Smoothstep(a, b, c),
		_ => b >= a ? 1.0f : 0.0f,
	};

	private static double Double(ExpressionFunction function, double a, double b, double c) => function switch
	{
		ExpressionFunction.Abs => Math.Abs(a),
		ExpressionFunction.Sign => a > 0.0 ? 1.0 : a < 0.0 ? -1.0 : a,
		ExpressionFunction.Min => Math.Min(a, b),
		ExpressionFunction.Max => Math.Max(a, b),
		ExpressionFunction.Clamp => Math.Min(Math.Max(a, b), c),
		ExpressionFunction.Floor => Math.Floor(a),
		ExpressionFunction.Ceil => Math.Ceiling(a),
		ExpressionFunction.Round => Math.Round(a),
		ExpressionFunction.Trunc => Math.Truncate(a),
		ExpressionFunction.Frac => a - Math.Floor(a),
		ExpressionFunction.Sqrt => Math.Sqrt(a),
		ExpressionFunction.Exp => Math.Exp(a),
		ExpressionFunction.Log => Math.Log(a),
		ExpressionFunction.Log2 => Math.Log2(a),
		ExpressionFunction.Log10 => Math.Log10(a),
		ExpressionFunction.Pow => Math.Pow(a, b),
		ExpressionFunction.Atan2 => Math.Atan2(a, b),
		ExpressionFunction.Sin => Math.Sin(a),
		ExpressionFunction.Cos => Math.Cos(a),
		ExpressionFunction.Tan => Math.Tan(a),
		ExpressionFunction.Asin => Math.Asin(a),
		ExpressionFunction.Acos => Math.Acos(a),
		ExpressionFunction.Atan => Math.Atan(a),
		ExpressionFunction.Radians => a * (Math.PI / 180.0),
		ExpressionFunction.Degrees => a * (180.0 / Math.PI),
		ExpressionFunction.Lerp => a + ((b - a) * c),
		ExpressionFunction.Smoothstep => Smoothstep(a, b, c),
		_ => b >= a ? 1.0 : 0.0,
	};

	// t = clamp((x - e0) / (e1 - e0), 0, 1); t * t * (3 - 2 * t); a step at e0 when the edges are equal.
	private static float Smoothstep(float edge0, float edge1, float x)
	{
		if (edge0 == edge1)
		{
			return x < edge0 ? 0.0f : 1.0f;
		}
		float t = MathF.Min(MathF.Max((x - edge0) / (edge1 - edge0), 0.0f), 1.0f);
		return t * t * (3.0f - (2.0f * t));
	}

	private static double Smoothstep(double edge0, double edge1, double x)
	{
		if (edge0 == edge1)
		{
			return x < edge0 ? 0.0 : 1.0;
		}
		double t = Math.Min(Math.Max((x - edge0) / (edge1 - edge0), 0.0), 1.0);
		return t * t * (3.0 - (2.0 * t));
	}

	// Functions of vectors: per component (abs to frac, min, max, clamp, lerp), and length, normalize, dot, distance and
	// cross as the vector types compute them.
	private static Slot VectorCall(ExpressionFunction function, SlotKind result, SlotKind vector, in Slot a, in Slot b, in Slot c)
	{
		Vector4 x = a.Vector4;
		Vector4 y = b.Vector4;
		Vector4 z = c.Vector4;
		switch (function)
		{
			case ExpressionFunction.Length:
				return Slot.FromFloat(MathF.Sqrt(Dot(vector, x, x)));
			case ExpressionFunction.Normalize:
				// As the vector types' Normalized: a vector of length zero (or NaN) is zero.
				float length = MathF.Sqrt(Dot(vector, x, x));
				return length > 0.0f ? Construct(result, x.X / length, x.Y / length, x.Z / length, x.W / length) : Construct(result, 0.0f, 0.0f, 0.0f, 0.0f);
			case ExpressionFunction.Dot:
				return Slot.FromFloat(Dot(vector, x, y));
			case ExpressionFunction.Distance:
				Vector4 difference = new(x.X - y.X, x.Y - y.Y, x.Z - y.Z, x.W - y.W);
				return Slot.FromFloat(MathF.Sqrt(Dot(vector, difference, difference)));
			case ExpressionFunction.Cross:
				return Slot.FromVector3(Vector3.Cross(a.Vector3, b.Vector3));
			case ExpressionFunction.Clamp when !IsVector(vector):
				// The bounds are floats: each component is clamped by them.
				float low = b.Float;
				float high = c.Float;
				return Construct(result, Float(function, x.X, low, high), Float(function, x.Y, low, high), Float(function, x.Z, low, high),
					Float(function, x.W, low, high));
			case ExpressionFunction.Lerp:
				float t = c.Float;
				return Construct(result, Float(function, x.X, y.X, t), Float(function, x.Y, y.Y, t), Float(function, x.Z, y.Z, t), Float(function, x.W, y.W, t));
			default:
				return Construct(result, Float(function, x.X, y.X, z.X), Float(function, x.Y, y.Y, z.Y), Float(function, x.Z, y.Z, z.Z),
					Float(function, x.W, y.W, z.W));
		}
	}

	// The dot product over the vector's components, summed in order.
	private static float Dot(SlotKind vector, Vector4 a, Vector4 b) => vector switch
	{
		SlotKind.Vector2 => (a.X * b.X) + (a.Y * b.Y),
		SlotKind.Vector3 => (a.X * b.X) + (a.Y * b.Y) + (a.Z * b.Z),
		_ => (a.X * b.X) + (a.Y * b.Y) + (a.Z * b.Z) + (a.W * b.W),
	};
}
