using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Shared by the visual-scripting tests: the library of the scripting API (built once; libraries are immutable), analysis
// of documents written inline, and the shared fixtures of Tests/Data/VisualScripting.
internal static class VisualScriptTesting
{
	private static readonly Lazy<NodeLibrary> s_Library = new(() => NodeLibraryBuilder.Build(NodeLibrarySources.ForScriptCore([])));

	internal static NodeLibrary Library => s_Library.Value;

	internal static string FixtureDirectory => Path.Combine(AppContext.BaseDirectory, "VisualScripting");

	internal static AnalyzedDocument Analyze(string json) => GraphAnalyzer.Analyze(Library, Encoding.UTF8.GetBytes(json));

	internal static AnalyzedDocument Analyze(NodeLibrary library, string json) => GraphAnalyzer.Analyze(library, Encoding.UTF8.GetBytes(json));

	// A content-form document with an event graph of the given nodes and links (JSON array contents) and optional variables
	// and functions.
	internal static string Document(string nodes, string links = "", string variables = "", string functions = "") =>
		$$"""{ "Variables": [{{variables}}], "Functions": [{{functions}}], "Graph": { "Nodes": [{{nodes}}], "Links": [{{links}}], "Comments": [] } }""";

	internal static IReadOnlyList<Diagnostic> Errors(AnalyzedDocument document) =>
		[.. document.Diagnostics.Where(static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error)];

	internal static string Describe(IEnumerable<Diagnostic> diagnostics) => string.Join("\n", diagnostics.Select(static diagnostic => diagnostic.ToString()));

	internal static void AssertNoErrors(AnalyzedDocument document)
	{
		IReadOnlyList<Diagnostic> errors = Errors(document);
		Assert.True(errors.Count == 0, $"{errors.Count} errors:\n{Describe(errors)}");
	}

	// The only diagnostic with the code; fails when there is none or more than one.
	internal static Diagnostic Single(AnalyzedDocument document, DiagnosticCode code)
	{
		List<Diagnostic> matches = [.. document.Diagnostics.Where(diagnostic => diagnostic.Code == code)];
		Assert.True(matches.Count == 1, $"expected one {code}, got {matches.Count}:\n{Describe(document.Diagnostics)}");
		return matches[0];
	}

	internal static bool Has(AnalyzedDocument document, DiagnosticCode code) => document.Diagnostics.Any(diagnostic => diagnostic.Code == code);

	internal static AnalyzedNode Node(AnalyzedDocument document, uint id) =>
		document.Graphs.SelectMany(static graph => graph.Nodes).Single(node => node.ID == id);

	internal static AnalyzedPin Pin(AnalyzedDocument document, uint node, string name, PinDirection direction = PinDirection.Out) =>
		Node(document, node).FindPin(name, direction) ?? throw new InvalidOperationException($"node {node} has no {direction} pin {name}");

	internal static NodeDefinition LibraryNode(string type) =>
		Library.FindNode(type) ?? throw new InvalidOperationException($"the library has no node {type}");

	internal static JsonElement ParseObject(byte[] json)
	{
		using JsonDocument document = JsonDocument.Parse(json);
		return document.RootElement.Clone();
	}
}
