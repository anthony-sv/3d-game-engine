using System;
using System.Runtime.InteropServices;

namespace Strada.VisualScripting;

// Where a slot holds a value of a type (spec section 5.3, Values).
internal enum SlotKind : byte
{
	Bool,
	Int,
	UInt,
	Long,
	ULong,
	Float,
	Double,
	Vector2,
	Vector3,
	Vector4,
	Quaternion,
	Color,
	// An enum value: the bits of its underlying value in ULong, sign-extended for signed underlying types.
	Enum,
	// A reference (strings, entities, script instances, components, assets, arrays, other classes) or a boxed struct
	// (Matrix4 and every struct the union cannot hold). Boxes are immutable: a new value is a new box.
	Object,
}

// A value of a running graph: an output, a staged input, a variable, a local or the state of a node. Booleans, numbers,
// enums and the math structs of up to 16 bytes are held in place, so evaluating them never allocates; every other value is
// the object at offset 16. Writing a slot always writes all of it, so a value never keeps a stale object alive.
[StructLayout(LayoutKind.Explicit, Size = 24)]
internal struct Slot
{
	[FieldOffset(0)]
	internal bool Bool;

	[FieldOffset(0)]
	internal int Int;

	[FieldOffset(0)]
	internal uint UInt;

	[FieldOffset(0)]
	internal long Long;

	[FieldOffset(0)]
	internal ulong ULong;

	[FieldOffset(0)]
	internal float Float;

	[FieldOffset(0)]
	internal double Double;

	[FieldOffset(0)]
	internal Vector2 Vector2;

	[FieldOffset(0)]
	internal Vector3 Vector3;

	[FieldOffset(0)]
	internal Vector4 Vector4;

	[FieldOffset(0)]
	internal Quaternion Quaternion;

	[FieldOffset(0)]
	internal Color Color;

	[FieldOffset(16)]
	internal object? Object;

	internal static Slot FromBool(bool value) => new() { Bool = value };

	internal static Slot FromInt(int value) => new() { Int = value };

	internal static Slot FromUInt(uint value) => new() { UInt = value };

	internal static Slot FromLong(long value) => new() { Long = value };

	internal static Slot FromULong(ulong value) => new() { ULong = value };

	internal static Slot FromFloat(float value) => new() { Float = value };

	internal static Slot FromDouble(double value) => new() { Double = value };

	internal static Slot FromVector2(Vector2 value) => new() { Vector2 = value };

	internal static Slot FromVector3(Vector3 value) => new() { Vector3 = value };

	internal static Slot FromVector4(Vector4 value) => new() { Vector4 = value };

	internal static Slot FromQuaternion(Quaternion value) => new() { Quaternion = value };

	internal static Slot FromColor(Color value) => new() { Color = value };

	internal static Slot FromObject(object? value) => new() { Object = value };

	// Strings are never null in a graph (spec section 2.2): a member that returns null gives "".
	internal static Slot FromString(string? value) => new() { Object = value ?? string.Empty };
}

// Values between slots and objects: the kind of slot each type uses, boxing and unboxing, enum bits and type defaults.
internal static class SlotValues
{
	internal static SlotKind KindOf(VisualType type) => type.ClrType is { } clrType ? KindOf(clrType) : SlotKind.Object;

	internal static SlotKind KindOf(Type type)
	{
		if (type.IsEnum)
		{
			return SlotKind.Enum;
		}
		return type switch
		{
			_ when type == typeof(bool) => SlotKind.Bool,
			_ when type == typeof(int) => SlotKind.Int,
			_ when type == typeof(uint) => SlotKind.UInt,
			_ when type == typeof(long) => SlotKind.Long,
			_ when type == typeof(ulong) => SlotKind.ULong,
			_ when type == typeof(float) => SlotKind.Float,
			_ when type == typeof(double) => SlotKind.Double,
			_ when type == typeof(Vector2) => SlotKind.Vector2,
			_ when type == typeof(Vector3) => SlotKind.Vector3,
			_ when type == typeof(Vector4) => SlotKind.Vector4,
			_ when type == typeof(Quaternion) => SlotKind.Quaternion,
			_ when type == typeof(Color) => SlotKind.Color,
			_ => SlotKind.Object,
		};
	}

