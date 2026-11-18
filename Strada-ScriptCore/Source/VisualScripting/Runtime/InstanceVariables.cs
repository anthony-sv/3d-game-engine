using System;
using System.Buffers;
using System.Collections.Generic;
using System.Text.Json;
using System.Text.Unicode;
using Strada.Interop;

namespace Strada.VisualScripting;

// The public variables of instances in the stored-map encoding of components (spec sections 2.3 and 5.4):
// { "<Name>": { "Type": "<ScriptFieldType>", "Value": ... } }, values as the script-field codec stores them.
internal sealed partial class VisualScriptRuntime
{
	// The public arrays ("<graph>.<variable>") whose too long values DescribeInstanceVariables has reported.
	private readonly HashSet<string> m_ReportedLongLists = new(StringComparer.Ordinal);

	// The public variables of the entity's instance with their current values, keyed by their current names, in document
	// order; null without an instance. A public array is a list (Docs/Architecture.md §10.6); one holding more elements than
	// a list can is left out (the engine would refuse every value with it), so what the component stores for it stays,
	// reported once per graph and variable.
	internal byte[]? DescribeInstanceVariables(ulong entity)
	{
		if (!m_Instances.TryGetValue(entity, out GraphInstance? instance))
		{
			return null;
		}
		ArrayBufferWriter<byte> buffer = new();
		using (Utf8JsonWriter writer = new(buffer))
		{
			writer.WriteStartObject();
			CompiledVariable[] variables = instance.Program.Variables;
			for (int index = 0; index < variables.Length; index++)
			{
				CompiledVariable variable = variables[index];
				ScriptFieldType fieldType = variable.Type.VariableFieldType;
				if (!variable.Public || fieldType == ScriptFieldType.None)
				{
					continue;
				}
				// The engine refuses quaternions without a direction: those cross as the identity.
				object? stored = NormalizeQuaternions(ScriptFieldCodec.ToStored(instance.ReadVariable(index), fieldType));
				if (stored is List<object?> { Count: > ScriptFieldCodec.MaxListElements } elements)
				{
					if (m_ReportedLongLists.Add($"{instance.Program.Name}.{variable.Name}"))
					{
						WarnVariables(instance, FormattableString.Invariant(
							$"{variable.Name}: holds {elements.Count} elements, more than a list's {ScriptFieldCodec.MaxListElements}: its value is not read back"));
					}
					continue;
				}
				writer.WriteStartObject(variable.Name);
				writer.WriteString("Type", ScriptFieldCodec.GetTypeName(fieldType));
				writer.WritePropertyName("Value");
				ScriptFieldCodec.WriteValue(writer, fieldType, stored);
				writer.WriteEndObject();
			}
			writer.WriteEndObject();
		}
		return buffer.WrittenSpan.ToArray();
	}

	// Sets public variables of the entity's instance from stored values, as its creation does; NotFound without an
	// instance.
	internal VisualScriptStatus SetInstanceVariables(ulong entity, ReadOnlySpan<byte> variables)
	{
		if (!m_Instances.TryGetValue(entity, out GraphInstance? instance))
		{
			return VisualScriptStatus.NotFound;
		}
		ApplyVariables(instance, variables);
		return VisualScriptStatus.Success;
	}

	// An entry applies to the public variable of its name, else to the public variable that had it as a former name; entries
	// that name no public variable are skipped silently, entries of the wrong shape or type with a Warning. An empty span
	// holds no entries.
	private void ApplyVariables(GraphInstance instance, ReadOnlySpan<byte> json)
	{
		if (json.IsEmpty)
		{
			return;
		}
		// JSON text is UTF-8 (RFC 8259): the parser checks only its structure, and the stored-field codec reads its strings.
		if (!Utf8.IsValid(json))
		{
			WarnVariables(instance, "variable values ignored: they are not JSON");
			return;
		}
		JsonDocument document;
		try
		{
			document = JsonDocument.Parse(json.ToArray());
		}
		catch (JsonException)
		{
			WarnVariables(instance, "variable values ignored: they are not JSON");
			return;
		}
		using (document)
		{
			if (!JsonText.HasValidText(document.RootElement))
			{
				WarnVariables(instance, "variable values ignored: they are not JSON");
				return;
			}
			if (document.RootElement.ValueKind != JsonValueKind.Object)
			{
				WarnVariables(instance, "variable values ignored: expected an object");
				return;
			}
			foreach (JsonProperty stored in document.RootElement.EnumerateObject())
			{
				if (!JsonText.TryGetName(stored, out string? name))
				{
					continue;
				}
				int index = FindPublicVariable(instance.Program, name);
				if (index < 0)
				{
					continue;
				}
				CompiledVariable variable = instance.Program.Variables[index];
				ScriptFieldType fieldType = variable.Type.VariableFieldType;
				string fieldTypeName = ScriptFieldCodec.GetTypeName(fieldType);
				if (stored.Value.ValueKind != JsonValueKind.Object || !stored.Value.TryGetProperty("Type", out JsonElement type) ||
					!stored.Value.TryGetProperty("Value", out JsonElement value))
				{
					WarnVariables(instance, $"{variable.Name}: stored value ignored: expected {{ \"Type\", \"Value\" }}");
					continue;
				}
				if (fieldType == ScriptFieldType.None || !JsonText.TryGetString(type, out string? typeName) || typeName != fieldTypeName)
				{
					WarnVariables(instance, $"{variable.Name}: stored {JsonText.Excerpt(type)} value ignored: the variable is a {fieldTypeName} now");
					continue;
				}
				object? decoded = NormalizeQuaternions(ScriptFieldCodec.ReadValue(value, fieldType, out string? error));
				if (error == null)
				{
					decoded = ScriptFieldCodec.FromStored(decoded, variable.Type.ClrType!, out error);
				}
				if (error != null)
				{
					WarnVariables(instance, $"{variable.Name}: stored value ignored: {error}");
					continue;
				}
				instance.Variables[index] = SlotValues.FromValue(decoded, variable.Type);
			}
		}
	}

	// A stored value (ScriptFieldCodec's) with every quaternion of length zero the identity and the others of length one, a
	// list's elements included: graphs hold quaternions so (LiteralCodec.Normalize), and the engine refuses one without a
	// direction.
	private static object? NormalizeQuaternions(object? stored)
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

	private static int FindPublicVariable(GraphProgram program, string name)
	{
		CompiledVariable[] variables = program.Variables;
		for (int index = 0; index < variables.Length; index++)
		{
			if (variables[index].Public && variables[index].Name == name)
			{
				return index;
			}
		}
		for (int index = 0; index < variables.Length; index++)
		{
			if (!variables[index].Public)
			{
				continue;
			}
			foreach (string formerName in variables[index].FormerNames)
			{
				if (formerName == name)
				{
					return index;
				}
			}
		}
		return -1;
	}

	private void WarnVariables(GraphInstance instance, string message)
	{
		m_Environment.Log(VisualScriptLogLevel.Warn,
			FormattableString.Invariant($"{instance.Program.Name} on {m_Environment.GetEntityName(instance.Entity)} ({instance.Entity}): {message}"));
	}
}
