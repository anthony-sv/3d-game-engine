using System;

namespace Strada;

/// <summary>Exposes a private or protected field of a script to the editor and to scene files, like a public field.</summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class SerializeFieldAttribute : Attribute
{
}

/// <summary>Keeps a serialized field out of the inspector; its value is still saved with the scene.</summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class HideInInspectorAttribute : Attribute
{
}

/// <summary>Limits the value the inspector lets a numeric field take.</summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class RangeAttribute : Attribute
{
	/// <summary>Limits the field to [<paramref name="min"/>, <paramref name="max"/>].</summary>
	public RangeAttribute(float min, float max)
	{
		Min = min;
		Max = max;
	}

	/// <summary>The smallest value.</summary>
	public float Min { get; }

	/// <summary>The largest value.</summary>
	public float Max { get; }
}

/// <summary>Explains a field in the inspector.</summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class TooltipAttribute : Attribute
{
	/// <summary>Shows <paramref name="text"/> when the field is hovered.</summary>
	public TooltipAttribute(string text)
	{
		Text = text;
	}

	/// <summary>The explanation.</summary>
	public string Text { get; }
}
