using System;
using System.Collections.Generic;
using System.Text.Json;

namespace Strada.VisualScripting;

// Analyzes documents (spec section 6.2): resolves every node against the library, shapes its pins from its settings,
// resolves wildcard types, decodes literals and checks every rule of spec sections 2 to 4. Any input gives a result: a
// document that is not a JSON object is a single Error InvalidDocument. The other graphs the document's nodes refer to come
// from a references bundle (spec-slice3 section 2.4); the function libraries among them are analyzed as documents of their
// own, with the same bundle, and kept in the runtime's cache (spec-slice3 section 4.4.3).
internal static class GraphAnalyzer
{
	// A document without references: every Graph setting names a graph that is absent.
	internal static AnalyzedDocument Analyze(NodeLibrary library, ReadOnlySpan<byte> utf8) => Analyze(library, utf8, GraphReferences.Empty, new LibraryAnalyses());

	// name is the document's asset path when known (a load), which messages then use for it. timings, when given, gets the
	// time of each phase added (spec-slice4 section 9.0).
	internal static AnalyzedDocument Analyze(NodeLibrary library, ReadOnlySpan<byte> utf8, GraphReferences references, LibraryAnalyses libraries,
		string? name = null, AnalysisTimings? timings = null) => AnalyzeRoot(library, utf8, references, libraries, name, timings).Analysis;

	// The analysis with the context it was made in: what it read of other graphs and the libraries it links (null for a
	// document that is not a JSON object, which reads nothing).
	internal static RootAnalysis AnalyzeRoot(NodeLibrary library, ReadOnlySpan<byte> utf8, GraphReferences references, LibraryAnalyses libraries,
		string? name, AnalysisTimings? timings)
	{
		long start = AnalysisTimings.Now();
		List<Diagnostic> diagnostics = [];
		GraphDocument? document = DocumentReader.Read(utf8, diagnostics);
		timings?.Lap(AnalysisPhase.Read, start);
		if (document == null)
		{
			return new RootAnalysis(new AnalyzedDocument(library, diagnostics, [], [], new AnalyzedGraph(0, null)), null);
		}
		// Only libraries of the bundle read the analyzed document's text (to cache what they read of it).
		ReferenceScope scope = new(library, references, libraries, name, document, references.Count > 0 ? utf8.ToArray() : []);
		AnalysisContext context = new(library, document, diagnostics, scope, references.Self, name) { Timings = timings };
		return new RootAnalysis(Analyze(context), context);
	}

	internal static AnalyzedDocument Analyze(AnalysisContext context)
	{
		GraphDocument document = context.Document;
		List<Diagnostic> diagnostics = context.Diagnostics;
		NodeLibrary library = context.Library;
		AnalysisTimings? timings = context.Timings;
		long mark = AnalysisTimings.Now();
		DeclareVariables(context);
		DeclareFunctions(context);
		mark = timings?.Lap(AnalysisPhase.Declare, mark) ?? mark;
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
		mark = timings?.Lap(AnalysisPhase.Create, mark) ?? mark;
		if (document.Kind == DocumentKind.Library)
		{
			CheckLibrary(context, eventGraph);
		}
		ComputeLatency(context);
		Dictionary<AnalyzedNode, VisualType> convertTargets = [];
		Dictionary<AnalyzedNode, ParsedFormula> formulas = [];
		NodeShaper shaper = new(context, convertTargets, formulas);
		foreach ((AnalyzedGraph graph, _) in graphs)
		{
			foreach (AnalyzedNode node in graph.Nodes)
			{
				shaper.Shape(node);
				// Pins whose types are final once shaped split before links name their fields; nodes with wildcards split
				// once they are resolved.
				if (PinSplitter.HasEntries(node))
				{
					if (PinSplitter.SplitsLate(node))
					{
						node.SplitPending = true;
					}
					else
					{
						PinSplitter.Split(context, node);
					}
				}
			}
		}
		mark = timings?.Lap(AnalysisPhase.Shape, mark) ?? mark;
		foreach ((AnalyzedGraph graph, DocumentGraph source) in graphs)
		{
			AttachLinks(context, graph, source);
		}
		mark = timings?.Lap(AnalysisPhase.Links, mark) ?? mark;
		TypeResolution types = new(context, convertTargets, formulas, node => SplitResolved(context, node));
		foreach ((AnalyzedGraph graph, _) in graphs)
		{
			types.Resolve(graph);
			// Every node that splits late is resolved by now; a link left waiting cannot attach.
			foreach (DeferredLink link in graph.DeferredLinks)
			{
				AttachDeferred(context, graph, link);
			}
		}
		mark = timings?.Lap(AnalysisPhase.Types, mark) ?? mark;
		foreach ((AnalyzedGraph graph, _) in graphs)
		{
			MarkDisabledNodes(context, graph);
		}
		foreach ((AnalyzedGraph graph, _) in graphs)
		{
			CheckLinks(context, graph);
		}
		if (document.Kind == DocumentKind.Script)
		{
			foreach ((AnalyzedGraph graph, _) in graphs)
			{
				CheckTargets(context, graph);
			}
		}
		mark = timings?.Lap(AnalysisPhase.Checks, mark) ?? mark;
		foreach ((AnalyzedGraph graph, _) in graphs)
		{
			foreach (AnalyzedNode node in graph.Nodes)
			{
				DecodeInputs(context, node);
			}
		}
		mark = timings?.Lap(AnalysisPhase.Decode, mark) ?? mark;
		GraphRules.Check(context, context.Functions, eventGraph);
		DemoteDisabledNodeDiagnostics(diagnostics, graphs);
		timings?.Lap(AnalysisPhase.Rules, mark);
		return new AnalyzedDocument(library, diagnostics, context.Variables, context.Functions, eventGraph)
		{
			Kind = document.Kind,
			Linked = Link(context.Links),
		};
	}

