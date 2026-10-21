using System;

namespace Strada.VisualScripting;

// The managed API (spec section 5.7) that VisualScriptComponent's public members forward to: sending a custom event to an
// entity's graph and reading and writing its variables from C#. Values convert as links do (spec section 3.4), applied
// to the runtime types of the values C# passes; the graph's types are those of the library its program compiled with.
internal sealed partial class VisualScriptRuntime
{
	// Runs the entity's Event.Custom of the name with the arguments, as a nested execution when one is running, else as an
	// outermost one. Each argument converts to its parameter's type by spec section 3.4 rules 1 and 4 to 7; null only for
	// reference types. False (with a Warning) when the entity runs no graph, the graph has no such event or the arguments
	// do not fit, and the event does not run then; true when it ran to its end or suspended. An aborting error in the event
	// is logged once, then thrown as an InvalidOperationException carrying the logged message.
	internal bool SendEvent(ulong entity, string name, ReadOnlySpan<object?> arguments)
	{
		if (!m_Instances.TryGetValue(entity, out GraphInstance? instance))
		{
			m_Environment.Log(VisualScriptLogLevel.Warn,
				FormattableString.Invariant($"SendEvent {name}: {m_Environment.GetEntityName(entity)} ({entity}) runs no visual script"));
			return false;
		}
		GraphProgram program = instance.Program;
		int custom = program.FindCustomEvent(name);
		if (custom < 0)
		{
			WarnApi(instance, $"SendEvent {name}: the graph has no custom event {name}");
			return false;
		}
		CompiledNode node = program.EventGraph.Nodes[custom];
		VisualType[] parameters = node.EventParameters;
		if (arguments.Length != parameters.Length)
		{
			string expected = Count(parameters.Length, "argument");
			WarnApi(instance, FormattableString.Invariant($"SendEvent {name}: the event takes {expected}, not {arguments.Length}"));
			return false;
		}
		Slot[] values = new Slot[parameters.Length];
		for (int index = 0; index < values.Length; index++)
		{
			if (!TryConvert(program.Library, arguments[index], parameters[index], true, out values[index]))
			{
				string argument = FormattableString.Invariant($"argument {index + 1} ({node.OutputPins[index]})");
				WarnApi(instance, $"SendEvent {name}: {argument} is {DescribeValue(arguments[index])}, which does not convert to {parameters[index].Id}");
				return false;
			}
		}
		Enter(program);
		try
		{
			values.CopyTo(instance.Frame.Slots, node.OutputBase);
			Execution execution = Acquire(instance, custom, 0);
			ExecutionResult result = Execute(execution, !IsExecuting);
			string? error = result == ExecutionResult.Aborted ? execution.Error!.Message : null;
			Finish(execution, result);
			if (error != null)
			{
				throw new InvalidOperationException(error);
			}
			return true;
		}
		finally
		{
			Leave(program);
		}
	}

	// Whether the entity's graph instance has a document variable (public or private) of the name; false without one.
	internal bool HasVariable(ulong entity, string name) =>
		m_Instances.TryGetValue(entity, out GraphInstance? instance) && instance.FindVariable(name) >= 0;

	// The value of a document variable as a value of type, by spec section 3.4 rules 1, 4 and 5 applied to the value (a
	// None value reads as null for reference types). Throws InvalidOperationException when the entity runs no graph,
	// ArgumentException for an unknown variable and InvalidCastException when the value does not convert.
	internal object? GetVariable(ulong entity, string name, Type type)
	{
		GraphInstance instance = RequireInstance(entity);
		int index = RequireVariable(instance, name);
		GraphProgram program = instance.Program;
		object? value = instance.ReadVariable(index);
		if (value == null)
		{
			return !type.IsValueType || Nullable.GetUnderlyingType(type) != null ? null
				: throw new InvalidCastException($"{program.Name}: the variable {name} is None, which is not a {type.FullName}");
		}
		if (type == typeof(object))
		{
			return value;
		}
		VisualType? target = program.Library.FindType(type);
		if (target == null || !TryConvert(program.Library, value, target, false, out Slot converted))
		{
			throw new InvalidCastException(
				$"{program.Name}: the variable {name} holds {DescribeValue(value)}, which does not convert to {type.FullName}");
		}
		return SlotValues.ToValue(converted, target);
	}

