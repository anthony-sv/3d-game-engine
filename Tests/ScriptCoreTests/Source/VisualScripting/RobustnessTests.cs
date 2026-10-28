using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Damaged documents (spec sections 5 and 6): mutations of every shared fixture go through analysis, Load, CreateInstance,
// every event, the variables and the managed API of runtimes over a fake environment, and through live edits of running
// instances. No exception escapes an operation, every execution ends (the budget stops endless loops), every status is the
// one the protocol specifies, and the only errors logged are graphs that cannot run and aborting errors of executions.
// The graphs read Time.DeltaTime, which other tests set: a round reproduces from its seed only without them.
[Collection(ProcessStaticsCollection.Name)]
public sealed class RobustnessTests
{
	private const ulong Graph = 100;
	private const ulong Hero = FuzzLibrary.Hero;
	private const ulong Other = FuzzLibrary.Other;
	private const string Name = "Assets/Fuzz.sgraph";

	private static readonly (ScriptEvent Event, float TimeStep, ulong Other)[] s_Events =
	[
		(ScriptEvent.Create, 0.0f, 0), (ScriptEvent.Update, 0.016f, 0), (ScriptEvent.FixedUpdate, 0.02f, 0), (ScriptEvent.CollisionEnter, 0.0f, Other),
		(ScriptEvent.Update, 0.25f, 0), (ScriptEvent.CollisionExit, 0.0f, Other), (ScriptEvent.TriggerEnter, 0.0f, 0), (ScriptEvent.Update, 0.5f, 0),
		(ScriptEvent.TriggerExit, 0.0f, Other), (ScriptEvent.Click, 0.0f, Other), (ScriptEvent.Update, 2.0f, 0), (ScriptEvent.Update, 0.016f, 0),
	];

