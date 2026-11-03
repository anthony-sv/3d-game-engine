using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace Strada.VisualScripting;

// The state one analysis shares between its passes: the library, the document's declarations and the diagnostics, and the
// other graphs the document's nodes refer to (spec-slice3 section 4): what the analysis read of them, and the function
// libraries its calls link.
internal sealed class AnalysisContext
{
	// Suggestion searches per analysis: a document full of unknown names stays quick to analyze.
	internal const int MaxSuggestionSearches = 256;

	private const string ThisGraph = "this graph";

	private readonly Dictionary<uint, AnalyzedVariable> m_VariablesById = [];
	private readonly Dictionary<uint, AnalyzedFunction> m_FunctionsById = [];
	private readonly List<AnalyzedVariable> m_Variables = [];
	private readonly List<AnalyzedFunction> m_Functions = [];
	private readonly Dictionary<AnalyzedNode, LibraryCall?> m_Calls = [];
	private readonly Dictionary<ulong, GraphFingerprint> m_Reads = [];
	private readonly Dictionary<ulong, LinkedLibrary> m_Links = [];
	private GraphTarget? m_SelfTarget;
	private int m_SuggestionSearches;

	// self is the document's handle (0 when unknown) and name its asset path (null when unknown): the graph its Graph
	// settings may name to mean itself.
	internal AnalysisContext(NodeLibrary library, GraphDocument document, List<Diagnostic> diagnostics, ReferenceScope scope, ulong self, string? name)
	{
		Library = library;
		Document = document;
		Diagnostics = diagnostics;
		Scope = scope;
		Self = self;
		SelfName = name;
	}

	internal NodeLibrary Library { get; }

	internal GraphDocument Document { get; }

	internal List<Diagnostic> Diagnostics { get; }

	internal ReferenceScope Scope { get; }

	internal ulong Self { get; }

	internal string? SelfName { get; }

	// Where the analysis adds the time of each phase (spec-slice4 section 9.0); null when nobody measures it. The libraries the
	// document calls are analyzed without it, inside the phase that needs them.
	internal AnalysisTimings? Timings { get; init; }

	// What the analysis read of each other graph (its fingerprint when read), and the libraries its calls link: a cached
	// analysis of a library is reused only while both are what they were (spec-slice3 section 4.4.3).
	internal IReadOnlyDictionary<ulong, GraphFingerprint> Reads => m_Reads;

	internal IReadOnlyDictionary<ulong, LinkedLibrary> Links => m_Links;

	// The document itself as a Graph setting names it ("this graph" when its asset path is unknown).
	internal GraphTarget SelfTarget => m_SelfTarget ??= new GraphTarget(Self, true, SelfName ?? ThisGraph,
		SelfName != null ? ReferencedGraph.Stem(SelfName) : ThisGraph, ReferenceForm.Full, Document, null);

	// The graph a Graph setting names: the document itself, the analyzed document of the outermost analysis (which the bundle
	// has no entry for), or a graph of the bundle, present or absent. Other graphs are recorded as read.
	internal GraphTarget FindGraph(ulong graph)
	{
		if (graph == Self)
		{
			return SelfTarget;
		}
		m_Reads[graph] = Scope.Fingerprint(graph);
		if (graph == Scope.Root)
		{
			string name = Scope.RootDisplayName;
			return new GraphTarget(graph, false, name, ReferencedGraph.Stem(name), ReferenceForm.Full, Scope.RootDocument, null);
		}
		if (Scope.Bundle.Find(graph) is not { } entry)
		{
			string name = ReferencedGraph.DefaultName(graph);
			return new GraphTarget(graph, false, name, name, ReferenceForm.Interface, null,
				$"no visual script {GraphSettings.Format(graph)} (deleted, or not a visual script)");
		}
		return new GraphTarget(graph, false, entry.Name, entry.DisplayName, entry.Form, entry.Document, entry.Error);
	}

	// What a Function.Call's Graph setting calls (spec-slice3 section 4.4.2): null for a call of the document's own function
	// (no Graph setting). Resolved once per node: latency and shaping both read it.
	internal LibraryCall? ResolveCall(AnalyzedNode node)
	{
		if (m_Calls.TryGetValue(node, out LibraryCall? known))
		{
			return known;
		}
		LibraryCall? call = ResolveCall(node.Source);
		m_Calls[node] = call;
		return call;
	}

