using System;
using System.Globalization;
using System.Text;

namespace Strada.VisualScripting;

// A conversion applied to a value as it is staged into an input (spec section 3.4), resolved to the exact pair of slot
// kinds when the program is compiled.
internal enum ConversionOp : byte
{
	None,
	IntToLong,
	IntToFloat,
	IntToDouble,
	UIntToLong,
	UIntToULong,
	UIntToFloat,
	UIntToDouble,
	LongToFloat,
	LongToDouble,
	ULongToFloat,
	ULongToDouble,
	FloatToDouble,
	// A value type to System.Object.
	Box,
	// A component to its entity.
	ComponentToEntity,
	// A typed asset to its handle (null gives AssetHandle.Invalid).
	AssetToHandle,
	// Any value to text (rule 7).
	ToText,
}

// The conversions of links (spec section 3.4 rules 4 to 7), the numeric casts of Convert (spec section 4.4) and the text
// form of values.
internal static class Conversions
{
	private const string NoneText = "None";
	// Arrays held as System.Object values (only those can hold arrays): nested arrays are listed to this depth, and the text
	// stops at about this length, so an array that holds itself, or many arrays, gives a short text in little time.
	private const int MaxArrayTextDepth = 16;
	private const int MaxObjectArrayText = 4096;
	// The first doubles past the ranges of long and ulong (long.MaxValue and ulong.MaxValue are not doubles).
	private const double TwoToThe63 = 9223372036854775808.0;
	private const double TwoToThe64 = 18446744073709551616.0;
	private static readonly object s_InvalidHandle = AssetHandle.Invalid;

	// The conversion op of a value of one type staged as another; null when the analysis would not have allowed it.
	internal static ConversionOp? Plan(VisualType from, VisualType to)
	{
		if (ReferenceEquals(from, to))
		{
			return ConversionOp.None;
		}
		ConversionKind? kind = TypeRules.GetJoinConversion(from, to) ?? TypeRules.GetConversion(from, to);
		return kind is { } found ? Plan(from, to, found) : null;
	}

	internal static ConversionOp Plan(VisualType from, VisualType to, ConversionKind kind) => kind switch
	{
		ConversionKind.Widen => Widening(SlotValues.KindOf(from), SlotValues.KindOf(to)),
		ConversionKind.Box => ConversionOp.Box,
		ConversionKind.ComponentToEntity => ConversionOp.ComponentToEntity,
		ConversionKind.AssetToHandle => ConversionOp.AssetToHandle,
		ConversionKind.ToText => from.ClrType == typeof(string) ? ConversionOp.None : ConversionOp.ToText,
		// Identity, and upcasts of references (the reference itself is the value).
		_ => from.IsClrReference || !to.IsClrReference ? ConversionOp.None : ConversionOp.Box,
	};

	// The conversion op of a numeric widening between two slot kinds.
	internal static ConversionOp Widening(SlotKind from, SlotKind to) => (from, to) switch
	{
		(SlotKind.Int, SlotKind.Long) => ConversionOp.IntToLong,
		(SlotKind.Int, SlotKind.Float) => ConversionOp.IntToFloat,
		(SlotKind.Int, SlotKind.Double) => ConversionOp.IntToDouble,
		(SlotKind.UInt, SlotKind.Long) => ConversionOp.UIntToLong,
		(SlotKind.UInt, SlotKind.ULong) => ConversionOp.UIntToULong,
		(SlotKind.UInt, SlotKind.Float) => ConversionOp.UIntToFloat,
		(SlotKind.UInt, SlotKind.Double) => ConversionOp.UIntToDouble,
		(SlotKind.Long, SlotKind.Float) => ConversionOp.LongToFloat,
		(SlotKind.Long, SlotKind.Double) => ConversionOp.LongToDouble,
		(SlotKind.ULong, SlotKind.Float) => ConversionOp.ULongToFloat,
		(SlotKind.ULong, SlotKind.Double) => ConversionOp.ULongToDouble,
		(SlotKind.Float, SlotKind.Double) => ConversionOp.FloatToDouble,
		_ => throw new InvalidOperationException($"{from} does not widen to {to}"),
	};

