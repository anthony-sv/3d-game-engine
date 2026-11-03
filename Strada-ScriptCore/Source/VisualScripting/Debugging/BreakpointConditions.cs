using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace Strada.VisualScripting;

// Where a name a condition reads comes from (spec-slice4 section 2.3), in the order names are looked up.
internal enum ConditionSource : byte
{
	// An output of a loop whose body holds the node: Index, or a ForEach's Element.
	Loop,
	// A local of the node's function.
	Local,
	// A parameter of the node's function: an output of its Function.Entry.
	Parameter,
	// An output of the one event whose chains reach the node.
	Event,
	// A variable of the document.
	Variable,
}

// A name of a node's scope (spec-slice4 section 2.3): as the graph names it and its identifier form, its type, where it comes
// from (Node: the loop's or event's ID; 0 for the others) and where its value is: the slot of a local or parameter in the
// node's frame, the slot of an event output in the event graph's frame, the index of a variable, or the index of a loop node
// (Element: the ForEach's element, else the loop's Index).
internal sealed record ConditionName(string Name, string Form, VisualType Type, ConditionSource Source, uint Node, int Slot, bool Element);

// The values a condition reads at a stop point: the instance's variables, the event graph's frame, the frame of the node's
// graph and the flow entries of its call (from FlowBase to FlowEnd), live from an execution or copied in a snapshot.
internal readonly struct ConditionValues
{
	private readonly Slot[] m_Variables;
	private readonly Slot[] m_Events;
	private readonly Slot[] m_Frame;
	private readonly FlowEntry[] m_Flow;
	private readonly int m_FlowBase;
	private readonly int m_FlowEnd;
	private readonly CompiledGraph m_Graph;

	private ConditionValues(Slot[] variables, Slot[] events, Slot[] frame, FlowEntry[] flow, int flowBase, int flowEnd, CompiledGraph graph)
	{
		m_Variables = variables;
		m_Events = events;
		m_Frame = frame;
		m_Flow = flow;
		m_FlowBase = flowBase;
		m_FlowEnd = flowEnd;
		m_Graph = graph;
	}

	// What an execution reads at its stop point: its top frame.
	internal static ConditionValues Of(Execution execution) => new(execution.Instance.Variables, execution.Calls[0].Frame.Slots, execution.TopFrame.Slots,
		execution.Flow, execution.Top.FlowBase, execution.FlowCount, execution.TopFrame.Graph);

	// What a snapshotted execution read in one of its frames (0: the event graph's).
	internal static ConditionValues Of(DebugSnapshotMember member, int frame)
	{
		DebugSnapshotFrame copy = member.Frames[frame];
		int end = frame + 1 < member.Frames.Length ? member.Frames[frame + 1].FlowBase : member.Flow.Length;
		return new ConditionValues(member.Variables, member.Frames[0].Slots, copy.Slots, member.Flow, copy.FlowBase, end, copy.Graph);
	}

	// A name's value as the interpreter reads it: a loop's outputs from its innermost entry of the frame's flow (outside an
	// iteration Index is 0 and Element its type default, LoadOperand's rule).
	internal Slot Read(ConditionName name)
	{
		switch (name.Source)
		{
			case ConditionSource.Loop:
				int entry = FindLoop(name.Slot);
				if (!name.Element)
				{
					return Slot.FromInt(entry >= 0 ? m_Flow[entry].Index : 0);
				}
				return entry >= 0 ? ArrayElements.Read(m_Flow[entry].Items!, m_Flow[entry].Index, name.Type) : m_Graph.Nodes[name.Slot].Constant;
			case ConditionSource.Event:
				return m_Events[name.Slot];
			case ConditionSource.Variable:
				return m_Variables[name.Slot];
			default:
				return m_Frame[name.Slot];
		}
	}

	private int FindLoop(int node)
	{
		for (int index = m_FlowEnd - 1; index >= m_FlowBase; index--)
		{
			if (m_Flow[index].Node == node && m_Flow[index].Kind is FlowKind.ForLoop or FlowKind.ForEach)
			{
				return index;
			}
		}
		return -1;
	}
}