	// The libraries the document's calls reach: those it calls, and theirs.
	internal static Dictionary<ulong, LinkedLibrary> Link(IReadOnlyDictionary<ulong, LinkedLibrary> links)
	{
		Dictionary<ulong, LinkedLibrary> linked = [];
		foreach (KeyValuePair<ulong, LinkedLibrary> link in links)
		{
			linked.TryAdd(link.Key, link.Value);
			foreach (KeyValuePair<ulong, LinkedLibrary> reached in link.Value.Analysis.Linked)
			{
				linked.TryAdd(reached.Key, reached.Value);
			}
		}
		return linked;
	}

	// A function library (spec-slice3 section 4.4.1) runs on no entity: its functions run for the graphs that call them, so
	// it has no variables and no event graph (comment boxes there are allowed).
	private static void CheckLibrary(AnalysisContext context, AnalyzedGraph eventGraph)
	{
		foreach (DocumentVariable variable in context.Document.Variables)
		{
			context.ReportVariable(DiagnosticCode.LibraryVariable, 0, variable.ID,
				$"{variable.Name}: a library has no variables: its functions run for the graph that calls them; use a local or an input");
		}
		foreach (AnalyzedNode node in eventGraph.Nodes)
		{
			context.ReportNode(DiagnosticCode.LibraryEventGraph, node, "a library has no event graph: its functions run when graphs call them");
		}
	}

	// In a script, an unlinked Target is Self, which runs this script (spec-slice3 sections 4.3, 4.5 and 4.7): a typed Send to
	// another script never reaches a receiver (a Warning), a variable of another script cannot be read or written on it (an
	// Error), and RunsGraph of another script is always false (a Warning). In a library an unlinked Target is the calling
	// entity, which may run any script.
	private static void CheckTargets(AnalysisContext context, AnalyzedGraph graph)
	{
		foreach (AnalyzedNode node in graph.Nodes)
		{
			if (node.Binding == null || node.FindPin("Target", PinDirection.In) is not { IsConnected: false })
			{
				continue;
			}
			string self = context.SelfTarget.Name;
			switch (node.Binding)
			{
				case CustomEventBinding { Target: { IsOtherScript: true } target } when node.Builtin == BuiltinNode.EventSend:
					context.ReportGraph(DiagnosticCode.TargetIsSelf, node, target.Graph,
						$"Target: Self runs {self}, not {target.Name}: link the entity to send to");
					break;
				case RemoteVariableBinding { OwnGraph: false } remote:
					string access = node.Builtin == BuiltinNode.VariableSet ? "write to" : "read from";
					context.ReportGraph(DiagnosticCode.UnlinkedTarget, node, remote.Graph,
						$"Target: Self runs {self}, not {remote.GraphName}: link the entity to {access}");
					break;
				case RunsGraphBinding { OwnGraph: false } runs:
					context.ReportGraph(DiagnosticCode.TargetIsSelf, node, runs.Graph,
						$"Target: Self runs {self}, so this is always false: link the entity to test");
					break;
			}
		}
	}

