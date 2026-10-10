using System;
using System.Globalization;
using System.Text.Json;

namespace Strada.Interop;

// The engine's ScriptFieldType; the names are those of scene files (ScriptField.cpp).
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

// Field values in the JSON encoding of scene files: 64-bit integers, entity IDs and asset handles as decimal strings,
// vectors, colors and quaternions (X, Y, Z, W) as number arrays.
internal static class ScriptFieldCodec
{
	internal static ScriptFieldType GetFieldType(Type type)
	{
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
			_ when type == typeof(AssetHandle) => ScriptFieldType.Asset,
			_ => ScriptFieldType.None,
		};
	}

	internal static void WriteValue(Utf8JsonWriter writer, ScriptFieldType type, object? value)
	{
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
			case ScriptFieldType.Asset:
				writer.WriteStringValue(((AssetHandle)value!).ID.ToString(CultureInfo.InvariantCulture));
				break;
			case ScriptFieldType.None:
			default:
				writer.WriteNullValue();
				break;
		}
	}

	// Null with an error message when the JSON does not hold a value of the type.
	internal static object? ReadValue(JsonElement json, ScriptFieldType type, out string? error)
	{
		error = null;
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
				return new AssetHandle(prefab);
			case ScriptFieldType.Asset when TryParseString(json, out ulong asset):
				return new AssetHandle(asset);
			default:
				error = $"expected a {type} value, not {json.GetRawText()}";
				return null;
		}
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
