using System;
using System.Buffers;
using System.Globalization;
using System.Text.Encodings.Web;
using System.Text.Json;
using Strada.Interop;

namespace Strada.VisualScripting;

// The library JSON of VisualScriptDescribeLibrary (spec section 6.1): { "Types": [...], "Nodes": [...] } in the library's
// order. Keys have a fixed order and optional keys are left out at their defaults, so the same library gives the same
// bytes; numbers are written culture-invariantly by the JSON writer.
internal static class LibraryWriter
{
	internal static byte[] Write(NodeLibrary library)
	{
		ArrayBufferWriter<byte> buffer = new();
		using (Utf8JsonWriter writer = new(buffer, CreateOptions()))
		{
			writer.WriteStartObject();
			writer.WriteStartArray("Types");
			foreach (VisualType type in library.Types)
			{
				WriteType(writer, type);
			}
			writer.WriteEndArray();
			writer.WriteStartArray("Nodes");
			foreach (NodeDefinition node in library.Nodes)
			{
				WriteNode(writer, node);
			}
			writer.WriteEndArray();
			writer.WriteEndObject();
		}
		return buffer.WrittenSpan.ToArray();
	}

	// Type strings hold '<', '>' and '+': the relaxed encoder writes them as they are (the text is JSON, never HTML).
	internal static JsonWriterOptions CreateOptions() => new() { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

	internal static void WriteType(Utf8JsonWriter writer, VisualType type)
	{
		writer.WriteStartObject();
		writer.WriteString("ID", type.Id);
		writer.WriteString("Title", type.Title);
		writer.WriteString("Kind", type.Kind.ToString());
		if (type.FieldType != ScriptFieldType.None)
		{
			writer.WriteString("FieldType", type.FieldType.ToString());
		}
		if (type.AssetType != NativeAssetType.None)
		{
			writer.WriteString("AssetType", type.AssetType.ToString());
		}
		if (type.HasLiteral)
		{
			writer.WriteBoolean("Literal", true);
		}
		if (type.Kind == TypeKind.Enum)
		{
			writer.WriteStartArray("Enumerators");
			foreach (EnumeratorInfo enumerator in type.Enumerators)
			{
				writer.WriteStartObject();
				writer.WriteString("Name", enumerator.Name);
				writer.WritePropertyName("Value");
				WriteEnumeratorValue(writer, enumerator.Value);
				writer.WriteEndObject();
			}
			writer.WriteEndArray();
			writer.WriteBoolean("Flags", type.IsFlags);
		}
		if (type.Makeable)
		{
			writer.WriteBoolean("Makeable", true);
		}
		if (type.Breakable)
		{
			writer.WriteBoolean("Breakable", true);
		}
		if (type.Fields.Count > 0)
		{
			writer.WriteStartArray("Fields");
			foreach (StructMember field in type.Fields)
			{
				writer.WriteStartObject();
				writer.WriteString("Name", field.Name);
				writer.WriteString("Type", field.Type.Id);
				writer.WriteEndObject();
			}
			writer.WriteEndArray();
		}
		if (type.Base != null)
		{
			writer.WriteString("Base", type.Base.Id);
		}
		if (type.Element != null)
		{
			writer.WriteString("Element", type.Element.Id);
		}
		writer.WriteStartArray("ImplicitTo");
		foreach (VisualType target in type.ImplicitTo)
		{
			writer.WriteStringValue(target.Id);
		}
		writer.WriteEndArray();
		writer.WriteEndObject();
	}

	internal static void WriteNode(Utf8JsonWriter writer, NodeDefinition node)
	{
		writer.WriteStartObject();
		writer.WriteString("Type", node.Type);
		writer.WriteString("Title", node.Title);
		if (node.Subtitle.Length > 0)
		{
			writer.WriteString("Subtitle", node.Subtitle);
		}
		writer.WriteString("Category", node.Category);
		if (node.Description.Length > 0)
		{
			writer.WriteString("Description", node.Description);
		}
		writer.WriteStartArray("Keywords");
		foreach (string keyword in node.Keywords)
		{
			writer.WriteStringValue(keyword);
		}
		writer.WriteEndArray();
		writer.WriteString("Kind", node.Kind.ToString());
		writer.WriteBoolean("Pure", node.Pure);
		writer.WriteBoolean("Latent", node.Latent);
		writer.WriteStartArray("Pins");
		foreach (PinDescription pin in node.Pins)
		{
			WritePin(writer, pin, pin.Type);
		}
		writer.WriteEndArray();
		writer.WriteStartArray("Settings");
		foreach (SettingDescription setting in node.Settings)
		{
			WriteSetting(writer, setting);
		}
		writer.WriteEndArray();
		writer.WriteEndObject();
	}

	// A pin with its type (the template's, or the one analysis resolved).
	internal static void WritePin(Utf8JsonWriter writer, PinDescription pin, VisualType type)
	{
		writer.WriteStartObject();
		writer.WriteString("Name", pin.Name);
		writer.WriteString("Direction", pin.Direction.ToString());
		writer.WriteString("Type", type.Id);
		if (pin.Label != null)
		{
			writer.WriteString("Label", pin.Label);
		}
		if (pin.HasDefault && type.HasLiteral)
		{
			writer.WritePropertyName("Default");
			LiteralCodec.Write(writer, type, pin.Default);
		}
		if (pin.DefaultsToSelf)
		{
			writer.WriteBoolean("DefaultsToSelf", true);
		}
		if (pin.Role != PinRole.None)
		{
			writer.WriteString("Role", pin.Role.ToString());
		}
		if (pin.Constraint != PinConstraint.None)
		{
			writer.WriteString("Constraint", pin.Constraint.ToString());
		}
		if (pin.Group != null)
		{
			writer.WriteString("Group", pin.Group);
		}
		if (pin.ElementOf != null)
		{
			writer.WriteString("ElementOf", pin.ElementOf);
		}
		writer.WriteEndObject();
	}

	private static void WriteSetting(Utf8JsonWriter writer, SettingDescription setting)
	{
		writer.WriteStartObject();
		writer.WriteString("Name", setting.Name);
		writer.WriteString("Kind", setting.Kind.ToString());
		switch (setting.Default)
		{
			case bool flag:
				writer.WriteBoolean("Default", flag);
				break;
			case int number:
				writer.WriteNumber("Default", number);
				break;
			case string text:
				writer.WriteString("Default", text);
				break;
			case Array:
				writer.WriteStartArray("Default");
				writer.WriteEndArray();
				break;
		}
		if (setting.Required)
		{
			writer.WriteBoolean("Required", true);
		}
		if (setting.Min is { } min)
		{
			writer.WriteNumber("Min", min);
		}
		if (setting.Max is { } max)
		{
			writer.WriteNumber("Max", max);
		}
		if (setting.Constraint != TypeConstraint.None)
		{
			writer.WriteString("Constraint", setting.Constraint.ToString());
		}
		if (setting.Type != null)
		{
			writer.WriteString("Type", setting.Type);
		}
		if (setting.Choices.Count > 0)
		{
			writer.WriteStartArray("Choices");
			foreach (string choice in setting.Choices)
			{
				writer.WriteStringValue(choice);
			}
			writer.WriteEndArray();
		}
		writer.WriteEndObject();
	}

	// An integer, or a decimal string for enums over 64-bit integers.
	private static void WriteEnumeratorValue(Utf8JsonWriter writer, object value)
	{
		switch (Type.GetTypeCode(Enum.GetUnderlyingType(value.GetType())))
		{
			case TypeCode.Int64:
				writer.WriteStringValue(Convert.ToInt64(value, CultureInfo.InvariantCulture).ToString(CultureInfo.InvariantCulture));
				break;
			case TypeCode.UInt64:
				writer.WriteStringValue(Convert.ToUInt64(value, CultureInfo.InvariantCulture).ToString(CultureInfo.InvariantCulture));
				break;
			case TypeCode.UInt32:
				writer.WriteNumberValue(Convert.ToUInt32(value, CultureInfo.InvariantCulture));
				break;
			default:
				writer.WriteNumberValue(Convert.ToInt64(value, CultureInfo.InvariantCulture));
				break;
		}
	}
}
