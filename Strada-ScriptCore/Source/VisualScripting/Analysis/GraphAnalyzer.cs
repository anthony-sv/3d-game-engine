using System;
using System.Collections.Generic;
using System.Text.Json;

namespace Strada.VisualScripting;

// Analyzes documents (spec section 6.2): resolves every node against the library, shapes its pins from its settings,
// resolves wildcard types, decodes literals and checks every rule of spec sections 2 to 4. Any input gives a result: a
// document that is not a JSON object is a single Error InvalidDocument.
internal static class GraphAnalyzer
{
	internal static AnalyzedDocument Analyze(NodeLibrary library, ReadOnlySpan<byte> utf8)
	{
		List<Diagnostic> diagnostics = [];
		GraphDocument? document = DocumentReader.Read(utf8, diagnostics);
		return document != null ? Analyze(library, document, diagnostics) : new AnalyzedDocument(library, diagnostics, [], [], new AnalyzedGraph(0, null));
	}

	internal static AnalyzedDocument Analyze(NodeLibrary library, GraphDocument document, List<Diagnostic> diagnostics)
	{
		AnalysisContext context = new(library, document, diagnostics);
		DeclareVariables(context);
		DeclareFunctions(context);
		AnalyzedGraph eventGraph = new(0, null);
		List<(AnalyzedGraph Graph, DocumentGraph Source)> graphs = [];
		for (int index = 0; index < document.Functions.Count; index++)
		{
			graphs.Add((context.Functions[index].Graph, document.Functions[index].Graph));
		}
		graphs.Add((eventGraph, document.Graph));

		foreach ((AnalyzedGraph graph, DocumentGraph source) in graphs)
		{
			CreateNodes(context, graph, source);
		}
		ComputeLatency(context);
		Dictionary<AnalyzedNode, VisualType> convertTargets = [];
		NodeShaper shaper = new(context, convertTargets);
		foreach ((AnalyzedGraph graph, _) in graphs)
		{
			foreach (AnalyzedNode node in graph.Nodes)
			{
				shaper.Shape(node);
			}
		}
		foreach ((AnalyzedGraph graph, DocumentGraph source) in graphs)
		{
			AttachLinks(context, graph, source);
		}
		TypeResolution types = new(context, convertTargets);
		foreach ((AnalyzedGraph graph, _) in graphs)
		{
			types.Resolve(graph);
		}
		foreach ((AnalyzedGraph graph, _) in graphs)
		{
			MarkDisabledNodes(context, graph);
		}
		foreach ((AnalyzedGraph graph, _) in graphs)
		{
			CheckLinks(context, graph);
		}
		foreach ((AnalyzedGraph graph, _) in graphs)
		{
			foreach (AnalyzedNode node in graph.Nodes)
			{
				DecodeInputs(context, node);
			}
		}
		GraphRules.Check(context, context.Functions, eventGraph);
		DemoteDisabledNodeDiagnostics(diagnostics, graphs);
		return new AnalyzedDocument(library, diagnostics, context.Variables, context.Functions, eventGraph);
	}

	private static void DeclareVariables(AnalysisContext context)
	{
		foreach (DocumentVariable variable in context.Document.Variables)
		{
			VisualType? type = Declarations.ResolveType(context, variable.Type, variable.Public, (code, message, suggestions) =>
				context.ReportVariable(code, 0, variable.ID, $"{variable.Name}: {message}", suggestions));
			object? value = type != null
				? Declarations.DecodeValue(type, variable.Value, (code, message) =>
					context.ReportVariable(code, 0, variable.ID, $"{variable.Name}: Value: {message}"))
				: null;
			context.Add(new AnalyzedVariable(variable.ID, variable.Name, variable.Type, type, value, variable.Public, variable.FormerNames, null));
		}
	}

	private static void DeclareFunctions(AnalysisContext context)
	{
		foreach (DocumentFunction source in context.Document.Functions)
		{
			List<AnalyzedParameter> inputs = DeclareParameters(context, source, source.Inputs, "input");
			List<AnalyzedParameter> outputs = DeclareParameters(context, source, source.Outputs, "output");
			AnalyzedFunction function = new(source.ID, source.Name, source.Pure, inputs, outputs);
			foreach (DocumentLocal local in source.Locals)
			{
				VisualType? type = Declarations.ResolveType(context, local.Type, false, (code, message, suggestions) =>
					context.ReportVariable(code, source.ID, local.ID, $"{source.Name}: local {local.Name}: {message}", suggestions));
				object? value = type != null
					? Declarations.DecodeValue(type, local.Value, (code, message) =>
						context.ReportVariable(code, source.ID, local.ID, $"{source.Name}: local {local.Name}: Value: {message}"))
					: null;
				function.AddLocal(new AnalyzedVariable(local.ID, local.Name, local.Type, type, value, false, [], function));
			}
			context.Add(function);
		}
	}