	private static void DeclareVariables(AnalysisContext context)
	{
		foreach (DocumentVariable variable in context.Document.Variables)
		{
			VisualType? type = Declarations.ResolveType(context, variable.Type, variable.Public, (code, message, suggestions) =>
				context.ReportVariable(code, 0, variable.ID, $"{variable.Name}: {message}", suggestions));
			object? value = type != null
				? Declarations.DecodeValue(type, variable.Value, (code, message) =>
					context.ReportVariable(code, 0, variable.ID, $"{variable.Name}: {message}"))
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
						context.ReportVariable(code, source.ID, local.ID, $"{source.Name}: local {local.Name}: {message}"))
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
			declared.Add(new AnalyzedParameter(parameter.Name, parameter.Type, type) { FormerNames = parameter.FormerNames });
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
			node.StoredPins = storedPins.GetValueOrDefault(documentNode.ID) ?? [];
			NodeResolver.Resolve(context, node, node.StoredPins);
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

	// An impure function is latent when its graph holds a latent node or calls a latent function (spec section 4.3), of the
	// document or of a library (whose latency is final once its own analysis ended: libraries call no cycles,
	// spec-slice3 section 4.4.3). A disabled node never waits (it passes the flow on), so it makes no function latent; latent
	// nodes and calls of latent functions are impure, so their Enabled alone says whether they are disabled.
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
						(builtin == BuiltinNode.FunctionCall && FindCallee(context, node) is { Latent: true }));
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

