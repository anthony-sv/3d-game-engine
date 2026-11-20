using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using System.Text.Json;

namespace Strada.Interop;

// A value a save slot or the player's settings hold: its field type and its stored form (ScriptFieldCodec.ToStored's), with
// the engine's rules for names and values (Strada/Script/SaveSlots.h: ValidateSavedValueName, ValidateSavedValue), so that a
// value a script sets is one the engine writes and reads back as it was.
internal readonly record struct SavedValue(ScriptFieldType Type, object? Stored);

internal static class SavedValueCodec
{
	// The engine's MaxSavedValueNameLength, MaxSaveSlotValues and MinSavedQuaternionLength.
	internal const int MaxNameBytes = 128;
	internal const int MaxValues = 4096;
	internal const float MinQuaternionLength = 1e-6f;

	private static readonly UTF8Encoding s_StrictUtf8 = new(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: true);

	// Why the engine refuses the name (1 to MaxNameBytes bytes of UTF-8 without control characters); null when it takes it.
	internal static string? CheckName(string name)
	{
		int bytes;
		try
		{
			bytes = s_StrictUtf8.GetByteCount(name);
		}
		catch (EncoderFallbackException)
		{
			return "a value's name is not valid text";
		}
		if (bytes == 0)
		{
			return "a value's name is empty";
		}
		if (bytes > MaxNameBytes)
		{
			return FormattableString.Invariant($"a value's name has at most {MaxNameBytes} bytes, not {bytes}");
		}
		foreach (char character in name)
		{
			if (character < 0x20 || (character >= 0x7F && character <= 0x9F))
			{
				return "a value's name holds a control character";
			}
		}
		return null;
	}

	// Throws ArgumentNullException for null and ArgumentException for a name the engine refuses.
	internal static void RequireName(string member, string name)
	{
		ArgumentNullException.ThrowIfNull(name);
		if (CheckName(name) is { } problem)
		{
			throw new ArgumentException($"{member}: {problem}", nameof(name));
		}
	}

	// The saved form of a value of T; throws ArgumentException for a T without a field type and for a value the engine
	// refuses: a list longer than ScriptFieldCodec.MaxListElements, a number that is not finite, a quaternion shorter than
	// MinQuaternionLength. Quaternions are kept of length one, as the engine reads them back.
	internal static SavedValue ToSaved<T>(string member, T value)
	{
		ScriptFieldType type = ScriptFieldCodec.GetMemberFieldType(typeof(T));
		if (type == ScriptFieldType.None)
		{
			throw new ArgumentException($"{member}: {typeof(T).Name} is no type of a saved value (numbers, text, vectors, colors, "
				+ "entities, assets, enums and lists of them are)", nameof(value));
		}
		object? stored = ScriptFieldCodec.ToStored(value, type);
		if (ScriptFieldCodec.IsList(type))
		{
			List<object?> elements = (List<object?>)stored!;
			if (elements.Count > ScriptFieldCodec.MaxListElements)
			{
				throw new ArgumentException(FormattableString.Invariant(
					$"{member}: a list holds at most {ScriptFieldCodec.MaxListElements} elements, not {elements.Count}"), nameof(value));
			}
			ScriptFieldType elementType = ScriptFieldCodec.GetElementType(type);
			for (int index = 0; index < elements.Count; index++)
			{
				if (CheckElement(elementType, elements[index]) is { } problem)
				{
					throw new ArgumentException(FormattableString.Invariant($"{member}: element {index}: {problem}"), nameof(value));
				}
			}
		}
		else if (CheckElement(type, stored) is { } problem)
		{
			throw new ArgumentException($"{member}: {problem}", nameof(value));
		}
		return new SavedValue(type, ScriptFieldCodec.NormalizeQuaternions(stored));
	}