// The names a condition on a node may read (spec-slice4 section 2.3), looked up in four steps, the first step that has a
// name winning: the outputs of the loops whose body holds the node (innermost first), the locals of its function, the outputs
// of its one start (a function's parameters, or the outputs of the one event that leads to it), and the document's
// variables. Names are matched by their identifier forms; two values of one step under one form are ambiguous.
internal sealed class ConditionScope
{
	private readonly Dictionary<string, ConditionName> m_Names = new(StringComparer.Ordinal);
	private readonly Dictionary<string, string> m_Refusals = new(StringComparer.Ordinal);
	private readonly List<ConditionName> m_Readable = [];

	private ConditionScope()
	{
	}

	// Every name the node can read, in lookup order.
	internal IReadOnlyList<ConditionName> Readable => m_Readable;

	// "Max Health" reads as Max_Health: each character other than an ASCII letter, digit or underscore becomes an underscore,
	// and a leading digit gains one before it.
	internal static string FormOf(string name)
	{
		StringBuilder form = new(name.Length + 1);
		foreach (char character in name)
		{
			form.Append(character is >= 'A' and <= 'Z' or >= 'a' and <= 'z' or >= '0' and <= '9' or '_' ? character : '_');
		}
		if (form.Length == 0 || form[0] is >= '0' and <= '9')
		{
			form.Insert(0, '_');
		}
		return form.ToString();
	}

	// The scope of the node of a compiled graph of the program.
	internal static ConditionScope Of(CompiledGraph graph, CompiledNode node, GraphProgram program)
	{
		ConditionScope scope = new();
		scope.AddLoops(graph, node);
		List<ConditionName> locals = [];
		foreach (LocalSlot local in graph.Function?.Locals ?? [])
		{
			locals.Add(new ConditionName(local.Name, FormOf(local.Name), local.Type, ConditionSource.Local, 0, local.Slot, false));
		}
		scope.AddStep(locals);
		if (graph.Function is { } function)
		{
			scope.AddStep(OutputsOf(graph.Nodes[function.EntryNode], ConditionSource.Parameter));
		}
		else
		{
			scope.AddEventOutputs(graph, node);
		}
		List<ConditionName> variables = [];
		for (int index = 0; index < program.Variables.Length; index++)
		{
			CompiledVariable variable = program.Variables[index];
			variables.Add(new ConditionName(variable.Name, FormOf(variable.Name), variable.Type, ConditionSource.Variable, 0, index, false));
		}
		scope.AddStep(variables);
		return scope;
	}

	// The name an identifier of a condition reads; null with the reason when it reads none: an unknown name (with the nearest
	// readable one), an ambiguous one, or an event output of a node several starts lead to.
	internal ConditionName? Find(string identifier, out string? error)
	{
		if (m_Names.TryGetValue(identifier, out ConditionName? name))
		{
			error = null;
			return name;
		}
		if (m_Refusals.TryGetValue(identifier, out string? refusal))
		{
			error = refusal;
			return null;
		}
		string? nearest = null;
		foreach (ConditionName readable in m_Readable)
		{
			if (string.Equals(readable.Form, identifier, StringComparison.OrdinalIgnoreCase))
			{
				nearest = readable.Form;
				break;
			}
		}
		nearest ??= Suggestions.Nearest(identifier, m_Readable.Select(static readable => readable.Form)).FirstOrDefault();
		error = nearest != null ? $"unknown name {identifier}: did you mean {nearest}?" : $"unknown name {identifier}";
		return null;
	}

	// The names of one step: a form the earlier steps have is theirs; one this step has twice is ambiguous.
	private void AddStep(List<ConditionName> names)
	{
		foreach (IGrouping<string, ConditionName> group in names.GroupBy(static name => name.Form, StringComparer.Ordinal))
		{
			if (m_Names.ContainsKey(group.Key) || m_Refusals.ContainsKey(group.Key))
			{
				continue;
			}
			ConditionName[] candidates = [.. group];
			if (candidates.Length > 1)
			{
				m_Refusals[group.Key] = Ambiguous(group.Key, candidates.Select(Describe));
				continue;
			}
			m_Names[group.Key] = candidates[0];
			m_Readable.Add(candidates[0]);
		}
	}