	private static List<AnalyzedParameter> DeclareParameters(AnalysisContext context, DocumentFunction function,
		IReadOnlyList<DocumentParameter> parameters, string what)
	{
		List<AnalyzedParameter> declared = [];
		foreach (DocumentParameter parameter in parameters)
		{
			if (Names.IsReserved(parameter.Name))
			{
				context.ReportFunction(DiagnosticCode.ReservedName, function.ID,
					$"{function.Name}: the {what} {parameter.Name} has a reserved pin name ({Names.ReservedList})");
			}
			VisualType? type = Declarations.ResolveType(context, parameter.Type, false, (code, message, suggestions) =>
				context.ReportFunction(code, function.ID, $"{function.Name}: the {what} {parameter.Name}: {message}", suggestions));
			declared.Add(new AnalyzedParameter(parameter.Name, parameter.Type, type));
		}
		return declared;
	}

	private static void CreateNodes(AnalysisContext context, AnalyzedGraph graph, DocumentGraph source)
	{
		Dictionary<uint, List<(PinDirection, string)>> storedPins = [];
		foreach (DocumentLink link in source.Links)
		{
			Store(storedPins, link.From.Node, PinDirection.Out, link.From.Pin);
			Store(storedPins, link.To.Node, PinDirection.In, link.To.Pin);
		}
		foreach (DocumentNode documentNode in source.Nodes)
		{
			AnalyzedNode node = new(documentNode, graph);
			graph.Add(node);
			NodeResolver.Resolve(context, node, storedPins.GetValueOrDefault(documentNode.ID) ?? []);
		}
	}

	private static void Store(Dictionary<uint, List<(PinDirection, string)>> storedPins, uint node, PinDirection direction, string pin)
	{
		if (!storedPins.TryGetValue(node, out List<(PinDirection, string)>? pins))
		{
			pins = [];
			storedPins[node] = pins;
		}
		pins.Add((direction, pin));
	}

	// An impure function is latent when its graph holds a latent node or calls a latent function (spec section 4.3). A
	// disabled node never waits (it passes the flow on), so it makes no function latent; latent nodes and calls of latent
	// functions are impure, so their Enabled alone says whether they are disabled.
	private static void ComputeLatency(AnalysisContext context)
	{
		bool changed = true;
		while (changed)
		{
			changed = false;
			foreach (AnalyzedFunction function in context.Functions)
			{
				if (function.Pure || function.Latent)
				{
					continue;
				}
				foreach (AnalyzedNode node in function.Graph.Nodes)
				{
					bool latent = !node.Source.Disabled && node.Builtin is { } builtin && (BuiltinNodes.IsLatent(builtin) ||
						(builtin == BuiltinNode.FunctionCall && node.Source.Settings.TryGet("Function", out JsonElement id) &&
						 id.ValueKind == JsonValueKind.Number && id.TryGetUInt32(out uint callee) && context.FindFunction(callee) is { Latent: true }));
					if (latent)
					{
						function.Latent = true;
						changed = true;
						break;
					}
				}
			}
		}
	}

	// Nodes whose Enabled is "Disabled" (once types are resolved, which decides whether a Reroute is pure): Function.Entry,
	// Function.Return and pure nodes cannot be disabled and run, with a Warning DisabledIgnored; every other one is a
	// pass-through, and its exit is where the flow entering it continues.
	private static void MarkDisabledNodes(AnalysisContext context, AnalyzedGraph graph)
	{
		foreach (AnalyzedNode node in graph.Nodes)
		{
			if (!node.Source.Disabled)
			{
				continue;
			}
			if (node.Builtin is BuiltinNode.FunctionEntry or BuiltinNode.FunctionReturn)
			{
				context.ReportNode(DiagnosticCode.DisabledIgnored, node, node.Builtin == BuiltinNode.FunctionEntry
					? $"{node.Title} cannot be disabled: every call of the function starts there, so it runs"
					: $"{node.Title} cannot be disabled: the call returns there, so it runs");
				continue;
			}
			if (node.Pure)
			{
				context.ReportNode(DiagnosticCode.DisabledIgnored, node,
					$"{node.Title} is pure: it runs only when a node reads it, so it cannot be disabled and runs (disable the node that reads it)");
				continue;
			}
			node.IsDisabled = true;
			node.PassThrough = FindPassThrough(node);
		}
	}

