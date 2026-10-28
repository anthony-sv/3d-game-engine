using System;
using System.Globalization;
using System.Runtime.CompilerServices;

namespace Strada.VisualScripting;

// The String and Array nodes (spec-slice3 sections 3.4.5 and 3.5): pure, one step each, evaluated from the node's staged
// inputs into its outputs. They allocate only their results. Strings compare ordinally and map case invariantly; lengths
// and indices count UTF-16 code units; no string node stops an execution on its own data. No array node changes an array:
// the ones that "change" one make a new one, of at most MaxMadeArrayElements elements.
internal static class Intrinsics
{
	internal const int MaxMadeArrayElements = 65_536;
	internal const string MadeArrayMessage = "arrays made in graphs hold at most 65,536 elements";
	// Arrays are never null in a graph (spec-slice3 section 3.4.2): the guard of that invariant.
	internal const string NoArray = "Array is None";

	// node.Invoker is the element equality operator of Contains and IndexOf when their element type has one.
	internal static void Evaluate(CompiledNode node, Slot[] slots)
	{
		int input = node.InputBase;
		int output = node.OutputBase;
		switch (node.Intrinsic)
		{
			case IntrinsicKind.StringLength:
				slots[output] = Slot.FromInt(Text(slots, input).Length);
				break;
			case IntrinsicKind.StringIsEmpty:
				slots[output] = Slot.FromBool(Text(slots, input).Length == 0);
				break;
			case IntrinsicKind.StringContains:
				slots[output] = Slot.FromBool(Text(slots, input).Contains(Text(slots, input + 1), StringComparison.Ordinal));
				break;
			case IntrinsicKind.StringStartsWith:
				slots[output] = Slot.FromBool(Text(slots, input).StartsWith(Text(slots, input + 1), StringComparison.Ordinal));
				break;
			case IntrinsicKind.StringEndsWith:
				slots[output] = Slot.FromBool(Text(slots, input).EndsWith(Text(slots, input + 1), StringComparison.Ordinal));
				break;
			case IntrinsicKind.StringIndexOf:
				string searched = Text(slots, input);
				int from = Math.Clamp(slots[input + 2].Int, 0, searched.Length);
				slots[output] = Slot.FromInt(searched.IndexOf(Text(slots, input + 1), from, StringComparison.Ordinal));
				break;
			case IntrinsicKind.StringSubstring:
				slots[output] = Slot.FromString(Substring(Text(slots, input), slots[input + 1].Int, slots[input + 2].Int));
				break;
			case IntrinsicKind.StringReplace:
				string text = Text(slots, input);
				string old = Text(slots, input + 1);
				// Replacing "" would throw in C#; there is nothing to replace.
				slots[output] = Slot.FromString(old.Length == 0 ? text : text.Replace(old, Text(slots, input + 2), StringComparison.Ordinal));
				break;
			case IntrinsicKind.StringToUpper:
				slots[output] = Slot.FromString(Text(slots, input).ToUpperInvariant());
				break;
			case IntrinsicKind.StringToLower:
				slots[output] = Slot.FromString(Text(slots, input).ToLowerInvariant());
				break;
			case IntrinsicKind.StringTrim:
				slots[output] = Slot.FromString(Text(slots, input).Trim());
				break;
			case IntrinsicKind.StringSplit:
				slots[output] = Slot.FromObject(Split(Text(slots, input), Text(slots, input + 1), slots[input + 2].Bool));
				break;
			case IntrinsicKind.StringJoin:
				slots[output] = Slot.FromString(Join(ArrayOf(slots, input), Text(slots, input + 1), node.FromType!));
				break;
			case IntrinsicKind.StringToInt:
				bool isInt = int.TryParse(Text(slots, input), NumberStyles.Integer, CultureInfo.InvariantCulture, out int integer);
				slots[output] = Slot.FromInt(isInt ? integer : 0);
				slots[output + 1] = Slot.FromBool(isInt);
				break;
			case IntrinsicKind.StringToFloat:
				bool isFloat = float.TryParse(Text(slots, input), NumberStyles.Float, CultureInfo.InvariantCulture, out float number) &&
					float.IsFinite(number);
				slots[output] = Slot.FromFloat(isFloat ? number : 0.0f);
				slots[output + 1] = Slot.FromBool(isFloat);
				break;
			case IntrinsicKind.ArrayContains:
				slots[output] = Slot.FromBool(IndexOf(node, ArrayOf(slots, input), slots[input + 1], 0, slots) >= 0);
				break;
			case IntrinsicKind.ArrayIndexOf:
				Array elements = ArrayOf(slots, input);
				slots[output] = Slot.FromInt(IndexOf(node, elements, slots[input + 1], Math.Clamp(slots[input + 2].Int, 0, elements.Length), slots));
				break;
			case IntrinsicKind.ArrayMake:
				slots[output] = Slot.FromObject(Make(node, slots, input));
				break;
			case IntrinsicKind.ArraySet:
				slots[output] = Slot.FromObject(Set(node, ArrayOf(slots, input), slots[input + 1].Int, slots[input + 2]));
				break;
			case IntrinsicKind.ArrayAdd:
				Array added = ArrayOf(slots, input);
				slots[output] = Slot.FromObject(Insert(node, added, added.Length, slots[input + 1]));
				break;
			case IntrinsicKind.ArrayRemoveAt:
				slots[output] = Slot.FromObject(RemoveAt(node, ArrayOf(slots, input), slots[input + 1].Int));
				break;
			case IntrinsicKind.ArrayInsert:
				Array target = ArrayOf(slots, input);
				int index = slots[input + 1].Int;
				if (index < 0 || index > target.Length)
				{
					throw new VisualScriptException(
						FormattableString.Invariant($"index {index} is outside [0, {target.Length}] where elements can be inserted"));
				}
				slots[output] = Slot.FromObject(Insert(node, target, index, slots[input + 2]));
				break;
			default:
				slots[output] = Slot.FromObject(Append(node, ArrayOf(slots, input), ArrayOf(slots, input + 1)));
				break;
		}
	}