	// Converts the value in place; from is the type of the value before the conversion.
	internal static void Apply(ref Slot slot, ConversionOp op, VisualType from)
	{
		switch (op)
		{
			case ConversionOp.None:
				break;
			case ConversionOp.Box:
				slot = Slot.FromObject(SlotValues.ToValue(slot, from));
				break;
			case ConversionOp.ComponentToEntity:
				slot = Slot.FromObject(((Component?)slot.Object)?.Entity);
				break;
			case ConversionOp.AssetToHandle:
				slot = Slot.FromObject(slot.Object is Asset asset ? asset.Handle : s_InvalidHandle);
				break;
			case ConversionOp.ToText:
				slot = Slot.FromString(ToText(slot, from));
				break;
			default:
				Widen(ref slot, op);
				break;
		}
	}

	// Applies a numeric widening (IntToLong to FloatToDouble) in place.
	internal static void Widen(ref Slot slot, ConversionOp op)
	{
		switch (op)
		{
			case ConversionOp.IntToLong:
				slot = Slot.FromLong(slot.Int);
				break;
			case ConversionOp.IntToFloat:
				slot = Slot.FromFloat(slot.Int);
				break;
			case ConversionOp.IntToDouble:
				slot = Slot.FromDouble(slot.Int);
				break;
			case ConversionOp.UIntToLong:
				slot = Slot.FromLong(slot.UInt);
				break;
			case ConversionOp.UIntToULong:
				slot = Slot.FromULong(slot.UInt);
				break;
			case ConversionOp.UIntToFloat:
				slot = Slot.FromFloat(slot.UInt);
				break;
			case ConversionOp.UIntToDouble:
				slot = Slot.FromDouble(slot.UInt);
				break;
			case ConversionOp.LongToFloat:
				slot = Slot.FromFloat(slot.Long);
				break;
			case ConversionOp.LongToDouble:
				slot = Slot.FromDouble(slot.Long);
				break;
			case ConversionOp.ULongToFloat:
				slot = Slot.FromFloat(slot.ULong);
				break;
			case ConversionOp.ULongToDouble:
				slot = Slot.FromDouble(slot.ULong);
				break;
			case ConversionOp.FloatToDouble:
				slot = Slot.FromDouble(slot.Float);
				break;
		}
	}

	// Rule 7: the invariant-culture text of a value ("(x, y, z)" for vectors; "None" for no reference; "[1, 2, 3]" for arrays,
	// spec-slice3 section 3.4.2 rule 4).
	internal static string ToText(in Slot slot, VisualType type) => SlotValues.KindOf(type) switch
	{
		SlotKind.Bool => slot.Bool ? bool.TrueString : bool.FalseString,
		SlotKind.Int => slot.Int.ToString(CultureInfo.InvariantCulture),
		SlotKind.UInt => slot.UInt.ToString(CultureInfo.InvariantCulture),
		SlotKind.Long => slot.Long.ToString(CultureInfo.InvariantCulture),
		SlotKind.ULong => slot.ULong.ToString(CultureInfo.InvariantCulture),
		SlotKind.Float => slot.Float.ToString(CultureInfo.InvariantCulture),
		SlotKind.Double => slot.Double.ToString(CultureInfo.InvariantCulture),
		SlotKind.Vector2 => slot.Vector2.ToString(),
		SlotKind.Vector3 => slot.Vector3.ToString(),
		SlotKind.Vector4 => slot.Vector4.ToString(),
		SlotKind.Quaternion => slot.Quaternion.ToString(),
		SlotKind.Color => slot.Color.ToString(),
		SlotKind.Enum => SlotValues.EnumOf(type.ClrType!, slot.ULong).ToString(),
		_ when type.Element is { } element && slot.Object is Array array => ArrayText(array, element),
		_ => ToText(slot.Object),
	};

	internal static string ToText(object? value)
	{
		if (value is not Array array)
		{
			return ScalarText(value);
		}
		StringBuilder text = new();
		AppendObjectArray(text, array, 0);
		return text.ToString();
	}