	// Where the flow entering a disabled node continues: its Then output, else Completed, else Default, else its first exec
	// output that is not a Tick output. Null when no flow enters it (it has no exec input: an event starts no execution) or
	// it has no such output (an unknown node's pins are typed by nothing, so it has no exec output).
	private static AnalyzedPin? FindPassThrough(AnalyzedNode node)
	{
		bool entered = false;
		AnalyzedPin? then = null;
		AnalyzedPin? completed = null;
		AnalyzedPin? otherwise = null;
		AnalyzedPin? first = null;
		foreach (AnalyzedPin pin in node.Pins)
		{
			if (!pin.Type.IsExec)
			{
				continue;
			}
			if (pin.IsInput)
			{
				entered = true;
				continue;
			}
			switch (pin.Name)
			{
				case "Then":
					then ??= pin;
					break;
				case "Completed":
					completed ??= pin;
					break;
				case "Default":
					otherwise ??= pin;
					break;
			}
			if (pin.Description.Role != PinRole.Tick)
			{
				first ??= pin;
			}
		}
		return entered ? then ?? completed ?? otherwise ?? first : null;
	}

	// Every finding located on a disabled node, on its node or at an end of its link, is a Warning: a disabled node never
	// makes a program broken, so disabling a broken node is how its graph keeps running. Node IDs are unique in a document.
	private static void DemoteDisabledNodeDiagnostics(List<Diagnostic> diagnostics, List<(AnalyzedGraph Graph, DocumentGraph Source)> graphs)
	{
		HashSet<uint> disabled = [];
		foreach ((AnalyzedGraph graph, _) in graphs)
		{
			foreach (AnalyzedNode node in graph.Nodes)
			{
				if (node.IsDisabled)
				{
					disabled.Add(node.ID);
				}
			}
		}
		if (disabled.Count == 0)
		{
			return;
		}
		foreach (Diagnostic diagnostic in diagnostics)
		{
			if ((diagnostic.Node is { } node && disabled.Contains(node)) ||
				(diagnostic.Link is { } link && (disabled.Contains(link.From.Node) || disabled.Contains(link.To.Node))))
			{
				diagnostic.Demote();
			}
		}
	}

	// Links become pins' links; endpoints that name no pin of their node are an Error UnknownPin, links into an event an
	// Error EventInputLinked.
	private static void AttachLinks(AnalysisContext context, AnalyzedGraph graph, DocumentGraph source)
	{
		foreach (DocumentLink documentLink in source.Links)
		{
			AnalyzedNode from = graph.FindNode(documentLink.From.Node)!;
			AnalyzedNode to = graph.FindNode(documentLink.To.Node)!;
			AnalyzedPin? output = FindPin(context, from, PinDirection.Out, documentLink.From.Pin, documentLink);
			AnalyzedPin? input = FindPin(context, to, PinDirection.In, documentLink.To.Pin, documentLink);
			if (output == null || input == null)
			{
				continue;
			}
			AnalyzedLink link = new(documentLink, output, input);
			output.AddLink(link);
			input.AddLink(link);
			graph.Add(link);
			if (to.Builtin is { } builtin && BuiltinNodes.IsEvent(builtin))
			{
				link.IsValid = false;
				context.ReportLink(DiagnosticCode.EventInputLinked, link, $"{to.Title} is an event: its inputs take stored values, not links");
			}
		}
		foreach (AnalyzedNode node in graph.Nodes)
		{
			if (node.PinsComplete)
			{
				continue;
			}
			foreach (KeyValuePair<string, JsonElement> input in node.Source.Inputs.Members)
			{
				FindPin(context, node, PinDirection.In, input.Key, null);
			}
		}
	}