	// Strings are never null in a graph (spec section 2.2).
	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	private static string Text(Slot[] slots, int slot) => (string?)slots[slot].Object ?? string.Empty;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	private static Array ArrayOf(Slot[] slots, int slot) => slots[slot].Object as Array ?? throw new VisualScriptException(NoArray);

	// Clamped, so a slice past the end is the part of the text there is: s = clamp(start, 0, n), and length characters from
	// it, or all the rest for a negative length.
	private static string Substring(string text, int start, int length)
	{
		int from = Math.Clamp(start, 0, text.Length);
		int count = length < 0 ? text.Length - from : Math.Min(length, text.Length - from);
		return text.Substring(from, count);
	}

	// C#'s ordinal Split: an empty separator gives the text itself; RemoveEmpty leaves out empty parts ("" then gives none).
	private static string[] Split(string text, string separator, bool removeEmpty)
	{
		string[] parts = text.Split(separator, removeEmpty ? StringSplitOptions.RemoveEmptyEntries : StringSplitOptions.None);
		if (parts.Length > MaxMadeArrayElements)
		{
			throw new VisualScriptException(MadeArrayMessage);
		}
		return parts;
	}

	// The elements as text by spec section 3.4 rule 7 (invariant, "None" for no reference, floats in their shortest form).
	private static string Join(Array parts, string separator, VisualType element)
	{
		if (parts.Length == 0)
		{
			return string.Empty;
		}
		DefaultInterpolatedStringHandler text = new(0, parts.Length, CultureInfo.InvariantCulture);
		for (int index = 0; index < parts.Length; index++)
		{
			if (index > 0)
			{
				text.AppendLiteral(separator);
			}
			text.AppendLiteral(Conversions.ToText(ArrayElements.Read(parts, index, element), element));
		}
		return text.ToStringAndClear();
	}

	// The first index from start whose element equals value, by the element type's equality (spec-slice3 section 3.4.2 rule 3),
	// compared in the node's scratch slots; -1 when there is none.
	private static int IndexOf(CompiledNode node, Array array, in Slot value, int start, Slot[] slots)
	{
		VisualType element = node.FromType!;
		int scratch = node.ScratchBase;
		for (int index = start; index < array.Length; index++)
		{
			slots[scratch] = ArrayElements.Read(array, index, element);
			slots[scratch + 1] = value;
			if (ValueEquality.Equal(node.ElementEquality, node.Kind, node.Invoker, slots, scratch, scratch + 2))
			{
				return index;
			}
		}
		return -1;
	}

	private static Array Make(CompiledNode node, Slot[] slots, int input)
	{
		VisualType element = node.FromType!;
		if (node.Count == 0)
		{
			return EmptyOf(node);
		}
		Array array = Array.CreateInstance(element.ClrType!, node.Count);
		for (int index = 0; index < node.Count; index++)
		{
			ArrayElements.Write(array, index, element, slots[input + index]);
		}
		return array;
	}

