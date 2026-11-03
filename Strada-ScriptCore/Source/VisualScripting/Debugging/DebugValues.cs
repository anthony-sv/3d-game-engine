using System;
using System.Globalization;
using System.Text;
using System.Text.Json;

namespace Strada.VisualScripting;

// Which run a value of a snapshotted frame comes from (spec-slice4 section 5.2): the stopped execution's (Current), another
// execution's or an untraced earlier run's (Earlier), none (NotComputed: the template's default in a frame that has stamps),
// or nobody can say (Unknown: no traced run stamped the frame).
internal enum DebugCurrency : byte
{
	Current,
	Earlier,
	NotComputed,
	Unknown,
}

// The one value encoding of the debugger (spec-slice4 section 5.4): a value crosses as the members of a JSON object, its
// "Type" (the type ID) and exactly one of "Value" (the literal form, a cut string, an array's first elements or null),
// "Entity" (an entity's ID) or "Text" (components and script instances by their entity, other objects and numbers or math
// structs that are not finite by ToString), with
// their extras: "Length" and "Truncated" for cut strings and arrays, "Valid": false for an entity the scene no longer has.
// Values are written as they are; the editor formats them for display. Main thread only (it reads entity names).
internal sealed class DebugValueWriter
{
	// Spec-slice4 section 5.4: the caps of strings, of the strings inside arrays, of texts and of arrays.
	internal const int MaxStringBytes = 256;
	internal const int MaxElementStringBytes = 64;
	internal const int MaxTextBytes = 256;
	internal const int MaxElements = 32;
	// Arrays nest this many levels; deeper ones are written as their length.
	internal const int MaxArrayLevels = 2;

	private readonly Utf8JsonWriter m_Writer;
	private readonly IVisualScriptEnvironment m_Environment;
	private readonly NodeLibrary m_Library;

	// library names the types of components and script instances in their texts.
	internal DebugValueWriter(Utf8JsonWriter writer, IVisualScriptEnvironment environment, NodeLibrary library)
	{
		m_Writer = writer;
		m_Environment = environment;
		m_Library = library;
	}

	internal Utf8JsonWriter Writer => m_Writer;

	// The members of a slot's value of the type in the open object, its Type first.
	internal void WriteMembers(in Slot slot, VisualType type)
	{
		m_Writer.WriteString("Type", type.Id);
		WriteValueMembers(SlotValues.ToValue(slot, type), type, 1, MaxStringBytes);
	}

	// A member holding the value as an object of its encoding without Type ("First", "Last", "Before").
	internal void WriteBare(string name, in Slot slot, VisualType type)
	{
		m_Writer.WriteStartObject(name);
		WriteValueMembers(SlotValues.ToValue(slot, type), type, 1, MaxStringBytes);
		m_Writer.WriteEndObject();
	}

	// A named value { "Name", "Type", ... } as the elements of Describe's and Values' sections are, left open for the members
	// its section adds.
	internal void StartNamed(string name, in Slot slot, VisualType type)
	{
		m_Writer.WriteStartObject();
		m_Writer.WriteString("Name", name);
		WriteMembers(slot, type);
	}

	// The currency of a node's values in a snapshotted frame (spec-slice4 section 5.2).
	internal static DebugCurrency CurrencyOf(ulong[]? stamps, int node, ulong serial)
	{
		if (stamps == null)
		{
			return DebugCurrency.Unknown;
		}
		ulong stamp = stamps[node];
		return stamp == serial ? DebugCurrency.Current : stamp == 0 ? DebugCurrency.NotComputed : DebugCurrency.Earlier;
	}

	private void WriteValueMembers(object? value, VisualType type, int level, int stringBytes)
	{
		switch (type.Kind)
		{
			case TypeKind.Array:
				WriteArray(value as Array, type, level);
				return;
			case TypeKind.String:
				WriteString("Value", value as string ?? string.Empty, stringBytes, withLength: true);
				return;
			case TypeKind.Entity:
				WriteEntity(value as Entity, type.IsScriptClass);
				return;
			case TypeKind.Component:
				if (value is Component component)
				{
					Entity owner = component.Entity;
					ulong id = owner?.ID ?? 0;
					WriteString("Text", $"{TitleOf(component.GetType(), type)} of {NameOf(id)}", MaxTextBytes, withLength: false);
					WriteId(id);
				}
				else
				{
					m_Writer.WriteNull("Value");
				}
				return;
		}
		// The literal form writes non-finite numbers as 0 (JSON has no NaN): a debugger shows them as they are, as Text.
		if (type.HasLiteral && type.ClrType is { } clrType && (value == null || clrType.IsInstanceOfType(value)) && IsFinite(value))
		{
			m_Writer.WritePropertyName("Value");
			LiteralCodec.Write(m_Writer, type, value);
			return;
		}
		if (value == null)
		{
			m_Writer.WriteNull("Value");
			return;
		}
		WriteString("Text", TextOf(value), MaxTextBytes, withLength: false);
	}

