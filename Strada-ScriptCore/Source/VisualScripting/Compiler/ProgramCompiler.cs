using System;
using System.Collections.Generic;
using System.Reflection;

namespace Strada.VisualScripting;

// Compiles analyzed documents into programs (spec section 5.1): the frame layout of each graph (a slot for every data
// output, a staging slot for every data input, the node states and the locals), the loads of the connected inputs with
// their conversions, where every exec output leads, and what each kind of node needs. A document with an Error diagnostic
// gives a broken program, which only says why it cannot run. A disabled node compiles as a pass-through whatever its
// kind: its findings are Warnings, so nothing of its kind (definition, settings, pins of unknown type) is read.
internal static class ProgramCompiler
{
	// The ScriptEvent values (Create 0 to Click 8) and their event nodes.
	private static readonly BuiltinNode[] s_ScriptEvents =
	[
		BuiltinNode.EventCreate, BuiltinNode.EventUpdate, BuiltinNode.EventFixedUpdate, BuiltinNode.EventDestroy, BuiltinNode.EventCollisionEnter,
		BuiltinNode.EventCollisionExit, BuiltinNode.EventTriggerEnter, BuiltinNode.EventTriggerExit, BuiltinNode.EventClick,
	];

	internal static GraphProgram Compile(AnalyzedDocument document, string name)
	{
		if (document.HasErrors)
		{
			return new GraphProgram(document.Library, name, document.ErrorCount, document.FirstError);
		}
		Dictionary<AnalyzedVariable, int> variableIndexes = [];
		CompiledVariable[] variables = new CompiledVariable[document.Variables.Count];
		for (int index = 0; index < variables.Length; index++)
		{
			AnalyzedVariable variable = document.Variables[index];
			variables[index] = new CompiledVariable(variable, SlotValues.FromValue(variable.Value, variable.Type!));
			variableIndexes[variable] = index;
		}
		Dictionary<AnalyzedFunction, CompiledFunction> functions = [];
		CompiledFunction[] compiledFunctions = new CompiledFunction[document.Functions.Count];
		for (int index = 0; index < compiledFunctions.Length; index++)
		{
			AnalyzedFunction function = document.Functions[index];
			compiledFunctions[index] = new CompiledFunction(function);
			functions[function] = compiledFunctions[index];
		}
		Context context = new(document.Library, variableIndexes, functions);
		for (int index = 0; index < compiledFunctions.Length; index++)
		{
			AnalyzedFunction source = document.Functions[index];
			CompiledFunction function = compiledFunctions[index];
			GraphCompiler compiler = new(context, source.Graph, function);
			function.Graph = compiler.Compile();
			function.Locals = compiler.Locals;
			foreach (CompiledNode node in function.Graph.Nodes)
			{
				if (node.Builtin == BuiltinNode.FunctionEntry)
				{
					function.EntryNode = node.Index;
				}
				else if (node.Builtin == BuiltinNode.FunctionReturn && function.Pure)
				{
					function.ReturnNode = node.Index;
				}
			}
			Slot[] outputs = new Slot[source.Outputs.Count];
			for (int output = 0; output < outputs.Length; output++)
			{
				outputs[output] = SlotValues.DefaultOf(source.Outputs[output].Type!);
			}
			function.OutputDefaults = outputs;
		}
		CompiledGraph eventGraph = new GraphCompiler(context, document.EventGraph, null).Compile();

		int[] events = new int[s_ScriptEvents.Length];
		Array.Fill(events, -1);
		List<InputEvent> inputEvents = [];
		foreach (AnalyzedNode node in document.EventGraph.Nodes)
		{
			// A disabled event starts no execution.
			if (node.IsDisabled || node.Builtin is not { } builtin)
			{
				continue;
			}
			int index = eventGraph.FindNode(node.ID);
			int scriptEvent = Array.IndexOf(s_ScriptEvents, builtin);
			if (scriptEvent >= 0)
			{
				events[scriptEvent] = index;
			}
			else if (BuiltinNodes.IsInputEvent(builtin) && node.Binding is InputEventBinding input)
			{
				int gamepad = node.FindPin("Gamepad", PinDirection.In) is { Value: int value } ? value : 0;
				inputEvents.Add(new InputEvent(index, builtin, unchecked((int)LiteralCodec.ToBits(input.Value)), gamepad));
			}
		}
		return new GraphProgram(document.Library, name, variables, compiledFunctions, eventGraph, events, [.. inputEvents]);
	}

	private sealed record Context(NodeLibrary Library, Dictionary<AnalyzedVariable, int> Variables, Dictionary<AnalyzedFunction, CompiledFunction> Functions);

