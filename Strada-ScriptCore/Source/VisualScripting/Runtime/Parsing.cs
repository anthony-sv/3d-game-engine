using System;
using System.Globalization;

namespace Strada.VisualScripting;

// Convert from text (spec section 4.4): bool from "true" or "false" in any case, numbers in the invariant culture, enums
// by enumerator name.
internal static class Parsing
{
	internal static bool TryParse(ConvertPlan plan, string text, VisualType to, out Slot value)
	{
		value = default;
		switch (plan)
		{
			case ConvertPlan.ParseBool:
				bool isTrue = string.Equals(text, bool.TrueString, StringComparison.OrdinalIgnoreCase);
				if (isTrue || string.Equals(text, bool.FalseString, StringComparison.OrdinalIgnoreCase))
				{
					value = Slot.FromBool(isTrue);
					return true;
				}
				return false;
			case ConvertPlan.ParseEnum:
				foreach (EnumeratorInfo enumerator in to.Enumerators)
				{
					if (enumerator.Name == text)
					{
						value = Slot.FromULong(LiteralCodec.ToBits(enumerator.Value));
						return true;
					}
				}
				return false;
			default:
				return TryParseNumber(text, SlotValues.KindOf(to), out value);
		}
	}

	private static bool TryParseNumber(string text, SlotKind kind, out Slot value)
	{
		const NumberStyles integer = NumberStyles.Integer;
		const NumberStyles floating = NumberStyles.Float | NumberStyles.AllowThousands;
		CultureInfo culture = CultureInfo.InvariantCulture;
		value = default;
		switch (kind)
		{
			case SlotKind.Int when int.TryParse(text, integer, culture, out int number):
				value = Slot.FromInt(number);
				return true;
			case SlotKind.UInt when uint.TryParse(text, integer, culture, out uint number):
				value = Slot.FromUInt(number);
				return true;
			case SlotKind.Long when long.TryParse(text, integer, culture, out long number):
				value = Slot.FromLong(number);
				return true;
			case SlotKind.ULong when ulong.TryParse(text, integer, culture, out ulong number):
				value = Slot.FromULong(number);
				return true;
			case SlotKind.Float when float.TryParse(text, floating, culture, out float number):
				value = Slot.FromFloat(number);
				return true;
			case SlotKind.Double when double.TryParse(text, floating, culture, out double number):
				value = Slot.FromDouble(number);
				return true;
			default:
				return false;
		}
	}
}
