using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace Strada.VisualScripting;

// Everything a caller's analysis reads of a function library it links (spec-slice4 section 9.4), as canonical bytes: the
// library's asset path, its error count and first error (a call's ReferenceError names them), and per function in document
// order its ID, name, purity, latency and the name, declared type, resolved type and former names of each input and output
// (what shaping a Function.Call, its latency, its renamed pins and its suggestions read). Two analyses of a library with equal
// interfaces give every caller the same analysis.
internal static class LibraryInterface
{
	internal static byte[] Of(AnalyzedDocument library, string name)
	{
		StringBuilder text = new();
		Field(text, name);
		Field(text, library.Kind.ToString());
		Field(text, library.ErrorCount);
		if (library.FirstError is { } first)
		{
			Field(text, first.Code.ToString());
			Field(text, first.Message);
			Field(text, first.Node is { } node ? (long)node : -1);
			Field(text, first.Variable is { } variable ? (long)variable : -1);
		}
		Field(text, library.Functions.Count);
		foreach (AnalyzedFunction function in library.Functions)
		{
			Field(text, function.ID);
			Field(text, function.Name);
			Field(text, function.Pure ? 1 : 0);
			Field(text, function.Latent ? 1 : 0);
			Parameters(text, function.Inputs);
			Parameters(text, function.Outputs);
		}
		return Encoding.UTF8.GetBytes(text.ToString());
	}

	private static void Parameters(StringBuilder text, IReadOnlyList<AnalyzedParameter> parameters)
	{
		Field(text, parameters.Count);
		foreach (AnalyzedParameter parameter in parameters)
		{
			Field(text, parameter.Name);
			Field(text, parameter.TypeId);
			Field(text, parameter.Type?.Id ?? string.Empty);
			Field(text, parameter.FormerNames.Count);
			foreach (string former in parameter.FormerNames)
			{
				Field(text, former);
			}
		}
	}

	// Each field with its length first, so that no two lists of fields give the same text.
	private static void Field(StringBuilder text, string value) => text.Append(value.Length.ToString(CultureInfo.InvariantCulture)).Append(':').Append(value);

	private static void Field(StringBuilder text, long value) => Field(text, value.ToString(CultureInfo.InvariantCulture));
}

// The analyses of the documents the runtime loaded (spec-slice4 section 9.4), by graph handle: when a function library
// changes, every graph that calls it is loaded again with an unchanged document and a bundle whose library is new. Its own
// analysis is the one it had as long as its text, name, handle and node library are the same, every other graph it read is
// what it was, and every library it links gives an analysis with the same interface (LibraryInterface): then the load
// reuses it with the libraries' new analyses, and only compiles. Only documents that link a library are kept: no other
// load can find its document and its reads unchanged. Main thread only, like the runtime that owns it.
internal sealed class RootAnalyses
{
	private readonly Dictionary<ulong, Entry> m_Entries = [];

	internal int Count => m_Entries.Count;

	// Loads that reused an analysis, and loads that analyzed (for tests).
	internal int Hits { get; private set; }

	internal int Misses { get; private set; }

	internal void Clear()
	{
		m_Entries.Clear();
	}

	internal void Remove(ulong graph)
	{
		m_Entries.Remove(graph);
	}

	// The analysis of the document a load compiles for the handle: the kept one when it still holds, else a new one, which
	// is kept when it links a library. libraries is the runtime's cache of library analyses, which both paths use.
	internal AnalyzedDocument Analyze(ulong graph, NodeLibrary library, string name, byte[] text, GraphReferences references, LibraryAnalyses libraries,
		AnalysisTimings? timings)
	{
		if (m_Entries.TryGetValue(graph, out Entry? entry) && Reuse(entry, library, name, text, references, libraries) is { } reused)
		{
			Hits++;
			return reused;
		}
		Misses++;
		RootAnalysis root = GraphAnalyzer.AnalyzeRoot(library, text, references, libraries, name, timings);
		if (root.Context is not { Links.Count: > 0 } context)
		{
			m_Entries.Remove(graph);
			return root.Analysis;
		}
		Dictionary<ulong, GraphFingerprint> reads = [];
		foreach (KeyValuePair<ulong, GraphFingerprint> read in context.Reads)
		{
			// A library read only through the calls that link it is compared by its interface; any other read of a graph is
			// compared whole.
			if (!context.Links.ContainsKey(read.Key) || context.DocumentReads.Contains(read.Key))
			{
				reads[read.Key] = read.Value;
			}
		}
		List<Link> links = [];
		foreach (KeyValuePair<ulong, LinkedLibrary> link in context.Links)
		{
			links.Add(new Link(link.Key, link.Value.Name, link.Value.Analysis, LibraryInterface.Of(link.Value.Analysis, link.Value.Name)));
		}
		m_Entries[graph] = new Entry(name, text, references.Self, library, context.Document, reads, links, root.Analysis);
		return root.Analysis;
	}

	// The kept analysis with the libraries it links now, or null when anything it read changed.
	private static AnalyzedDocument? Reuse(Entry entry, NodeLibrary library, string name, byte[] text, GraphReferences references, LibraryAnalyses libraries)
	{
		if (!ReferenceEquals(entry.Library, library) || entry.Name != name || entry.Self != references.Self || !entry.Text.AsSpan().SequenceEqual(text))
		{
			return null;
		}
		// The scope the analysis had: the same document, read from the same text, and the bundle of this load.
		ReferenceScope scope = new(library, references, libraries, name, entry.Document, references.Count > 0 ? text : []);
		foreach (KeyValuePair<ulong, GraphFingerprint> read in entry.Reads)
		{
			if (!scope.Fingerprint(read.Key).Matches(read.Value))
			{
				return null;
			}
		}
		Dictionary<ulong, LinkedLibrary> linked = [];
		Link[] current = new Link[entry.Links.Count];
		for (int index = 0; index < entry.Links.Count; index++)
		{
			Link link = entry.Links[index];
			// As the analysis resolved the calls: a library of the bundle, sent in full, not on a cycle with the document.
			if (references.Find(link.Graph) is not { Form: ReferenceForm.Full, IsLibrary: true } callee || callee.Name != link.Name ||
				link.Graph == scope.Root || scope.IsCycle(references.Self, link.Graph) || scope.GetAnalysis(callee) is not { } analysis)
			{
				return null;
			}
			if (!ReferenceEquals(analysis, link.Analysis) && !LibraryInterface.Of(analysis, callee.Name).AsSpan().SequenceEqual(link.Interface))
			{
				return null;
			}
			current[index] = link with { Analysis = analysis };
			linked[link.Graph] = new LinkedLibrary(analysis, callee.Name);
		}
		// Each library is kept as the analysis this load linked, so that the next load of the same bundle compares identities.
		for (int index = 0; index < current.Length; index++)
		{
			entry.Links[index] = current[index];
		}
		return entry.Analysis.WithLinked(GraphAnalyzer.Link(linked));
	}

	// A library the analysis linked: its handle and asset path, the analysis it linked and that analysis' interface.
	private sealed record Link(ulong Graph, string Name, AnalyzedDocument Analysis, byte[] Interface);

	// Text is the document's text; Document what it reads as, which the reuse's scope finds the calls of. Reads are the
	// graphs the analysis read other than the libraries it links through calls, as it read them.
	private sealed record Entry(string Name, byte[] Text, ulong Self, NodeLibrary Library, GraphDocument Document,
		IReadOnlyDictionary<ulong, GraphFingerprint> Reads, List<Link> Links, AnalyzedDocument Analysis);
}
