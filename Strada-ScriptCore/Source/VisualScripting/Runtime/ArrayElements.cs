using System;

namespace Strada.VisualScripting;

// Reads and writes elements of arrays as slots (spec-slice3 section 3.4.2 rule 5): the element types a slot holds in place
// have a typed path that neither boxes nor allocates, enum arrays through the integer array of their underlying type (the
// CLR lets an enum array be read as one), and references are read through the array's object view. Other structs box.
internal static class ArrayElements
{
	internal static Slot Read(Array array, int index, VisualType element)
	{
		Type type = element.ClrType!;
		object view = array;
		switch (SlotValues.KindOf(type))
		{
			case SlotKind.Object when !type.IsValueType:
				object? value = ((object?[])view)[index];
				return type == typeof(string) ? Slot.FromString((string?)value) : Slot.FromObject(value);
			case SlotKind.Bool when view is bool[] bools:
				return Slot.FromBool(bools[index]);
			case SlotKind.Int when view is int[] ints:
				return Slot.FromInt(ints[index]);
			case SlotKind.UInt when view is uint[] uints:
				return Slot.FromUInt(uints[index]);
			case SlotKind.Long when view is long[] longs:
				return Slot.FromLong(longs[index]);
			case SlotKind.ULong when view is ulong[] ulongs:
				return Slot.FromULong(ulongs[index]);
			case SlotKind.Float when view is float[] floats:
				return Slot.FromFloat(floats[index]);
			case SlotKind.Double when view is double[] doubles:
				return Slot.FromDouble(doubles[index]);
			case SlotKind.Vector2 when view is Vector2[] vectors:
				return Slot.FromVector2(vectors[index]);
			case SlotKind.Vector3 when view is Vector3[] vectors:
				return Slot.FromVector3(vectors[index]);
			case SlotKind.Vector4 when view is Vector4[] vectors:
				return Slot.FromVector4(vectors[index]);
			case SlotKind.Quaternion when view is Quaternion[] quaternions:
				return Slot.FromQuaternion(quaternions[index]);
			case SlotKind.Color when view is Color[] colors:
				return Slot.FromColor(colors[index]);
			case SlotKind.Enum:
				return Slot.FromULong(ReadEnumBits(view, index, type));
			default:
				return SlotValues.FromValue(array.GetValue(index), element);
		}
	}

	internal static void Write(Array array, int index, VisualType element, in Slot value)
	{
		Type type = element.ClrType!;
		object view = array;
		switch (SlotValues.KindOf(type))
		{
			case SlotKind.Object when !type.IsValueType:
				((object?[])view)[index] = value.Object;
				break;
			case SlotKind.Bool when view is bool[] bools:
				bools[index] = value.Bool;
				break;
			case SlotKind.Int when view is int[] ints:
				ints[index] = value.Int;
				break;
			case SlotKind.UInt when view is uint[] uints:
				uints[index] = value.UInt;
				break;
			case SlotKind.Long when view is long[] longs:
				longs[index] = value.Long;
				break;
			case SlotKind.ULong when view is ulong[] ulongs:
				ulongs[index] = value.ULong;
				break;
			case SlotKind.Float when view is float[] floats:
				floats[index] = value.Float;
				break;
			case SlotKind.Double when view is double[] doubles:
				doubles[index] = value.Double;
				break;
			case SlotKind.Vector2 when view is Vector2[] vectors:
				vectors[index] = value.Vector2;
				break;
			case SlotKind.Vector3 when view is Vector3[] vectors:
				vectors[index] = value.Vector3;
				break;
			case SlotKind.Vector4 when view is Vector4[] vectors:
				vectors[index] = value.Vector4;
				break;
			case SlotKind.Quaternion when view is Quaternion[] quaternions:
				quaternions[index] = value.Quaternion;
				break;
			case SlotKind.Color when view is Color[] colors:
				colors[index] = value.Color;
				break;
			case SlotKind.Enum:
				WriteEnumBits(view, index, type, value.ULong);
				break;
			default:
				array.SetValue(SlotValues.ToValue(value, element), index);
				break;
		}
	}

	// The bits of an enum element: its underlying value, sign-extended for signed underlying types (as slots hold enums).
	private static ulong ReadEnumBits(object array, int index, Type enumType) => Type.GetTypeCode(Enum.GetUnderlyingType(enumType)) switch
	{
		TypeCode.SByte => unchecked((ulong)((sbyte[])array)[index]),
		TypeCode.Byte => ((byte[])array)[index],
		TypeCode.Int16 => unchecked((ulong)((short[])array)[index]),
		TypeCode.UInt16 => ((ushort[])array)[index],
		TypeCode.Int32 => unchecked((ulong)((int[])array)[index]),
		TypeCode.UInt32 => ((uint[])array)[index],
		TypeCode.Int64 => unchecked((ulong)((long[])array)[index]),
		_ => ((ulong[])array)[index],
	};

	private static void WriteEnumBits(object array, int index, Type enumType, ulong bits)
	{
		switch (Type.GetTypeCode(Enum.GetUnderlyingType(enumType)))
		{
			case TypeCode.SByte:
				((sbyte[])array)[index] = unchecked((sbyte)bits);
				break;
			case TypeCode.Byte:
				((byte[])array)[index] = unchecked((byte)bits);
				break;
			case TypeCode.Int16:
				((short[])array)[index] = unchecked((short)bits);
				break;
			case TypeCode.UInt16:
				((ushort[])array)[index] = unchecked((ushort)bits);
				break;
			case TypeCode.Int32:
				((int[])array)[index] = unchecked((int)bits);
				break;
			case TypeCode.UInt32:
				((uint[])array)[index] = unchecked((uint)bits);
				break;
			case TypeCode.Int64:
				((long[])array)[index] = unchecked((long)bits);
				break;
			default:
				((ulong[])array)[index] = bits;
				break;
		}
	}
}
