using System;
using System.Collections.Generic;
using System.Reflection;

namespace Strada.VisualScripting;

// What the interpreter does with a node (spec section 4). Impure kinds are entered by the flow; pure kinds are evaluated in
// the plans of the nodes that consume their outputs.
internal enum Opcode : byte
{
	// Impure.
	Event,
	Send,
	Branch,
	Sequence,
	Switch,
	ForLoop,
	ForEach,
	While,
	DoOnce,
	DoN,
	FlipFlop,
	Gate,
	Latent,
	VariableSet,
	FunctionEntry,
	FunctionReturn,
	FunctionCall,
	ExecReroute,
	Member,
	// A disabled node, whatever its kind: the flow passes on to its exit, its outputs take inputs of their types.
	PassThrough,
	// Pure.
	Self,
	Literal,
	Reroute,
	Select,
	IsValid,
	Convert,
	Make,
	Break,
	Format,
	ArrayLength,
	ArrayGet,
	VariableGet,
	PureCall,
	PureMember,
	Arithmetic,
	Negate,
	Compare,
	Equality,
	Concatenate,
	Boolean,
	OperatorMethod,
	// A formula (spec-slice3 section 3.3.8).
	Expression,
	// A String or Array node (spec-slice3 sections 3.4.5 and 3.5): CompiledNode.Intrinsic says which.
	Intrinsic,
}

// The String and Array nodes Opcode.Intrinsic evaluates (Intrinsics.Evaluate).
internal enum IntrinsicKind : byte
{
	StringLength,
	StringIsEmpty,
	StringContains,
	StringStartsWith,
	StringEndsWith,
	StringIndexOf,
	StringSubstring,
	StringReplace,
	StringToUpper,
	StringToLower,
	StringTrim,
	StringSplit,
	StringJoin,
	StringToInt,
	StringToFloat,
	ArrayContains,
	ArrayIndexOf,
	ArrayMake,
	ArraySet,
	ArrayAdd,
	ArrayRemoveAt,
	ArrayInsert,
	ArrayAppend,
}

// Where a staged input takes its value from.
internal enum OperandSource : byte
{
	// A slot of the frame: an output of another node.
	Slot,
	// The Index of the innermost active ForLoop or ForEach entry of a node in the current call frame (type default outside).
	LoopIndex,
	// The Element of the innermost active ForEach entry of a node (type default outside).
	LoopElement,
}

// The load of one connected data input into its staging slot.
internal readonly struct Operand
{
	internal Operand(int target, OperandSource source, int index, ConversionOp conversion, VisualType from)
	{
		Target = target;
		Source = source;
		Index = index;
		Conversion = conversion;
		From = from;
	}

	internal int Target { get; }

	internal OperandSource Source { get; }

	// The source slot, or the loop node.
	internal int Index { get; }

	internal ConversionOp Conversion { get; }

	// The type of the value before the conversion.
	internal VisualType From { get; }
}

// Where the flow goes when it leaves a node through an exec output: a node of the same graph and the exec input it
// enters. Node is -1 for an output without a link (the chain ends).
internal readonly record struct ExecLink(int Node, int Entry)
{
	internal static ExecLink None { get; } = new(-1, 0);
}

// The arithmetic and comparison operators.
internal enum OperatorKind : byte
{
	Add,
	Subtract,
	Multiply,
	Divide,
	Modulo,
	Less,
	LessOrEqual,
	Greater,
	GreaterOrEqual,
	And,
	Or,
	Xor,
	Not,
}

// How IsValid judges its value (spec section 4.4), by the value's type.
internal enum ValidityKind : byte
{
	Entity,
	Component,
	Asset,
	Reference,
}

// The equality table of spec section 4.4, by the operands' join type.
internal enum EqualityKind : byte
{
	Numeric,
	Bool,
	String,
	Enum,
	Entity,
	Asset,
	Component,
	Method,
	Object,
	// Arrays element by element (spec-slice3 section 3.4.2 rule 3), by the elements' equality (CompiledNode.ElementEquality).
	Array,
}

