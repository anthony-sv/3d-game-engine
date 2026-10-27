using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Reflection;

namespace Strada.VisualScripting;

// How a node's stored type string found its library node (spec section 4.5).
internal enum NodeResolution
{
	Exact,
	// Through the redirect table.
	Redirect,
	// The only overload of the member whose pins take the node's stored pin names.
	Unique,
	Unknown,
}

// A document as analysis understands it (spec section 6.2): every variable, function, node, pin and link with its resolved
// type, every literal decoded, the settings of every node bound, and the diagnostics. The compiler builds programs from it;
// the analysis JSON is written from it. A document with an Error diagnostic is broken: it is analyzed as far as it goes
// and never runs.
internal sealed class AnalyzedDocument
{
	internal AnalyzedDocument(NodeLibrary library, IReadOnlyList<Diagnostic> diagnostics, IReadOnlyList<AnalyzedVariable> variables,
		IReadOnlyList<AnalyzedFunction> functions, AnalyzedGraph eventGraph)
	{
		Library = library;
		Diagnostics = diagnostics;
		Variables = variables;
		Functions = functions;
		EventGraph = eventGraph;
		List<AnalyzedGraph> graphs = [];
		foreach (AnalyzedFunction function in functions)
		{
			graphs.Add(function.Graph);
		}
		graphs.Add(eventGraph);
		Graphs = graphs;
		foreach (Diagnostic diagnostic in diagnostics)
		{
			if (diagnostic.Severity == DiagnosticSeverity.Error)
			{
				FirstError ??= diagnostic;
				ErrorCount++;
			}
		}
	}

	internal NodeLibrary Library { get; }

	internal IReadOnlyList<Diagnostic> Diagnostics { get; }

	internal IReadOnlyList<AnalyzedVariable> Variables { get; }

	internal IReadOnlyList<AnalyzedFunction> Functions { get; }

	internal AnalyzedGraph EventGraph { get; }

	// The function graphs in document order, then the event graph.
	internal IReadOnlyList<AnalyzedGraph> Graphs { get; }

	internal int ErrorCount { get; }

	internal Diagnostic? FirstError { get; }

	internal bool HasErrors => ErrorCount > 0;
}

// A document variable, or a local of a function. Type is null when the declared type is unknown or not allowed; Value is
// the initial value (the decoded Value, else the type default).
internal sealed class AnalyzedVariable
{
	internal AnalyzedVariable(uint id, string name, string typeId, VisualType? type, object? value, bool isPublic,
		IReadOnlyList<string> formerNames, AnalyzedFunction? function)
	{
		ID = id;
		Name = name;
		TypeId = typeId;
		Type = type;
		Value = value;
		Public = isPublic;
		FormerNames = formerNames;
		Function = function;
	}

	internal uint ID { get; }

	internal string Name { get; }

	internal string TypeId { get; }

	internal VisualType? Type { get; }

	internal object? Value { get; }

	internal bool Public { get; }

	internal IReadOnlyList<string> FormerNames { get; }

	// The function a local belongs to; null for document variables.
	internal AnalyzedFunction? Function { get; }

	internal bool IsLocal => Function != null;
}

// An input or output of a function, or a parameter of a custom event. Type is null when the declared type is unknown or
// not allowed.
internal sealed record AnalyzedParameter(string Name, string TypeId, VisualType? Type);

internal sealed class AnalyzedFunction
{
	private readonly List<AnalyzedVariable> m_Locals = [];

	internal AnalyzedFunction(uint id, string name, bool pure, IReadOnlyList<AnalyzedParameter> inputs, IReadOnlyList<AnalyzedParameter> outputs)
	{
		ID = id;
		Name = name;
		Pure = pure;
		Inputs = inputs;
		Outputs = outputs;
		Graph = new AnalyzedGraph(id, this);
	}

	internal uint ID { get; }

	internal string Name { get; }