	// A graph at the loader's limit of 10000 nodes (spec section 1.3) analyzes, compiles and runs: a chain of 1666 additions
	// to a variable, and one pure expression 5000 nodes deep feeding a last one.
	[Fact]
	public void AGraphAtTheLoadersLimitsAnalyzesCompilesAndRuns()
	{
		DocumentBuilder document = new();
		uint total = document.Variable("Total", "int", "0");
		GraphBuilder graph = document.Graph;
		uint previous = graph.Node("Event.Update");
		for (int group = 0; group < 1666; group++)
		{
			uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
			uint set = graph.Set(total);
			graph.Link(graph.Get(total), "Value", add, "A");
			graph.Link(add, "Result", set, "Value");
			graph.Flow(previous, set);
			previous = set;
		}
		uint expression = graph.Get(total);
		for (int depth = 0; depth < 4999; depth++)
		{
			uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
			graph.Link(expression, depth == 0 ? "Value" : "Result", add, "A");
			expression = add;
		}
		uint last = graph.Set(total);
		graph.Link(expression, "Result", last, "Value");
		graph.Flow(previous, last);
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update, 0.016f));
		Assert.Equal(1666 + 4999, runtime.Variable("Total"));
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update, 0.016f));
		Assert.Equal(2 * (1666 + 4999), runtime.Variable("Total"));
	}

	// The fixtures run in the fuzz library: its stand-ins have the pins of the engine members they replace, so the
	// documents analyze without errors and run every event without an aborting error (spec section 11.1).
	[Fact]
	public void TheFixturesRunWithTheStandInsOfEngineMembers()
	{
		FuzzStandIns.Calls = 0;
		foreach ((string name, JsonNode document) in FuzzLibrary.Fixtures)
		{
			byte[] text = Encoding.UTF8.GetBytes(document.ToJsonString());
			AnalyzedDocument analyzed = GraphAnalyzer.Analyze(FuzzLibrary.Library, text);
			VisualScriptTesting.AssertNoErrors(analyzed);
			Assert.All(analyzed.Graphs.SelectMany(static graph => graph.Nodes), node =>
				Assert.True(node.Resolution is NodeResolution.Exact or NodeResolution.Redirect, $"{name}: {node} resolved {node.Resolution}"));

			FakeEnvironment environment = FuzzLibrary.CreateEnvironment();
			VisualScriptRuntime runtime = new(environment, () => FuzzLibrary.Library);
			runtime.Load(Graph, Name, text);
			Assert.Equal(VisualScriptStatus.Success, runtime.CreateInstance(Hero, Graph, []));
			foreach ((ScriptEvent scriptEvent, float timeStep, ulong other) in s_Events)
			{
				Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(Hero, scriptEvent, timeStep, other));
			}
			Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(Hero, ScriptEvent.Destroy, 0.0f, 0));
			Assert.False(environment.Errors.Any(), $"{name}:\n{string.Join("\n", environment.Errors)}");
		}
		Assert.True(FuzzStandIns.Calls > 0);
	}

	[Fact]
	public void DamagedDocumentsAnswerWhatTheProtocolSpecifiesAndNeverThrow()
	{
		DocumentMutator mutator = new(0x53545241444156u);
		Tally tally = new();
		LiveRuntime live = new();
		int rounds = Fuzzing.GetRounds(240);
		IReadOnlyList<(string Name, JsonNode Document)> fixtures = FuzzLibrary.Fixtures;
		for (int round = 0; round < rounds; round++)
		{
			(string fixture, JsonNode original) = fixtures[round % fixtures.Count];
			string context = $"round {round} ({fixture})";
			// Even rounds damage the document's structure, odd ones change what a working graph does.
			byte[] text = mutator.Write(round % 2 == 0 ? mutator.Damage(original, round / 2) : mutator.Rewire(original, round / 2));
			try
			{
				new Round(context, mutator, tally).Run(text);
				live.Run(context, text, round % 8 == 7 ? Encoding.UTF8.GetBytes(original.ToJsonString()) : null, tally);
			}
			catch (Exception exception) when (exception is not Xunit.Sdk.IAssertionException)
			{
				Assert.Fail($"{context}: an operation threw {exception}\ndocument: {Encoding.UTF8.GetString(text[..Math.Min(text.Length, 4000)])}");
			}
		}
		TestContext.Current.TestOutputHelper?.WriteLine($"{rounds} rounds: {tally}");
		// The damage reaches every outcome: refused documents, broken programs, graphs that ran, aborts and the budget.
		Assert.True(tally.InvalidDocuments > 0, $"no document was refused as a whole: {tally}");
		Assert.True(tally.Broken > 0, $"no program was broken: {tally}");
		Assert.True(tally.Ran > 0, $"no damaged graph ran: {tally}");
		Assert.True(tally.Aborted > 0, $"no execution aborted: {tally}");
		Assert.True(tally.OverBudget > 0, $"no endless execution was stopped by the budget: {tally}");
		Assert.True(tally.Swapped > 0, $"no live edit swapped a running instance: {tally}");
		Assert.True(tally.Disabled > 0, $"no graph with a disabled node ran: {tally}");
		Assert.True(tally.Demoted > 0, $"no graph ran with an error located on a disabled node: {tally}");
		Assert.True(tally.Split > 0, $"no graph with a split pin ran: {tally}");
	}

	// Formulas through the same operations (spec-slice3 section 3.9): a document of Expression nodes damaged as any document
	// is, and with its formulas' texts edited.
	[Fact]
	public void DamagedFormulasAnswerWhatTheProtocolSpecifiesAndNeverThrow()
	{
		JsonNode original = JsonNode.Parse(FormulaDocument)!;
		DocumentMutator mutator = new(0x464F524D554C4145u);
		FuzzRandom random = new(0x464F524D554C4146u);
		Tally tally = new();
		int rounds = Fuzzing.GetRounds(150);
		for (int round = 0; round < rounds; round++)
		{
			string context = $"round {round} (formulas)";
			JsonNode? document = (round % 3) switch
			{
				0 => DamageFormulas(original, random),
				1 => mutator.Damage(original, round / 3),
				_ => mutator.Rewire(original, round / 3),
			};
			byte[] text = mutator.Write(document);
			try
			{
				new Round(context, mutator, tally).Run(text);
			}
			catch (Exception exception) when (exception is not Xunit.Sdk.IAssertionException)
			{
				Assert.Fail($"{context}: an operation threw {exception}\ndocument: {Encoding.UTF8.GetString(text)}");
			}
		}
		TestContext.Current.TestOutputHelper?.WriteLine($"{rounds} rounds: {tally}");
		Assert.True(tally.Broken > 0, $"no program was broken: {tally}");
		Assert.True(tally.Ran > 0, $"no damaged graph ran: {tally}");
		Assert.True(tally.Aborted > 0, $"no execution aborted: {tally}");
	}

	// Three formulas feeding three variables every Update; the second divides by zero every third Update.
	private const string FormulaDocument = """
		{
			"Variables": [
				{ "ID": 1, "Name": "Total", "Type": "float", "Value": 0.0 },
				{ "ID": 2, "Name": "Count", "Type": "int", "Value": 0 },
				{ "ID": 3, "Name": "Where", "Type": "Strada.Vector3" }
			],
			"Functions": [],
			"Graph": {
				"Nodes": [
					{ "ID": 10, "Type": "Event.Update" },
					{ "ID": 11, "Type": "Flow.Sequence", "Settings": { "Count": 3 } },
					{ "ID": 12, "Type": "Expression", "Settings": { "Formula": "total + dt * speed" }, "Inputs": { "speed": 2.5 } },
					{ "ID": 13, "Type": "Variable.Get", "Settings": { "Variable": 1 } },
					{ "ID": 14, "Type": "Variable.Set", "Settings": { "Variable": 1 } },
					{ "ID": 15, "Type": "Expression", "Settings": { "Formula": "n % 3 == 2 ? n / (n - n) : n + 1" } },
					{ "ID": 16, "Type": "Variable.Get", "Settings": { "Variable": 2 } },
					{ "ID": 17, "Type": "Variable.Set", "Settings": { "Variable": 2 } },
					{ "ID": 18, "Type": "Expression", "Settings": { "Formula": "normalize(p + vec3(1, n, 0)) * clamp(total, 0, 10) + vec3(p.z, p.y, p.x)" } },
					{ "ID": 19, "Type": "Variable.Get", "Settings": { "Variable": 3 } },
					{ "ID": 20, "Type": "Variable.Set", "Settings": { "Variable": 3 } }
				],
				"Links": [
					{ "From": [10, "Then"], "To": [11, "Execute"] },
					{ "From": [11, "Then0"], "To": [14, "Execute"] },
					{ "From": [11, "Then1"], "To": [17, "Execute"] },
					{ "From": [11, "Then2"], "To": [20, "Execute"] },
					{ "From": [10, "DeltaTime"], "To": [12, "dt"] },
					{ "From": [13, "Value"], "To": [12, "total"] },
					{ "From": [12, "Result"], "To": [14, "Value"] },
					{ "From": [16, "Value"], "To": [15, "n"] },
					{ "From": [15, "Result"], "To": [17, "Value"] },
					{ "From": [19, "Value"], "To": [18, "p"] },
					{ "From": [16, "Value"], "To": [18, "n"] },
					{ "From": [13, "Value"], "To": [18, "total"] },
					{ "From": [18, "Result"], "To": [20, "Value"] }
				],
				"Comments": []
			}
		}
		""";

	// Array literals, the Array and String nodes and an inferred generic method through the same operations (spec-slice3
	// section 3.9): the document damaged as any document is, and with its array literals replaced by damaged ones.
	[Fact]
	public void DamagedArraysAnswerWhatTheProtocolSpecifiesAndNeverThrow()
	{
		JsonNode original = JsonNode.Parse(ArrayDocument)!;
		DocumentMutator mutator = new(0x4152524159534641u);
		FuzzRandom random = new(0x4152524159534642u);
		Tally tally = new();
		int rounds = Fuzzing.GetRounds(150);
		for (int round = 0; round < rounds; round++)
		{
			string context = $"round {round} (arrays)";
			JsonNode? document = (round % 3) switch
			{
				0 => DamageArrays(original, random),
				1 => mutator.Damage(original, round / 3),
				_ => mutator.Rewire(original, round / 3),
			};
			byte[] text = mutator.Write(document);
			try
			{
				new Round(context, mutator, tally).Run(text);
			}
			catch (Exception exception) when (exception is not Xunit.Sdk.IAssertionException)
			{
				Assert.Fail($"{context}: an operation threw {exception}\ndocument: {Encoding.UTF8.GetString(text)}");
			}
		}
		TestContext.Current.TestOutputHelper?.WriteLine($"{rounds} rounds: {tally}");
		Assert.True(tally.Broken > 0, $"no program was broken: {tally}");
		Assert.True(tally.Ran > 0, $"no damaged graph ran: {tally}");
		Assert.True(tally.Aborted > 0, $"no execution aborted: {tally}");
	}

	// Array variables and literals feeding the Array nodes, a String.Split and Join, and a generic method inferred from links;
	// Array.Get reads past the end of Numbers last in every Update.
	private const string ArrayDocument = """
		{
			"Variables": [
				{ "ID": 1, "Name": "Numbers", "Type": "int[]", "Value": [1, 2, 3] },
				{ "ID": 2, "Name": "Points", "Type": "Strada.Vector3[]", "Value": [[0, 0, 0], [1, 2, 3]] },
				{ "ID": 3, "Name": "Keys", "Type": "Strada.KeyCode[]", "Value": ["Space", "W"] },
				{ "ID": 4, "Name": "Count", "Type": "int", "Value": 0 }
			],
			"Functions": [],
			"Graph": {
				"Nodes": [
					{ "ID": 10, "Type": "Event.Update" },
					{ "ID": 11, "Type": "Flow.Sequence", "Settings": { "Count": 4 } },
					{ "ID": 12, "Type": "Variable.Get", "Settings": { "Variable": 1 } },
					{ "ID": 13, "Type": "Array.Add", "Inputs": { "Value": 4 } },
					{ "ID": 14, "Type": "Variable.Set", "Settings": { "Variable": 1 } },
					{ "ID": 15, "Type": "Array.Make", "Settings": { "Type": "Strada.Vector3", "Count": 2 }, "Inputs": { "Element1": [4, 5, 6] } },
					{ "ID": 16, "Type": "Array.Append" },
					{ "ID": 17, "Type": "Variable.Get", "Settings": { "Variable": 2 } },
					{ "ID": 18, "Type": "Variable.Set", "Settings": { "Variable": 2 } },
					{ "ID": 19, "Type": "Variable.Get", "Settings": { "Variable": 4 } },
					{ "ID": 20, "Type": "Expression", "Settings": { "Formula": "n % 2 == 0 ? 99 : 1" } },
					{ "ID": 21, "Type": "Array.Get" },
					{ "ID": 22, "Type": "Variable.Set", "Settings": { "Variable": 4 } },
					{ "ID": 23, "Type": "Literal", "Settings": { "Type": "string[]", "Value": ["a", "b"] } },
					{ "ID": 24, "Type": "String.Join", "Inputs": { "Separator": "," } },
					{ "ID": 25, "Type": "String.Split" },
					{ "ID": 26, "Type": "Array.IndexOf", "Inputs": { "Value": "W" } },
					{ "ID": 27, "Type": "Variable.Get", "Settings": { "Variable": 3 } },
					{ "ID": 28, "Type": "Call:Strada.ScriptCore.Tests.FuzzStandIns.Pick<T>(T,T,bool)", "Inputs": { "First": 1, "TakeSecond": true } },
					{ "ID": 29, "Type": "Variable.Set", "Settings": { "Variable": 4 } }
				],
				"Links": [
					{ "From": [10, "Then"], "To": [11, "Execute"] },
					{ "From": [11, "Then0"], "To": [14, "Execute"] },
					{ "From": [11, "Then1"], "To": [18, "Execute"] },
					{ "From": [11, "Then2"], "To": [29, "Execute"] },
					{ "From": [11, "Then3"], "To": [22, "Execute"] },
					{ "From": [12, "Value"], "To": [13, "Array"] },
					{ "From": [13, "Result"], "To": [14, "Value"] },
					{ "From": [17, "Value"], "To": [16, "A"] },
					{ "From": [15, "Array"], "To": [16, "B"] },
					{ "From": [16, "Result"], "To": [18, "Value"] },
					{ "From": [19, "Value"], "To": [20, "n"] },
					{ "From": [12, "Value"], "To": [21, "Array"] },
					{ "From": [20, "Result"], "To": [21, "Index"] },
					{ "From": [21, "Element"], "To": [22, "Value"] },
					{ "From": [23, "Value"], "To": [24, "Parts"] },
					{ "From": [24, "Result"], "To": [25, "Text"] },
					{ "From": [27, "Value"], "To": [26, "Array"] },
					{ "From": [26, "Index"], "To": [28, "Second"] },
					{ "From": [28, "Result"], "To": [29, "Value"] }
				],
				"Comments": []
			}
		}
		""";

	// The document with array literals (variable Values, Literal and input values) replaced by damaged arrays: elements of
	// other types, nulls, nested arrays, too many elements, or no array at all.
	private static JsonNode DamageArrays(JsonNode original, FuzzRandom random)
	{
		JsonNode document = original.DeepClone();
		JsonNode Damaged(JsonNode? value)
		{
			JsonArray array = value as JsonArray ?? [];
			JsonNode?[] elements = [.. array.Select(static element => element?.DeepClone())];
			switch (random.Pick(6))
			{
				case 0:
					return JsonValue.Create("not an array")!;
				case 1:
					return new JsonArray([.. elements, null]);
				case 2:
					return new JsonArray([.. elements, new JsonArray(1, 2)]);
				case 3:
					return new JsonArray([.. elements, JsonValue.Create(random.Pick(2) == 0 ? "Nope" : "1e999")]);
				case 4:
					return new JsonArray([.. Enumerable.Range(0, 4096 + random.Pick(3)).Select(static index => (JsonNode?)JsonValue.Create(index))]);
				default:
					return new JsonArray();
			}
		}
		foreach (JsonNode? variable in document["Variables"]!.AsArray())
		{
			if (variable!["Value"] is JsonArray value && random.Pick(2) == 0)
			{
				variable["Value"] = Damaged(value);
			}
		}
		foreach (JsonNode? node in document["Graph"]!["Nodes"]!.AsArray())
		{
			if (node!["Settings"]?["Value"] is JsonArray literal && random.Pick(2) == 0)
			{
				node["Settings"]!["Value"] = Damaged(literal);
			}
			if (node["Inputs"] is JsonObject inputs && random.Pick(3) == 0)
			{
				foreach (string key in inputs.Select(static input => input.Key).ToList())
				{
					inputs[key] = Damaged(inputs[key]);
				}
			}
		}
		return document;
	}

	// The document with the texts of its formulas edited as typing edits them.
	private static JsonNode DamageFormulas(JsonNode original, FuzzRandom random)
	{
		JsonNode document = original.DeepClone();
		foreach (JsonNode? node in document["Graph"]!["Nodes"]!.AsArray())
		{
			if (node!["Settings"]?["Formula"] is JsonValue formula && formula.TryGetValue(out string? text) && random.Pick(2) == 0)
			{
				node["Settings"]!["Formula"] = FormulaGenerator.Damage(random, text);
			}
		}
		return document;
	}

	// What the rounds reached.
	private sealed class Tally
	{
		internal int InvalidDocuments { get; set; }

		internal int Broken { get; set; }

		internal int Ran { get; set; }

		internal int Aborted { get; set; }

		internal int OverBudget { get; set; }

		internal int Swapped { get; set; }

		// Graphs that ran with a disabled node in their event graph, and graphs that ran although a finding of an Error code
		// was located on a disabled node (a Warning there).
		internal int Disabled { get; set; }

		internal int Demoted { get; set; }

		// Graphs that ran with a split pin in their event graph.
		internal int Split { get; set; }

		public override string ToString() => FormattableString.Invariant(
			$"{InvalidDocuments} refused, {Broken} broken, {Ran} ran, {Aborted} aborted, {OverBudget} over budget, {Swapped} swapped, {Disabled} ran with a disabled node, {Demoted} ran with an error on one, {Split} ran with a split pin");
	}

	// One damaged document through a runtime of its own: every operation of the protocol, with the statuses spec sections
	// 5.1, 5.2, 5.4, 5.7 and 6 give it.
	private sealed class Round
	{
		private readonly string m_Context;
		private readonly DocumentMutator m_Mutator;
		private readonly Tally m_Tally;
		private readonly FakeEnvironment m_Environment = FuzzLibrary.CreateEnvironment();
		private readonly VisualScriptRuntime m_Runtime;

		internal Round(string context, DocumentMutator mutator, Tally tally)
		{
			m_Context = context;
			m_Mutator = mutator;
			m_Tally = tally;
			m_Runtime = new VisualScriptRuntime(m_Environment, () => FuzzLibrary.Library);
		}

		internal void Run(byte[] text)
		{
			(int errors, string diagnostics) = Analyze(text);
			Load(text, errors, diagnostics);
			GraphProgram? program = m_Runtime.FindEffectiveProgram(Graph);

			int logs = m_Environment.Logs.Count;
			VisualScriptStatus created = m_Runtime.CreateInstance(Hero, Graph, []);
			if (program == null)
			{
				Check(created == VisualScriptStatus.Failure, $"CreateInstance of a broken program answered {created}");
				List<string> logged = ErrorsSince(logs);
				Check(logged.Count == 1 && logged[0].StartsWith($"{Name} cannot run: ", StringComparison.Ordinal), $"expected one cannot-run line, got:\n{string.Join("\n", logged)}");
			}
			else
			{
				Check(created == VisualScriptStatus.Success, $"CreateInstance of a working program answered {created}");
				m_Tally.Ran++;
				if (program.EventGraph.Nodes.Any(static node => node.IsDisabled))
				{
					m_Tally.Disabled++;
				}
				if (HasDemotedError(diagnostics))
				{
					m_Tally.Demoted++;
				}
				if (program.EventGraph.Nodes.Any(static node => node.IsSplit))
				{
					m_Tally.Split++;
				}
				RunInstance(program);
			}
			Check(m_Runtime.CreateInstance(Hero, Graph + 1, []) == VisualScriptStatus.NotFound, "CreateInstance of a graph never loaded");
			Check(m_Runtime.Unload(Graph) == VisualScriptStatus.Success, "Unload of a loaded graph");
			Check(m_Runtime.Unload(Graph) == VisualScriptStatus.NotFound, "Unload of an unloaded graph");
			Check(m_Runtime.CreateInstance(Hero, Graph, []) == VisualScriptStatus.NotFound, "CreateInstance of an unloaded graph");
		}

		// Whether a finding of an Error code is a Warning: it is located on a disabled node.
		private static bool HasDemotedError(string diagnostics)
		{
			using JsonDocument array = JsonDocument.Parse(diagnostics);
			return array.RootElement.EnumerateArray().Any(static diagnostic => diagnostic.GetProperty("Severity").GetString() == "Warning" &&
				Diagnostic.GetSeverity(Enum.Parse<DiagnosticCode>(diagnostic.GetProperty("Code").GetString()!)) == DiagnosticSeverity.Error);
		}

		// Analyze answers Success with the analysis of any bytes: one InvalidDocument for what is not a JSON object. Returns
		// the Error count and the diagnostics array.
		private (int Errors, string Diagnostics) Analyze(byte[] text)
		{
			HostReply reply = m_Runtime.AnalyzeDocument(text);
			Check(reply.Status == VisualScriptStatus.Success && reply.Text != null, $"Analyze answered {reply.Status}");
			using JsonDocument analysis = JsonDocument.Parse(reply.Text!);
			JsonElement diagnostics = analysis.RootElement.GetProperty("Diagnostics");
			Check(analysis.RootElement.GetProperty("Nodes").ValueKind == JsonValueKind.Array, "the analysis has no Nodes array");
			int errors = diagnostics.EnumerateArray().Count(static diagnostic => diagnostic.GetProperty("Severity").GetString() == "Error");
			if (!IsJsonObject(text))
			{
				Check(diagnostics.GetArrayLength() == 1 && diagnostics[0].GetProperty("Code").GetString() == "InvalidDocument",
					$"a document that is not a JSON object analyzed to {diagnostics.GetRawText()}");
				m_Tally.InvalidDocuments++;
			}
			return (errors, diagnostics.GetRawText());
		}

		// Load always stores a program and answers the diagnostics the analysis has; the program is broken exactly when one
		// of them is an Error.
		private void Load(byte[] text, int errors, string expected)
		{
			HostReply reply = m_Runtime.LoadDocument(Graph, Name, text);
			Check(reply.Status == VisualScriptStatus.Success && reply.Text != null, $"Load answered {reply.Status}");
			using JsonDocument diagnostics = JsonDocument.Parse(reply.Text!);
			Check(diagnostics.RootElement.GetRawText() == expected, "Load answered other diagnostics than Analyze");
			GraphProgram? latest = m_Runtime.FindLatestProgram(Graph);
			Check(latest != null && latest.IsBroken == errors > 0 && latest.ErrorCount == errors, "the stored program does not match its diagnostics");
			Check(ReferenceEquals(m_Runtime.FindEffectiveProgram(Graph), errors > 0 ? null : latest), "the effective program is not the working one");
			if (errors > 0)
			{
				m_Tally.Broken++;
			}
		}

		private void RunInstance(GraphProgram program)
		{
			for (int index = 0; index < s_Events.Length; index++)
			{
				(ScriptEvent scriptEvent, float timeStep, ulong other) = s_Events[index];
				Invoke(Hero, scriptEvent, timeStep, other);
				if (index == 3)
				{
					UseVariables(program);
				}
				if (index == 6)
				{
					SendEvents(program);
				}
			}
			// A second instance takes the first one's public variables, sometimes damaged.
			byte[] variables = Describe(Hero, program);
			if (m_Mutator.Pick(2) == 0)
			{
				variables = m_Mutator.Write(m_Mutator.Damage(JsonNode.Parse(variables), m_Mutator.Pick(3)));
			}
			Check(m_Runtime.CreateInstance(Other, Graph, variables) == VisualScriptStatus.Success, "CreateInstance with overrides");
			Invoke(Other, ScriptEvent.Create, 0.0f, 0);
			Invoke(Other, ScriptEvent.Update, 0.1f, 0);
			Check(m_Runtime.DestroyInstance(Other) == VisualScriptStatus.Success, "DestroyInstance of an instance");

			Invoke(Hero, ScriptEvent.Destroy, 0.0f, 0);
			Check(m_Runtime.Invoke(Hero, ScriptEvent.Update, 0.016f, 0) == VisualScriptStatus.NotFound, "Invoke after Destroy");
			Check(m_Runtime.DestroyInstance(Hero) == VisualScriptStatus.NotFound, "DestroyInstance after Destroy");
			Check(m_Runtime.DescribeVariables(Hero).Status == VisualScriptStatus.NotFound, "DescribeInstanceVariables after Destroy");
			Check(m_Runtime.SetInstanceVariables(Hero, variables) == VisualScriptStatus.NotFound, "SetInstanceVariables after Destroy");
			Check(!m_Runtime.SendEvent(Hero, "Anything", []), "SendEvent after Destroy");
		}

		// Invoke answers Success or Failure for an entity with an instance; Failure exactly when an execution aborted, each
		// abort logged once in the format of spec section 5.3.
		private void Invoke(ulong entity, ScriptEvent scriptEvent, float timeStep, ulong other)
		{
			int logs = m_Environment.Logs.Count;
			VisualScriptStatus status = m_Runtime.Invoke(entity, scriptEvent, timeStep, other);
			List<string> errors = ErrorsSince(logs);
			Check(status is VisualScriptStatus.Success or VisualScriptStatus.Failure, $"Invoke {scriptEvent} answered {status}");
			Check(status == VisualScriptStatus.Failure == errors.Count > 0, $"Invoke {scriptEvent} answered {status} with {errors.Count} errors logged");
			CheckAborts(errors, m_Tally);
			if (status == VisualScriptStatus.Failure)
			{
				m_Tally.Aborted++;
			}
		}

		// The variables read back, round-trip through the override encoding and convert by the rules of spec section 5.7.
		private void UseVariables(GraphProgram program)
		{
			byte[] described = Describe(Hero, program);
			Check(m_Runtime.SetInstanceVariables(Hero, described) == VisualScriptStatus.Success, "SetInstanceVariables of the described values");
			Check(Describe(Hero, program).AsSpan().SequenceEqual(described), "described variables do not survive their own overrides");
			Check(m_Runtime.SetInstanceVariables(Hero, m_Mutator.Write(m_Mutator.Damage(JsonNode.Parse(described), 2))) == VisualScriptStatus.Success,
				"SetInstanceVariables of damaged values");
			foreach (CompiledVariable variable in program.Variables)
			{
				Check(m_Runtime.HasVariable(Hero, variable.Name), $"HasVariable {variable.Name}");
				object? value = m_Runtime.GetVariable(Hero, variable.Name, typeof(object));
				try
				{
					m_Runtime.SetVariable(Hero, variable.Name, value?.GetType() ?? typeof(object), value);
				}
				catch (InvalidCastException) when (value == null)
				{
					// None of System.Object does not convert to a variable of a narrower reference type.
				}
			}
			Check(!m_Runtime.HasVariable(Hero, "No Such Variable"), "HasVariable of an unknown name");
			Assert.Throws<ArgumentException>(() => m_Runtime.GetVariable(Hero, "No Such Variable", typeof(object)));
		}

		// Every custom event that is not disabled, sent with the type defaults of its parameters and with one argument too
		// many.
		private void SendEvents(GraphProgram program)
		{
			foreach (CompiledNode node in program.EventGraph.Nodes)
			{
				if (node.Op != Opcode.Event || node.Builtin != BuiltinNode.EventCustom)
				{
					continue;
				}
				object?[] arguments = [.. node.EventParameters.Select(static type => SlotValues.ToValue(SlotValues.DefaultOf(type), type))];
				int logs = m_Environment.Logs.Count;
				try
				{
					Check(m_Runtime.SendEvent(Hero, node.EventName!, arguments), $"SendEvent {node.EventName} did not run");
				}
				catch (InvalidOperationException exception)
				{
					List<string> errors = ErrorsSince(logs);
					Check(errors.Count == 1 && errors[0] == exception.Message, "SendEvent threw other than the logged abort");
				}
				CheckAborts(ErrorsSince(logs), m_Tally);
				Check(!m_Runtime.SendEvent(Hero, node.EventName!, [.. arguments, 1]), "SendEvent with an argument too many");
			}
			Check(!m_Runtime.SendEvent(Hero, "No Such Event", []), "SendEvent of an unknown event");
		}

		// DescribeInstanceVariables answers the public variables in document order.
		private byte[] Describe(ulong entity, GraphProgram program)
		{
			HostReply reply = m_Runtime.DescribeVariables(entity);
			Check(reply.Status == VisualScriptStatus.Success && reply.Text != null, $"DescribeInstanceVariables answered {reply.Status}");
			using JsonDocument variables = JsonDocument.Parse(reply.Text!);
			string[] names = [.. variables.RootElement.EnumerateObject().Select(static member => member.Name)];
			string[] expected = [.. program.Variables.Where(static variable => variable.Public).Select(static variable => variable.Name)];
			Check(names.SequenceEqual(expected), "the described variables are not the public variables in document order");
			return reply.Text!;
		}

		private List<string> ErrorsSince(int logs) => [.. m_Environment.Logs.Skip(logs).Where(static log => log.Level == VisualScriptLogLevel.Error)
			.Select(static log => log.Message)];

		private void Check(bool condition, string what)
		{
			Assert.True(condition, $"{m_Context}: {what}");
		}
	}

	// A runtime whose two instances live through the whole fuzz run: every damaged document is loaded over their graph, and
	// working ones swap the program they run (spec section 5.6) between unrelated documents, with fibers and node states
	// carried over or cancelled. Every few rounds an undamaged fixture repairs the graph.
	private sealed class LiveRuntime
	{
		private readonly FakeEnvironment m_Environment = FuzzLibrary.CreateEnvironment();
		private readonly VisualScriptRuntime m_Runtime;
		// The instance of each entity and the program it ran in the last round.
		private readonly Dictionary<ulong, (GraphInstance Instance, GraphProgram Program)> m_Running = [];
		private int m_Rounds;

		internal LiveRuntime()
		{
			m_Runtime = new VisualScriptRuntime(m_Environment, () => FuzzLibrary.Library);
		}

		internal void Run(string context, byte[] text, byte[]? repair, Tally tally)
		{
			Assert.True(m_Runtime.LoadDocument(Graph, Name, text).Status == VisualScriptStatus.Success, $"{context}: live Load");
			if (repair != null)
			{
				Assert.True(m_Runtime.LoadDocument(Graph, Name, repair).Status == VisualScriptStatus.Success, $"{context}: live Load");
			}
			GraphProgram? effective = m_Runtime.FindEffectiveProgram(Graph);
			if (++m_Rounds % 25 == 0)
			{
				m_Runtime.DestroyAllInstances();
				Assert.Equal(0, m_Runtime.InstanceCount);
			}
			foreach (ulong entity in (ulong[])[Hero, Other])
			{
				GraphInstance? instance = m_Runtime.FindInstance(entity);
				if (instance == null)
				{
					if (effective == null)
					{
						continue;
					}
					Assert.True(m_Runtime.CreateInstance(entity, Graph, []) == VisualScriptStatus.Success, $"{context}: live CreateInstance");
					Invoke(context, entity, ScriptEvent.Create, 0.0f, tally);
					continue;
				}
				if (m_Running.TryGetValue(entity, out (GraphInstance Instance, GraphProgram Program) last) && ReferenceEquals(last.Instance, instance) &&
					!ReferenceEquals(last.Program, instance.Program))
				{
					tally.Swapped++;
				}
				Invoke(context, entity, ScriptEvent.Update, 0.1f, tally);
				Invoke(context, entity, ScriptEvent.Update, 0.6f, tally);
				m_Running[entity] = (instance, instance.Program);
			}
		}

		private void Invoke(string context, ulong entity, ScriptEvent scriptEvent, float timeStep, Tally tally)
		{
			int logs = m_Environment.Logs.Count;
			VisualScriptStatus status = m_Runtime.Invoke(entity, scriptEvent, timeStep, Other);
			List<string> errors = [.. m_Environment.Logs.Skip(logs).Where(static log => log.Level == VisualScriptLogLevel.Error).Select(static log => log.Message)];
			Assert.True(status is VisualScriptStatus.Success or VisualScriptStatus.Failure, $"{context}: live Invoke {scriptEvent} answered {status}");
			Assert.True(status == VisualScriptStatus.Failure == errors.Count > 0, $"{context}: live Invoke answered {status} with {errors.Count} errors");
			CheckAborts(errors, tally);
		}
	}

	// Aborting errors name the graph, the node and the entity, and carry a message of the VM: the members the fuzz graphs
	// call never throw, so an exception's type in a message is a defect of the VM.
	private static void CheckAborts(List<string> errors, Tally tally)
	{
		foreach (string error in errors)
		{
			Assert.True(error.StartsWith($"{Name}: ", StringComparison.Ordinal) && error.Contains(" (node ", StringComparison.Ordinal), $"not an abort: {error}");
			Assert.False(error.Contains("Exception: ", StringComparison.Ordinal), $"the VM failed: {error}");
			if (error.EndsWith("execution exceeded 1,000,000 steps (endless loop?)", StringComparison.Ordinal))
			{
				tally.OverBudget++;
			}
		}
	}

	private static bool IsJsonObject(byte[] text)
	{
		try
		{
			using JsonDocument document = JsonDocument.Parse(text, new JsonDocumentOptions { MaxDepth = DocumentReader.MaxDepth });
			return document.RootElement.ValueKind == JsonValueKind.Object;
		}
		catch (JsonException)
		{
			return false;
		}
	}
}