	// The loops whose body holds the node: those it is reachable from through their Body exit without passing through the
	// loop again. Of the loops with an output of one name, the innermost (held by every other) is read; loops that do not
	// nest make the name ambiguous.
	private void AddLoops(CompiledGraph graph, CompiledNode node)
	{
		Dictionary<int, HashSet<int>> bodies = [];
		foreach (CompiledNode loop in graph.Nodes)
		{
			if (loop.Op is Opcode.ForLoop or Opcode.ForEach or Opcode.While)
			{
				bodies[loop.Index] = BodyOf(graph, loop);
			}
		}
		List<CompiledNode> holding = [.. bodies.Where(body => body.Value.Contains(node.Index)).Select(body => graph.Nodes[body.Key])];
		Dictionary<string, List<(CompiledNode Loop, ConditionName Name)>> byForm = new(StringComparer.Ordinal);
		foreach (CompiledNode loop in holding)
		{
			for (int output = 0; output < loop.OutputCount; output++)
			{
				string pin = loop.OutputPins[output];
				bool element = loop.Op == Opcode.ForEach && pin == "Element";
				if (pin != "Index" && !element)
				{
					continue;
				}
				ConditionName name = new(pin, FormOf(pin), loop.OutputTypes[output], ConditionSource.Loop, loop.Id, loop.Index, element);
				if (!byForm.TryGetValue(name.Form, out List<(CompiledNode, ConditionName)>? candidates))
				{
					candidates = [];
					byForm[name.Form] = candidates;
				}
				candidates.Add((loop, name));
			}
		}
		foreach ((string form, List<(CompiledNode Loop, ConditionName Name)> candidates) in byForm)
		{
			// The innermost candidates: no other candidate lies in their body.
			List<(CompiledNode Loop, ConditionName Name)> innermost =
				[.. candidates.Where(candidate => !candidates.Any(other => other.Loop != candidate.Loop && bodies[candidate.Loop.Index].Contains(other.Loop.Index)))];
			if (innermost.Count == 1)
			{
				m_Names[form] = innermost[0].Name;
				m_Readable.Add(innermost[0].Name);
			}
			else
			{
				m_Refusals[form] = Ambiguous(form, innermost.Select(static candidate => Naming(candidate.Loop)));
			}
		}
	}

	// The nodes reachable from a loop's Body exit along exec links, not through the loop itself.
	private static HashSet<int> BodyOf(CompiledGraph graph, CompiledNode loop)
	{
		HashSet<int> body = [];
		Stack<int> pending = new();
		pending.Push(loop.Exits[0].Node);
		while (pending.TryPop(out int index))
		{
			if (index < 0 || index == loop.Index || !body.Add(index))
			{
				continue;
			}
			foreach (ExecLink link in ChainExits(graph.Nodes[index]))
			{
				pending.Push(link.Node);
			}
		}
		return body;
	}

	// The event outputs a node of the event graph reads: those of its one start. A node several starts lead to (two events,
	// or an event and a Tick or timeline output, which start chains of their own) reads none, and says why when asked.
	private void AddEventOutputs(CompiledGraph graph, CompiledNode node)
	{
		List<CompiledNode> starts = [];
		foreach (CompiledNode start in graph.Nodes)
		{
			List<ExecLink> exits = [];
			if (start.Op == Opcode.Event)
			{
				exits.AddRange(start.Exits);
			}
			else if (start.Op == Opcode.Latent && start.TickExit >= 0)
			{
				exits.Add(start.Exits[start.TickExit]);
			}
			else if (start.Op == Opcode.Latent && start.Timeline is { } timeline)
			{
				exits.Add(start.Exits[timeline.UpdateExit]);
				exits.Add(start.Exits[timeline.FinishedExit]);
				exits.AddRange(timeline.EventExits.Select(exit => start.Exits[exit]));
			}
			else
			{
				continue;
			}
			if (start == node || Reaches(graph, exits, node.Index))
			{
				starts.Add(start);
			}
		}
		if (starts.Count == 1)
		{
			if (starts[0].Op == Opcode.Event)
			{
				AddStep(OutputsOf(starts[0], ConditionSource.Event));
			}
			return;
		}
		foreach (CompiledNode start in starts.Where(static start => start.Op == Opcode.Event))
		{
			CompiledNode other = starts.First(candidate => candidate != start);
			foreach (ConditionName output in OutputsOf(start, ConditionSource.Event))
			{
				m_Refusals.TryAdd(output.Form,
					$"{output.Form} is an output of {Naming(start)}, but {Naming(other)} also leads here: a condition reads event outputs only when one event leads to the node");
			}
		}
	}

