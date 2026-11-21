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
				object? stored = ScriptFieldCodec.NormalizeQuaternions(ScriptFieldCodec.ToStored(instance.ReadVariable(index), fieldType));
				if (stored is List<object?> { Count: > ScriptFieldCodec.MaxListElements } elements)
				{
					ReportLongList(instance, variable, elements.Count);
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

	// The public variables of the entity's instance whose values differ from what the last call answered of them (every one at
	// the instance's first call and after a live edit gave it another program, and a variable given a value since), compared
	// with a copy of each answer (ReadBackCopy) without converting the running value to the stored form, as ScriptRegistry's
	// DescribeChangedInstanceFields does for C# fields: { "Changed": { "<Name>": { "Type", "Value" } }, "Unchanged": [
	// "<Name>" ] }, the unchanged ones by name in document order. A public array holding more elements than a list can is in
	// neither, as DescribeInstanceVariables leaves it out. Null without an instance. Only the engine's read-back of a running
	// scene calls it (Scene::RefreshVisualScriptVariables), since the copies are the instance's.
	internal byte[]? DescribeChangedInstanceVariables(ulong entity)
	{
		if (!m_Instances.TryGetValue(entity, out GraphInstance? instance))
		{
			return null;
		}
		CompiledVariable[] variables = instance.Program.Variables;
		ReadBackCopy?[] copies = instance.ReadBackCopies ??= new ReadBackCopy?[variables.Length];
		List<string> unchanged = [];
		ArrayBufferWriter<byte> buffer = new();
		using (Utf8JsonWriter writer = new(buffer))
		{
			writer.WriteStartObject();
			writer.WriteStartObject("Changed");
			for (int index = 0; index < variables.Length; index++)
			{
				CompiledVariable variable = variables[index];
				ScriptFieldType fieldType = variable.Type.VariableFieldType;
				if (!variable.Public || fieldType == ScriptFieldType.None)
				{
					continue;
				}
				object? value = instance.ReadVariable(index);
				if (copies[index] is { } copy && copy.Matches(value))
				{
					unchanged.Add(variable.Name);
					continue;
				}
				int count = ReadBackCopy.CountOf(value);
				if (count > ScriptFieldCodec.MaxListElements)
				{
					ReportLongList(instance, variable, count);
					copies[index] = null;
					continue;
				}
				writer.WriteStartObject(variable.Name);
				writer.WriteString("Type", ScriptFieldCodec.GetTypeName(fieldType));
				writer.WritePropertyName("Value");
				ScriptFieldCodec.WriteValue(writer, fieldType, ScriptFieldCodec.NormalizeQuaternions(ScriptFieldCodec.ToStored(value, fieldType)));
				writer.WriteEndObject();
				ReadBackCopy taken = copies[index] ?? ReadBackCopy.Create(variable.Type.ClrType!);
				taken.Take(value);
				copies[index] = taken;
			}
			writer.WriteEndObject();
			writer.WriteStartArray("Unchanged");
			foreach (string name in unchanged)
			{
				writer.WriteStringValue(name);
			}
			writer.WriteEndArray();
			writer.WriteEndObject();
		}
		return buffer.WrittenSpan.ToArray();
	}

	// A public array too long to read back is reported once per graph and variable.
	private void ReportLongList(GraphInstance instance, CompiledVariable variable, int count)
	{
		if (m_ReportedLongLists.Add($"{instance.Program.Name}.{variable.Name}"))
		{
			WarnVariables(instance, FormattableString.Invariant(
				$"{variable.Name}: holds {count} elements, more than a list's {ScriptFieldCodec.MaxListElements}: its value is not read back"));
		}
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
				// Whatever becomes of the value, the next read-back of changes answers what the variable holds then.
				if (instance.ReadBackCopies is { } copies)
				{
					copies[index] = null;
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
				object? decoded = ScriptFieldCodec.NormalizeQuaternions(ScriptFieldCodec.ReadValue(value, fieldType, out string? error));
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