// The node library of the fuzz tests: the scripting API without the members of the shared fixtures that reach the engine,
// whose node types redirect to stand-ins with the same pins (FuzzStandIns). ScriptCoreTests runs without the engine, and a
// binding called there would end the test process (spec section 5.5).
internal static class FuzzLibrary
{
	// The entity that runs the graphs, and the one contacts and clicks report.
	internal const ulong Hero = 7;
	internal const ulong Other = 8;

	private const string StandIns = "Strada.ScriptCore.Tests.FuzzStandIns";

	private static readonly Type[] s_EngineTypes =
		[typeof(Entity), typeof(Input), typeof(Log), typeof(Physics), typeof(Strada.Testing.TestReporter), typeof(TransformComponent)];

	private static readonly Lazy<NodeLibrary> s_Library = new(static () => NodeLibraryBuilder.Build(new NodeLibrarySources
	{
		ApiTypes = [.. NodeLibrarySources.GetApiTypes().Where(static type => Array.IndexOf(s_EngineTypes, type) < 0), typeof(FuzzStandIns)],
		ScriptClasses = [typeof(Mover)],
		Redirects =
		[
			new NodeRedirect("Call:Strada.Entity.GetComponent<T>()", $"Call:{StandIns}.GetComponent<T>(Strada.Entity)", []),
			new NodeRedirect("Call:Strada.Input.IsKeyDown(Strada.KeyCode)", $"Call:{StandIns}.IsKeyDown(Strada.KeyCode)", []),
			new NodeRedirect("Call:Strada.Log.Info(string)", $"Call:{StandIns}.Info(string)", []),
			new NodeRedirect("Call:Strada.Physics.Raycast(Strada.Vector3,Strada.Vector3,float,out Strada.RaycastHit,uint)",
				$"Call:{StandIns}.Raycast(Strada.Vector3,Strada.Vector3,float,out Strada.RaycastHit,uint)", []),
			new NodeRedirect("Call:Strada.Testing.TestReporter.Pass(string)", $"Call:{StandIns}.Pass(string)", []),
			new NodeRedirect("Get:Strada.Entity.Children", $"Call:{StandIns}.ChildrenOf(Strada.Entity)", [new PinRename(PinDirection.Out, "Value", "Result")]),
			new NodeRedirect("Get:Strada.Entity.Name", $"Call:{StandIns}.NameOf(Strada.Entity)", [new PinRename(PinDirection.Out, "Value", "Result")]),
			new NodeRedirect("Set:Strada.Entity.Name", $"Call:{StandIns}.SetName(Strada.Entity,string)", []),
			new NodeRedirect("Get:Strada.TransformComponent.Translation", $"Call:{StandIns}.TranslationOf(Strada.Entity)",
				[new PinRename(PinDirection.Out, "Value", "Result")]),
			new NodeRedirect("Set:Strada.TransformComponent.Translation", $"Call:{StandIns}.SetTranslation(Strada.Entity,Strada.Vector3)", []),
		],
	}));