	private static AnalyzedPin? FindPin(AnalysisContext context, AnalyzedNode node, PinDirection direction, string stored, DocumentLink? link)
	{
		string name = node.Rename(direction, stored);
		AnalyzedPin? pin = node.FindPin(name, direction);
		if (pin != null)
		{
			return pin;
		}
		if (!node.PinsComplete)
		{
			// Unknown nodes and nodes whose settings are broken get the pins their links and inputs name.
			return node.AddPin(BuiltinNodes.Wildcard(name, direction, PinConstraint.None, WildcardSource.None));
		}
		if (link == null)
		{
			return null;
		}
		List<string> candidates = [];
		foreach (AnalyzedPin candidate in node.Pins)
		{
			if (candidate.Direction == direction)
			{
				candidates.Add(candidate.Name);
			}
		}
		string side = direction == PinDirection.In ? "input" : "output";
		string other = node.FindPin(name, direction == PinDirection.In ? PinDirection.Out : PinDirection.In) != null
			? $" ({name} is an {(direction == PinDirection.In ? "output" : "input")})"
			: string.Empty;
		context.ReportPin(DiagnosticCode.UnknownPin, node, name, direction, $"{node.Title} has no {side} pin {JsonText.Quote(name)}{other}", link,
			context.Suggest(name, candidates));
		return null;
	}

	// Exec links: exec to exec, one link per exec output. Data links: one per input, and a conversion from the output's type
	// to the input's (spec section 3.4); a wildcard input accepts what satisfies its constraint, at its resolved type. Links
	// are checked in document order, which is the order of every pin's links: an exec output or a data input has an earlier
	// valid link exactly when it is in taken, so a pin with thousands of links costs no more per link than one with one.
	private static void CheckLinks(AnalysisContext context, AnalyzedGraph graph)
	{
		HashSet<AnalyzedPin> taken = [];
		foreach (AnalyzedLink link in graph.Links)
		{
			if (!link.IsValid)
			{
				continue;
			}
			AnalyzedPin from = link.From;
			AnalyzedPin to = link.To;
			// Exec connects only to exec (rule 2), wildcards included: a Reroute linked to the flow was resolved to exec.
			if (from.Type.IsExec != to.Type.IsExec)
			{
				link.IsValid = false;
				context.ReportLink(DiagnosticCode.TypeMismatch, link, from.Type.IsExec
					? $"the exec output {from.Node.ID}.{from.Name} cannot connect to the data input {to.Node.ID}.{to.Name} ({to.Type.Id})"
					: $"the {from.Type.Id} output {from.Node.ID}.{from.Name} cannot connect to the exec input {to.Node.ID}.{to.Name}");
				continue;
			}
			if (!from.Type.IsExec && !from.Type.HasValues && from.Node.IsDisabled)
			{
				// An output of a disabled node that nothing typed (an unknown node's, or one its settings could not shape)
				// gives no value: the input takes its own, as if unconnected.
				link.IsValid = false;
				context.ReportLink(DiagnosticCode.UnresolvedWildcard, link,
					$"the type of {from.Node.ID}.{from.Name} is unknown and {from.Node.Title} is disabled: {to.Node.ID}.{to.Name} takes no value from it");
				continue;
			}
			if (from.Type.IsExec)
			{
				if (!taken.Add(from))
				{
					link.IsValid = false;
					context.ReportPin(DiagnosticCode.MultipleExecLinks, from.Node, from.Name, PinDirection.Out,
						$"the exec output {from.Name} has a link already: an exec output continues one chain (Flow.Sequence runs several)", link.Document);
					continue;
				}
				link.Conversion = ConversionKind.Identity;
				continue;
			}
			if (taken.Contains(to))
			{
				link.IsValid = false;
				context.ReportPin(DiagnosticCode.MultipleDataLinks, to.Node, to.Name, PinDirection.In,
					$"the input {to.Name} has a link already: an input takes its value from one output", link.Document);
				continue;
			}
			CheckConversion(context, link);
			if (link.IsValid)
			{
				taken.Add(to);
			}
		}
	}