	private sealed class GraphCompiler
	{
		private readonly Context m_Context;
		private readonly AnalyzedGraph m_Graph;
		private readonly CompiledFunction? m_Function;
		private readonly List<Slot> m_Template = [];
		private readonly Dictionary<AnalyzedPin, int> m_OutputSlots = [];
		private readonly Dictionary<AnalyzedNode, int> m_Indexes = [];
		private readonly Dictionary<AnalyzedVariable, int> m_LocalSlots = [];
		private readonly List<LocalSlot> m_Locals = [];
		private readonly List<CompiledNode> m_Nodes = [];
		// The Make node of each split input, and the Break node that gives each field pin of a split output.
		private readonly Dictionary<AnalyzedPin, int> m_Makes = [];
		private readonly Dictionary<AnalyzedPin, int> m_Breaks = [];
		private readonly List<(CompiledNode Node, AnalyzedPin Pin)> m_Splits = [];

		internal GraphCompiler(Context context, AnalyzedGraph graph, CompiledFunction? function)
		{
			m_Context = context;
			m_Graph = graph;
			m_Function = function;
		}

		// The locals of a function's graph and their slots, in declaration order.
		internal LocalSlot[] Locals => [.. m_Locals];

		internal CompiledGraph Compile()
		{
			for (int index = 0; index < m_Graph.Nodes.Count; index++)
			{
				m_Indexes[m_Graph.Nodes[index]] = index;
			}
			foreach (AnalyzedVariable local in m_Graph.Owner?.Locals ?? [])
			{
				int slot = Allocate(SlotValues.FromValue(local.Value, local.Type!));
				m_LocalSlots[local] = slot;
				m_Locals.Add(new LocalSlot(local.ID, local.Type!, slot));
			}
			int count = m_Graph.Nodes.Count;
			List<AnalyzedPin>[] inputs = new List<AnalyzedPin>[count];
			for (int index = 0; index < count; index++)
			{
				AnalyzedNode source = m_Graph.Nodes[index];
				CompiledNode node = new(source, index, OpcodeOf(source));
				m_Nodes.Add(node);
				inputs[index] = DataInputs(source, node);
				AllocateOutputs(source, node);
			}
			foreach (AnalyzedNode source in m_Graph.Nodes)
			{
				if (!source.HasSplitPins)
				{
					continue;
				}
				foreach (AnalyzedPin pin in source.Pins)
				{
					if (pin.IsSplit)
					{
						AddSplit(source, pin);
					}
				}
			}
			for (int index = 0; index < count; index++)
			{
				AnalyzedNode source = m_Graph.Nodes[index];
				CompiledNode node = m_Nodes[index];
				CompileExecPins(source, node);
				CompileInputs(source, node, inputs[index]);
				CompileKind(source, node);
			}
			foreach ((CompiledNode node, AnalyzedPin pin) in m_Splits)
			{
				CompileSplit(node, pin);
			}
			return new CompiledGraph([.. m_Nodes], [.. m_Template], m_Function);
		}

		// The Make of a split input: its field pins are its inputs (staged like any input) and its output is the input's
		// value. The Break of a split output: the output is its input and its outputs are the field pins (which links read
		// like any output). Nested splits get theirs too. Neither has state, so a live edit that splits or recombines a pin
		// changes no node of the document.
		private void AddSplit(AnalyzedNode owner, AnalyzedPin pin)
		{
			IReadOnlyList<AnalyzedPin> fields = pin.Fields!;
			CompiledNode node = new(owner, m_Nodes.Count, pin.IsInput ? Opcode.Make : Opcode.Break, pin.Type);
			m_Nodes.Add(node);
			m_Splits.Add((node, pin));
			if (pin.IsInput)
			{
				m_Makes[pin] = node.Index;
				node.InputBase = m_Template.Count;
				node.InputCount = fields.Count;
				foreach (AnalyzedPin field in fields)
				{
					Allocate(SlotValues.DefaultOf(field.Type));
				}
				// Its output is the staged value of the input it makes, which is no output pin: it reports no value to a debugger.
				node.OutputBase = Allocate(SlotValues.DefaultOf(pin.Type));
			}
			else
			{
				node.InputBase = Allocate(SlotValues.DefaultOf(pin.Type));
				node.InputCount = 1;
				node.OutputBase = m_Template.Count;
				List<string> names = [];
				List<VisualType> types = [];
				foreach (AnalyzedPin field in fields)
				{
					m_Breaks[field] = node.Index;
					m_OutputSlots[field] = Allocate(SlotValues.DefaultOf(field.Type));
					names.Add(field.Name);
					types.Add(field.Type);
				}
				node.OutputCount = fields.Count;
				node.OutputPins = [.. names];
				node.OutputTypes = [.. types];
			}
			foreach (AnalyzedPin field in fields)
			{
				if (field.IsSplit)
				{
					AddSplit(owner, field);
				}
			}
		}