	private static readonly Lazy<IReadOnlyList<(string Name, JsonNode Document)>> s_Fixtures = new(static () =>
	{
		List<(string Name, JsonNode Document)> fixtures = [];
		foreach (string name in (string[])["Events.json", "Flow.json", "Data.json", "Functions.json", "Variables.json", "Reflected.json", "Editor.json"])
		{
			fixtures.Add((name, JsonNode.Parse(File.ReadAllBytes(Path.Combine(VisualScriptTesting.FixtureDirectory, "Documents", name)))!));
		}
		JsonNode example = JsonNode.Parse(File.ReadAllBytes(Path.Combine(VisualScriptTesting.FixtureDirectory, "Example.sgraph")))!;
		fixtures.Add(("Example.sgraph", example["VisualScript"]!.DeepClone()));
		return fixtures;
	});

	internal static NodeLibrary Library => s_Library.Value;

	// The documents of spec section 11.1 in content form: the six documents, then the content of Example.sgraph.
	internal static IReadOnlyList<(string Name, JsonNode Document)> Fixtures => s_Fixtures.Value;

	// Hero runs the graphs; Other, a scripted entity, is what contacts and clicks report. Neither has components, so
	// component members abort before they could reach the engine.
	internal static FakeEnvironment CreateEnvironment()
	{
		FakeEnvironment environment = new();
		environment.Add(Hero, "Hero");
		environment.Add(Other, "Other", new Mover());
		return environment;
	}
}

