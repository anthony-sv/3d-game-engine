using System;
using System.Collections.Generic;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Unicode;

namespace Strada.VisualScripting;

// How a graph of a references bundle is sent (spec-slice3 section 2.4): the whole document of a graph whose functions the
// document calls (a library), or the interface of the others (Kind, Variables and the event graph's custom events).
internal enum ReferenceForm
{
	Full,
	Interface,
}

// A graph of a references bundle: its handle, its asset path (Name) and either the document sent for it or why it is absent
// (Error). Document is null for an absent graph, and for a document that is not an object (Problems says why).
internal sealed class ReferencedGraph
{
	internal ReferencedGraph(ulong graph, string name, ReferenceForm form, GraphDocument? document, byte[] text, IReadOnlyList<Diagnostic> problems,
		string? error)
	{
		Graph = graph;
		Name = name.Length > 0 ? name : DefaultName(graph);
		DisplayName = Stem(Name);
		FileName = AfterLastSlash(Name);
		Form = form;
		Document = document;
		Text = text;
		Problems = problems;
		Error = error;
	}

	internal ulong Graph { get; }

	// The asset path ("Scripts/Motion.sgraph"); "visual script <handle>" when the bundle names none.
	internal string Name { get; }

	// The file stem ("Motion"), else Name: how titles and messages name the graph briefly.
	internal string DisplayName { get; }

	// The file name ("Motion.sgraph").
	internal string FileName { get; }

	internal ReferenceForm Form { get; }

	internal GraphDocument? Document { get; }

	// The UTF-8 text of the document as the bundle holds it (empty for an absent graph): what analyses of a library are
	// cached by.
	internal byte[] Text { get; }

	// What reading the document found wrong (InvalidDocument diagnostics).
	internal IReadOnlyList<Diagnostic> Problems { get; }

	// Why the graph is absent (unregistered, another asset type, a missing file, one that does not load); null when the bundle
	// holds its document.
	internal string? Error { get; }

	internal bool IsLibrary => Document is { Kind: DocumentKind.Library };

	internal static string DefaultName(ulong graph) => "visual script " + graph.ToString(CultureInfo.InvariantCulture);

	// "Scripts/Motion.sgraph" → "Motion".
	internal static string Stem(string name)
	{
		string file = AfterLastSlash(name);
		int dot = file.LastIndexOf('.');
		string stem = dot > 0 ? file[..dot] : file;
		return stem.Length > 0 ? stem : name;
	}

	private static string AfterLastSlash(string name)
	{
		int slash = name.LastIndexOf('/');
		return slash >= 0 && slash + 1 < name.Length ? name[(slash + 1)..] : name;
	}
}

// The references bundle of an analysis or a load (spec-slice3 section 2.4): the handle of the document itself (Self, 0 when
// unknown) and the other graphs its nodes refer to, by handle. The VM accepts any bytes: a bundle that is not of this form
// holds no graph, and an entry that is not of it is left out, so every Graph setting naming it finds no visual script.
internal sealed class GraphReferences
{
	// The bundle holds documents in its Graphs entries: three levels below its root.
	private const int MaxDepth = DocumentReader.MaxDepth + 3;

	private readonly Dictionary<ulong, ReferencedGraph> m_Graphs;

	private GraphReferences(ulong self, Dictionary<ulong, ReferencedGraph> graphs)
	{
		Self = self;
		m_Graphs = graphs;
	}

	internal static GraphReferences Empty { get; } = new(0, []);

	internal ulong Self { get; }

	internal int Count => m_Graphs.Count;

	internal IEnumerable<ReferencedGraph> Graphs => m_Graphs.Values;

	internal ReferencedGraph? Find(ulong graph) => m_Graphs.GetValueOrDefault(graph);

	internal static GraphReferences Parse(ReadOnlySpan<byte> utf8)
	{
		if (utf8.IsEmpty || !Utf8.IsValid(utf8))
		{
			return Empty;
		}
		JsonElement root;
		try
		{
			using JsonDocument json = JsonDocument.Parse(utf8.ToArray(), new JsonDocumentOptions { MaxDepth = MaxDepth });
			root = json.RootElement.Clone();
		}
		catch (JsonException)
		{
			return Empty;
		}
		if (root.ValueKind != JsonValueKind.Object || !JsonText.HasTextNames(root))
		{
			return Empty;
		}
		ulong self = root.TryGetProperty("Self", out JsonElement selfValue) && TryReadHandle(selfValue, out ulong handle) ? handle : 0;
		Dictionary<ulong, ReferencedGraph> graphs = [];
		if (root.TryGetProperty("Graphs", out JsonElement list) && list.ValueKind == JsonValueKind.Array)
		{
			foreach (JsonElement element in list.EnumerateArray())
			{
				if (ReadEntry(element) is { } entry && entry.Graph != self)
				{
					graphs.TryAdd(entry.Graph, entry);
				}
			}
		}
		return new GraphReferences(self, graphs);
	}

	// A handle: a JSON string holding a decimal number from 1 to 2^64 - 1 (spec section 2.1's literal of
	// Strada.VisualScript); leading zeros are allowed, as the engine's reader allows them.
	internal static bool TryReadHandle(JsonElement value, out ulong handle)
	{
		handle = 0;
		return JsonText.TryGetString(value, out string? text) && text.Length > 0 &&
			ulong.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out handle) && handle != 0;
	}

	private static ReferencedGraph? ReadEntry(JsonElement element)
	{
		if (element.ValueKind != JsonValueKind.Object || !JsonText.HasTextNames(element) ||
			!element.TryGetProperty("Graph", out JsonElement graphValue) || !TryReadHandle(graphValue, out ulong graph))
		{
			return null;
		}
		string name = element.TryGetProperty("Name", out JsonElement nameValue) && JsonText.TryGetString(nameValue, out string? text) ? text : string.Empty;
		if (!element.TryGetProperty("Document", out JsonElement document))
		{
			string error = element.TryGetProperty("Error", out JsonElement errorValue) && JsonText.TryGetString(errorValue, out string? message) &&
				message.Length > 0
				? message
				: $"no visual script {graph.ToString(CultureInfo.InvariantCulture)} (deleted, or not a visual script)";
			return new ReferencedGraph(graph, name, ReferenceForm.Interface, null, [], [], error);
		}
		ReferenceForm form;
		if (element.TryGetProperty("Form", out JsonElement formValue) && JsonText.TryGetString(formValue, out string? formName) &&
			formName is "Full" or "Interface")
		{
			form = formName == "Full" ? ReferenceForm.Full : ReferenceForm.Interface;
		}
		else
		{
			return null;
		}
		List<Diagnostic> problems = [];
		GraphDocument? read = DocumentReader.Read(document, problems);
		return new ReferencedGraph(graph, name, form, read, JsonMarshal.GetRawUtf8Value(document).ToArray(), problems,
			read == null ? "the document sent for it is not a visual script" : null);
	}
}