// A compiled node: its opcode, where its inputs, outputs and state live in its frame, where its exec outputs lead, and
// what its kind needs. One class serves every kind so the interpreter reaches everything without casts.
internal sealed class CompiledNode
{
	internal CompiledNode(AnalyzedNode source, int index, Opcode op)
	{
		Id = source.ID;
		// Only a disabled node can be of a type the library does not have: it keeps its stored type.
		Type = source.Definition?.Type ?? source.Source.Type;
		Title = source.Title;
		Index = index;
		Op = op;
		Builtin = source.Builtin;
		Pure = source.Pure;
		IsDisabled = source.IsDisabled;
	}

	// A split pin of a node (spec-slice2 section 3.3): the Make that composes a split input from its field pins, or the Break
	// that gives a split output's fields. It is a pure node of the graph's Nodes after the document's nodes, evaluated by the
	// nodes that read it, with the ID, type and title of the node whose pin it splits; graphs never find it by ID.
	internal CompiledNode(AnalyzedNode owner, int index, Opcode op, VisualType type)
	{
		Id = owner.ID;
		Type = owner.Definition?.Type ?? owner.Source.Type;
		Title = owner.Title;
		Index = index;
		Op = op;
		Pure = true;
		IsSplit = true;
		ToType = type;
	}

	internal uint Id { get; }

	// The Make or Break of a split pin.
	internal bool IsSplit { get; }

	// The library node type (canonical): a live edit carries the node's state over when it is unchanged (spec section 5.6).
	internal string Type { get; }

	// A disabled node (Opcode.PassThrough). A live edit that disables or enables a node removes it and adds a new one: its
	// state, outputs and the fibers waiting in it or passing through it do not carry over.
	internal bool IsDisabled { get; }

	internal string Title { get; }

	internal int Index { get; }

	internal Opcode Op { get; }

	internal BuiltinNode? Builtin { get; }

	internal bool Pure { get; }

	// The staging slots of the data inputs (in the kind's order) and the slots of the data outputs.
	internal int InputBase { get; set; }

	internal int InputCount { get; set; }

	internal int OutputBase { get; set; }

	internal int OutputCount { get; set; }

	// The names and types of the data outputs, by slot (OutputBase onwards).
	internal string[] OutputPins { get; set; } = [];

	internal VisualType[] OutputTypes { get; set; } = [];

	// The node's own state (DoOnce and Gate: open; DoN: the counter; FlipFlop: IsA; latent nodes: the fiber waiting in
	// them while they are pending).
	internal int StateSlot { get; set; } = -1;

	internal Operand[] Operands { get; set; } = [];

	// The pure nodes whose outputs feed the inputs, which the plan evaluates first.
	internal int[] PureSources { get; set; } = [];

	// The pure nodes to evaluate before the inputs are loaded, in dependency order; computed the first time it is needed.
	internal int[]? Plan { get; set; }

	internal ExecLink[] Exits { get; set; } = [];

	// The names of the exec outputs, by exit (positions that outlive a program are node IDs and pin names).
	internal string[] ExitPins { get; set; } = [];

	// Latent nodes: the exit a resumed execution continues at (their Continuation output), and the exit whose firings start
	// executions of their own (Tween's and Interval's Tick output).
	internal int ContinuationExit { get; set; } = -1;

	internal int TickExit { get; set; } = -1;

	// Tween: how Alpha follows the elapsed time.
	internal Easing Easing { get; set; }

	// An unconnected Target input that means Self.
	internal bool TargetIsSelf { get; set; }

	// A disabled node: the staged input each data output takes (an index from InputBase), -1 for an output that takes its
	// type default.
	internal int[] PassThroughSources { get; set; } = [];

	// Literal: its value. ForEach: the type default Element reads outside an active iteration. Convert from text: the type
	// default, the result of text it cannot read. Made once, so evaluating the node never boxes a default.
	internal Slot Constant { get; set; }

	// Sequence: the number of outputs. Switch: the number of cases (Default is the exit after them). Variable nodes: the
	// variable's index (document variables) or slot (locals). Array.Make: the number of elements.
	internal int Count { get; set; }

	internal bool IsLocal { get; set; }

	internal OperatorKind Operator { get; set; }

	internal SlotKind Kind { get; set; }

	// Equality: how values compare. NotEqual inverts the result.
	internal EqualityKind Equality { get; set; }

	// The equality of array elements: an Equality of arrays, Array.Contains and Array.IndexOf (Kind is the elements' slot kind,
	// Member their equality operator method, FromType their type).
	internal EqualityKind ElementEquality { get; set; }

