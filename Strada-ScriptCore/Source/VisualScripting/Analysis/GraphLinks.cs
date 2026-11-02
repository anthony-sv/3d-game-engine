using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text.Json;

namespace Strada.VisualScripting;

// A function library a program links (spec-slice3 section 4.4.4): its analysis, whose functions are compiled into the
// program, and its asset path, which messages about its nodes name.
internal sealed record LinkedLibrary(AnalyzedDocument Analysis, string Name);

// What a node's Graph setting holds (spec-slice3 section 4.1).
internal enum GraphSettingState
{
	// No Graph member: the node refers to its own document.
	Absent,
	// A Graph member that is not the decimal handle of a visual script.
	Invalid,
	Handle,
}

internal static class GraphSettings
{
	internal const string Name = "Graph";

	internal const string InvalidMessage =
		"Graph: expected the decimal handle of a visual script, as asset.list gives it; remove the setting to refer to this script";

	internal static GraphSettingState Read(DocumentNode node, out ulong graph)
	{
		graph = 0;
		if (!node.Settings.TryGet(Name, out JsonElement value))
		{
			return GraphSettingState.Absent;
		}
		return GraphReferences.TryReadHandle(value, out graph) ? GraphSettingState.Handle : GraphSettingState.Invalid;
	}

	internal static string Format(ulong graph) => graph.ToString(CultureInfo.InvariantCulture);
}

// The graph a Graph setting names, as one analysis resolves it: the analyzed document itself (IsSelf), or another graph
// with its document, or an absent one (Document null, Error says why).
internal sealed class GraphTarget
{
	internal GraphTarget(ulong graph, bool isSelf, string name, string displayName, ReferenceForm form, GraphDocument? document, string? error)
	{
		Graph = graph;
		IsSelf = isSelf;
		Name = name;
		DisplayName = displayName;
		Form = form;
		Document = document;
		Error = error;
	}

	internal ulong Graph { get; }

	internal bool IsSelf { get; }

	// The asset path ("Scripts/Door.sgraph").
	internal string Name { get; }

	// The file stem ("Door").
	internal string DisplayName { get; }

	internal ReferenceForm Form { get; }

	internal GraphDocument? Document { get; }

	internal string? Error { get; }

	internal bool IsLibrary => Document is { Kind: DocumentKind.Library };
}

// What an analysis of a library read of another graph, compared when the library's cached analysis is looked up: the graph
// as the bundle sends it (its form, name and document text, or its error), or the analyzed document the bundle has no entry
// for, or nothing.
internal readonly struct GraphFingerprint
{
	private readonly byte m_Source;
	private readonly string m_Name;
	private readonly byte[] m_Text;

	private GraphFingerprint(byte source, string name, byte[] text)
	{
		m_Source = source;
		m_Name = name;
		m_Text = text;
	}

	internal static GraphFingerprint Missing { get; } = new(0, string.Empty, []);

	internal static GraphFingerprint OfRoot(string name, byte[] text) => new(1, name, text);

	internal static GraphFingerprint Of(ReferencedGraph graph) => graph.Document != null
		? new GraphFingerprint(graph.Form == ReferenceForm.Full ? (byte)2 : (byte)3, graph.Name, graph.Text)
		: new GraphFingerprint(4, graph.Name, System.Text.Encoding.UTF8.GetBytes(graph.Error ?? string.Empty));

	internal bool Matches(GraphFingerprint other) => m_Source == other.m_Source && m_Name == other.m_Name && m_Text.AsSpan().SequenceEqual(other.m_Text);
}

// The analyses of function libraries the runtime keeps (spec-slice3 section 4.4.3), by handle: a library analyzed for one
// graph serves every graph that calls it, as long as its text, the node library and everything its analysis read are
// unchanged, and each library it calls is analyzed as the very analysis it was linked with. Main thread only, like the
// runtime that owns it.
internal sealed class LibraryAnalyses
{
	private readonly Dictionary<ulong, Entry> m_Entries = [];

	internal int Count => m_Entries.Count;

	// Lookups that found an analysis, and lookups that analyzed (for tests).
	internal int Hits { get; private set; }

	internal int Misses { get; private set; }

	internal void Clear()
	{
		m_Entries.Clear();
	}

	internal Entry? Find(ulong graph) => m_Entries.GetValueOrDefault(graph);

	internal void CountHit()
	{
		Hits++;
	}

	internal void Store(ulong graph, Entry entry)
	{
		Misses++;
		m_Entries[graph] = entry;
	}

	// Name is the library's asset path, which its own messages name (a cycle through it, a node naming its own graph).
	internal sealed record Entry(string Name, byte[] Text, NodeLibrary Library, IReadOnlyDictionary<ulong, GraphFingerprint> Reads,
		IReadOnlyDictionary<ulong, LinkedLibrary> Links, AnalyzedDocument Analysis);
}