	// A data link between pins whose types are known: the conversion it needs, or an Error TypeMismatch.
	private static void CheckConversion(AnalysisContext context, AnalyzedLink link)
	{
		AnalyzedPin from = link.From;
		AnalyzedPin to = link.To;
		if (!from.Type.HasValues || !to.Type.HasValues)
		{
			return;
		}
		ConversionKind? conversion;
		if (to.Description.Source == WildcardSource.Links)
		{
			if (!TypeRules.Satisfies(from.Type, to.Description.Constraint))
			{
				link.IsValid = false;
				context.ReportLink(DiagnosticCode.TypeMismatch, link,
					$"{to.Node.Title}'s {to.Name} takes {TypeRules.Describe(to.Description.Constraint)}, not {from.Type.Id}");
				return;
			}
			conversion = TypeRules.GetJoinConversion(from.Type, to.Type);
		}
		else
		{
			conversion = TypeRules.GetConversion(from.Type, to.Type);
		}
		if (conversion == null)
		{
			link.IsValid = false;
			context.ReportLink(DiagnosticCode.TypeMismatch, link,
				$"{from.Node.ID}.{from.Name} ({from.Type.Id}) cannot connect to {to.Node.ID}.{to.Name} ({to.Type.Id}): use Convert");
			return;
		}
		link.Conversion = conversion;
	}

	// The stored literals of a node's inputs (spec sections 2.1 and 3.5): decoded with the resolved pin types; and the value
	// every unconnected input takes.
	private static void DecodeInputs(AnalysisContext context, AnalyzedNode node)
	{
		foreach (AnalyzedPin pin in node.Pins)
		{
			if (!pin.IsInput || pin.Description.Source != WildcardSource.Links || pin.IsConnected || pin.Type.HasValues || pin.Type.IsExec)
			{
				continue;
			}
			// FormatString placeholders are text when unconnected; any other unconnected wildcard has nothing to resolve from.
			if (node.Builtin == BuiltinNode.FormatString)
			{
				pin.Type = context.Library.String;
				continue;
			}
			string hint = node.Builtin is { } builtin && BuiltinNodes.IsBinaryWildcardOperator(builtin)
				? " (or connect the other operand and store a value here)"
				: string.Empty;
			context.ReportPin(DiagnosticCode.UnresolvedWildcard, node, pin.Name, PinDirection.In,
				$"the type of {pin.Name} is unknown until it is connected{hint}");
		}
		foreach (KeyValuePair<string, JsonElement> input in node.Source.Inputs.Members)
		{
			string name = node.Rename(PinDirection.In, input.Key);
			AnalyzedPin? pin = node.FindPin(name, PinDirection.In);
			if (pin == null)
			{
				List<string> candidates = [];
				foreach (AnalyzedPin candidate in node.Pins)
				{
					if (candidate.IsInput && !candidate.Type.IsExec)
					{
						candidates.Add(candidate.Name);
					}
				}
				context.ReportPin(DiagnosticCode.UnusedInput, node, name, PinDirection.In,
					$"{node.Title} has no input {JsonText.Quote(name)}: the stored value is not used", null, context.Suggest(name, candidates));
				continue;
			}
			if (pin.IsConnected)
			{
				continue;
			}
			if (pin.Type.IsExec)
			{
				context.ReportPin(DiagnosticCode.InvalidValue, node, name, PinDirection.In, $"{name} is an exec input: it takes no value");
				continue;
			}
			if (pin.Description.DefaultsToSelf)
			{
				context.ReportPin(DiagnosticCode.InvalidValue, node, name, PinDirection.In, $"{name}: leave Target unconnected to use Self");
				continue;
			}
			if (!pin.Type.HasValues)
			{
				continue;
			}
			DecodedLiteral decoded = LiteralCodec.Decode(pin.Type, input.Value);
			if (decoded.WasZeroQuaternion)
			{
				context.ReportPin(DiagnosticCode.ZeroQuaternion, node, name, PinDirection.In, $"{name}: a quaternion of length zero is the identity");
			}
			switch (decoded.Problem)
			{
				case LiteralProblem.InvalidValue:
					context.ReportPin(DiagnosticCode.InvalidValue, node, name, PinDirection.In, $"{name}: {decoded.Message}");
					continue;
				case LiteralProblem.EntityValue:
					context.ReportPin(DiagnosticCode.EntityValue, node, name, PinDirection.In, $"{name}: {decoded.Message}");
					continue;
			}
			pin.HasLiteral = true;
			pin.Literal = decoded.Value;
		}
		foreach (AnalyzedPin pin in node.Pins)
		{
			if (pin.IsInput && pin.Type.HasValues && !pin.IsConnected)
			{
				pin.Value = pin.HasLiteral ? pin.Literal
					: pin.Description.HasDefault && ReferenceEquals(pin.Description.Type, pin.Type) ? pin.Description.Default
					: LiteralCodec.DefaultOf(pin.Type);
			}
		}
	}
}