		private void CompileSplit(CompiledNode node, AnalyzedPin pin)
		{
			List<Operand> operands = [];
			List<int> pureSources = [];
			if (pin.IsInput)
			{
				IReadOnlyList<AnalyzedPin> fields = pin.Fields!;
				for (int index = 0; index < fields.Count; index++)
				{
					CompileInput(fields[index], node.InputBase + index, fields[index].Type, operands, pureSources);
				}
			}
			else
			{
				operands.Add(Load(pin, node.InputBase, ConversionOp.None, pureSources));
			}
			node.Operands = [.. operands];
			node.PureSources = [.. pureSources];
		}

		private int Allocate(Slot initial)
		{
			m_Template.Add(initial);
			return m_Template.Count - 1;
		}

		private static Opcode OpcodeOf(AnalyzedNode node)
		{
			if (node.IsDisabled)
			{
				return Opcode.PassThrough;
			}
			if (node.Definition is MemberNodeDefinition)
			{
				return node.Pure ? Opcode.PureMember : Opcode.Member;
			}
			BuiltinNode builtin = node.Builtin ?? throw new InvalidOperationException($"{node} has no node kind");
			if (BuiltinNodes.IsEvent(builtin))
			{
				return Opcode.Event;
			}
			if (BuiltinNodes.IsLatent(builtin))
			{
				return Opcode.Latent;
			}
			return builtin switch
			{
				BuiltinNode.EventSend => Opcode.Send,
				BuiltinNode.FlowBranch => Opcode.Branch,
				BuiltinNode.FlowSequence => Opcode.Sequence,
				BuiltinNode.FlowSwitch => Opcode.Switch,
				BuiltinNode.FlowForLoop => Opcode.ForLoop,
				BuiltinNode.FlowForEach => Opcode.ForEach,
				BuiltinNode.FlowWhile => Opcode.While,
				BuiltinNode.FlowDoOnce => Opcode.DoOnce,
				BuiltinNode.FlowDoN => Opcode.DoN,
				BuiltinNode.FlowFlipFlop => Opcode.FlipFlop,
				BuiltinNode.FlowGate => Opcode.Gate,
				BuiltinNode.VariableGet => Opcode.VariableGet,
				BuiltinNode.VariableSet => Opcode.VariableSet,
				BuiltinNode.FunctionEntry => Opcode.FunctionEntry,
				BuiltinNode.FunctionReturn => Opcode.FunctionReturn,
				BuiltinNode.FunctionCall => node.Pure ? Opcode.PureCall : Opcode.FunctionCall,
				BuiltinNode.Self => Opcode.Self,
				BuiltinNode.Literal => Opcode.Literal,
				BuiltinNode.Reroute => node.Pure ? Opcode.Reroute : Opcode.ExecReroute,
				BuiltinNode.Select => Opcode.Select,
				BuiltinNode.IsValid => Opcode.IsValid,
				BuiltinNode.Convert => Opcode.Convert,
				BuiltinNode.Make => Opcode.Make,
				BuiltinNode.Break => Opcode.Break,
				BuiltinNode.FormatString => Opcode.Format,
				BuiltinNode.ArrayLength => Opcode.ArrayLength,
				BuiltinNode.ArrayGet => Opcode.ArrayGet,
				_ => OperatorOpcode(node, builtin),
			};
		}

		private static Opcode OperatorOpcode(AnalyzedNode node, BuiltinNode builtin)
		{
			if (builtin is BuiltinNode.OperatorAnd or BuiltinNode.OperatorOr or BuiltinNode.OperatorXor or BuiltinNode.OperatorNot)
			{
				return Opcode.Boolean;
			}
			OperatorBinding binding = node.Binding as OperatorBinding ?? throw new InvalidOperationException($"{node} has no operator binding");
			if (builtin is BuiltinNode.OperatorEqual or BuiltinNode.OperatorNotEqual)
			{
				return Opcode.Equality;
			}
			if (binding.Mode == OperatorMode.Method)
			{
				return Opcode.OperatorMethod;
			}
			return builtin switch
			{
				BuiltinNode.OperatorNegate => Opcode.Negate,
				BuiltinNode.OperatorLess or BuiltinNode.OperatorLessOrEqual or BuiltinNode.OperatorGreater or BuiltinNode.OperatorGreaterOrEqual => Opcode.Compare,
				_ => binding.Mode == OperatorMode.Concatenate ? Opcode.Concatenate : Opcode.Arithmetic,
			};
		}