// The graphs one analysis may read (spec-slice3 sections 2.4 and 4.4.3): the bundle, the analyzed document (the root, which
// the bundle has no entry for), the libraries analyzed so far and the cycles among libraries, found from the raw settings of
// the root and the bundle's documents before any library is analyzed.
internal sealed class ReferenceScope
{
	private readonly Dictionary<ulong, AnalyzedDocument> m_Analyses = [];
	private readonly HashSet<ulong> m_Analyzing = [];
	private Dictionary<ulong, int>? m_Components;
	private Dictionary<ulong, List<ulong>>? m_Calls;

	internal ReferenceScope(NodeLibrary library, GraphReferences bundle, LibraryAnalyses cache, string? rootName, GraphDocument root, byte[] rootText)
	{
		Library = library;
		Bundle = bundle;
		Cache = cache;
		RootName = rootName;
		RootDocument = root;
		RootText = rootText;
	}

	internal NodeLibrary Library { get; }

	internal GraphReferences Bundle { get; }

	internal LibraryAnalyses Cache { get; }

	// The analyzed document's handle (the bundle's Self; 0 when unknown), asset path (null when unknown) and document.
	internal ulong Root => Bundle.Self;

	internal string? RootName { get; }

	internal GraphDocument RootDocument { get; }

	internal byte[] RootText { get; }

	// The root's asset path for messages of other graphs' analyses.
	internal string RootDisplayName => RootName ?? ReferencedGraph.DefaultName(Root);

	internal GraphFingerprint Fingerprint(ulong graph)
	{
		if (graph == Root && Root != 0)
		{
			return GraphFingerprint.OfRoot(RootDisplayName, RootText);
		}
		return Bundle.Find(graph) is { } entry ? GraphFingerprint.Of(entry) : GraphFingerprint.Missing;
	}

	// Whether the call from the library caller to the library callee lies on a cycle of libraries: the two are in one
	// strongly connected component of the calls (a graph naming itself is no cycle: it is an InvalidSetting).
	internal bool IsCycle(ulong caller, ulong callee)
	{
		if (caller == callee)
		{
			return false;
		}
		Dictionary<ulong, int> components = Components();
		return components.TryGetValue(caller, out int a) && components.TryGetValue(callee, out int b) && a == b;
	}

	// The handles of the libraries of the cycle a call lies on, from the callee back to the caller; every member of the
	// caller's component (the reads that decide the cycle).
	internal List<ulong> CyclePath(ulong caller, ulong callee)
	{
		Dictionary<ulong, int> components = Components();
		int component = components[caller];
		Dictionary<ulong, ulong> previous = [];
		Queue<ulong> pending = new();
		pending.Enqueue(callee);
		previous[callee] = callee;
		while (pending.TryDequeue(out ulong graph) && graph != caller)
		{
			foreach (ulong next in m_Calls!.GetValueOrDefault(graph) ?? [])
			{
				if (components.GetValueOrDefault(next, -1) == component && previous.TryAdd(next, graph))
				{
					pending.Enqueue(next);
				}
			}
		}
		List<ulong> path = [caller];
		for (ulong graph = caller; graph != callee; graph = previous[graph])
		{
			path.Add(previous[graph]);
		}
		path.Reverse();
		return path;
	}

	internal List<ulong> ComponentOf(ulong graph)
	{
		Dictionary<ulong, int> components = Components();
		int component = components[graph];
		List<ulong> members = [];
		foreach (KeyValuePair<ulong, int> entry in components)
		{
			if (entry.Value == component)
			{
				members.Add(entry.Key);
			}
		}
		members.Sort();
		return members;
	}

	// The analysis of the library of the bundle, from this analysis' libraries, the cache, or analyzed now with the same
	// bundle; null when the library is being analyzed already (calls that lie on a cycle never get here). The libraries its
	// calls reach are analyzed first, callees before callers, so analyses never nest: a chain of libraries as long as the
	// bundle holds needs no more stack than one library.
	internal AnalyzedDocument? GetAnalysis(ReferencedGraph graph)
	{
		if (m_Analyses.TryGetValue(graph.Graph, out AnalyzedDocument? known))
		{
			return known;
		}
		if (m_Analyzing.Contains(graph.Graph))
		{
			return null;
		}
		foreach (ulong library in CalleesFirst(graph.Graph))
		{
			Analyze(library == graph.Graph ? graph : Bundle.Find(library)!);
		}
		return m_Analyses[graph.Graph];
	}

