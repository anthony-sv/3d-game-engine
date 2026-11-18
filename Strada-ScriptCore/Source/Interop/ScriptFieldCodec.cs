using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Text.Json;
using Strada.VisualScripting;

namespace Strada.Interop;

// The engine's ScriptFieldType; the names are those of scene files (ScriptField.cpp). These are the element types: the list
// of one is the element type with ScriptFieldCodec.ListFlag set (ScriptFieldCodec.MakeListType), named after it with "[]"
// (ScriptFieldCodec.GetTypeName), as the engine's list types are.
internal enum ScriptFieldType
{
	None,
	Bool,
	Int32,
	UInt32,
	Int64,
	UInt64,
	Float,
	Double,
	String,
	Vector2,
	Vector3,
	Vector4,
	Quaternion,
	Color,
	Entity,
	Prefab,
	Asset,
}

// Field values in the JSON encoding of scene files: 64-bit integers, entity IDs and asset handles as decimal strings (0 for
// no entity or prefab, which are null in C#), vectors, colors and quaternions (X, Y, Z, W) as number arrays, and a list as
// the array of its elements' values (at most MaxListElements). Enum fields are stored as their integer values and typed
// asset references (Mesh, Material, ...) as asset handles: ToStored and FromStored convert between the field's C# value
// and the stored one, a list's stored value being a List<object?> of its elements' (Docs/Architecture.md §7.3, §10.3).
internal static class ScriptFieldCodec
{
	// The bit of a list type, as the engine's ScriptFieldListFlag.
	internal const int ListFlag = 0x80;

	// The most elements a list holds in files and across the boundary (the engine's MaxScriptFieldListElements).
	internal const int MaxListElements = 4096;

	internal static bool IsList(ScriptFieldType type) => ((int)type & ListFlag) != 0;

	// The element type of a list type; an element type is its own.
	internal static ScriptFieldType GetElementType(ScriptFieldType type) => (ScriptFieldType)((int)type & ~ListFlag);

	// The list type of an element type; None for None and for a list type (there are no lists of lists).
	internal static ScriptFieldType MakeListType(ScriptFieldType elementType) =>
		elementType == ScriptFieldType.None || IsList(elementType) ? ScriptFieldType.None : (ScriptFieldType)((int)elementType | ListFlag);

	// The type's name in files and across the boundary: "Float", "Float[]" for a list.
	internal static string GetTypeName(ScriptFieldType type) => IsList(type) ? $"{GetElementType(type)}[]" : type.ToString();

	// The field type of a C# script field: GetFieldType's for a single value, and the list of the element's for a
	// single-dimensional array T[] or a List<T> whose T has one (no arrays or lists of them); None otherwise.
	internal static ScriptFieldType GetMemberFieldType(Type type)
	{
		Type? elementType = GetListElementType(type);
		return elementType == null ? GetFieldType(type) : MakeListType(GetFieldType(elementType));
	}

	// T of a single-dimensional array T[] or a List<T>; null for other types.
	internal static Type? GetListElementType(Type type)
	{
		if (type.IsSZArray)
		{
			return type.GetElementType();
		}
		return type.IsGenericType && type.GetGenericTypeDefinition() == typeof(List<>) ? type.GetGenericArguments()[0] : null;
	}

	// The field type of a single value of the C# type: an element type, or None (lists are GetMemberFieldType's).
	internal static ScriptFieldType GetFieldType(Type type)
	{
		if (type.IsEnum)
		{
			return Type.GetTypeCode(Enum.GetUnderlyingType(type)) switch
			{
				TypeCode.SByte or TypeCode.Byte or TypeCode.Int16 or TypeCode.UInt16 or TypeCode.Int32 => ScriptFieldType.Int32,
				TypeCode.UInt32 => ScriptFieldType.UInt32,
				TypeCode.Int64 => ScriptFieldType.Int64,
				TypeCode.UInt64 => ScriptFieldType.UInt64,
				_ => ScriptFieldType.None,
			};
		}
		if (GetAssetType(type) != null)
		{
			return ScriptFieldType.Asset;
		}
		return type switch
		{
			_ when type == typeof(bool) => ScriptFieldType.Bool,
			_ when type == typeof(int) => ScriptFieldType.Int32,
			_ when type == typeof(uint) => ScriptFieldType.UInt32,
			_ when type == typeof(long) => ScriptFieldType.Int64,
			_ when type == typeof(ulong) => ScriptFieldType.UInt64,
			_ when type == typeof(float) => ScriptFieldType.Float,
			_ when type == typeof(double) => ScriptFieldType.Double,
			_ when type == typeof(string) => ScriptFieldType.String,
			_ when type == typeof(Vector2) => ScriptFieldType.Vector2,
			_ when type == typeof(Vector3) => ScriptFieldType.Vector3,
			_ when type == typeof(Vector4) => ScriptFieldType.Vector4,
			_ when type == typeof(Quaternion) => ScriptFieldType.Quaternion,
			_ when type == typeof(Color) => ScriptFieldType.Color,
			_ when type == typeof(Entity) => ScriptFieldType.Entity,
			_ when type == typeof(Prefab) => ScriptFieldType.Prefab,
			_ when type == typeof(AssetHandle) => ScriptFieldType.Asset,
			_ => ScriptFieldType.None,
		};
	}

