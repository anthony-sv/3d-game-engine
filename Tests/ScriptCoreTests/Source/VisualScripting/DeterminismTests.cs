using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Text.Json.Nodes;
using System.Threading;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// What the VM answers is the same bytes in every build, run and culture (spec section 6.1): the library JSON, the analysis
// JSON and the diagnostics of every document, damaged ones included, and the statuses, log lines and variables of graphs
// that run. Numbers and messages use the invariant culture, case changes are culture-independent, and nothing depends on
// hash order or object identity: two builds of the library give the same answers. The graphs read Time.DeltaTime, which
// other tests set, so the runs compared run without them.
[Collection(ProcessStaticsCollection.Name)]
public sealed class DeterminismTests
{
	// German and French decimal commas, Turkish casing (i and I), Swedish minus signs (U+2212), Arabic and Japanese
	// calendars and digits, and a culture whose number symbols are all unusual.
	private static readonly Lazy<CultureInfo[]> s_Cultures = new(static () =>
	{
		CultureInfo odd = (CultureInfo)CultureInfo.InvariantCulture.Clone();
		odd.NumberFormat.NegativeSign = "~";
		odd.NumberFormat.PositiveSign = "#";
		odd.NumberFormat.NumberDecimalSeparator = "|";
		odd.NumberFormat.NumberGroupSeparator = "_";
		odd.NumberFormat.NaNSymbol = "nan?";
		odd.NumberFormat.PositiveInfinitySymbol = "inf?";
		odd.NumberFormat.NegativeInfinitySymbol = "-inf?";
		return [new CultureInfo("de-DE"), new CultureInfo("fr-FR"), new CultureInfo("tr-TR"), new CultureInfo("sv-SE"), new CultureInfo("ar-SA"),
			new CultureInfo("ja-JP"), odd];
	});

	[Fact]
	public void TheLibraryIsTheSameInEveryCultureAndBuild()
	{
		byte[] expected = LibraryWriter.Write(VisualScriptTesting.Library);
		Assert.Equal(expected, LibraryWriter.Write(NodeLibraryBuilder.Build(NodeLibrarySources.ForScriptCore([], []))));
		foreach (CultureInfo culture in s_Cultures.Value)
		{
			byte[] library = InCulture(culture, static () => LibraryWriter.Write(NodeLibraryBuilder.Build(NodeLibrarySources.ForScriptCore([], []))));
			Assert.True(expected.AsSpan().SequenceEqual(library), $"the library differs in {Describe(culture)}");
		}
	}

	[Fact]
	public void AnalysesAreTheSameInEveryCultureAndWithEveryBuildOfTheLibrary()
	{
		NodeLibrary rebuilt = NodeLibraryBuilder.Build(NodeLibrarySources.ForScriptCore([], []));
		foreach ((string name, byte[] document) in Documents(Fuzzing.GetRounds(56)))
		{
			byte[] expected = AnalysisWriter.Write(GraphAnalyzer.Analyze(VisualScriptTesting.Library, document));
			byte[] diagnostics = AnalysisWriter.WriteDiagnostics(GraphAnalyzer.Analyze(VisualScriptTesting.Library, document).Diagnostics);
			Assert.True(expected.AsSpan().SequenceEqual(AnalysisWriter.Write(GraphAnalyzer.Analyze(rebuilt, document))),
				$"{name} analyzes differently with another build of the library");
			foreach (CultureInfo culture in s_Cultures.Value)
			{
				(byte[] analysis, byte[] loaded) = InCulture(culture, () =>
				{
					AnalyzedDocument analyzed = GraphAnalyzer.Analyze(VisualScriptTesting.Library, document);
					return (AnalysisWriter.Write(analyzed), AnalysisWriter.WriteDiagnostics(analyzed.Diagnostics));
				});
				Assert.True(expected.AsSpan().SequenceEqual(analysis), $"{name} analyzes differently in {Describe(culture)}");
				Assert.True(diagnostics.AsSpan().SequenceEqual(loaded), $"{name} has other diagnostics in {Describe(culture)}");
			}
		}
	}

	[Fact]
	public void RunningGraphsAnswerAndLogTheSameInEveryCulture()
	{
		foreach ((string name, byte[] document) in Documents(Fuzzing.GetRounds(40), FuzzLibrary.Fixtures))
		{
			IReadOnlyList<string> expected = Run(document);
			foreach (CultureInfo culture in s_Cultures.Value)
			{
				IReadOnlyList<string> run = InCulture(culture, () => Run(document));
				Assert.True(expected.SequenceEqual(run), $"{name} runs differently in {Describe(culture)}:\n{string.Join("\n", expected)}\n---\n{string.Join("\n", run)}");
			}
		}
	}