	private static bool Reaches(CompiledGraph graph, List<ExecLink> exits, int target)
	{
		HashSet<int> visited = [];
		Stack<int> pending = new();
		foreach (ExecLink exit in exits)
		{
			pending.Push(exit.Node);
		}
		while (pending.TryPop(out int index))
		{
			if (index < 0 || !visited.Add(index))
			{
				continue;
			}
			if (index == target)
			{
				return true;
			}
			foreach (ExecLink link in ChainExits(graph.Nodes[index]))
			{
				pending.Push(link.Node);
			}
		}
		return false;
	}

	// The exits through which a chain goes on from a node: all of them, except those that start chains of their own (a
	// Tween's or Interval's Tick, a Timeline's outputs: its inputs end the chain).
	private static IEnumerable<ExecLink> ChainExits(CompiledNode node)
	{
		if (node.Op == Opcode.Latent && node.Timeline != null)
		{
			yield break;
		}
		for (int exit = 0; exit < node.Exits.Length; exit++)
		{
			if (node.Op != Opcode.Latent || exit != node.TickExit)
			{
				yield return node.Exits[exit];
			}
		}
	}

	private static List<ConditionName> OutputsOf(CompiledNode node, ConditionSource source)
	{
		List<ConditionName> outputs = [];
		for (int output = 0; output < node.OutputCount; output++)
		{
			string pin = node.OutputPins[output];
			outputs.Add(new ConditionName(pin, FormOf(pin), node.OutputTypes[output], source, node.Id, node.OutputBase + output, false));
		}
		return outputs;
	}

	private static string Ambiguous(string form, IEnumerable<string> candidates)
	{
		List<string> names = [.. candidates];
		string listed = names.Count == 2 ? $"{names[0]} or {names[1]}" : $"{string.Join(", ", names.Take(names.Count - 1))} or {names[^1]}";
		return $"{form} is ambiguous here: {listed}";
	}

	private static string Describe(ConditionName name) => name.Source switch
	{
		ConditionSource.Local => $"the local {name.Name}",
		ConditionSource.Parameter => $"the parameter {name.Name}",
		ConditionSource.Event => $"the event output {name.Name}",
		_ => $"the variable {name.Name}",
	};

	internal static string Naming(CompiledNode node) => FormattableString.Invariant($"{node.Title} (node {node.Id})");
}

// A breakpoint condition or an evaluated formula, compiled for one node of one compiled graph (spec-slice4 section 2.3): the
// formula reads the names of the node's scope; its string, enum and entity comparisons are computed into bool inputs before
// ExpressionMachine runs it over a scratch frame the formula owns. Evaluating allocates nothing.
internal sealed class CompiledCondition
{
	// Spec-slice4 section 2.1: the longest condition or formula.
	internal const int MaxBytes = 1024;

	private readonly ExpressionProgram? m_Program;
	private readonly Slot[] m_Scratch = [];
	// By input of the formula: the name it reads, null for an input only comparisons read.
	private readonly ConditionName?[] m_Inputs = [];
	private readonly Comparison[] m_Comparisons = [];
	private readonly int m_Output;

	private CompiledCondition(ConditionScope scope, string? error)
	{
		Scope = scope;
		Error = error;
	}