	private LibraryCall? ResolveCall(DocumentNode node)
	{
		switch (GraphSettings.Read(node, out ulong graph))
		{
			case GraphSettingState.Absent:
				return null;
			case GraphSettingState.Invalid:
				return new LibraryCall(LibraryCallStatus.InvalidSetting, null, null, GraphSettings.InvalidMessage);
		}
		if (graph == Self)
		{
			return new LibraryCall(LibraryCallStatus.Self, SelfTarget, null, "Graph: this graph's own functions are called without a Graph setting");
		}
		GraphTarget target = FindGraph(graph);
		if (target.Document == null)
		{
			return new LibraryCall(LibraryCallStatus.Absent, target, null, $"Graph: {target.Error}");
		}
		if (!target.IsLibrary)
		{
			return new LibraryCall(LibraryCallStatus.NotALibrary, target, null,
				$"{target.Name} is not a function library: only libraries' functions are called from other graphs");
		}
		if (target.Form != ReferenceForm.Full)
		{
			return new LibraryCall(LibraryCallStatus.Absent, target, null, $"Graph: {target.Name} was sent without its functions");
		}
		// The outermost analyzed document is analyzed already: a library that calls it lies on a cycle through it.
		if (graph == Scope.Root || Scope.IsCycle(Self, graph))
		{
			return new LibraryCall(LibraryCallStatus.Cycle, target, null, DescribeCycle(graph));
		}
		AnalyzedDocument? analysis = Scope.GetAnalysis(Scope.Bundle.Find(graph)!);
		if (analysis == null)
		{
			return new LibraryCall(LibraryCallStatus.Cycle, target, null, DescribeCycle(graph));
		}
		m_Links[graph] = new LinkedLibrary(analysis, target.Name);
		return new LibraryCall(LibraryCallStatus.Library, target, analysis, null);
	}

	// "Motion.sgraph calls Physics.sgraph, which calls Motion.sgraph: libraries cannot depend on themselves". Every library
	// of the cycle decides it, so each is read.
	private string DescribeCycle(ulong callee)
	{
		List<ulong> path;
		if (Scope.IsCycle(Self, callee))
		{
			path = Scope.CyclePath(Self, callee);
			foreach (ulong member in Scope.ComponentOf(Self))
			{
				if (member != Self)
				{
					m_Reads[member] = Scope.Fingerprint(member);
				}
			}
		}
		else
		{
			path = [callee, Self];
		}
		StringBuilder text = new(FileNameOfGraph(Self));
		for (int index = 0; index < path.Count; index++)
		{
			text.Append(index == 0 ? " calls " : ", which calls ").Append(FileNameOfGraph(path[index]));
		}
		return text.Append(": libraries cannot depend on themselves").ToString();
	}

	private string FileNameOfGraph(ulong graph)
	{
		if (graph == Self)
		{
			return SelfName != null ? ReferencedGraph.FileNameOf(SelfName) : "this library";
		}
		if (graph == Scope.Root)
		{
			return ReferencedGraph.FileNameOf(Scope.RootDisplayName);
		}
		return Scope.Bundle.Find(graph)?.FileName ?? ReferencedGraph.DefaultName(graph);
	}

	internal IReadOnlyList<AnalyzedVariable> Variables => m_Variables;

	internal IReadOnlyList<AnalyzedFunction> Functions => m_Functions;

	internal void Add(AnalyzedVariable variable)
	{
		m_Variables.Add(variable);
		m_VariablesById[variable.ID] = variable;
	}

	internal void Add(AnalyzedFunction function)
	{
		m_Functions.Add(function);
		m_FunctionsById[function.ID] = function;
	}

	// A document variable, or a local of the function (IDs are unique across the whole document).
	internal AnalyzedVariable? FindVariable(uint id, AnalyzedFunction? function)
	{
		if (function != null)
		{
			foreach (AnalyzedVariable local in function.Locals)
			{
				if (local.ID == id)
				{
					return local;
				}
			}
		}
		return m_VariablesById.TryGetValue(id, out AnalyzedVariable? variable) ? variable : null;
	}

	internal AnalyzedFunction? FindFunction(uint id) => m_FunctionsById.TryGetValue(id, out AnalyzedFunction? function) ? function : null;