	// The engine's name of the asset type of typed asset reference fields (Prefab fields have their own field type); null
	// otherwise.
	internal static string? GetAssetType(Type type)
	{
		string? assetType = AssetTypes.GetTypeName(type);
		return assetType == "Prefab" ? null : assetType;
	}

	// The stored form of a field's value: enums as integers of the field type, typed asset references as handles, a list (an
	// array or a List<T>; null is the empty list) as a List<object?> of its elements' stored forms.
	internal static object? ToStored(object? value, ScriptFieldType type)
	{
		if (IsList(type))
		{
			ScriptFieldType elementType = GetElementType(type);
			List<object?> elements = [];
			if (value is IList list)
			{
				elements.Capacity = list.Count;
				foreach (object? element in list)
				{
					elements.Add(ToStored(element, elementType));
				}
			}
			return elements;
		}
		return value switch
		{
			Enum enumerator => type switch
			{
				ScriptFieldType.Int32 => Convert.ToInt32(enumerator, CultureInfo.InvariantCulture),
				ScriptFieldType.UInt32 => Convert.ToUInt32(enumerator, CultureInfo.InvariantCulture),
				ScriptFieldType.Int64 => Convert.ToInt64(enumerator, CultureInfo.InvariantCulture),
				_ => Convert.ToUInt64(enumerator, CultureInfo.InvariantCulture),
			},
			Asset asset when type == ScriptFieldType.Asset => asset.Handle,
			null when type == ScriptFieldType.Asset => AssetHandle.Invalid,
			_ => value,
		};
	}

	// A stored value with every quaternion, a list's elements included, as the engine reads it: of length zero the identity
	// (the engine refuses a quaternion without a direction, and with it the whole map or class list it is in), else of length
	// one (LiteralCodec.Normalize).
	internal static object? NormalizeQuaternions(object? stored)
	{
		switch (stored)
		{
			case Quaternion quaternion:
				return LiteralCodec.Normalize(quaternion, out _);
			case List<object?> elements:
				for (int index = 0; index < elements.Count; index++)
				{
					if (elements[index] is Quaternion element)
					{
						elements[index] = LiteralCodec.Normalize(element, out _);
					}
				}
				return elements;
			default:
				return stored;
		}
	}

	// The value a field of the C# type takes for a stored value; null with an error message when it does not fit (an enum
	// value out of the enum's range, also as an element of a list). A stored list becomes a new array or List<T> of the
	// elements' values.
	internal static object? FromStored(object? stored, Type fieldType, out string? error)
	{
		error = null;
		if (GetListElementType(fieldType) is { } elementType)
		{
			if (stored is not List<object?> elements)
			{
				error = $"expected a list for {fieldType.Name}";
				return null;
			}
			IList values = fieldType.IsArray ? Array.CreateInstance(elementType, elements.Count) : (IList)Activator.CreateInstance(fieldType)!;
			for (int index = 0; index < elements.Count; index++)
			{
				object? value = FromStored(elements[index], elementType, out error);
				if (error != null)
				{
					error = $"element {index}: {error}";
					return null;
				}
				if (fieldType.IsArray)
				{
					values[index] = value;
				}
				else
				{
					values.Add(value);
				}
			}
			return values;
		}
		if (fieldType.IsEnum)
		{
			Type underlying = Enum.GetUnderlyingType(fieldType);
			if (!FitsInteger(stored, Type.GetTypeCode(underlying)))
			{
				error = $"{stored} is out of the range of {fieldType.Name} ({underlying.Name})";
				return null;
			}
			return Enum.ToObject(fieldType, stored!);
		}
		string? assetType = GetAssetType(fieldType);
		if (assetType != null && stored is AssetHandle handle)
		{
			return handle.IsValid ? AssetTypes.Create(assetType, handle) : null;
		}
		return stored;
	}