	private CompiledCondition(ConditionScope scope, ExpressionProgram program, Slot[] scratch, ConditionName?[] inputs, Comparison[] comparisons, int output,
		VisualType type, IReadOnlyList<string> reads)
	{
		Scope = scope;
		m_Program = program;
		m_Scratch = scratch;
		m_Inputs = inputs;
		m_Comparisons = comparisons;
		m_Output = output;
		Type = type;
		Reads = reads;
	}

	internal ConditionScope Scope { get; }

	// Why the formula does not compile; null when it does.
	internal string? Error { get; }

	// The type of the formula's value (bool for a condition); null when it does not compile.
	internal VisualType? Type { get; }

	// The names the formula reads, in the order it names them.
	internal IReadOnlyList<string> Reads { get; } = [];

	// Compiles a formula for a node of a compiled graph of the program; condition: it must give a bool. An empty formula
	// compiles to nothing (Error and Type null): only its scope.
	internal static CompiledCondition Compile(string text, CompiledGraph graph, CompiledNode node, GraphProgram program, bool condition)
	{
		ConditionScope scope = ConditionScope.Of(graph, node, program);
		if (text.Length == 0)
		{
			return new CompiledCondition(scope, null);
		}
		if (JsonText.ByteCount(text) > MaxBytes)
		{
			return new CompiledCondition(scope, FormattableString.Invariant($"a condition has at most {MaxBytes} bytes"));
		}
		NodeLibrary library = program.Library;
		ParsedFormula formula = ExpressionParser.Parse(text, static name => Suggestions.Nearest(name, ExpressionParser.CalleeNames),
			ExpressionParserOptions.Condition);
		if (formula.Error is { } parseError)
		{
			return new CompiledCondition(scope, parseError.Message);
		}
		HashSet<int> values = ValueInputs(formula.Root!);
		ConditionName?[] inputs = new ConditionName?[formula.Inputs.Count];
		List<SlotKind> kinds = [];
		List<string> reads = [];
		for (int index = 0; index < inputs.Length; index++)
		{
			ConditionName? name = scope.Find(formula.Inputs[index], out string? error);
			if (name == null)
			{
				return new CompiledCondition(scope, error);
			}
			reads.Add(name.Name);
			if (!values.Contains(index))
			{
				kinds.Add(SlotKind.Bool);
				continue;
			}
			if (ExpressionChecker.KindOf(name.Type) is not { } kind)
			{
				return new CompiledCondition(scope, Refused(name));
			}
			inputs[index] = name;
			kinds.Add(kind);
		}
		Comparison[] comparisons = new Comparison[formula.Comparisons.Count];
		for (int index = 0; index < comparisons.Length; index++)
		{
			ExpressionComparison comparison = formula.Comparisons[index];
			ConditionName name = scope.Find(formula.Inputs[comparison.Name], out _)!;
			if (Lower(name, comparison, out string? error) is not { } lowered)
			{
				return new CompiledCondition(scope, error);
			}
			comparisons[index] = lowered;
			kinds.Add(SlotKind.Bool);
		}
		(TypedExpression? root, ExpressionError? checkError) = ExpressionChecker.Check(library, formula, kinds);
		if (root == null)
		{
			return new CompiledCondition(scope, checkError!.Message);
		}
		if (condition && root.Type != SlotKind.Bool)
		{
			return new CompiledCondition(scope, $"the condition must be true or false, not {Article(ExpressionChecker.Name(root.Type))}");
		}
		if (ExpressionChecker.TypeOf(library, root.Type) is not { } type)
		{
			return new CompiledCondition(scope, $"the formula gives a {ExpressionChecker.Name(root.Type)}, a type this node library does not have");
		}
		// The scratch frame: the inputs, the comparisons' results, the value, then the formula's constants and temporaries.
		int output = kinds.Count;
		List<Slot> template = [.. new Slot[output + 1]];
		VisualType boolType = ExpressionChecker.TypeOf(library, SlotKind.Bool)!;
		List<VisualType> inputTypes = [.. kinds.Select((_, index) => index < inputs.Length && inputs[index] is { } read ? read.Type : boolType)];
		ExpressionProgram code = ExpressionCompiler.Compile(new ExpressionBinding(text, root, inputTypes, type), 0, output, slot =>
		{
			template.Add(slot);
			return template.Count - 1;
		});
		return new CompiledCondition(scope, code, [.. template], inputs, comparisons, output, type, [.. reads.Distinct(StringComparer.Ordinal)]);
	}

