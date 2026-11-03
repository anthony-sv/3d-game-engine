using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Reflection;
using System.Text;
using Strada.VisualScripting;

namespace Strada.ScriptCore.Tests;

// A deterministic text of everything a compiled program holds (spec-slice4 section 9.4's oracle): its variables, its
// functions in (graph, ID) order and its event graph, each node with every member it compiled to. References between
// programs' parts are written by what they name (a function by its graph, ID and name; a type by its ID), never by identity,
// so two compilations of equal analyses give equal texts. The caches nodes fill when they first run are left out.
internal static class ProgramDump
{
	// Members that are caches or back references, not what the node compiled to.
	private static readonly HashSet<string> s_Skipped = ["Owner", "Plan", "InputPlans", "Invoker", "Debug"];

	internal static string Of(GraphProgram program)
	{
		StringBuilder text = new();
		text.Append(FormattableString.Invariant($"program {program.Name} errors {program.ErrorCount} {program.FirstError?.Message}\n"));
		foreach (CompiledVariable variable in program.Variables)
		{
			text.Append(FormattableString.Invariant($"variable {variable.Id} {variable.Name} {variable.Type.Id} {Describe(variable.Initial)} {variable.Public} "));
			text.AppendJoin(',', variable.FormerNames).Append('\n');
		}
		foreach (CompiledFunction function in program.Functions.OrderBy(static function => function.Graph).ThenBy(static function => function.Id))
		{
			text.Append(Name(function)).Append(FormattableString.Invariant($" pure {function.Pure} entry {function.EntryNode} return {function.ReturnNode}"));
			text.Append(" inputs ").AppendJoin(',', function.InputTypes.Select(static type => type.Id));
			text.Append(" outputs ").AppendJoin(',', function.OutputTypes.Select(static type => type.Id));
			text.Append(" locals ").AppendJoin(',', function.Locals.Select(static local => FormattableString.Invariant($"{local.Id}:{local.Name}:{local.Type.Id}:{local.Slot}")));
			text.Append(" defaults ").AppendJoin(',', function.OutputDefaults.Select(static slot => Describe(slot))).Append('\n');
			Append(text, function.Body);
		}
		Append(text, program.EventGraph);
		text.Append("events ").AppendJoin(',', program.Events).Append('\n');
		text.Append("input events ").AppendJoin(',', program.InputEvents.Select(static input => input.ToString())).Append('\n');
		return text.ToString();
	}

	private static void Append(StringBuilder text, CompiledGraph graph)
	{
		text.Append(FormattableString.Invariant($"graph {graph.Graph} {graph.Name} counters {graph.CounterCount} template "));
		text.AppendJoin(',', graph.Template.Select(static slot => Describe(slot))).Append('\n');
		foreach (CompiledNode node in graph.Nodes)
		{
			text.Append("  node");
			foreach (PropertyInfo property in Properties(typeof(CompiledNode)))
			{
				text.Append(' ').Append(property.Name).Append('=').Append(Describe(property.GetValue(node), 0));
			}
			text.Append('\n');
		}
	}

	private static string Name(CompiledFunction function) => FormattableString.Invariant($"function {function.Graph}:{function.Id} {function.Name}");

	private static IEnumerable<PropertyInfo> Properties(Type type) =>
		type.GetProperties(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
			.Where(static property => property.GetIndexParameters().Length == 0 && !s_Skipped.Contains(property.Name))
			.OrderBy(static property => property.Name, StringComparer.Ordinal);

	private static string Describe(Slot slot)
	{
		Vector4 bits = slot.Vector4;
		return FormattableString.Invariant(
			$"[{BitConverter.SingleToInt32Bits(bits.X)} {BitConverter.SingleToInt32Bits(bits.Y)} {BitConverter.SingleToInt32Bits(bits.Z)} {BitConverter.SingleToInt32Bits(bits.W)} {Describe(slot.Object, 0)}]");
	}

	private static string Describe(object? value, int depth)
	{
		switch (value)
		{
			case null:
				return "null";
			case string literal:
				return RuntimeTesting.Quote(literal);
			case Enum or bool:
				return value.ToString()!;
			case IFormattable formattable when value.GetType().IsPrimitive:
				return formattable.ToString(null, CultureInfo.InvariantCulture);
			case Slot slot:
				return Describe(slot);
			case VisualType type:
				return "type " + type.Id;
			case CompiledFunction function:
				return Name(function);
			case CompiledGraph graph:
				return FormattableString.Invariant($"graph {graph.Graph} {graph.Name}");
			case GraphProgram program:
				return "program " + program.Name;
			case Type type:
				return "clr " + type.FullName;
			case MemberInfo member:
				return FormattableString.Invariant($"member {member.DeclaringType?.FullName}.{member}");
			case Delegate:
				return "delegate";
			case IDictionary dictionary:
				return "{" + string.Join(",", dictionary.Keys.Cast<object>()
					.Select(key => (Key: Describe(key, depth + 1), Value: Describe(dictionary[key], depth + 1)))
					.OrderBy(static entry => entry.Key, StringComparer.Ordinal)
					.Select(static entry => entry.Key + ":" + entry.Value)) + "}";
			case IEnumerable items:
				return "[" + string.Join(",", items.Cast<object?>().Select(item => Describe(item, depth + 1))) + "]";
		}
		if (depth > 8)
		{
			return value.GetType().Name;
		}
		Type kind = value.GetType();
		StringBuilder text = new(kind.Name);
		text.Append('(');
		foreach (PropertyInfo property in Properties(kind))
		{
			text.Append(property.Name).Append('=').Append(Describe(property.GetValue(value), depth + 1)).Append(';');
		}
		foreach (FieldInfo field in kind.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic)
			.Where(static field => !field.Name.Contains('<', StringComparison.Ordinal)).OrderBy(static field => field.Name, StringComparer.Ordinal))
		{
			text.Append(field.Name).Append('=').Append(Describe(field.GetValue(value), depth + 1)).Append(';');
		}
		return text.Append(')').ToString();
	}
}