		// The data inputs whose values the node loads, in the order the interpreter reads them (pin order). Event nodes
		// load nothing: their inputs are constants the program reads when it is compiled.
		private List<AnalyzedPin> DataInputs(AnalyzedNode source, CompiledNode node)
		{
			List<AnalyzedPin> inputs = [];
			if (node.Op == Opcode.Event)
			{
				return inputs;
			}
			if (node.Op == Opcode.PassThrough)
			{
				inputs = PassThroughInputs(source, node);
			}
			else
			{
				foreach (AnalyzedPin pin in source.Pins)
				{
					if (pin.IsInput && !pin.Type.IsExec)
					{
						inputs.Add(pin);
					}
				}
			}
			ExpectNames(source, inputs, node.Op switch
			{
				Opcode.Branch or Opcode.While => ["Condition"],
				Opcode.Switch => ["Selection"],
				Opcode.ForLoop => ["First", "Last"],
				Opcode.ForEach => ["Array"],
				Opcode.DoN => ["N"],
				Opcode.Select => ["Condition", "True", "False"],
				Opcode.IsValid or Opcode.Convert or Opcode.Break => ["Value"],
				Opcode.Reroute => ["In"],
				Opcode.ArrayLength => ["Array"],
				Opcode.ArrayGet => ["Array", "Index"],
				Opcode.Negate => ["A"],
				Opcode.Boolean => source.Builtin == BuiltinNode.OperatorNot ? ["A"] : ["A", "B"],
				Opcode.Arithmetic or Opcode.Compare or Opcode.Equality or Opcode.Concatenate => ["A", "B"],
				Opcode.OperatorMethod => source.Builtin == BuiltinNode.OperatorNegate ? ["A"] : ["A", "B"],
				Opcode.VariableSet => ["Value"],
				Opcode.Self or Opcode.Literal or Opcode.VariableGet or Opcode.FunctionEntry => [],
				Opcode.Latent => source.Builtin switch
				{
					BuiltinNode.FlowWaitFrames => ["Frames"],
					BuiltinNode.FlowWaitUntil => ["Condition"],
					BuiltinNode.FlowInterval => ["Interval", "Count"],
					_ => ["Duration"],
				},
				_ => null,
			});
			node.InputBase = m_Template.Count;
			node.InputCount = inputs.Count;
			foreach (AnalyzedPin pin in inputs)
			{
				Allocate(SlotValues.DefaultOf(StagingType(source, node, pin)));
			}
			return inputs;
		}

		// A disabled node's data outputs, in pin order, each take the first data input of the same type that no earlier output
		// takes, in pin order (a disabled Variable.Set passes its Value on); the others take their type default. Only the
		// inputs an output takes are loaded, so only their pure sources are evaluated. Pins of unknown type take part in
		// nothing, and neither does an unconnected Target: it holds no value, it means Self only to the node that runs.
		private static List<AnalyzedPin> PassThroughInputs(AnalyzedNode source, CompiledNode node)
		{
			List<AnalyzedPin> candidates = [];
			foreach (AnalyzedPin pin in source.Pins)
			{
				if (pin.IsInput && !pin.Type.IsExec && pin.Type.HasValues && !(pin.Description.DefaultsToSelf && !pin.IsConnected))
				{
					candidates.Add(pin);
				}
			}
			bool[] taken = new bool[candidates.Count];
			List<AnalyzedPin> inputs = [];
			List<int> sources = [];
			foreach (AnalyzedPin pin in source.Pins)
			{
				if (pin.IsInput || pin.Type.IsExec)
				{
					continue;
				}
				int staged = -1;
				for (int candidate = 0; pin.Type.HasValues && candidate < candidates.Count; candidate++)
				{
					if (!taken[candidate] && candidates[candidate].Type.Id == pin.Type.Id)
					{
						taken[candidate] = true;
						staged = inputs.Count;
						inputs.Add(candidates[candidate]);
						break;
					}
				}
				sources.Add(staged);
			}
			node.PassThroughSources = [.. sources];
			return inputs;
		}