	private static bool FitsInteger(object? value, TypeCode target)
	{
		(long Min, ulong Max) range = target switch
		{
			TypeCode.SByte => (sbyte.MinValue, (ulong)sbyte.MaxValue),
			TypeCode.Byte => (byte.MinValue, byte.MaxValue),
			TypeCode.Int16 => (short.MinValue, (ulong)short.MaxValue),
			TypeCode.UInt16 => (ushort.MinValue, ushort.MaxValue),
			TypeCode.Int32 => (int.MinValue, int.MaxValue),
			TypeCode.UInt32 => (uint.MinValue, uint.MaxValue),
			TypeCode.Int64 => (long.MinValue, long.MaxValue),
			_ => (0, ulong.MaxValue),
		};
		return value switch
		{
			int number => number >= range.Min && (number < 0 || (ulong)number <= range.Max),
			uint number => number <= range.Max,
			long number => number >= range.Min && (number < 0 || (ulong)number <= range.Max),
			ulong number => number <= range.Max,
			_ => false,
		};
	}

	// Writes a stored value (ToStored's) of the type; a list's elements in order.
	internal static void WriteValue(Utf8JsonWriter writer, ScriptFieldType type, object? value)
	{
		if (IsList(type))
		{
			ScriptFieldType elementType = GetElementType(type);
			writer.WriteStartArray();
			if (value is IList elements)
			{
				foreach (object? element in elements)
				{
					WriteValue(writer, elementType, element);
				}
			}
			writer.WriteEndArray();
			return;
		}
		switch (type)
		{
			case ScriptFieldType.Bool:
				writer.WriteBooleanValue((bool)value!);
				break;
			case ScriptFieldType.Int32:
				writer.WriteNumberValue((int)value!);
				break;
			case ScriptFieldType.UInt32:
				writer.WriteNumberValue((uint)value!);
				break;
			case ScriptFieldType.Int64:
				writer.WriteStringValue(((long)value!).ToString(CultureInfo.InvariantCulture));
				break;
			case ScriptFieldType.UInt64:
				writer.WriteStringValue(((ulong)value!).ToString(CultureInfo.InvariantCulture));
				break;
			case ScriptFieldType.Float:
				WriteFloat(writer, (float)value!);
				break;
			case ScriptFieldType.Double:
				double number = (double)value!;
				writer.WriteNumberValue(double.IsFinite(number) ? number : 0.0);
				break;
			case ScriptFieldType.String:
				writer.WriteStringValue((string?)value ?? string.Empty);
				break;
			case ScriptFieldType.Vector2:
				Vector2 vector2 = (Vector2)value!;
				WriteFloats(writer, vector2.X, vector2.Y);
				break;
			case ScriptFieldType.Vector3:
				Vector3 vector3 = (Vector3)value!;
				WriteFloats(writer, vector3.X, vector3.Y, vector3.Z);
				break;
			case ScriptFieldType.Vector4:
				Vector4 vector4 = (Vector4)value!;
				WriteFloats(writer, vector4.X, vector4.Y, vector4.Z, vector4.W);
				break;
			case ScriptFieldType.Quaternion:
				Quaternion quaternion = (Quaternion)value!;
				WriteFloats(writer, quaternion.X, quaternion.Y, quaternion.Z, quaternion.W);
				break;
			case ScriptFieldType.Color:
				Color color = (Color)value!;
				WriteFloats(writer, color.R, color.G, color.B, color.A);
				break;
			case ScriptFieldType.Entity:
				writer.WriteStringValue((((Entity?)value)?.ID ?? 0).ToString(CultureInfo.InvariantCulture));
				break;
			case ScriptFieldType.Prefab:
				writer.WriteStringValue((((Prefab?)value)?.Handle.ID ?? 0).ToString(CultureInfo.InvariantCulture));
				break;
			case ScriptFieldType.Asset:
				writer.WriteStringValue(((AssetHandle)value!).ID.ToString(CultureInfo.InvariantCulture));
				break;
			case ScriptFieldType.None:
			default:
				writer.WriteNullValue();
				break;
		}
	}