	internal bool Pure { get; }

	// An impure function whose graph holds a latent node or calls a latent function (spec section 4.3).
	internal bool Latent { get; set; }

	internal IReadOnlyList<AnalyzedParameter> Inputs { get; }

	internal IReadOnlyList<AnalyzedParameter> Outputs { get; }

	internal IReadOnlyList<AnalyzedVariable> Locals => m_Locals;

	internal AnalyzedGraph Graph { get; }

	internal void AddLocal(AnalyzedVariable local)
	{
		m_Locals.Add(local);
	}
}

// The event graph (Function 0) or a function's graph.
internal sealed class AnalyzedGraph
{
	private readonly List<AnalyzedNode> m_Nodes = [];
	private readonly List<AnalyzedLink> m_Links = [];
	private readonly Dictionary<uint, AnalyzedNode> m_NodesById = [];
	private readonly List<DeferredLink> m_DeferredLinks = [];
	private readonly Dictionary<AnalyzedNode, List<DeferredLink>> m_DeferredLinksByNode = [];
	private bool m_LinksUnordered;

	internal AnalyzedGraph(uint function, AnalyzedFunction? owner)
	{
		Function = function;
		Owner = owner;
	}

	internal uint Function { get; }

	// The function the graph belongs to; null for the event graph.
	internal AnalyzedFunction? Owner { get; }

	internal IReadOnlyList<AnalyzedNode> Nodes => m_Nodes;

	// In link order, whatever order the links were attached in.
	internal IReadOnlyList<AnalyzedLink> Links
	{
		get
		{
			LinkOrder.Restore(m_Links, ref m_LinksUnordered);
			return m_Links;
		}
	}

	internal AnalyzedNode? FindNode(uint id) => m_NodesById.TryGetValue(id, out AnalyzedNode? node) ? node : null;

	internal void Add(AnalyzedNode node)
	{
		m_Nodes.Add(node);
		m_NodesById[node.ID] = node;
	}

	internal void Add(AnalyzedLink link)
	{
		LinkOrder.Append(m_Links, link, ref m_LinksUnordered);
	}

	// Links whose endpoints may name field pins of a node that splits its pins once its wildcards are resolved, in link order.
	internal IReadOnlyList<DeferredLink> DeferredLinks => m_DeferredLinks;

	internal void Defer(DeferredLink link)
	{
		m_DeferredLinks.Add(link);
		if (link.OutputWaits)
		{
			DeferUnder(link.From, link);
		}
		if (link.InputWaits && !(link.OutputWaits && link.To == link.From))
		{
			DeferUnder(link.To, link);
		}
	}

	// The deferred links with an end that waits for the node to split, in link order.
	internal IReadOnlyList<DeferredLink> GetDeferredLinks(AnalyzedNode node) =>
		m_DeferredLinksByNode.TryGetValue(node, out List<DeferredLink>? links) ? links : [];

	private void DeferUnder(AnalyzedNode node, DeferredLink link)
	{
		if (!m_DeferredLinksByNode.TryGetValue(node, out List<DeferredLink>? links))
		{
			links = [];
			m_DeferredLinksByNode[node] = links;
		}
		links.Add(link);
	}
}

// Lists of links kept in link order. Links that name field pins of split wildcards are attached once those are resolved and
// split, after links that come later: they are appended, and the list is sorted once when it is next read, so attaching
// costs constant time and a list read after many late links costs one sort.
internal static class LinkOrder
{
	internal static void Append(List<AnalyzedLink> links, AnalyzedLink link, ref bool unordered)
	{
		if (links.Count > 0 && links[^1].Document.Index > link.Document.Index)
		{
			unordered = true;
		}
		links.Add(link);
	}

	internal static void Restore(List<AnalyzedLink> links, ref bool unordered)
	{
		if (!unordered)
		{
			return;
		}
		// Link indices are unique in a graph, so any sort is stable.
		links.Sort(static (a, b) => a.Document.Index.CompareTo(b.Document.Index));
		unordered = false;
	}
}