		// Outputs get slots in pin order; DoN's Counter and FlipFlop's IsA are their nodes' state. A latent node's state slot
		// holds the fiber waiting in it while it is pending (none at first).
		private void AllocateOutputs(AnalyzedNode source, CompiledNode node)
		{
			node.OutputBase = m_Template.Count;
			List<string> names = [];
			List<VisualType> types = [];
			foreach (AnalyzedPin pin in source.Pins)
			{
				if (pin.IsInput || pin.Type.IsExec)
				{
					continue;
				}
				m_OutputSlots[pin] = Allocate(pin.Type.HasValues ? SlotValues.DefaultOf(pin.Type) : default);
				names.Add(pin.Name);
				types.Add(pin.Type);
			}
			node.OutputCount = names.Count;
			node.OutputPins = [.. names];
			node.OutputTypes = [.. types];
			switch (node.Op)
			{
				case Opcode.DoN:
				case Opcode.FlipFlop:
					node.StateSlot = node.OutputBase;
					break;
				case Opcode.DoOnce:
				case Opcode.Gate:
					node.StateSlot = Allocate(Slot.FromBool(((GateBinding)source.Binding!).StartOpen));
					break;
				case Opcode.Latent:
					node.StateSlot = Allocate(default);
					break;
			}
		}

		// Exec inputs and outputs in pin order; an output leads to the exec input its link enters. A disabled node has one exit,
		// its pass-through, or none when its chain ends there; every exec input enters it alike.
		private void CompileExecPins(AnalyzedNode source, CompiledNode node)
		{
			if (node.Op == Opcode.PassThrough)
			{
				if (source.PassThrough is { } exit)
				{
					node.ExitPins = [exit.Name];
					node.Exits = [ExitTarget(exit)];
				}
				return;
			}
			List<string> entries = [];
			List<string> exits = [];
			List<ExecLink> links = [];
			foreach (AnalyzedPin pin in source.Pins)
			{
				if (!pin.Type.IsExec)
				{
					continue;
				}
				if (pin.IsInput)
				{
					entries.Add(pin.Name);
					continue;
				}
				if (pin.Description.Role == PinRole.Continuation)
				{
					node.ContinuationExit = exits.Count;
				}
				else if (pin.Description.Role == PinRole.Tick)
				{
					node.TickExit = exits.Count;
				}
				exits.Add(pin.Name);
				links.Add(ExitTarget(pin));
			}
			node.ExitPins = [.. exits];
			node.Exits = [.. links];
			string[]? expectedEntries = node.Op switch
			{
				Opcode.ForLoop or Opcode.ForEach => ["Execute", "Break"],
				Opcode.DoOnce or Opcode.DoN => ["Execute", "Reset"],
				Opcode.Gate => ["Enter", "Open", "Close", "Toggle"],
				Opcode.Branch or Opcode.Sequence or Opcode.Switch or Opcode.While or Opcode.FlipFlop or Opcode.Send or Opcode.VariableSet => ["Execute"],
				// Entry 0 starts a latent node, entry 1 stops a Tween or Interval.
				Opcode.Latent => source.Builtin switch
				{
					BuiltinNode.FlowTween => ["Execute", "Stop"],
					BuiltinNode.FlowInterval => ["Start", "Stop"],
					_ => ["Execute"],
				},
				_ => null,
			};
			string[]? expectedExits = node.Op switch
			{
				Opcode.Branch => ["True", "False"],
				Opcode.ForLoop or Opcode.ForEach or Opcode.While => ["Body", "Completed"],
				Opcode.DoOnce => ["Completed"],
				Opcode.DoN or Opcode.Gate => ["Exit"],
				Opcode.FlipFlop => ["A", "B"],
				Opcode.Event or Opcode.Send or Opcode.VariableSet or Opcode.FunctionCall => ["Then"],
				Opcode.Latent => source.Builtin switch
				{
					BuiltinNode.FlowTween => ["Update", "Completed"],
					BuiltinNode.FlowInterval => ["Tick", "Completed"],
					_ => ["Completed"],
				},
				_ => null,
			};
			if ((expectedEntries != null && !SameNames(entries, expectedEntries)) || (expectedExits != null && !SameNames(exits, expectedExits)))
			{
				throw new InvalidOperationException($"{source} does not have the exec pins of its kind");
			}
		}

		// Where the flow leaving through an exec output goes: the exec input its valid link enters, else nowhere.
		private ExecLink ExitTarget(AnalyzedPin output)
		{
			foreach (AnalyzedLink link in output.Links)
			{
				if (link.IsValid)
				{
					return new ExecLink(m_Indexes[link.To.Node], EntryIndex(link.To));
				}
			}
			return ExecLink.None;
		}

		private static int EntryIndex(AnalyzedPin input)
		{
			int index = 0;
			foreach (AnalyzedPin pin in input.Node.Pins)
			{
				if (!pin.IsInput || !pin.Type.IsExec)
				{
					continue;
				}
				if (ReferenceEquals(pin, input))
				{
					return index;
				}
				index++;
			}
			throw new InvalidOperationException($"{input} is not an exec input");
		}