	// The formula's value read from the values of a stop point. Throws VisualScriptException with the part of the formula that
	// failed (an integer division by zero); must not be called for a formula that does not compile.
	internal Slot Evaluate(in ConditionValues values)
	{
		for (int index = 0; index < m_Inputs.Length; index++)
		{
			if (m_Inputs[index] is { } name)
			{
				m_Scratch[index] = values.Read(name);
			}
		}
		foreach (Comparison comparison in m_Comparisons)
		{
			m_Scratch[comparison.Input] = Slot.FromBool(comparison.Holds(values.Read(comparison.Name)));
		}
		ExpressionMachine.Run(m_Program!, m_Scratch);
		return m_Scratch[m_Output];
	}

	// The inputs the formula reads as values, not only through comparisons.
	private static HashSet<int> ValueInputs(ExpressionSyntax root)
	{
		HashSet<int> inputs = [];
		Stack<ExpressionSyntax> pending = new();
		pending.Push(root);
		while (pending.TryPop(out ExpressionSyntax? syntax))
		{
			if (syntax.Kind == ExpressionSyntaxKind.Input)
			{
				inputs.Add(syntax.Input);
			}
			foreach (ExpressionSyntax operand in syntax.Operands)
			{
				pending.Push(operand);
			}
		}
		return inputs;
	}

	// A comparison of a string with a string literal, of an enum with the name of one of its values, or of an entity with
	// None.
	private static Comparison? Lower(ConditionName name, ExpressionComparison comparison, out string? error)
	{
		error = null;
		VisualType type = name.Type;
		if (type.ClrType == typeof(string) && comparison.Text != null)
		{
			return new Comparison(name, comparison.Input, comparison.NotEqual, ComparisonKind.Text, comparison.Text, 0);
		}
		if (type.Kind == TypeKind.Enum && comparison.Text != null)
		{
			foreach (EnumeratorInfo enumerator in type.Enumerators)
			{
				if (enumerator.Name == comparison.Text)
				{
					return new Comparison(name, comparison.Input, comparison.NotEqual, ComparisonKind.Enum, null, LiteralCodec.ToBits(enumerator.Value));
				}
			}
			error = $"{type.Title} has no value {comparison.Text}: {string.Join(", ", type.Enumerators.Select(static enumerator => enumerator.Name))}";
			return null;
		}
		if (type.Kind == TypeKind.Entity && comparison.Text == null)
		{
			return new Comparison(name, comparison.Input, comparison.NotEqual, ComparisonKind.Entity, null, 0);
		}
		string kind = Article(NameOf(type));
		error = type.Kind == TypeKind.Entity ? $"{name.Form} is {kind}: an entity compares only with None"
			: type.ClrType == typeof(string) || type.Kind == TypeKind.Enum ? $"{name.Form} is {kind}: None compares only with entities"
			: $"{name.Form} is {kind}: string literals compare only with strings and enums, and None only with entities";
		return null;
	}

	// Why a name's value cannot be read in a formula: strings, enums and entities only compare, other types not at all.
	private static string Refused(ConditionName name)
	{
		VisualType type = name.Type;
		string kind = Article(NameOf(type));
		if (type.ClrType == typeof(string))
		{
			return $"{name.Form} is {kind}: compare it with == or != to a string literal, as in {name.Form} == \"Serve\"";
		}
		if (type.Kind == TypeKind.Enum)
		{
			string value = type.Enumerators.Count > 0 ? type.Enumerators[0].Name : "Value";
			return $"{name.Form} is {kind}: compare it with == or != to the name of one of its values, as in {name.Form} == \"{value}\"";
		}
		if (type.Kind == TypeKind.Entity)
		{
			return $"{name.Form} is {kind}: compare it with == or != to None, as in {name.Form} != None";
		}
		return $"{name.Form} is {kind}: conditions read numbers, booleans and vectors, and compare strings, enums and entities";
	}