// A link analysis attaches once every node whose field pin one of its ends may name has split (an end that waits); an end
// that names a pin of its own never holds it back. FindsOutput and FindsInput are false for an end already found to name no
// pin (and reported): the link never attaches, but its other end is still checked. Done once it was attached or dropped.
internal sealed class DeferredLink(DocumentLink document, AnalyzedNode from, AnalyzedNode to, bool findsOutput, bool outputWaits,
	bool findsInput, bool inputWaits)
{
	internal DocumentLink Document { get; } = document;

	internal AnalyzedNode From { get; } = from;

	internal AnalyzedNode To { get; } = to;

	internal bool FindsOutput { get; } = findsOutput;

	internal bool OutputWaits { get; } = outputWaits;

	internal bool FindsInput { get; } = findsInput;

	internal bool InputWaits { get; } = inputWaits;

	internal bool Done { get; set; }

	// Whether an end still waits for its node to split.
	internal bool IsWaiting => (OutputWaits && From.SplitPending) || (InputWaits && To.SplitPending);
}

internal sealed class AnalyzedNode
{
	private readonly List<AnalyzedPin> m_Pins = [];

	internal AnalyzedNode(DocumentNode source, AnalyzedGraph graph)
	{
		Source = source;
		Graph = graph;
		Title = source.Type;
	}

	internal DocumentNode Source { get; }

	internal AnalyzedGraph Graph { get; }

	internal uint ID => Source.ID;

	internal NodeResolution Resolution { get; set; } = NodeResolution.Unknown;

	internal NodeDefinition? Definition { get; set; }

	// Nodes resolved by redirect or by a unique match: the current type string and the pins it renames.
	internal string? CanonicalType { get; set; }

	internal IReadOnlyList<PinRename> PinRenames { get; set; } = [];

	internal string Title { get; set; }

	internal string Subtitle { get; set; } = string.Empty;

	internal NodeKind Kind { get; set; } = NodeKind.Unknown;

	internal bool Pure { get; set; }

	internal bool Latent { get; set; }

	internal IReadOnlyList<AnalyzedPin> Pins => m_Pins;

	// False when the node's pins could not be determined (an unknown node, or a setting the pins depend on is missing or
	// invalid): pins that links and Inputs name are then added as unresolved wildcards instead of raising UnknownPin.
	internal bool PinsComplete { get; set; } = true;

	// What the node's settings resolved to; null for unknown nodes and nodes whose settings are invalid.
	internal NodeBinding? Binding { get; set; }

	// A node with wildcards whose Split lists pins: it splits them once its wildcards are resolved (spec-slice2 section 3.3).
	internal bool SplitPending { get; set; }

	// A node whose Enabled is "Disabled" and that can be disabled (not pure, not Function.Entry or Function.Return): it
	// compiles as a pass-through, whatever its kind, settings and diagnostics, and its diagnostics are Warnings.
	internal bool IsDisabled { get; set; }

	// Whether a node whose Enabled is "Disabled" is a pass-through: Function.Entry, Function.Return and pure nodes run. Final
	// once the node is resolved (a Reroute of the flow is not pure), which type resolution does before the nodes it feeds.
	internal bool CanBeDisabled => Builtin is not (BuiltinNode.FunctionEntry or BuiltinNode.FunctionReturn) && !Pure;

	// A disabled node: the exec output the flow entering it continues at; null when its chain ends there (it has no exec
	// output, or no exec input to enter).
	internal AnalyzedPin? PassThrough { get; set; }

	internal BuiltinNode? Builtin => (Definition as BuiltinNodeDefinition)?.Node;

	// The pin of a name among the node's own pins, split or not: the pins its kind reads and compiles.
	internal AnalyzedPin? FindPin(string name, PinDirection direction)
	{
		foreach (AnalyzedPin pin in m_Pins)
		{
			if (pin.Direction == direction && pin.Name == name)
			{
				return pin;
			}
		}
		return null;
	}

