using System;
using System.Text;

namespace Strada.VisualScripting;

// Display names of types and members (spec section 4.5): PascalCase split into words, component types without their
// "Component" suffix.
internal static class Titles
{
	private const string ComponentSuffix = " Component";

	// "AddForce" → "Add Force", "UIButton" → "UI Button", "PerspectiveFOV" → "Perspective FOV", "Deg2Rad" → "Deg2 Rad",
	// "Vector3" → "Vector3". A word starts at an upper-case letter after a lower-case letter or a digit, and at the last
	// upper-case letter of an acronym followed by a lower-case letter.
	internal static string Words(string name)
	{
		StringBuilder words = new(name.Length + 8);
		for (int index = 0; index < name.Length; index++)
		{
			char character = name[index];
			if (character == '_')
			{
				if (words.Length > 0 && words[^1] != ' ')
				{
					words.Append(' ');
				}
				continue;
			}
			if (index > 0 && char.IsAsciiLetterUpper(character) && words.Length > 0 && words[^1] != ' ')
			{
				char previous = name[index - 1];
				bool afterWord = char.IsAsciiLetterLower(previous) || char.IsAsciiDigit(previous);
				bool endsAcronym = char.IsAsciiLetterUpper(previous) && index + 1 < name.Length && char.IsAsciiLetterLower(name[index + 1]);
				if (afterWord || endsAcronym)
				{
					words.Append(' ');
				}
			}
			words.Append(character);
		}
		return words.ToString();
	}

	// The title of a type: the primitives by their C# name, arrays as "<element> Array", component types without the
	// "Component" suffix ("Rigid Body").
	internal static string OfType(Type type)
	{
		if (type.IsSZArray)
		{
			return OfType(type.GetElementType()!) + " Array";
		}
		string? primitive = type switch
		{
			_ when type == typeof(bool) => "Bool",
			_ when type == typeof(int) => "Int",
			_ when type == typeof(uint) => "UInt",
			_ when type == typeof(long) => "Long",
			_ when type == typeof(ulong) => "ULong",
			_ when type == typeof(float) => "Float",
			_ when type == typeof(double) => "Double",
			_ when type == typeof(string) => "String",
			_ when type == typeof(object) => "Object",
			_ => null,
		};
		if (primitive != null)
		{
			return primitive;
		}
		string title = Words(type.Name);
		return title.EndsWith(ComponentSuffix, StringComparison.Ordinal) && title.Length > ComponentSuffix.Length
			? title[..^ComponentSuffix.Length]
			: title;
	}
}