	// The library and every library its calls reach that is not analyzed yet, each after the libraries it calls (a
	// depth-first walk of the calls with its own stack). The calls an analysis reports as cycles (into the root, or within a
	// component of the calls) need no analysis of their callee, so the walk does not follow them: the libraries it lists are
	// those the analyses ask for.
	private List<ulong> CalleesFirst(ulong graph)
	{
		Components();
		List<ulong> order = [];
		HashSet<ulong> visited = [graph];
		Stack<(ulong Graph, int Next)> walk = new();
		walk.Push((graph, 0));
		while (walk.TryPop(out (ulong Graph, int Next) top))
		{
			if (m_Calls!.TryGetValue(top.Graph, out List<ulong>? callees) && top.Next < callees.Count)
			{
				walk.Push((top.Graph, top.Next + 1));
				ulong callee = callees[top.Next];
				if (callee != Root && !IsCycle(top.Graph, callee) && !m_Analyses.ContainsKey(callee) && visited.Add(callee))
				{
					walk.Push((callee, 0));
				}
				continue;
			}
			order.Add(top.Graph);
		}
		return order;
	}

	private void Analyze(ReferencedGraph graph)
	{
		m_Analyzing.Add(graph.Graph);
		try
		{
			AnalyzedDocument analysis;
			if (Cache.Find(graph.Graph) is { } cached && IsCurrent(cached, graph))
			{
				Cache.CountHit();
				analysis = cached.Analysis;
			}
			else
			{
				List<Diagnostic> diagnostics = [.. graph.Problems];
				AnalysisContext context = new(Library, graph.Document!, diagnostics, this, graph.Graph, graph.Name);
				analysis = GraphAnalyzer.Analyze(context);
				Cache.Store(graph.Graph, new LibraryAnalyses.Entry(graph.Name, graph.Text, Library, context.Reads, context.Links, analysis));
			}
			m_Analyses[graph.Graph] = analysis;
		}
		finally
		{
			m_Analyzing.Remove(graph.Graph);
		}
	}

	// Whether the cached analysis is the one analyzing the library now would give. Every library it linked must be analyzed
	// in this scope already (CalleesFirst put the callees first) as the very analysis it linked; one that is not lies on a
	// cycle now, which the cached analysis did not see.
	private bool IsCurrent(LibraryAnalyses.Entry cached, ReferencedGraph graph)
	{
		if (!ReferenceEquals(cached.Library, Library) || cached.Name != graph.Name || !cached.Text.AsSpan().SequenceEqual(graph.Text))
		{
			return false;
		}
		foreach (KeyValuePair<ulong, GraphFingerprint> read in cached.Reads)
		{
			if (!Fingerprint(read.Key).Matches(read.Value))
			{
				return false;
			}
		}
		foreach (KeyValuePair<ulong, LinkedLibrary> link in cached.Links)
		{
			if (Bundle.Find(link.Key) is not { Form: ReferenceForm.Full, IsLibrary: true } callee || callee.Name != link.Value.Name ||
				!m_Analyses.TryGetValue(link.Key, out AnalyzedDocument? analysis) || !ReferenceEquals(analysis, link.Value.Analysis))
			{
				return false;
			}
		}
		return true;
	}

	// The components of the calls between libraries: every Function.Call with a Graph setting in the root and in the bundle's
	// documents, whether enabled or not and whatever its Function, is a call of the library it names.
	private Dictionary<ulong, int> Components()
	{
		if (m_Components != null)
		{
			return m_Components;
		}
		Dictionary<ulong, List<ulong>> calls = [];
		List<ulong> vertices = [];
		AddCalls(calls, vertices, Root, RootDocument);
		foreach (ReferencedGraph graph in Bundle.Graphs)
		{
			if (graph.Form == ReferenceForm.Full && graph.Document != null)
			{
				AddCalls(calls, vertices, graph.Graph, graph.Document);
			}
		}
		Dictionary<ulong, int> components = [];
		int index = 0;
		foreach (List<ulong> component in GraphRules.StronglyConnected(vertices, calls))
		{
			foreach (ulong graph in component)
			{
				components[graph] = index;
			}
			index++;
		}
		m_Calls = calls;
		m_Components = components;
		return components;
	}

	private void AddCalls(Dictionary<ulong, List<ulong>> calls, List<ulong> vertices, ulong caller, GraphDocument document)
	{
		if (calls.ContainsKey(caller))
		{
			return;
		}
		List<ulong> callees = [];
		calls[caller] = callees;
		vertices.Add(caller);
		AddCalls(callees, caller, document.Graph);
		foreach (DocumentFunction function in document.Functions)
		{
			AddCalls(callees, caller, function.Graph);
		}
	}

	private void AddCalls(List<ulong> callees, ulong caller, DocumentGraph graph)
	{
		foreach (DocumentNode node in graph.Nodes)
		{
			if (node.Type == BuiltinNodes.TypeOf(BuiltinNode.FunctionCall) && GraphSettings.Read(node, out ulong callee) == GraphSettingState.Handle &&
				callee != caller && IsCallableLibrary(callee) && !callees.Contains(callee))
			{
				callees.Add(callee);
			}
		}
	}

	// A graph calls can reach: the root when it is a library, or a library the bundle sends in full.
	private bool IsCallableLibrary(ulong graph) => graph == Root && Root != 0
		? RootDocument.Kind == DocumentKind.Library
		: Bundle.Find(graph) is { Form: ReferenceForm.Full, IsLibrary: true };
}