	// The first of the three scratch slots of the nodes that compare array elements: the two values compared, and the
	// result of an equality operator method.
	internal int ScratchBase { get; set; } = -1;

	// Opcode.Intrinsic: the String or Array node.
	internal IntrinsicKind Intrinsic { get; set; }

	internal bool Invert { get; set; }

	internal ValidityKind Validity { get; set; }

	// Types the kind needs: Convert's From and To, Array.Get's element type, the struct of Make and Break, the element type
	// (From) and array type (To) of the Array nodes and String.Join.
	internal VisualType? FromType { get; set; }

	internal VisualType? ToType { get; set; }

	internal ConvertPlan ConvertPlan { get; set; }

	// Convert by a link conversion (spec section 3.4 rules 4 to 6).
	internal ConversionOp Conversion { get; set; }

	// Switch: the exit of each case value (ints and enum bits as longs, strings ordinal).
	internal Dictionary<long, int>? NumberCases { get; set; }

	internal Dictionary<string, int>? TextCases { get; set; }

	// FormatString: literal text and placeholder indexes.
	internal FormatSegment[] Segments { get; set; } = [];

	// Function calls: the function they call.
	internal CompiledFunction? Function { get; set; }

	// Event.Send and Event.Custom: the event's name and parameter types.
	internal string? EventName { get; set; }

	internal VisualType[] EventParameters { get; set; } = [];

	// Reflected nodes, operator methods and conversion methods: the member, its target and its invoker (built the first
	// time the node runs).
	internal MemberInfo? Member { get; set; }

	internal MemberNodeKind MemberKind { get; set; }

	internal TargetKind Target { get; set; }

	internal Type? DeclaringType { get; set; }

	internal MemberInvoker? Invoker { get; set; }

	// Expression: the formula's program.
	internal ExpressionProgram? Expression { get; set; }

	public override string ToString() => $"{Title} (node {Id})";
}

// The nodes of the event graph or of a function's graph, and the layout of its frame: the template every frame starts
// from holds the type defaults, the unconnected inputs' values and the initial node states.
internal sealed class CompiledGraph
{
	private readonly Dictionary<uint, int> m_IndexById = [];

	internal CompiledGraph(CompiledNode[] nodes, Slot[] template, CompiledFunction? function)
	{
		Nodes = nodes;
		Template = template;
		Function = function;
		foreach (CompiledNode node in nodes)
		{
			if (!node.IsSplit)
			{
				m_IndexById[node.Id] = node.Index;
			}
		}
	}

	// The document's nodes in order, then the Make and Break nodes of their split pins.
	internal CompiledNode[] Nodes { get; }

	internal Slot[] Template { get; }

	// Null for the event graph.
	internal CompiledFunction? Function { get; }

	internal int FindNode(uint id) => m_IndexById.TryGetValue(id, out int index) ? index : -1;
}

// A local of a function and the slot it has in the function's frames.
internal readonly record struct LocalSlot(uint Id, VisualType Type, int Slot);

// A function of the document. Frames of impure calls come from a pool, so calls do not allocate once it holds enough.
internal sealed class CompiledFunction
{
	private readonly Stack<Frame> m_Pool = new();

	internal CompiledFunction(AnalyzedFunction source)
	{
		Id = source.ID;
		Name = source.Name;
		Pure = source.Pure;
		InputTypes = TypesOf(source.Inputs);
		OutputTypes = TypesOf(source.Outputs);
	}

	internal uint Id { get; }

	internal string Name { get; }

	internal bool Pure { get; }

	// The types of the inputs and outputs, in order: a suspended call frame survives a live edit only while its function
	// keeps them and the types of its locals (spec section 5.6).
	internal VisualType[] InputTypes { get; }

	internal VisualType[] OutputTypes { get; }

	internal LocalSlot[] Locals { get; set; } = [];

	internal CompiledGraph Graph { get; set; } = null!;

	internal int EntryNode { get; set; } = -1;

	// Pure functions: the Return node whose inputs are the outputs.
	internal int ReturnNode { get; set; } = -1;

	// The type defaults of the outputs (a call that ends without Function.Return outputs them).
	internal Slot[] OutputDefaults { get; set; } = [];