	// The function a local belongs to, when the ID names a local.
	internal AnalyzedFunction? FindLocalOwner(uint id)
	{
		foreach (AnalyzedFunction function in m_Functions)
		{
			foreach (AnalyzedVariable local in function.Locals)
			{
				if (local.ID == id)
				{
					return function;
				}
			}
		}
		return null;
	}

	// The nearest candidates for an unknown name (none once the analysis made MaxSuggestionSearches searches).
	internal IReadOnlyList<string> Suggest(string text, IEnumerable<string> candidates, bool bounded = true, bool transpositions = false)
	{
		if (m_SuggestionSearches >= MaxSuggestionSearches)
		{
			return [];
		}
		m_SuggestionSearches++;
		return Suggestions.Nearest(text, candidates, bounded, transpositions);
	}

	internal void ReportNode(DiagnosticCode code, AnalyzedNode node, string message, IReadOnlyList<string>? suggestions = null)
	{
		Diagnostics.Add(new Diagnostic(code, node.Graph.Function, message) { Node = node.ID, Suggestions = suggestions ?? [] });
	}

	// A finding of a node about another graph (spec-slice3 section 4.8): it names that graph.
	internal void ReportGraph(DiagnosticCode code, AnalyzedNode node, ulong graph, string message, IReadOnlyList<string>? suggestions = null)
	{
		Diagnostics.Add(new Diagnostic(code, node.Graph.Function, message) { Node = node.ID, Graph = graph, Suggestions = suggestions ?? [] });
	}

	internal void ReportPin(DiagnosticCode code, AnalyzedNode node, string pin, PinDirection direction, string message,
		DocumentLink? link = null, IReadOnlyList<string>? suggestions = null)
	{
		Diagnostics.Add(new Diagnostic(code, node.Graph.Function, message)
		{
			Node = node.ID,
			Pin = pin,
			Direction = direction,
			Link = link,
			Suggestions = suggestions ?? [],
		});
	}

	// A finding about a node's setting: span is the part of the setting's string it names, in UTF-16 indices of text, which
	// the diagnostic holds as UTF-8 byte offsets.
	internal void ReportSetting(DiagnosticCode code, AnalyzedNode node, string setting, string text, (int Start, int End)? span, string message,
		IReadOnlyList<string>? suggestions = null)
	{
		Diagnostics.Add(new Diagnostic(code, node.Graph.Function, message)
		{
			Node = node.ID,
			Setting = setting,
			Span = span is { } range ? (ByteOffset(text, range.Start), ByteOffset(text, range.End)) : null,
			Suggestions = suggestions ?? [],
		});
	}

	// The UTF-8 byte offset of a UTF-16 index of a text.
	internal static int ByteOffset(string text, int index) => Encoding.UTF8.GetByteCount(text.AsSpan(0, index));

	internal void ReportLink(DiagnosticCode code, AnalyzedLink link, string message)
	{
		Diagnostics.Add(new Diagnostic(code, link.To.Node.Graph.Function, message) { Link = link.Document });
	}

	internal void ReportVariable(DiagnosticCode code, uint function, uint variable, string message, IReadOnlyList<string>? suggestions = null)
	{
		Diagnostics.Add(new Diagnostic(code, function, message) { Variable = variable, Suggestions = suggestions ?? [] });
	}

	internal void ReportFunction(DiagnosticCode code, uint function, string message, IReadOnlyList<string>? suggestions = null)
	{
		Diagnostics.Add(new Diagnostic(code, function, message) { Suggestions = suggestions ?? [] });
	}

	internal static string Format(long value) => value.ToString(CultureInfo.InvariantCulture);
}

internal enum LibraryCallStatus
{
	// The Graph setting is not the handle of a visual script.
	InvalidSetting,
	// It names the document itself, whose functions are called without one.
	Self,
	// No visual script of that handle, or one sent without its functions.
	Absent,
	NotALibrary,
	// The call lies on a cycle of libraries.
	Cycle,
	// A function library, analyzed.
	Library,
}

// What a Function.Call's Graph setting calls: the graph it names (null for an invalid setting), and the library's analysis
// when the call can link it, else the message of the finding.
internal sealed record LibraryCall(LibraryCallStatus Status, GraphTarget? Target, AnalyzedDocument? Analysis, string? Message);