		// The loads of connected inputs, the constants of unconnected ones (in the template), and the pure nodes feeding
		// them.
		private void CompileInputs(AnalyzedNode source, CompiledNode node, List<AnalyzedPin> inputs)
		{
			List<Operand> operands = [];
			List<int> pureSources = [];
			for (int index = 0; index < inputs.Count; index++)
			{
				AnalyzedPin pin = inputs[index];
				if (pin.Description.DefaultsToSelf && pin.Source == null)
				{
					node.TargetIsSelf = true;
					continue;
				}
				CompileInput(pin, node.InputBase + index, StagingType(source, node, pin), operands, pureSources);
			}
			node.Operands = [.. operands];
			node.PureSources = [.. pureSources];
		}

		// An input staged at target as the type staging: the load of its link, the value its split pin's Make makes, or its
		// constant.
		private void CompileInput(AnalyzedPin pin, int target, VisualType staging, List<Operand> operands, List<int> pureSources)
		{
			if (pin.Source is { } link)
			{
				AnalyzedPin from = link.From;
				ConversionOp conversion = ReferenceEquals(staging, pin.Type)
					? Conversions.Plan(from.Type, pin.Type, link.Conversion ?? ConversionKind.Identity)
					: Conversions.Plan(from.Type, staging) ?? throw new InvalidOperationException($"{from} does not convert to {staging.Id} for {pin}");
				operands.Add(Load(from, target, conversion, pureSources));
				return;
			}
			if (m_Makes.TryGetValue(pin, out int make))
			{
				ConversionOp conversion = Conversions.Plan(pin.Type, staging) ?? throw new InvalidOperationException($"{pin} does not convert to {staging.Id}");
				operands.Add(new Operand(target, OperandSource.Slot, m_Nodes[make].OutputBase, conversion, pin.Type));
				AddPureSource(pureSources, make);
				return;
			}
			Slot constant = SlotValues.FromValue(pin.Value, pin.Type);
			if (!ReferenceEquals(staging, pin.Type))
			{
				ConversionOp conversion = Conversions.Plan(pin.Type, staging) ?? throw new InvalidOperationException($"{pin} does not convert to {staging.Id}");
				Conversions.Apply(ref constant, conversion, pin.Type);
			}
			m_Template[target] = constant;
		}

		// The load of an output into a staging slot: a field pin of a split output is an output of its Break, which the plan
		// evaluates first, like a pure node's output.
		private Operand Load(AnalyzedPin from, int target, ConversionOp conversion, List<int> pureSources)
		{
			if (m_Breaks.TryGetValue(from, out int split))
			{
				AddPureSource(pureSources, split);
				return new Operand(target, OperandSource.Slot, m_OutputSlots[from], conversion, from.Type);
			}
			OperandSource kind = LoopOutput(from);
			int sourceIndex = kind == OperandSource.Slot ? m_OutputSlots[from] : m_Indexes[from.Node];
			if (from.Node.Pure)
			{
				AddPureSource(pureSources, m_Indexes[from.Node]);
			}
			return new Operand(target, kind, sourceIndex, conversion, from.Type);
		}

		private static void AddPureSource(List<int> pureSources, int node)
		{
			if (!pureSources.Contains(node))
			{
				pureSources.Add(node);
			}
		}

		// ForLoop's Index and ForEach's Index and Element are read from the active loop entry, not from slots; a disabled loop
		// runs no iteration, and its outputs are slots like every disabled node's.
		private static OperandSource LoopOutput(AnalyzedPin output) => output.Node.IsDisabled ? OperandSource.Slot : output.Node.Builtin switch
		{
			BuiltinNode.FlowForLoop when output.Name == "Index" => OperandSource.LoopIndex,
			BuiltinNode.FlowForEach when output.Name == "Index" => OperandSource.LoopIndex,
			BuiltinNode.FlowForEach when output.Name == "Element" => OperandSource.LoopElement,
			_ => OperandSource.Slot,
		};