	// An array's elements by rule 7, separated by ", ", in brackets ("[]" when empty).
	private static string ArrayText(Array array, VisualType element)
	{
		StringBuilder text = new("[");
		for (int index = 0; index < array.Length; index++)
		{
			text.Append(index > 0 ? ", " : string.Empty).Append(ToText(ArrayElements.Read(array, index, element), element));
		}
		return text.Append(']').ToString();
	}

	private static string ScalarText(object? value) => value switch
	{
		null => NoneText,
		string text => text,
		IFormattable formattable => formattable.ToString(null, CultureInfo.InvariantCulture),
		_ => value.ToString() ?? string.Empty,
	};

	// An array of a type the library may not have (one held as a System.Object): its elements, nested arrays to
	// MaxArrayTextDepth, and "..." for the rest once the text is MaxObjectArrayText long.
	private static void AppendObjectArray(StringBuilder text, Array array, int depth)
	{
		text.Append('[');
		int index = 0;
		foreach (object? element in array)
		{
			if (text.Length >= MaxObjectArrayText)
			{
				text.Append(index > 0 ? ", ..." : "...");
				break;
			}
			text.Append(index++ > 0 ? ", " : string.Empty);
			if (element is not Array nested)
			{
				text.Append(ScalarText(element));
			}
			else if (depth + 1 < MaxArrayTextDepth)
			{
				AppendObjectArray(text, nested, depth + 1);
			}
			else
			{
				text.Append("[...]");
			}
		}
		text.Append(']');
	}

	// Convert's numeric casts (spec section 4.4): C# unchecked casts, except that floating-point values truncate toward zero
	// and saturate when they become integers, NaN giving 0.
	internal static Slot NumericCast(in Slot slot, SlotKind from, SlotKind to)
	{
		switch (from)
		{
			case SlotKind.Float:
				return FromFloating(slot.Float, to);
			case SlotKind.Double:
				return FromFloating(slot.Double, to);
			case SlotKind.UInt:
				return FromUnsigned(slot.UInt, to);
			case SlotKind.ULong:
				return FromUnsigned(slot.ULong, to);
			case SlotKind.Int:
				return FromSigned(slot.Int, to);
			default:
				return FromSigned(slot.Long, to);
		}
	}

	private static Slot FromSigned(long value, SlotKind to) => to switch
	{
		SlotKind.Int => Slot.FromInt(unchecked((int)value)),
		SlotKind.UInt => Slot.FromUInt(unchecked((uint)value)),
		SlotKind.Long => Slot.FromLong(value),
		SlotKind.ULong => Slot.FromULong(unchecked((ulong)value)),
		SlotKind.Float => Slot.FromFloat(value),
		_ => Slot.FromDouble(value),
	};

	private static Slot FromUnsigned(ulong value, SlotKind to) => to switch
	{
		SlotKind.Int => Slot.FromInt(unchecked((int)value)),
		SlotKind.UInt => Slot.FromUInt(unchecked((uint)value)),
		SlotKind.Long => Slot.FromLong(unchecked((long)value)),
		SlotKind.ULong => Slot.FromULong(value),
		SlotKind.Float => Slot.FromFloat(value),
		_ => Slot.FromDouble(value),
	};

	private static Slot FromFloating(double value, SlotKind to) => to switch
	{
		SlotKind.Int => Slot.FromInt(double.IsNaN(value) ? 0 : value <= int.MinValue ? int.MinValue : value >= int.MaxValue ? int.MaxValue : (int)value),
		SlotKind.UInt => Slot.FromUInt(double.IsNaN(value) || value <= 0.0 ? 0u : value >= uint.MaxValue ? uint.MaxValue : (uint)value),
		SlotKind.Long => Slot.FromLong(double.IsNaN(value) ? 0L : value <= -TwoToThe63 ? long.MinValue : value >= TwoToThe63 ? long.MaxValue : (long)value),
		SlotKind.ULong => Slot.FromULong(double.IsNaN(value) || value <= 0.0 ? 0UL : value >= TwoToThe64 ? ulong.MaxValue : (ulong)value),
		SlotKind.Float => Slot.FromFloat((float)value),
		_ => Slot.FromDouble(value),
	};
}