	internal Frame AcquireFrame()
	{
		if (m_Pool.TryPop(out Frame? frame))
		{
			return frame;
		}
		return new Frame(Graph);
	}

	// Resets the frame to the template, so pooled frames hold no values of finished calls.
	internal void ReleaseFrame(Frame frame)
	{
		Array.Copy(Graph.Template, frame.Slots, frame.Slots.Length);
		m_Pool.Push(frame);
	}

	private static VisualType[] TypesOf(IReadOnlyList<AnalyzedParameter> parameters)
	{
		VisualType[] types = new VisualType[parameters.Count];
		for (int index = 0; index < types.Length; index++)
		{
			types[index] = parameters[index].Type!;
		}
		return types;
	}
}

// A document variable: its initial value and what overrides it (spec section 5.4).
internal sealed class CompiledVariable
{
	internal CompiledVariable(AnalyzedVariable source, Slot initial)
	{
		Id = source.ID;
		Name = source.Name;
		Type = source.Type!;
		Initial = initial;
		Public = source.Public;
		FormerNames = source.FormerNames;
	}

	internal uint Id { get; }

	internal string Name { get; }

	internal VisualType Type { get; }

	internal Slot Initial { get; }

	internal bool Public { get; }

	internal IReadOnlyList<string> FormerNames { get; }
}

// An input event of the event graph (spec section 4.1), polled during the Update dispatch.
internal readonly record struct InputEvent(int Node, BuiltinNode Kind, int Value, int Gamepad);

// A program: a compiled document, or a broken one that says why it cannot run (spec section 5.1). Programs never change
// once compiled, except for the members' invokers and the plans, which nodes create the first time they need them, and
// the count of their activity.
internal sealed class GraphProgram
{
	private readonly Dictionary<string, int> m_CustomEvents = new(StringComparer.Ordinal);
	private readonly Dictionary<uint, CompiledFunction> m_FunctionsById = [];

	// A broken program.
	internal GraphProgram(NodeLibrary library, string name, int errorCount, Diagnostic? firstError)
	{
		Library = library;
		Name = name;
		ErrorCount = errorCount;
		FirstError = firstError;
		EventGraph = new CompiledGraph([], [], null);
	}

	internal GraphProgram(NodeLibrary library, string name, CompiledVariable[] variables, CompiledFunction[] functions, CompiledGraph eventGraph,
		int[] events, InputEvent[] inputEvents)
	{
		Library = library;
		Name = name;
		Variables = variables;
		EventGraph = eventGraph;
		Events = events;
		InputEvents = inputEvents;
		foreach (CompiledFunction function in functions)
		{
			m_FunctionsById[function.Id] = function;
		}
		foreach (CompiledNode node in eventGraph.Nodes)
		{
			// A disabled custom event receives nothing.
			if (node.Op == Opcode.Event && node.Builtin == BuiltinNode.EventCustom)
			{
				m_CustomEvents[node.EventName!] = node.Index;
			}
		}
	}

	// The library the program was compiled with: the types of its values (the managed API converts values with them).
	internal NodeLibrary Library { get; }

	// The display name in messages (the asset path).
	internal string Name { get; }

	internal int ErrorCount { get; }

	internal Diagnostic? FirstError { get; }

	internal bool IsBroken => ErrorCount > 0;

	internal CompiledVariable[] Variables { get; } = [];

	internal CompiledGraph EventGraph { get; }

	// The dispatches and executions of the program now running (on the host stack, not suspended): a live edit swaps the
	// program of its instances only while this is 0 (spec section 5.6).
	internal int Activity { get; set; }

	// The event node of each ScriptEvent (Create to Click), -1 when the graph has none.
	internal int[] Events { get; } = [];

	// In Nodes order.
	internal InputEvent[] InputEvents { get; } = [];

	// The Event.Custom node of a name, -1 when there is none.
	internal int FindCustomEvent(string name) => m_CustomEvents.TryGetValue(name, out int node) ? node : -1;

	internal CompiledFunction? FindFunction(uint id) => m_FunctionsById.GetValueOrDefault(id);

	internal int FindVariable(string name)
	{
		for (int index = 0; index < Variables.Length; index++)
		{
			if (Variables[index].Name == name)
			{
				return index;
			}
		}
		return -1;
	}
}