// Stand-ins for the members of the shared fixtures that reach the engine, with their pins: static members whose Target
// parameter takes the place of the entity a member of Strada.Entity or a component acts on. They never throw.
public static class FuzzStandIns
{
	[ThreadStatic]
	private static int s_Calls;

	// An unconstrained generic method (spec-slice3 section 3.6), whose type links decide.
	[PureNode]
	public static T Pick<T>(T first, T second, bool takeSecond) => takeSecond ? second : first;

	[ThreadStatic]
	private static List<string>? s_Messages;

	[ThreadStatic]
	private static Vector3 s_Translation;

	// How often graphs called a stand-in on this thread.
	internal static int Calls
	{
		get => s_Calls;
		set => s_Calls = value;
	}

	// What graphs log on this thread while a test records it (null: nothing is recorded), and the translation they set last.
	internal static List<string>? Messages
	{
		get => s_Messages;
		set => s_Messages = value;
	}

	internal static Vector3 Translation
	{
		get => s_Translation;
		set => s_Translation = value;
	}

	public static void Info(string message)
	{
		s_Calls++;
		s_Messages?.Add(message);
	}

	public static void Pass(string name)
	{
		s_Calls++;
	}

	[PureNode]
	public static bool IsKeyDown(KeyCode key)
	{
		s_Calls++;
		return key == KeyCode.Space;
	}