	// What a run of the document through every event of an instance gives, in order: the statuses, the log lines and the
	// described variables.
	private static List<string> Run(byte[] document)
	{
		FakeEnvironment environment = FuzzLibrary.CreateEnvironment();
		VisualScriptRuntime runtime = new(environment, () => FuzzLibrary.Library);
		List<string> results = [Encoding.UTF8.GetString(runtime.LoadDocument(1, "Assets/Graph.sgraph", document).Text!)];
		results.Add(runtime.CreateInstance(FuzzLibrary.Hero, 1, []).ToString());
		foreach ((ScriptEvent scriptEvent, float timeStep) in (ReadOnlySpan<(ScriptEvent, float)>)[(ScriptEvent.Create, 0.0f), (ScriptEvent.Update, 0.25f),
			(ScriptEvent.FixedUpdate, 0.02f), (ScriptEvent.Update, 0.5f), (ScriptEvent.CollisionEnter, 0.0f), (ScriptEvent.Update, 1.5f)])
		{
			results.Add(runtime.Invoke(FuzzLibrary.Hero, scriptEvent, timeStep, FuzzLibrary.Other).ToString());
		}
		HostReply variables = runtime.DescribeVariables(FuzzLibrary.Hero);
		results.Add(variables.Text != null ? Encoding.UTF8.GetString(variables.Text) : variables.Status.ToString());
		results.AddRange(environment.Logs.Select(static log => $"{log.Level}: {log.Message}"));
		return results;
	}

	// The shared fixtures, then damaged and rewired versions of them (fixed seeds).
	private static IEnumerable<(string Name, byte[] Document)> Documents(int damaged, IReadOnlyList<(string Name, JsonNode Document)>? fixtures = null)
	{
		fixtures ??= FuzzLibrary.Fixtures;
		foreach ((string name, JsonNode document) in fixtures)
		{
			yield return (name, Encoding.UTF8.GetBytes(document.ToJsonString()));
		}
		DocumentMutator mutator = new(0x44455445524D49u);
		for (int round = 0; round < damaged; round++)
		{
			(string name, JsonNode document) = fixtures[round % fixtures.Count];
			JsonNode? changed = round % 2 == 0 ? mutator.Damage(document, round / 2) : mutator.Rewire(document, round / 2);
			yield return (FormattableString.Invariant($"{name} (damaged in round {round})"), mutator.Write(changed));
		}
	}

	private static T InCulture<T>(CultureInfo culture, Func<T> action)
	{
		CultureInfo current = Thread.CurrentThread.CurrentCulture;
		CultureInfo currentUi = Thread.CurrentThread.CurrentUICulture;
		try
		{
			Thread.CurrentThread.CurrentCulture = culture;
			Thread.CurrentThread.CurrentUICulture = culture;
			return action();
		}
		finally
		{
			Thread.CurrentThread.CurrentCulture = current;
			Thread.CurrentThread.CurrentUICulture = currentUi;
		}
	}

	private static string Describe(CultureInfo culture) => culture.Name.Length > 0 ? culture.Name : "a culture of unusual number symbols";

	// The String nodes map case and read and write numbers as the invariant culture does (spec-slice3 section 3.5), so Turkish
	// i and I and German decimal commas change nothing.
	[Fact]
	public void StringNodesGiveTheInvariantResultsInEveryCulture()
	{
		static List<string> Run()
		{
			DocumentBuilder document = new();
			GraphBuilder graph = document.Graph;
			uint previous = graph.Node("Event.Create");
			foreach ((string type, string inputs, string output) in new[]
			{
				("String.ToUpper", """{ "Text": "title i \u0131 \u00DF" }""", "Result"),
				("String.ToLower", """{ "Text": "TITLE I \u0130 \u00C4" }""", "Result"),
				("String.ToFloat", """{ "Text": "1.5" }""", "Value"),
				("String.ToFloat", """{ "Text": "1,5" }""", "Success"),
				("String.ToInt", """{ "Text": " -12 " }""", "Value"),
				("String.Contains", """{ "Text": "TITLE", "Substring": "title" }""", "Result"),
			})
			{
				uint write = graph.Write(graph.Node(type, inputs: inputs), output);
				graph.Flow(previous, write);
				previous = write;
			}
			uint join = graph.Node("String.Join");
			graph.Link(graph.Literal("float[]", "[1.5, -0.25]"), "Value", join, "Parts");
			graph.Flow(previous, graph.Write(join, "Result"));
			TestRuntime runtime = TestRuntime.Start(document);
			runtime.Invoke(ScriptEvent.Create);
			return runtime.Records;
		}
		List<string> expected = InCulture(CultureInfo.InvariantCulture, Run);
		// The invariant mapping leaves the dotless i and the sharp s as they are.
		Assert.Equal("TITLE I \u0131 \u00DF", expected[0]);
		Assert.Equal(["1.5", "False", "-12", "False", "1.5, -0.25"], expected[2..]);
		foreach (CultureInfo culture in s_Cultures.Value)
		{
			Assert.Equal(expected, InCulture(culture, Run));
		}
	}
}