	// Whether a number, or every component of a math struct, is finite.
	private static bool IsFinite(object? value) => value switch
	{
		float number => float.IsFinite(number),
		double number => double.IsFinite(number),
		Vector2 vector => float.IsFinite(vector.X) && float.IsFinite(vector.Y),
		Vector3 vector => float.IsFinite(vector.X) && float.IsFinite(vector.Y) && float.IsFinite(vector.Z),
		Vector4 vector => IsFinite(vector),
		Quaternion quaternion => float.IsFinite(quaternion.X) && float.IsFinite(quaternion.Y) && float.IsFinite(quaternion.Z) && float.IsFinite(quaternion.W),
		Color color => float.IsFinite(color.R) && float.IsFinite(color.G) && float.IsFinite(color.B) && float.IsFinite(color.A),
		Matrix4 matrix => IsFinite(matrix.Column0) && IsFinite(matrix.Column1) && IsFinite(matrix.Column2) && IsFinite(matrix.Column3),
		_ => true,
	};

	private static bool IsFinite(Vector4 vector) => float.IsFinite(vector.X) && float.IsFinite(vector.Y) && float.IsFinite(vector.Z) && float.IsFinite(vector.W);

	// An entity: its ID ("0" for None), "Valid": false when the scene no longer has it; a script instance also by its text,
	// "Mover on Player (5678)".
	private void WriteEntity(Entity? entity, bool scriptClass)
	{
		ulong id = entity?.ID ?? 0;
		if (scriptClass && entity != null)
		{
			WriteString("Text", $"{TitleOf(entity.GetType(), null)} on {NameOf(id)}", MaxTextBytes, withLength: false);
		}
		WriteId(id);
	}

	private void WriteId(ulong id)
	{
		m_Writer.WriteString("Entity", id.ToString(CultureInfo.InvariantCulture));
		if (id != 0 && !m_Environment.IsEntityValid(id))
		{
			m_Writer.WriteBoolean("Valid", false);
		}
	}

	// The first MaxElements elements, each an object of its encoding without Type, the full Length and Truncated when cut;
	// arrays below MaxArrayLevels are written as their length only.
	private void WriteArray(Array? array, VisualType type, int level)
	{
		int length = array?.Length ?? 0;
		VisualType element = type.Element ?? VisualType.Wildcard;
		m_Writer.WriteStartArray("Value");
		int count = Math.Min(length, MaxElements);
		for (int index = 0; index < count; index++)
		{
			object? item = array!.GetValue(index);
			m_Writer.WriteStartObject();
			if (element.Kind == TypeKind.Array && level >= MaxArrayLevels)
			{
				m_Writer.WriteNumber("Length", (item as Array)?.Length ?? 0);
			}
			else
			{
				WriteValueMembers(item, element, level + 1, MaxElementStringBytes);
			}
			m_Writer.WriteEndObject();
		}
		m_Writer.WriteEndArray();
		m_Writer.WriteNumber("Length", length);
		if (count < length)
		{
			m_Writer.WriteBoolean("Truncated", true);
		}
	}

	// A text cut at maxBytes of UTF-8 on a code point (lone surrogates become U+FFFD, which JSON can hold); a cut string value
	// also says its whole length in UTF-16 units and that it was cut.
	private void WriteString(string name, string text, int maxBytes, bool withLength)
	{
		string written = Cut(text, maxBytes, out bool cut);
		m_Writer.WriteString(name, written);
		if (cut && withLength)
		{
			m_Writer.WriteNumber("Length", text.Length);
			m_Writer.WriteBoolean("Truncated", true);
		}
	}

	// "Player (123)", or the ID alone for an entity without a name.
	private string NameOf(ulong id)
	{
		string name = m_Environment.GetEntityName(id);
		return name.Length > 0 ? string.Create(CultureInfo.InvariantCulture, $"{name} ({id})") : id.ToString(CultureInfo.InvariantCulture);
	}

	// The library's title of a value's type ("Rigid Body"), else the declared type's, else the CLR type's name.
	private string TitleOf(Type type, VisualType? declared) => m_Library.FindType(type)?.Title ?? declared?.Title ?? type.Name;

	// What an object without a literal form says of itself, in the invariant culture. Its ToString is code of the game or of
	// a library: what it throws is reported as the text, never past the protocol.
	private static string TextOf(object value)
	{
		try
		{
			return (value is IFormattable formattable ? formattable.ToString(null, CultureInfo.InvariantCulture) : value.ToString()) ?? string.Empty;
		}
		catch (Exception exception)
		{
			return $"{value.GetType().Name} (its ToString threw {exception.GetType().Name})";
		}
	}

	// The longest prefix of whole code points of at most maxBytes UTF-8 bytes, lone surrogates replaced by U+FFFD; cut when
	// the text had more.
	internal static string Cut(string text, int maxBytes, out bool cut)
	{
		cut = false;
		StringBuilder? builder = null;
		int bytes = 0;
		int index = 0;
		while (index < text.Length)
		{
			bool valid = Rune.DecodeFromUtf16(text.AsSpan(index), out Rune rune, out int consumed) == System.Buffers.OperationStatus.Done;
			if (!valid)
			{
				rune = Rune.ReplacementChar;
			}
			if (bytes + rune.Utf8SequenceLength > maxBytes)
			{
				cut = true;
				break;
			}
			if (!valid && builder == null)
			{
				builder = new StringBuilder(text, 0, index, text.Length);
			}
			if (builder != null)
			{
				builder.Append(valid ? text.AsSpan(index, consumed) : rune.ToString());
			}
			bytes += rune.Utf8SequenceLength;
			index += consumed;
		}
		return builder != null ? builder.ToString() : cut ? text[..index] : text;
	}
}
