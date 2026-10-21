using System;
using System.Diagnostics.CodeAnalysis;
using System.Text;
using System.Text.Json;

namespace Strada.VisualScripting;

// Reading documents safely: strings that are not valid UTF-16 (an escaped unpaired surrogate) count as invalid instead of
// throwing, and messages quote values briefly.
internal static class JsonText
{
	private const int ExcerptLength = 48;

	internal static bool TryGetString(JsonElement element, [NotNullWhen(true)] out string? value)
	{
		value = null;
		if (element.ValueKind != JsonValueKind.String)
		{
			return false;
		}
		try
		{
			value = element.GetString();
			return value != null;
		}
		catch (InvalidOperationException)
		{
			return false;
		}
	}

	internal static bool TryGetName(JsonProperty property, [NotNullWhen(true)] out string? name)
	{
		try
		{
			name = property.Name;
			return true;
		}
		catch (InvalidOperationException)
		{
			name = null;
			return false;
		}
	}

	// The value as JSON text, shortened for messages.
	internal static string Excerpt(JsonElement element) => Shorten(element.GetRawText());

	internal static string Quote(string text) => "\"" + Shorten(text) + "\"";

	internal static string Shorten(string text)
	{
		if (text.Length <= ExcerptLength)
		{
			return text;
		}
		int length = ExcerptLength - 3;
		if (char.IsHighSurrogate(text[length - 1]))
		{
			length--;
		}
		return text[..length] + "...";
	}

	// Whether a string has a control character (U+0000-U+001F, U+007F).
	internal static bool HasControlCharacter(string text)
	{
		foreach (char character in text)
		{
			if (character < ' ' || character == '\u007F')
			{
				return true;
			}
		}
		return false;
	}

	internal static int ByteCount(string text) => Encoding.UTF8.GetByteCount(text);
}

// The rule for user-declared names (spec section 1.3): variables, locals, functions, parameters, custom events and their
// parameters, FormatString placeholders.
internal static class Names
{
	internal const int MaxBytes = 64;

	// Pin names no user-declared pin may take (spec section 4.0).
	private static readonly string[] s_Reserved = ["Execute", "Then", "Target", "Self", "Default"];

	// Null when the name is valid, else what is wrong with it.
	internal static string? Check(string name)
	{
		int bytes = JsonText.ByteCount(name);
		if (bytes == 0)
		{
			return "a name cannot be empty";
		}
		if (bytes > MaxBytes)
		{
			return FormattableString.Invariant($"a name has at most {MaxBytes} bytes");
		}
		if (JsonText.HasControlCharacter(name))
		{
			return "a name cannot contain control characters";
		}
		if (name[0] == ' ' || name[^1] == ' ')
		{
			return "a name cannot start or end with a space";
		}
		return name.Contains('.', StringComparison.Ordinal) ? "a name cannot contain '.'" : null;
	}

	internal static bool IsReserved(string name) => Array.IndexOf(s_Reserved, name) >= 0;

	internal static string ReservedList => string.Join(", ", s_Reserved);
}