	private static Array Set(CompiledNode node, Array array, int index, in Slot value)
	{
		CheckIndex(array, index);
		CheckMade(array.Length);
		Array copy = (Array)array.Clone();
		ArrayElements.Write(copy, index, node.FromType!, value);
		return copy;
	}

	// A copy of the array with value before the element at index (index Length appends).
	private static Array Insert(CompiledNode node, Array array, int index, in Slot value)
	{
		CheckMade(array.Length + 1L);
		Array result = Array.CreateInstance(node.FromType!.ClrType!, array.Length + 1);
		Array.Copy(array, 0, result, 0, index);
		ArrayElements.Write(result, index, node.FromType!, value);
		Array.Copy(array, index, result, index + 1, array.Length - index);
		return result;
	}

	private static Array RemoveAt(CompiledNode node, Array array, int index)
	{
		CheckIndex(array, index);
		CheckMade(array.Length - 1);
		if (array.Length == 1)
		{
			return EmptyOf(node);
		}
		Array result = Array.CreateInstance(node.FromType!.ClrType!, array.Length - 1);
		Array.Copy(array, 0, result, 0, index);
		Array.Copy(array, index + 1, result, index, array.Length - index - 1);
		return result;
	}

	// One copy of each, so joining arrays is linear.
	private static Array Append(CompiledNode node, Array a, Array b)
	{
		long length = (long)a.Length + b.Length;
		CheckMade(length);
		if (length == 0)
		{
			return EmptyOf(node);
		}
		Array result = Array.CreateInstance(node.FromType!.ClrType!, (int)length);
		Array.Copy(a, 0, result, 0, a.Length);
		Array.Copy(b, 0, result, a.Length, b.Length);
		return result;
	}

	private static Array EmptyOf(CompiledNode node) => node.ToType!.EmptyArray ?? Array.CreateInstance(node.FromType!.ClrType!, 0);

	private static void CheckIndex(Array array, int index)
	{
		if (index < 0 || index >= array.Length)
		{
			throw new VisualScriptException(FormattableString.Invariant($"index {index} is outside the array (length {array.Length})"));
		}
	}

	private static void CheckMade(long length)
	{
		if (length > MaxMadeArrayElements)
		{
			throw new VisualScriptException(MadeArrayMessage);
		}
	}
}

// The equality table (spec section 4.4) of two values staged in consecutive slots, and of arrays element by element
// (spec-slice3 section 3.4.2 rule 3). An equality operator method writes its result to a slot of its own.
internal static class ValueEquality
{
	// Whether slots[at] equals slots[at + 1]; a Method equality calls invoker, which writes its result to slots[result].
	internal static bool Equal(EqualityKind kind, SlotKind slotKind, MemberInvoker? invoker, Slot[] slots, int at, int result)
	{
		ref readonly Slot a = ref slots[at];
		ref readonly Slot b = ref slots[at + 1];
		switch (kind)
		{
			case EqualityKind.Numeric:
				return Operators.Equal(slotKind, a, b);
			case EqualityKind.Bool:
				return a.Bool == b.Bool;
			case EqualityKind.String:
				return string.Equals((string?)a.Object, (string?)b.Object, StringComparison.Ordinal);
			case EqualityKind.Enum:
				return a.ULong == b.ULong;
			case EqualityKind.Entity:
				return (Entity?)a.Object == (Entity?)b.Object;
			case EqualityKind.Asset:
				return (Asset?)a.Object == (Asset?)b.Object;
			case EqualityKind.Component:
				if (a.Object is not Component first || b.Object is not Component second)
				{
					return a.Object == null && b.Object == null;
				}
				return first.GetType() == second.GetType() && first.Entity?.ID == second.Entity?.ID;
			case EqualityKind.Method:
				invoker!(slots, at, result);
				return slots[result].Bool;
			default:
				return Equals(a.Object, b.Object);
		}
	}

	// Arrays of the node's element type: the same length and equal elements in order, compared in the node's scratch slots.
	internal static bool ArraysEqual(CompiledNode node, Array a, Array b, Slot[] slots)
	{
		if (a.Length != b.Length)
		{
			return false;
		}
		VisualType element = node.FromType!;
		int scratch = node.ScratchBase;
		for (int index = 0; index < a.Length; index++)
		{
			slots[scratch] = ArrayElements.Read(a, index, element);
			slots[scratch + 1] = ArrayElements.Read(b, index, element);
			if (!Equal(node.ElementEquality, node.Kind, node.Invoker, slots, scratch, scratch + 2))
			{
				return false;
			}
		}
		return true;
	}
}