	// The value as T when it is of T's field type and fits T (an enum's range); false with why not otherwise.
	internal static bool TryFromSaved<T>(SavedValue saved, out T value, out string? problem)
	{
		value = default!;
		ScriptFieldType type = ScriptFieldCodec.GetMemberFieldType(typeof(T));
		if (type == ScriptFieldType.None || type != saved.Type)
		{
			problem = $"it holds a value of type {ScriptFieldCodec.GetTypeName(saved.Type)}, not {typeof(T).Name}";
			return false;
		}
		object? converted = ScriptFieldCodec.FromStored(saved.Stored, typeof(T), out problem);
		if (problem != null)
		{
			return false;
		}
		value = (T)converted!;
		return true;
	}

	// Writes { "Type", "Value" }.
	internal static void Write(Utf8JsonWriter writer, SavedValue saved)
	{
		writer.WriteStartObject();
		writer.WriteString("Type", ScriptFieldCodec.GetTypeName(saved.Type));
		writer.WritePropertyName("Value");
		ScriptFieldCodec.WriteValue(writer, saved.Type, saved.Stored);
		writer.WriteEndObject();
	}

	// Reads { "Type", "Value" }; null with why not when it holds no value.
	internal static SavedValue? Read(JsonElement json, out string? problem)
	{
		if (json.ValueKind != JsonValueKind.Object || !json.TryGetProperty("Type", out JsonElement typeName) ||
			typeName.ValueKind != JsonValueKind.String || !json.TryGetProperty("Value", out JsonElement value))
		{
			problem = "expected { \"Type\", \"Value\" }";
			return null;
		}
		ScriptFieldType type = ParseTypeName(typeName.GetString()!);
		if (type == ScriptFieldType.None)
		{
			problem = $"unknown type {typeName.GetRawText()}";
			return null;
		}
		object? stored = ScriptFieldCodec.ReadValue(value, type, out problem);
		return problem == null ? new SavedValue(type, stored) : null;
	}

	// The field type ScriptFieldCodec.GetTypeName names; None for none.
	internal static ScriptFieldType ParseTypeName(string name)
	{
		bool isList = name.EndsWith("[]", StringComparison.Ordinal);
		string elementName = isList ? name[..^2] : name;
		// Only an enumerator's own name: TryParse also takes numbers and lists of names.
		if (!Enum.TryParse(elementName, ignoreCase: false, out ScriptFieldType element) || element == ScriptFieldType.None ||
			Enum.GetName(element) != elementName)
		{
			return ScriptFieldType.None;
		}
		return isList ? ScriptFieldCodec.MakeListType(element) : element;
	}

	private static string? CheckElement(ScriptFieldType type, object? stored) => stored switch
	{
		float number when type == ScriptFieldType.Float => float.IsFinite(number) ? null : "the number is not finite",
		double number when type == ScriptFieldType.Double => double.IsFinite(number) ? null : "the number is not finite",
		Vector2 vector => float.IsFinite(vector.X) && float.IsFinite(vector.Y) ? null : "a component is not finite",
		Vector3 vector => float.IsFinite(vector.X) && float.IsFinite(vector.Y) && float.IsFinite(vector.Z) ? null : "a component is not finite",
		Vector4 vector => AreFinite(vector.X, vector.Y, vector.Z, vector.W) ? null : "a component is not finite",
		Color color => AreFinite(color.R, color.G, color.B, color.A) ? null : "a component is not finite",
		Quaternion quaternion => CheckQuaternion(quaternion),
		_ => null,
	};

	private static string? CheckQuaternion(Quaternion quaternion)
	{
		if (!AreFinite(quaternion.X, quaternion.Y, quaternion.Z, quaternion.W))
		{
			return "a component is not finite";
		}
		float length = MathF.Sqrt(quaternion.X * quaternion.X + quaternion.Y * quaternion.Y + quaternion.Z * quaternion.Z +
			quaternion.W * quaternion.W);
		return length >= MinQuaternionLength
			? null
			: string.Create(CultureInfo.InvariantCulture, $"the quaternion's length is below {MinQuaternionLength}");
	}

	private static bool AreFinite(float x, float y, float z, float w) => float.IsFinite(x) && float.IsFinite(y) && float.IsFinite(z) && float.IsFinite(w);
}