	// The function a Function.Call calls, from its raw settings: a function of the document, or of the library its Graph
	// names; null when the settings name none.
	private static AnalyzedFunction? FindCallee(AnalysisContext context, AnalyzedNode node)
	{
		if (!node.Source.Settings.TryGet("Function", out JsonElement id) || id.ValueKind != JsonValueKind.Number || !id.TryGetUInt32(out uint callee))
		{
			return null;
		}
		return context.ResolveCall(node) is { } call ? call.Analysis?.FindFunction(callee) : context.FindFunction(callee);
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
			if (!node.CanBeDisabled)
			{
				context.ReportNode(DiagnosticCode.DisabledIgnored, node, node.Builtin switch
				{
					BuiltinNode.FunctionEntry => $"{node.Title} cannot be disabled: every call of the function starts there, so it runs",
					BuiltinNode.FunctionReturn => $"{node.Title} cannot be disabled: the call returns there, so it runs",
					_ => $"{node.Title} is pure: it runs only when a node reads it, so it cannot be disabled and runs (disable the node that reads it)",
				});
				continue;
			}
			node.IsDisabled = true;
			node.PassThrough = FindPassThrough(node);
		}
	}

	// Where the flow entering a disabled node continues: its Then output, else Completed, else Default, else its first exec
	// output, none of them a Tick output (a Timeline's event track named Completed is no exit). Null when no flow enters it
	// (it has no exec input: an event starts no execution) or it has no such output (an unknown node's pins are typed by
	// nothing, so it has no exec output; every exec output of a Timeline is a Tick output).
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
			if (pin.Description.Role == PinRole.Tick)
			{
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
			first ??= pin;
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
	// Error EventInputLinked. An endpoint that may name a field pin of a node that splits once it is resolved waits for it
	// (DeferredLinks).
	private static void AttachLinks(AnalysisContext context, AnalyzedGraph graph, DocumentGraph source)
	{
		foreach (DocumentLink documentLink in source.Links)
		{
			AnalyzedNode from = graph.FindNode(documentLink.From.Node)!;
			AnalyzedNode to = graph.FindNode(documentLink.To.Node)!;
			AnalyzedPin? output = FindPin(context, from, PinDirection.Out, documentLink.From.Pin, documentLink, out bool outputWaits);
			AnalyzedPin? input = FindPin(context, to, PinDirection.In, documentLink.To.Pin, documentLink, out bool inputWaits);
			if (outputWaits || inputWaits)
			{
				graph.Defer(new DeferredLink(documentLink, from, to, output != null || outputWaits, outputWaits, input != null || inputWaits, inputWaits));
				continue;
			}
			if (output != null && input != null)
			{
				Attach(context, graph, documentLink, output, input);
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
				FindPin(context, node, PinDirection.In, input.Key, null, out _);
			}
		}
	}

	private static void Attach(AnalysisContext context, AnalyzedGraph graph, DocumentLink documentLink, AnalyzedPin output, AnalyzedPin input)
	{
		AnalyzedLink link = new(documentLink, output, input);
		output.AddLink(link);
		input.AddLink(link);
		graph.Add(link);
		if (input.Node.Builtin is { } builtin && BuiltinNodes.IsEvent(builtin))
		{
			link.IsValid = false;
			context.ReportLink(DiagnosticCode.EventInputLinked, link, $"{input.Node.Title} is an event: its inputs take stored values, not links");
		}
	}

	// A node that splits once it is resolved: splits its pins, then attaches the links that waited for them. A link from a
	// field pin into a pin of its own of a node that resolves later attaches here, before that node resolves (type
	// resolution orders it after this one), so the link types it.
	private static void SplitResolved(AnalysisContext context, AnalyzedNode node)
	{
		if (!node.SplitPending)
		{
			return;
		}
		PinSplitter.Split(context, node);
		node.SplitPending = false;
		foreach (DeferredLink link in node.Graph.GetDeferredLinks(node))
		{
			AttachDeferred(context, node.Graph, link);
		}
	}

	// Attaches a deferred link once no end waits for its node to split: an end that names no pin is an Error UnknownPin. Each
	// link is visited once for each end that waits, and once after type resolution.
	private static void AttachDeferred(AnalysisContext context, AnalyzedGraph graph, DeferredLink link)
	{
		if (link.Done || link.IsWaiting)
		{
			return;
		}
		link.Done = true;
		// An end found to name no pin before was reported then.
		AnalyzedPin? output = link.FindsOutput ? FindPin(context, link.From, PinDirection.Out, link.Document.From.Pin, link.Document, out _) : null;
		AnalyzedPin? input = link.FindsInput ? FindPin(context, link.To, PinDirection.In, link.Document.To.Pin, link.Document, out _) : null;
		if (output != null && input != null)
		{
			Attach(context, graph, link.Document, output, input);
		}
	}

	// The pin a link endpoint or an Inputs key names (null when it names none: an Error UnknownPin for a link). Waits is
	// true, and nothing is reported, when the name may be a field pin of a node that has not split yet.
	private static AnalyzedPin? FindPin(AnalysisContext context, AnalyzedNode node, PinDirection direction, string stored, DocumentLink? link,
		out bool waits)
	{
		waits = false;
		string name = node.Rename(direction, stored);
		AnalyzedPin? pin = node.FindShownPin(name, direction);
		if (pin != null)
		{
			return pin;
		}
		if (!node.PinsComplete)
		{
			// Unknown nodes and nodes whose settings are broken get the pins their links and inputs name.
			return node.AddPin(BuiltinNodes.Wildcard(name, direction, PinConstraint.None, WildcardSource.None));
		}
		if (node.SplitPending && PinSplitter.MayNameField(node, name, direction))
		{
			waits = true;
			return null;
		}
		if (link == null)
		{
			return null;
		}
		List<string> candidates = [];
		foreach (AnalyzedPin candidate in node.GetShownPins())
		{
			if (candidate.Direction == direction)
			{
				candidates.Add(candidate.Name);
			}
		}
		context.ReportPin(DiagnosticCode.UnknownPin, node, name, direction, PinSplitter.UnknownPinMessage(node, name, direction), link,
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

	// A data link: the conversion it needs, or an Error TypeMismatch; a link from a wildcard output nothing typed is an Error
	// UnresolvedWildcard.
	private static void CheckConversion(AnalysisContext context, AnalyzedLink link)
	{
		AnalyzedPin from = link.From;
		AnalyzedPin to = link.To;
		if (!from.Type.HasValues)
		{
			// A wildcard output that resolution could not type: no value of a known type crosses the link, so no node may read
			// it. An output that is no wildcard has no type when what declares it has none (a variable or parameter of an unknown
			// type, an unknown node, broken settings), and an input of a node whose pins are not complete takes any link: each
			// is an Error where it is declared, and its links say nothing more. The link stays: the program is broken, and its
			// input is not unconnected.
			if (from.Description.Source != WildcardSource.None && to.Node.PinsComplete)
			{
				context.ReportLink(DiagnosticCode.UnresolvedWildcard, link,
					$"the type of {from.Node.ID}.{from.Name} is unknown: {to.Node.ID}.{to.Name} takes no value from it");
			}
			return;
		}
		// The constraint is checked before the input's own type: an input whose only links come from disabled nodes it refuses
		// is typed as if unconnected (TypeResolution), and those links are errors here.
		if (to.Description.Source == WildcardSource.Links && !TypeRules.Satisfies(from.Type, to.Description.Constraint))
		{
			link.IsValid = false;
			context.ReportLink(DiagnosticCode.TypeMismatch, link,
				$"{to.Node.Title}'s {to.Name} takes {TypeRules.Describe(to.Description.Constraint)}, not {from.Type.Id}");
			return;
		}
		if (!to.Type.HasValues)
		{
			// An input nothing typed: one of an unknown node, or a wildcard whose links have no common type (an Error on its node).
			return;
		}
		ConversionKind? conversion;
		if (to.Description.Source == WildcardSource.Links)
		{
			conversion = TypeRules.GetJoinConversion(from.Type, to.Type);
		}
		else
		{
			conversion = TypeRules.GetConversion(from.Type, to.Type);
		}
		if (conversion == null)
		{
			link.IsValid = false;
			// The advice names Convert only when a Convert node converts between the two types.
			string advice = TypeResolution.PlanConversion(from.Type, to.Type) != null
				? "use Convert"
				: $"the types do not convert, so link a {to.Type.Id} or break the link";
			context.ReportLink(DiagnosticCode.TypeMismatch, link,
				$"{from.Node.ID}.{from.Name} ({from.Type.Id}) cannot connect to {to.Node.ID}.{to.Name} ({to.Type.Id}): {advice}");
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
			// An Expression's input stays untyped only when its links' types are no formula operands: CheckLinks reported them.
			if (node.Builtin == BuiltinNode.Expression && pin.Links.Count > 0)
			{
				continue;
			}
			string hint = node.Builtin switch
			{
				{ } builtin when BuiltinNodes.IsBinaryWildcardOperator(builtin) => " (or connect the other operand and store a value here)",
				BuiltinNode.ArrayMake => " (or set Type: the element type)",
				_ => string.Empty,
			};
			// An input whose links are all errors (from disabled nodes whose outputs it refuses, say) is unconnected.
			context.ReportPin(DiagnosticCode.UnresolvedWildcard, node, pin.Name, PinDirection.In, pin.Links.Count > 0
				? $"the type of {pin.Name} is unknown: none of its links gives it a value{hint}"
				: $"the type of {pin.Name} is unknown until it is connected{hint}");
		}
		foreach (KeyValuePair<string, JsonElement> input in node.Source.Inputs.Members)
		{
			string name = node.Rename(PinDirection.In, input.Key);
			AnalyzedPin? pin = node.FindShownPin(name, PinDirection.In);
			if (pin == null)
			{
				// Split is authoritative (spec-slice2 section 3.3): a literal of a split input is not used, and one of a field
				// of an input that is not split names no pin.
				if (node.FindSplitPin(name, PinDirection.In) != null)
				{
					context.ReportPin(DiagnosticCode.UnusedInput, node, name, PinDirection.In,
						$"{name} is split: the stored value is not used (its fields take values of their own)");
					continue;
				}
				if (PinSplitter.FindUnsplitParent(node, name, PinDirection.In) != null)
				{
					context.ReportPin(DiagnosticCode.UnknownPin, node, name, PinDirection.In, PinSplitter.UnknownPinMessage(node, name, PinDirection.In));
					continue;
				}
				List<string> candidates = [];
				foreach (AnalyzedPin candidate in node.GetShownPins())
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
					context.ReportPin(DiagnosticCode.InvalidValue, node, name, PinDirection.In, decoded.Describe(name));
					continue;
				case LiteralProblem.EntityValue:
					context.ReportPin(DiagnosticCode.EntityValue, node, name, PinDirection.In, decoded.Describe(name));
					continue;
			}
			pin.HasLiteral = true;
			pin.Literal = decoded.Value;
		}
		// Field pins of split inputs take values like any input; a split input takes the struct its fields make.
		foreach (AnalyzedPin pin in node.GetShownPins())
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

// An analysis of a document and the context it was made in (null when the document is not a JSON object).
internal sealed record RootAnalysis(AnalyzedDocument Analysis, AnalysisContext? Context);