	// The pin a link endpoint or Inputs key names (spec-slice2 section 3.3): one of the node's own pins that is not split, else
	// a field pin "<Pin>.<Field>" of a split pin (nested ones included) that is not split itself. An exact pin name wins over
	// a split reading, and a split pin is no pin of its own.
	internal AnalyzedPin? FindShownPin(string name, PinDirection direction)
	{
		AnalyzedPin? exact = FindPin(name, direction);
		if (exact != null)
		{
			return exact.IsSplit ? null : exact;
		}
		return FindField(m_Pins, name, direction, out AnalyzedPin? field) && !field.IsSplit ? field : null;
	}

	// The split pin of a path: one of the node's own pins or a field pin. Null when the path names no split pin.
	internal AnalyzedPin? FindSplitPin(string name, PinDirection direction)
	{
		AnalyzedPin? exact = FindPin(name, direction);
		if (exact != null)
		{
			return exact.IsSplit ? exact : null;
		}
		return FindField(m_Pins, name, direction, out AnalyzedPin? field) && field.IsSplit ? field : null;
	}

	// Whether one of the node's pins is split.
	internal bool HasSplitPins { get; set; }

	// The pins analysis shows (the analysis JSON's Pins): the node's own pins in order, each split pin replaced by its field
	// pins in field order, nested ones likewise.
	internal IReadOnlyList<AnalyzedPin> GetShownPins()
	{
		if (!HasSplitPins)
		{
			return m_Pins;
		}
		List<AnalyzedPin> shown = [];
		AddShown(m_Pins, shown);
		return shown;
	}

	private static void AddShown(IReadOnlyList<AnalyzedPin> pins, List<AnalyzedPin> shown)
	{
		foreach (AnalyzedPin pin in pins)
		{
			if (pin.Fields is { } fields)
			{
				AddShown(fields, shown);
			}
			else
			{
				shown.Add(pin);
			}
		}
	}

	// The field pin of a path among the fields of the split pins of a list, nested ones included.
	private static bool FindField(IReadOnlyList<AnalyzedPin> pins, string name, PinDirection direction, [NotNullWhen(true)] out AnalyzedPin? field)
	{
		foreach (AnalyzedPin pin in pins)
		{
			if (pin.Direction != direction || pin.Fields is not { } fields || !IsPathUnder(name, pin.Name))
			{
				continue;
			}
			foreach (AnalyzedPin candidate in fields)
			{
				if (candidate.Name == name)
				{
					field = candidate;
					return true;
				}
			}
			if (FindField(fields, name, direction, out field))
			{
				return true;
			}
		}
		field = null;
		return false;
	}

	// Whether a pin path continues another with '.' ("Hit.Point" is under "Hit").
	internal static bool IsPathUnder(string path, string parent) =>
		path.Length > parent.Length + 1 && path[parent.Length] == '.' && path.StartsWith(parent, StringComparison.Ordinal);

	internal AnalyzedPin AddPin(PinDescription description)
	{
		AnalyzedPin pin = new(this, description);
		m_Pins.Add(pin);
		return pin;
	}

	// The current name of a stored pin name or pin path (the renames of a redirect): the rename of the name itself, else the
	// rename of the longest pin name the path continues with '.' (Value → Result renames "Value.Y" to "Result.Y"), as the
	// editor renames split paths with their pins (spec-slice2 section 2.7).
	internal string Rename(PinDirection direction, string name)
	{
		PinRename? best = null;
		foreach (PinRename rename in PinRenames)
		{
			if (rename.Direction != direction)
			{
				continue;
			}
			if (rename.From == name)
			{
				return rename.To;
			}
			if (IsPathUnder(name, rename.From) && (best is not { } longest || rename.From.Length > longest.From.Length))
			{
				best = rename;
			}
		}
		return best is { } found ? string.Concat(found.To, name.AsSpan(found.From.Length)) : name;
	}