	// The stored value the JSON holds; null with an error message when it holds no value of the type. A list is an array of
	// at most MaxListElements values of its element type, read into a List<object?>; the error names a bad element's index.
	internal static object? ReadValue(JsonElement json, ScriptFieldType type, out string? error)
	{
		error = null;
		if (IsList(type))
		{
			return ReadList(json, GetElementType(type), out error);
		}
		switch (type)
		{
			case ScriptFieldType.Bool when json.ValueKind is JsonValueKind.True or JsonValueKind.False:
				return json.GetBoolean();
			case ScriptFieldType.Int32 when json.ValueKind == JsonValueKind.Number && json.TryGetInt32(out int int32):
				return int32;
			case ScriptFieldType.UInt32 when json.ValueKind == JsonValueKind.Number && json.TryGetUInt32(out uint uint32):
				return uint32;
			case ScriptFieldType.Int64 when TryParseString(json, out long int64):
				return int64;
			case ScriptFieldType.UInt64 when TryParseString(json, out ulong uint64):
				return uint64;
			case ScriptFieldType.Float when json.ValueKind == JsonValueKind.Number && json.TryGetSingle(out float single):
				return single;
			case ScriptFieldType.Double when json.ValueKind == JsonValueKind.Number && json.TryGetDouble(out double number):
				return number;
			case ScriptFieldType.String when json.ValueKind == JsonValueKind.String:
				return json.GetString();
			case ScriptFieldType.Vector2 when TryReadFloats(json, 2, out float[] floats):
				return new Vector2(floats[0], floats[1]);
			case ScriptFieldType.Vector3 when TryReadFloats(json, 3, out float[] floats):
				return new Vector3(floats[0], floats[1], floats[2]);
			case ScriptFieldType.Vector4 when TryReadFloats(json, 4, out float[] floats):
				return new Vector4(floats[0], floats[1], floats[2], floats[3]);
			case ScriptFieldType.Quaternion when TryReadFloats(json, 4, out float[] floats):
				return new Quaternion(floats[0], floats[1], floats[2], floats[3]);
			case ScriptFieldType.Color when TryReadFloats(json, 4, out float[] floats):
				return new Color(floats[0], floats[1], floats[2], floats[3]);
			case ScriptFieldType.Entity when TryParseString(json, out ulong entity):
				return entity != 0 ? new Entity(entity) : null;
			case ScriptFieldType.Prefab when TryParseString(json, out ulong prefab):
				return prefab != 0 ? new Prefab(new AssetHandle(prefab)) : null;
			case ScriptFieldType.Asset when TryParseString(json, out ulong asset):
				return new AssetHandle(asset);
			default:
				error = $"expected a {type} value, not {json.GetRawText()}";
				return null;
		}
	}

	private static List<object?>? ReadList(JsonElement json, ScriptFieldType elementType, out string? error)
	{
		if (json.ValueKind != JsonValueKind.Array)
		{
			error = $"expected an array of {elementType} values, not {json.GetRawText()}";
			return null;
		}
		int count = json.GetArrayLength();
		if (count > MaxListElements)
		{
			error = FormattableString.Invariant($"a list holds at most {MaxListElements} elements, not {count}");
			return null;
		}
		List<object?> elements = new(count);
		int index = 0;
		foreach (JsonElement element in json.EnumerateArray())
		{
			object? value = ReadValue(element, elementType, out error);
			if (error != null)
			{
				error = FormattableString.Invariant($"element {index}: {error}");
				return null;
			}
			elements.Add(value);
			index++;
		}
		error = null;
		return elements;
	}

	private static void WriteFloat(Utf8JsonWriter writer, float value)
	{
		// JSON has no NaN or infinity.
		writer.WriteNumberValue(float.IsFinite(value) ? value : 0.0f);
	}

	private static void WriteFloats(Utf8JsonWriter writer, params ReadOnlySpan<float> values)
	{
		writer.WriteStartArray();
		foreach (float value in values)
		{
			WriteFloat(writer, value);
		}
		writer.WriteEndArray();
	}

	private static bool TryReadFloats(JsonElement json, int count, out float[] values)
	{
		values = new float[count];
		if (json.ValueKind != JsonValueKind.Array || json.GetArrayLength() != count)
		{
			return false;
		}
		int index = 0;
		foreach (JsonElement element in json.EnumerateArray())
		{
			if (element.ValueKind != JsonValueKind.Number || !element.TryGetSingle(out values[index]))
			{
				return false;
			}
			index++;
		}
		return true;
	}

	private static bool TryParseString<T>(JsonElement json, out T value)
		where T : IParsable<T>
	{
		value = default!;
		return json.ValueKind == JsonValueKind.String && T.TryParse(json.GetString(), CultureInfo.InvariantCulture, out value!);
	}
}