	public static bool Raycast(Vector3 origin, Vector3 direction, float maxDistance, out RaycastHit hit, uint layerMask = Physics.AllLayers)
	{
		s_Calls++;
		// Something 2.5 along the ray, the entity Other.
		hit = new RaycastHit(new Entity(FuzzLibrary.Other), origin + (direction * 2.5f), Vector3.Up, 2.5f);
		return maxDistance > 1.0f && layerMask != 0;
	}

	[PureNode]
	public static T? GetComponent<T>(Entity? target)
		where T : Component
	{
		s_Calls++;
		return null;
	}

	[PureNode]
	public static string NameOf(Entity? target)
	{
		s_Calls++;
		return target != null ? "Hero" : string.Empty;
	}

	public static void SetName(Entity? target, string value)
	{
		s_Calls++;
	}

	// One child, the next entity, for every entity.
	[PureNode]
	public static Entity[] ChildrenOf(Entity? target)
	{
		s_Calls++;
		return target != null ? [new Entity(target.ID + 1)] : [];
	}

	[PureNode]
	public static Vector3 TranslationOf(Entity? target)
	{
		s_Calls++;
		return target != null ? new Vector3(1.0f, 2.0f, 3.0f) : default;
	}

	public static void SetTranslation(Entity? target, Vector3 value)
	{
		s_Calls++;
		s_Translation = value;
	}
}