	public override string ToString() => $"{Source.Type} (node {ID})";
}

internal sealed class AnalyzedPin
{
	private readonly List<AnalyzedLink> m_Links = [];
	private bool m_LinksUnordered;

	internal AnalyzedPin(AnalyzedNode node, PinDescription description)
	{
		Node = node;
		Description = description;
		Type = description.Type;
	}

	// A field pin of a split pin: its name is the path "<Pin>.<Field>".
	private AnalyzedPin(AnalyzedPin parent, PinDescription description, StructMember member)
		: this(parent.Node, description)
	{
		Parent = parent;
		Member = member;
	}

	internal AnalyzedNode Node { get; }

	// A field pin: the split pin it is a field of, and its field. Null for the node's own pins.
	internal AnalyzedPin? Parent { get; }

	internal StructMember? Member { get; }

	// A split pin (spec-slice2 section 3.3): its field pins in field order, which links and literals reach instead of it.
	// Null when the pin is not split.
	internal IReadOnlyList<AnalyzedPin>? Fields { get; private set; }

	internal bool IsSplit => Fields != null;

	internal PinDescription Description { get; }

	internal string Name => Description.Name;

	internal PinDirection Direction => Description.Direction;

	internal bool IsInput => Description.Direction == PinDirection.In;

	// The resolved type: the description's type, or what wildcard resolution found (still the wildcard when unresolved).
	internal VisualType Type { get; set; }

	// Incoming links of inputs, outgoing links of outputs, in link order (including links that are errors), whatever order they
	// were attached in.
	internal IReadOnlyList<AnalyzedLink> Links
	{
		get
		{
			LinkOrder.Restore(m_Links, ref m_LinksUnordered);
			return m_Links;
		}
	}

	// The link an input takes its value from: its first link that is not an error.
	internal AnalyzedLink? Source
	{
		get
		{
			foreach (AnalyzedLink link in Links)
			{
				if (link.IsValid)
				{
					return link;
				}
			}
			return null;
		}
	}

	internal bool IsConnected => Source != null;

	// Whether the document stores a literal for the input (in Inputs) that decoded; Literal is its value.
	internal bool HasLiteral { get; set; }

	internal object? Literal { get; set; }

	// The value of an unconnected input: its literal, else the pin default, else the type default.
	internal object? Value { get; set; }

	internal void AddLink(AnalyzedLink link)
	{
		LinkOrder.Append(m_Links, link, ref m_LinksUnordered);
	}

	// Splits the pin into one field pin per field of its type, in field order.
	internal void Split(IReadOnlyList<(PinDescription Description, StructMember Member)> fields)
	{
		List<AnalyzedPin> pins = [];
		foreach ((PinDescription description, StructMember member) in fields)
		{
			pins.Add(new AnalyzedPin(this, description, member));
		}
		Fields = pins;
		Node.HasSplitPins = true;
	}

	public override string ToString() => $"{Node.ID}.{Name} ({Direction})";
}

// A link from an output to an input. Conversion is how the value converts (spec section 3.4); IsValid is false for links
// that are errors (a second link into a data input or out of an exec output, a type mismatch, a link into an event).
internal sealed class AnalyzedLink
{
	internal AnalyzedLink(DocumentLink source, AnalyzedPin from, AnalyzedPin to)
	{
		Document = source;
		From = from;
		To = to;
	}

	internal DocumentLink Document { get; }

	internal AnalyzedPin From { get; }

	internal AnalyzedPin To { get; }

	internal ConversionKind? Conversion { get; set; }

	internal bool IsValid { get; set; } = true;

	internal bool IsExec => From.Type.IsExec || To.Type.IsExec;
}

// What the settings of a node resolved to: the compiler's view of each kind of node.
internal abstract record NodeBinding;