	// Sets a document variable to a value of type (its runtime type when it is not null) of the variable's type or one that
	// converts to it by spec section 3.4 rules 1, 4 and 5; null for variables of reference types, when type converts. The
	// exceptions are GetVariable's.
	internal void SetVariable(ulong entity, string name, Type type, object? value)
	{
		GraphInstance instance = RequireInstance(entity);
		int index = RequireVariable(instance, name);
		GraphProgram program = instance.Program;
		VisualType variableType = program.Variables[index].Type;
		Slot slot;
		if (value == null)
		{
			VisualType? source = program.Library.FindType(type);
			if (!variableType.IsClrReference || source == null || !IsSafeConversion(TypeRules.GetConversion(source, variableType)))
			{
				throw new InvalidCastException($"{program.Name}: None of {type.FullName} does not convert to the variable {name} ({variableType.Id})");
			}
			slot = SlotValues.FromValue(null, variableType);
		}
		else if (!TryConvert(program.Library, value, variableType, false, out slot))
		{
			throw new InvalidCastException($"{program.Name}: {DescribeValue(value)} does not convert to the variable {name} ({variableType.Id})");
		}
		instance.Variables[index] = slot;
	}

	private GraphInstance RequireInstance(ulong entity) => m_Instances.TryGetValue(entity, out GraphInstance? instance) ? instance
		: throw new InvalidOperationException(FormattableString.Invariant($"{m_Environment.GetEntityName(entity)} ({entity}) runs no visual script"));

	private static int RequireVariable(GraphInstance instance, string name)
	{
		int index = instance.FindVariable(name);
		return index >= 0 ? index : throw new ArgumentException($"the graph {instance.Program.Name} has no variable {name}", nameof(name));
	}

	// Converts a value from C# to a graph type: rule 1 (the same type), 4 (numeric widening) and 5 (upcasts, components to
	// their entity, anything to System.Object); with links, also rule 6 (typed assets to their handle) and 7 (text). The
	// value's type is its runtime type; a value of a type the library does not have only converts to System.Object or, as
	// text, to string.
	private static bool TryConvert(NodeLibrary library, object? value, VisualType target, bool links, out Slot slot)
	{
		slot = default;
		if (value == null)
		{
			if (!target.IsClrReference)
			{
				return false;
			}
			slot = SlotValues.FromValue(null, target);
			return true;
		}
		if (target.ClrType == typeof(object))
		{
			slot = Slot.FromObject(value);
			return true;
		}
		if (library.FindType(value.GetType()) is not { } source)
		{
			if (links && target.ClrType == typeof(string))
			{
				slot = Slot.FromString(Conversions.ToText(value));
				return true;
			}
			return false;
		}
		ConversionKind? conversion = TypeRules.GetConversion(source, target);
		if (conversion is not { } kind || (!links && !IsSafeConversion(kind)))
		{
			return false;
		}
		slot = SlotValues.FromValue(value, source);
		Conversions.Apply(ref slot, Conversions.Plan(source, target, kind), source);
		return true;
	}

	// Spec section 3.4 rules 1, 4 and 5.
	private static bool IsSafeConversion(ConversionKind? kind) =>
		kind is ConversionKind.Identity or ConversionKind.Widen or ConversionKind.Upcast or ConversionKind.Box or ConversionKind.ComponentToEntity;

	private static string DescribeValue(object? value) => value == null ? "None" : $"a {value.GetType().FullName}";

	private static string Count(int count, string noun) => FormattableString.Invariant($"{count} {noun}{(count == 1 ? string.Empty : "s")}");

	private void WarnApi(GraphInstance instance, string message)
	{
		m_Environment.Log(VisualScriptLogLevel.Warn,
			FormattableString.Invariant($"{instance.Program.Name} on {m_Environment.GetEntityName(instance.Entity)} ({instance.Entity}): {message}"));
	}
}