	// The slot of a value boxed as the type's CLR type (null for no reference).
	internal static Slot FromValue(object? value, VisualType type) => KindOf(type) switch
	{
		SlotKind.Bool => Slot.FromBool((bool)value!),
		SlotKind.Int => Slot.FromInt((int)value!),
		SlotKind.UInt => Slot.FromUInt((uint)value!),
		SlotKind.Long => Slot.FromLong((long)value!),
		SlotKind.ULong => Slot.FromULong((ulong)value!),
		SlotKind.Float => Slot.FromFloat((float)value!),
		SlotKind.Double => Slot.FromDouble((double)value!),
		SlotKind.Vector2 => Slot.FromVector2((Vector2)value!),
		SlotKind.Vector3 => Slot.FromVector3((Vector3)value!),
		SlotKind.Vector4 => Slot.FromVector4((Vector4)value!),
		SlotKind.Quaternion => Slot.FromQuaternion((Quaternion)value!),
		SlotKind.Color => Slot.FromColor((Color)value!),
		SlotKind.Enum => Slot.FromULong(LiteralCodec.ToBits(value!)),
		_ when type.ClrType == typeof(string) => Slot.FromString((string?)value),
		_ => Slot.FromObject(value),
	};

	// The value of a slot of the type, boxed as its CLR type.
	internal static object? ToValue(in Slot slot, VisualType type) => KindOf(type) switch
	{
		SlotKind.Bool => slot.Bool,
		SlotKind.Int => slot.Int,
		SlotKind.UInt => slot.UInt,
		SlotKind.Long => slot.Long,
		SlotKind.ULong => slot.ULong,
		SlotKind.Float => slot.Float,
		SlotKind.Double => slot.Double,
		SlotKind.Vector2 => slot.Vector2,
		SlotKind.Vector3 => slot.Vector3,
		SlotKind.Vector4 => slot.Vector4,
		SlotKind.Quaternion => slot.Quaternion,
		SlotKind.Color => slot.Color,
		SlotKind.Enum => EnumOf(type.ClrType!, slot.ULong),
		_ => slot.Object,
	};

	// The slot of the type default (spec section 2.2). Boxed defaults are shared: boxes are never changed.
	internal static Slot DefaultOf(VisualType type) => FromValue(LiteralCodec.DefaultOf(type), type);

	// An enum value from the bits of its underlying value.
	internal static Enum EnumOf(Type enumType, ulong bits) => (Enum)(IsSigned(Enum.GetUnderlyingType(enumType))
		? Enum.ToObject(enumType, unchecked((long)bits))
		: Enum.ToObject(enumType, bits));

	// The bits an integer value takes as a value of an enum's underlying type: truncated to its width, then sign-extended
	// for signed types (spec section 4.4: an integer converts to any value of the underlying type, as unchecked C# casts do).
	internal static ulong ToEnumBits(ulong value, Type enumType) => Type.GetTypeCode(Enum.GetUnderlyingType(enumType)) switch
	{
		TypeCode.SByte => unchecked((ulong)(sbyte)value),
		TypeCode.Byte => unchecked((byte)value),
		TypeCode.Int16 => unchecked((ulong)(short)value),
		TypeCode.UInt16 => unchecked((ushort)value),
		TypeCode.Int32 => unchecked((ulong)(int)value),
		TypeCode.UInt32 => unchecked((uint)value),
		_ => value,
	};

	internal static bool IsSigned(Type integerType)
	{
		TypeCode code = Type.GetTypeCode(integerType);
		return code is TypeCode.SByte or TypeCode.Int16 or TypeCode.Int32 or TypeCode.Int64;
	}
}