// Variable.Get and Variable.Set.
internal sealed record VariableBinding(AnalyzedVariable Variable) : NodeBinding;

// Function.Entry, Function.Return and Function.Call.
internal sealed record FunctionBinding(AnalyzedFunction Function) : NodeBinding;

// The input events: the key, mouse button or gamepad button (an enum value).
internal sealed record InputEventBinding(object Value) : NodeBinding;

// Event.Custom and Event.Send.
internal sealed record CustomEventBinding(string Name, IReadOnlyList<AnalyzedParameter> Parameters) : NodeBinding;

internal sealed record SequenceBinding(int Count) : NodeBinding;

// A case of Flow.Switch: its value (an int, a string or an enum value) and its output pin.
internal sealed record SwitchCase(object Value, string Pin);

internal sealed record SwitchBinding(VisualType Type, IReadOnlyList<SwitchCase> Cases) : NodeBinding;

// Flow.DoOnce (open unless StartClosed) and Flow.Gate (StartOpen).
internal sealed record GateBinding(bool StartOpen) : NodeBinding;

internal sealed record TweenBinding(Easing Easing) : NodeBinding;

internal sealed record LiteralBinding(VisualType Type, object? Value) : NodeBinding;

// Make and Break: the struct type, whose Fields are the pins.
internal sealed record StructBinding(VisualType Type) : NodeBinding;

// A piece of a FormatString: literal text, or the value of the placeholder input at Placeholder (an index among the
// node's placeholders, in pin order).
internal readonly record struct FormatSegment(string? Text, int Placeholder);

internal sealed record FormatBinding(IReadOnlyList<FormatSegment> Segments, IReadOnlyList<string> Placeholders) : NodeBinding;

// How Convert converts (spec section 4.4).
internal enum ConvertPlan
{
	Identity,
	// Numeric to numeric: C# unchecked casts (float to integer truncates and saturates, NaN gives 0).
	NumericCast,
	EnumToInteger,
	IntegerToEnum,
	// String to bool ("true"/"false", any case), a number (invariant culture) or an enum (an enumerator name); a failure
	// gives the type default and a Warning.
	ParseBool,
	ParseNumber,
	ParseEnum,
	// Spec section 3.4 rule 7.
	ToText,
	// An entity or script class to a script class: the entity's script instance when it is of that class, else null.
	ScriptInstance,
	// Spec section 3.4 rules 4 to 6 (Link holds which).
	Link,
	// A user conversion (op_Implicit or op_Explicit).
	Method,
}

internal sealed record ConvertBinding(VisualType From, VisualType To, ConvertPlan Plan, ConversionKind? Link, MethodInfo? Method) : NodeBinding;

// How an operator node computes (spec section 4.4).
internal enum OperatorMode
{
	// Numeric primitives after the join (Operand), C# unchecked arithmetic, IEEE comparisons.
	Numeric,
	// Add with a string operand: the other operand as text (rule 7).
	Concatenate,
	// The operator method Method (op_Addition, op_Equality, ...).
	Method,
	// And, Or, Xor, Not.
	Boolean,
	// Equality of bools, strings (ordinal) and enums by value.
	ValueEquality,
	// Entities and script classes by ID (null equals null).
	EntityEquality,
	// Assets by type and handle (Asset.Equals).
	AssetEquality,
	// Components by component type and entity ID (null equals null).
	ComponentEquality,
	// Equals(object), when the type declares no equality operator.
	ObjectEquality,
}

internal sealed record OperatorBinding(OperatorMode Mode, VisualType? Operand, MethodInfo? Method) : NodeBinding;

// A reflected node: its definition and the member it calls (generic methods closed over their TypeArguments).
internal sealed record MemberBinding(MemberNodeDefinition Definition, MemberInfo Member, IReadOnlyList<VisualType> TypeArguments) : NodeBinding;