		// The type an input is staged as: its pin's type, except operators (their operands at the join type or the operator
		// method's parameter types) and the text inputs of Concatenate.
		private VisualType StagingType(AnalyzedNode source, CompiledNode node, AnalyzedPin pin)
		{
			switch (node.Op)
			{
				case Opcode.Arithmetic:
				case Opcode.Compare:
				case Opcode.Negate:
					return ((OperatorBinding)source.Binding!).Operand!;
				case Opcode.Equality when source.Binding is OperatorBinding { Mode: not OperatorMode.Method, Operand: { } join }:
					return join;
				case Opcode.Equality:
				case Opcode.OperatorMethod:
					// An equality operator declared on a base class the library does not have takes the operands as they are.
					OperatorBinding binding = (OperatorBinding)source.Binding!;
					int parameter = pin.Name == "A" ? 0 : 1;
					return m_Context.Library.FindType(binding.Method!.GetParameters()[parameter].ParameterType) ?? binding.Operand ??
						throw new InvalidOperationException($"{binding.Method} takes a type the library does not have");
				case Opcode.Concatenate:
				case Opcode.Format:
					return m_Context.Library.String;
				default:
					return pin.Type;
			}
		}

		private void CompileKind(AnalyzedNode source, CompiledNode node)
		{
			switch (node.Op)
			{
				case Opcode.Event when source.Builtin == BuiltinNode.EventCustom:
				case Opcode.Send:
					CustomEventBinding custom = (CustomEventBinding)source.Binding!;
					node.EventName = custom.Name;
					node.EventParameters = TypesOf(custom.Parameters);
					break;
				case Opcode.Sequence:
					node.Count = ((SequenceBinding)source.Binding!).Count;
					break;
				case Opcode.Switch:
					CompileSwitch(source, node);
					break;
				case Opcode.VariableGet:
				case Opcode.VariableSet:
					AnalyzedVariable variable = ((VariableBinding)source.Binding!).Variable;
					node.IsLocal = variable.IsLocal;
					node.Count = variable.IsLocal ? m_LocalSlots[variable] : m_Context.Variables[variable];
					break;
				case Opcode.FunctionCall:
				case Opcode.PureCall:
					node.Function = m_Context.Functions[((FunctionBinding)source.Binding!).Function];
					break;
				case Opcode.Literal:
					LiteralBinding literal = (LiteralBinding)source.Binding!;
					node.Constant = SlotValues.FromValue(literal.Value, literal.Type);
					break;
				case Opcode.ForEach:
					VisualType element = source.FindPin("Element", PinDirection.Out)!.Type;
					node.Constant = element.HasValues ? SlotValues.DefaultOf(element) : default;
					break;
				case Opcode.Latent when source.Binding is TweenBinding tween:
					node.Easing = tween.Easing;
					break;
				case Opcode.IsValid:
					VisualType value = source.FindPin("Value", PinDirection.In)!.Type;
					node.Validity = value.Kind switch
					{
						TypeKind.Entity => ValidityKind.Entity,
						TypeKind.Component => ValidityKind.Component,
						TypeKind.Asset => ValidityKind.Asset,
						_ => ValidityKind.Reference,
					};
					break;
				case Opcode.Convert:
					CompileConvert(source, node);
					break;
				case Opcode.Make:
				case Opcode.Break:
					node.ToType = ((StructBinding)source.Binding!).Type;
					break;
				case Opcode.Format:
					node.Segments = [.. ((FormatBinding)source.Binding!).Segments];
					break;
				case Opcode.ArrayGet:
					node.FromType = source.FindPin("Element", PinDirection.Out)!.Type;
					break;
				case Opcode.Arithmetic:
				case Opcode.Compare:
				case Opcode.Negate:
					node.Operator = OperatorOf(source.Builtin!.Value);
					node.Kind = SlotValues.KindOf(((OperatorBinding)source.Binding!).Operand!);
					break;
				case Opcode.Boolean:
					node.Operator = OperatorOf(source.Builtin!.Value);
					break;
				case Opcode.Equality:
					CompileEquality(source, node);
					break;
				case Opcode.OperatorMethod:
					node.Member = ((OperatorBinding)source.Binding!).Method;
					break;
				case Opcode.Member:
				case Opcode.PureMember:
					MemberBinding member = (MemberBinding)source.Binding!;
					node.Member = member.Member;
					node.MemberKind = member.Definition.MemberKind;
					node.Target = member.Definition.Target;
					node.DeclaringType = member.Member.DeclaringType;
					break;
			}
		}

		private static void CompileSwitch(AnalyzedNode source, CompiledNode node)
		{
			SwitchBinding binding = (SwitchBinding)source.Binding!;
			node.Kind = SlotValues.KindOf(binding.Type);
			if (binding.Type.ClrType == typeof(string))
			{
				Dictionary<string, int> cases = new(StringComparer.Ordinal);
				for (int index = 0; index < binding.Cases.Count; index++)
				{
					cases[(string)binding.Cases[index].Value] = index;
				}
				node.TextCases = cases;
			}
			else
			{
				Dictionary<long, int> cases = [];
				for (int index = 0; index < binding.Cases.Count; index++)
				{
					object value = binding.Cases[index].Value;
					cases[value is int number ? number : unchecked((long)LiteralCodec.ToBits(value))] = index;
				}
				node.NumberCases = cases;
			}
			// The exits are the cases in order, then Default.
			node.Count = binding.Cases.Count;
		}