	// "a float", "an int", "a Rigid Body".
	private static string Article(string title) => (title.Length > 0 && "aeiouAEIOU".Contains(title[0], StringComparison.Ordinal) ? "an " : "a ") + title;

	// A type in messages: the primitives and string by their IDs, as formulas name them ("a float"), the others by their titles
	// ("a Rigid Body").
	private static string NameOf(VisualType type) => type.ClrType is { IsPrimitive: true } || type.ClrType == typeof(string) ? type.Id : type.Title;

	private enum ComparisonKind : byte
	{
		Text,
		Enum,
		Entity,
	}

	// A lowered comparison: ordinal string equality, equality of enum bits, or an entity's ID against 0 (None).
	private sealed record Comparison(ConditionName Name, int Input, bool NotEqual, ComparisonKind Kind, string? Text, ulong Bits)
	{
		internal bool Holds(Slot value)
		{
			bool equal = Kind switch
			{
				ComparisonKind.Text => string.Equals(value.Object as string ?? string.Empty, Text, StringComparison.Ordinal),
				ComparisonKind.Enum => value.ULong == Bits,
				_ => ((value.Object as Entity)?.ID ?? 0) == 0,
			};
			return equal != NotEqual;
		}
	}
}

// The scope, formulas and Evaluate of breakpoint conditions over a program's nodes (spec-slice4 section 2.3).
internal static class BreakpointConditions
{
	// A node of a document in a compiled graph of a program: the graph that holds it, or null.
	internal static (CompiledGraph Graph, CompiledNode Node)? Find(GraphProgram program, ulong handle, ulong document, uint id)
	{
		foreach (CompiledGraph graph in GraphsOf(program))
		{
			if (ProgramTrace.DocumentOf(graph, handle) != document)
			{
				continue;
			}
			int index = graph.FindNode(id);
			if (index >= 0)
			{
				return (graph, graph.Nodes[index]);
			}
		}
		return null;
	}

	internal static IEnumerable<CompiledGraph> GraphsOf(GraphProgram program)
	{
		yield return program.EventGraph;
		foreach (CompiledFunction function in program.Functions)
		{
			yield return function.Body;
		}
	}

	// Whether the flow enters the node: a pure node's breakpoint, and one on a pure function's Entry or Return, stops each
	// instruction that evaluates it and takes no condition (spec-slice4 section 2.2).
	internal static bool IsEntered(CompiledNode node) => !node.Pure && node.Owner.Function?.Pure != true;

	internal const string PureCondition = "a breakpoint on a node no flow enters takes no condition: it stops each instruction that evaluates the node";

	// Evaluate (spec-slice4 section 2.3): the formulas read-only against a frame of a snapshotted execution (0: its event
	// graph's), each its value and its type, or why it gives none.
	internal static (Slot Value, VisualType? Type, string? Error) Evaluate(string formula, DebugSnapshotMember member, int frame)
	{
		DebugSnapshotFrame copy = member.Frames[frame];
		CompiledNode? node = frame == member.Frames.Length - 1 ? member.Node : copy.Graph.Nodes[member.Frames[frame + 1].CallNode];
		if (node == null)
		{
			return (default, null, "the frame stands at no node");
		}
		CompiledCondition compiled = CompiledCondition.Compile(formula, copy.Graph, node, member.Program, condition: false);
		if (compiled.Error != null || compiled.Type == null)
		{
			return (default, null, compiled.Error ?? "the formula is empty");
		}
		try
		{
			return (compiled.Evaluate(ConditionValues.Of(member, frame)), compiled.Type, null);
		}
		catch (VisualScriptException exception)
		{
			return (default, null, exception.Message);
		}
	}
}
