using System;

namespace Strada.VisualScripting;

// An aborting error the VM detects itself (spec section 5.3, Errors): the execution stops and the message is logged once.
// Exceptions thrown by members are aborting errors as well.
internal sealed class VisualScriptException : Exception
{
	internal VisualScriptException(string message)
		: base(message)
	{
	}
}

// The numeric operators (spec section 4.4): both operands staged at their join type, C# unchecked arithmetic, IEEE for
// floating point. Integer division or modulo by zero, and the one quotient that overflows (the minimum divided by -1, as
// C# throws an OverflowException for it), are aborting errors.
internal static class Operators
{
	internal static Slot Arithmetic(OperatorKind op, SlotKind kind, in Slot a, in Slot b) => kind switch
	{
		SlotKind.Int => Slot.FromInt(Int(op, a.Int, b.Int)),
		SlotKind.UInt => Slot.FromUInt(UInt(op, a.UInt, b.UInt)),
		SlotKind.Long => Slot.FromLong(Long(op, a.Long, b.Long)),
		SlotKind.ULong => Slot.FromULong(ULong(op, a.ULong, b.ULong)),
		SlotKind.Float => Slot.FromFloat(Float(op, a.Float, b.Float)),
		_ => Slot.FromDouble(Double(op, a.Double, b.Double)),
	};

	// C# unary minus: uint operands are staged as long, ulong ones have no node.
	internal static Slot Negate(SlotKind kind, in Slot a) => kind switch
	{
		SlotKind.Int => Slot.FromInt(unchecked(-a.Int)),
		SlotKind.Long => Slot.FromLong(unchecked(-a.Long)),
		SlotKind.Float => Slot.FromFloat(-a.Float),
		_ => Slot.FromDouble(-a.Double),
	};

	internal static bool Compare(OperatorKind op, SlotKind kind, in Slot a, in Slot b) => kind switch
	{
		SlotKind.Int => Compare(op, a.Int.CompareTo(b.Int)),
		SlotKind.UInt => Compare(op, a.UInt.CompareTo(b.UInt)),
		SlotKind.Long => Compare(op, a.Long.CompareTo(b.Long)),
		SlotKind.ULong => Compare(op, a.ULong.CompareTo(b.ULong)),
		// IEEE: every comparison with NaN is false.
		SlotKind.Float => CompareFloating(op, a.Float, b.Float),
		_ => CompareFloating(op, a.Double, b.Double),
	};

	// IEEE equality: NaN equals nothing.
	internal static bool Equal(SlotKind kind, in Slot a, in Slot b) => kind switch
	{
		SlotKind.Int => a.Int == b.Int,
		SlotKind.UInt => a.UInt == b.UInt,
		SlotKind.Long => a.Long == b.Long,
		SlotKind.ULong => a.ULong == b.ULong,
		SlotKind.Float => a.Float == b.Float,
		_ => a.Double == b.Double,
	};

	internal static bool Boolean(OperatorKind op, bool a, bool b) => op switch
	{
		OperatorKind.And => a && b,
		OperatorKind.Or => a || b,
		OperatorKind.Xor => a ^ b,
		_ => !a,
	};

	private static int Int(OperatorKind op, int a, int b)
	{
		switch (op)
		{
			case OperatorKind.Add:
				return unchecked(a + b);
			case OperatorKind.Subtract:
				return unchecked(a - b);
			case OperatorKind.Multiply:
				return unchecked(a * b);
			default:
				CheckDivisor(op, b == 0, a == int.MinValue && b == -1, "int");
				return op == OperatorKind.Divide ? a / b : a % b;
		}
	}

	private static uint UInt(OperatorKind op, uint a, uint b)
	{
		switch (op)
		{
			case OperatorKind.Add:
				return unchecked(a + b);
			case OperatorKind.Subtract:
				return unchecked(a - b);
			case OperatorKind.Multiply:
				return unchecked(a * b);
			default:
				CheckDivisor(op, b == 0, false, string.Empty);
				return op == OperatorKind.Divide ? a / b : a % b;
		}
	}

	private static long Long(OperatorKind op, long a, long b)
	{
		switch (op)
		{
			case OperatorKind.Add:
				return unchecked(a + b);
			case OperatorKind.Subtract:
				return unchecked(a - b);
			case OperatorKind.Multiply:
				return unchecked(a * b);
			default:
				CheckDivisor(op, b == 0, a == long.MinValue && b == -1, "long");
				return op == OperatorKind.Divide ? a / b : a % b;
		}
	}

	private static ulong ULong(OperatorKind op, ulong a, ulong b)
	{
		switch (op)
		{
			case OperatorKind.Add:
				return unchecked(a + b);
			case OperatorKind.Subtract:
				return unchecked(a - b);
			case OperatorKind.Multiply:
				return unchecked(a * b);
			default:
				CheckDivisor(op, b == 0, false, string.Empty);
				return op == OperatorKind.Divide ? a / b : a % b;
		}
	}

	private static float Float(OperatorKind op, float a, float b) => op switch
	{
		OperatorKind.Add => a + b,
		OperatorKind.Subtract => a - b,
		OperatorKind.Multiply => a * b,
		OperatorKind.Divide => a / b,
		_ => a % b,
	};

	private static double Double(OperatorKind op, double a, double b) => op switch
	{
		OperatorKind.Add => a + b,
		OperatorKind.Subtract => a - b,
		OperatorKind.Multiply => a * b,
		OperatorKind.Divide => a / b,
		_ => a % b,
	};

	// type names the integer type whose minimum overflows when divided by -1.
	private static void CheckDivisor(OperatorKind op, bool zero, bool overflows, string type)
	{
		if (zero)
		{
			throw new VisualScriptException(op == OperatorKind.Divide ? "integer division by zero" : "integer modulo by zero");
		}
		if (overflows)
		{
			throw new VisualScriptException(op == OperatorKind.Divide ? $"integer overflow: the smallest {type} divided by -1"
				: $"integer overflow: the remainder of the smallest {type} divided by -1");
		}
	}

	private static bool Compare(OperatorKind op, int order) => op switch
	{
		OperatorKind.Less => order < 0,
		OperatorKind.LessOrEqual => order <= 0,
		OperatorKind.Greater => order > 0,
		_ => order >= 0,
	};

	private static bool CompareFloating(OperatorKind op, double a, double b) => op switch
	{
		OperatorKind.Less => a < b,
		OperatorKind.LessOrEqual => a <= b,
		OperatorKind.Greater => a > b,
		_ => a >= b,
	};
}