		private static void CompileConvert(AnalyzedNode source, CompiledNode node)
		{
			ConvertBinding binding = (ConvertBinding)source.Binding!;
			node.FromType = binding.From;
			node.ToType = binding.To;
			node.ConvertPlan = binding.Plan;
			node.Member = binding.Method;
			if (binding.Plan == ConvertPlan.Link)
			{
				node.Conversion = Conversions.Plan(binding.From, binding.To, binding.Link!.Value);
			}
			else if (binding.Plan == ConvertPlan.EnumToInteger)
			{
				// Enum bits hold the underlying value sign-extended: read them as a long or a ulong.
				node.Kind = SlotValues.IsSigned(Enum.GetUnderlyingType(binding.From.ClrType!)) ? SlotKind.Long : SlotKind.ULong;
			}
			else if (binding.Plan is ConvertPlan.ParseBool or ConvertPlan.ParseNumber or ConvertPlan.ParseEnum)
			{
				// Text that cannot be read gives the type default.
				node.Constant = SlotValues.DefaultOf(binding.To);
			}
		}

		private static void CompileEquality(AnalyzedNode source, CompiledNode node)
		{
			OperatorBinding binding = (OperatorBinding)source.Binding!;
			node.Invert = source.Builtin == BuiltinNode.OperatorNotEqual;
			VisualType? join = binding.Operand;
			node.Equality = binding.Mode switch
			{
				OperatorMode.Numeric => EqualityKind.Numeric,
				OperatorMode.ValueEquality when join!.Kind == TypeKind.Enum => EqualityKind.Enum,
				OperatorMode.ValueEquality when join!.ClrType == typeof(string) => EqualityKind.String,
				OperatorMode.ValueEquality => EqualityKind.Bool,
				OperatorMode.EntityEquality => EqualityKind.Entity,
				OperatorMode.AssetEquality => EqualityKind.Asset,
				OperatorMode.ComponentEquality => EqualityKind.Component,
				OperatorMode.Method => EqualityKind.Method,
				_ => EqualityKind.Object,
			};
			node.Kind = join != null ? SlotValues.KindOf(join) : SlotKind.Object;
			node.Member = binding.Method;
		}

		private static OperatorKind OperatorOf(BuiltinNode builtin) => builtin switch
		{
			BuiltinNode.OperatorAdd => OperatorKind.Add,
			BuiltinNode.OperatorSubtract => OperatorKind.Subtract,
			BuiltinNode.OperatorMultiply => OperatorKind.Multiply,
			BuiltinNode.OperatorDivide => OperatorKind.Divide,
			BuiltinNode.OperatorModulo => OperatorKind.Modulo,
			BuiltinNode.OperatorLess => OperatorKind.Less,
			BuiltinNode.OperatorLessOrEqual => OperatorKind.LessOrEqual,
			BuiltinNode.OperatorGreater => OperatorKind.Greater,
			BuiltinNode.OperatorGreaterOrEqual => OperatorKind.GreaterOrEqual,
			BuiltinNode.OperatorAnd => OperatorKind.And,
			BuiltinNode.OperatorOr => OperatorKind.Or,
			BuiltinNode.OperatorXor => OperatorKind.Xor,
			// Negate and Not have one operand.
			_ => OperatorKind.Not,
		};

		private static VisualType[] TypesOf(IReadOnlyList<AnalyzedParameter> parameters)
		{
			VisualType[] types = new VisualType[parameters.Count];
			for (int index = 0; index < types.Length; index++)
			{
				types[index] = parameters[index].Type!;
			}
			return types;
		}

		private static void ExpectNames(AnalyzedNode source, List<AnalyzedPin> pins, string[]? names)
		{
			if (names == null)
			{
				return;
			}
			List<string> actual = [];
			foreach (AnalyzedPin pin in pins)
			{
				actual.Add(pin.Name);
			}
			if (!SameNames(actual, names))
			{
				throw new InvalidOperationException($"{source} does not have the data inputs of its kind");
			}
		}

		private static bool SameNames(List<string> actual, string[] expected)
		{
			if (actual.Count != expected.Length)
			{
				return false;
			}
			for (int index = 0; index < expected.Length; index++)
			{
				if (actual[index] != expected[index])
				{
					return false;
				}
			}
			return true;
		}
	}
}
