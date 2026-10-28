using System;
using System.Linq;
using System.Text;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// What the analysis finds about the other graphs a document's nodes name (spec-slice3 sections 4.1 to 4.4, 4.8): the Graph
// setting, calls of function libraries, typed sends, the rules of a library document, renamed library parameters, and how
// the findings are written.
public sealed class CrossGraphAnalysisTests
{
	private static readonly string s_Door = CrossGraphTesting.Door.ToString(System.Globalization.CultureInfo.InvariantCulture);

	[Theory]
	[InlineData("42")]
	[InlineData("\"0\"")]
	[InlineData("\"Door\"")]
	[InlineData("\"-5\"")]
	[InlineData("\"\"")]
	[InlineData("\"18446744073709551616\"")]
	public void AGraphSettingThatIsNoHandleIsAnInvalidSetting(string graph)
	{
		foreach (string node in (string[])
			[
				$$"""{ "ID": 1, "Type": "Function.Call", "Settings": { "Function": 1, "Graph": {{graph}} } }""",
				$$"""{ "ID": 1, "Type": "Event.Send", "Settings": { "Name": "Hit", "Graph": {{graph}} } }""",
			])
		{
			AnalyzedDocument analysis = CrossGraphTesting.Analyze(VisualScriptTesting.Document(node), new BundleBuilder().Build());
			Diagnostic diagnostic = VisualScriptTesting.Single(analysis, DiagnosticCode.InvalidSetting);
			Assert.Equal(DiagnosticSeverity.Error, diagnostic.Severity);
			Assert.Equal(
				"Graph: expected the decimal handle of a visual script, as asset.list gives it; remove the setting to refer to this script",
				diagnostic.Message);
			Assert.Equal(0UL, diagnostic.Graph);
			Assert.False(VisualScriptTesting.Has(analysis, DiagnosticCode.UnknownSetting));
		}
	}

	[Fact]
	public void TheGraphsOwnFunctionsAreCalledWithoutAGraphSetting()
	{
		DocumentBuilder document = new();
		FunctionBuilder run = document.Function("Run");
		document.Graph.Node("Function.Call", CrossGraphTesting.Call(run, TestRuntime.Graph));

		AnalyzedDocument analysis = CrossGraphTesting.Analyze(document.Build(), new BundleBuilder().Build());
		Diagnostic diagnostic = VisualScriptTesting.Single(analysis, DiagnosticCode.InvalidSetting);
		Assert.Equal("Graph: this graph's own functions are called without a Graph setting", diagnostic.Message);
		Assert.Equal(DiagnosticSeverity.Error, diagnostic.Severity);
	}

	[Fact]
	public void CallsOfGraphsThatAreAbsentOrNoLibrariesAreErrors()
	{
		DocumentBuilder document = new();
		uint absent = document.Graph.Node("Function.Call", CrossGraphTesting.Call(1, 42));
		uint failed = document.Graph.Node("Function.Call", CrossGraphTesting.Call(1, 43));
		uint script = document.Graph.Node("Function.Call", CrossGraphTesting.Call(1, CrossGraphTesting.Door));
		string bundle = new BundleBuilder().Absent(43, "Scripts/Gone.sgraph does not load: it is not JSON")
			.Full(CrossGraphTesting.Door, CrossGraphTesting.DoorName, CrossGraphTesting.DoorScript()).Build();

		AnalyzedDocument analysis = CrossGraphTesting.Analyze(document.Build(), bundle);
		Diagnostic[] unknown = [.. analysis.Diagnostics.Where(static diagnostic => diagnostic.Code == DiagnosticCode.UnknownGraph)];
		Assert.Equal(2, unknown.Length);
		Assert.Equal((absent, 42UL, "Graph: no visual script 42 (deleted, or not a visual script)"), (unknown[0].Node!.Value, unknown[0].Graph, unknown[0].Message));
		Assert.Equal((failed, 43UL, "Graph: Scripts/Gone.sgraph does not load: it is not JSON"), (unknown[1].Node!.Value, unknown[1].Graph, unknown[1].Message));
		Diagnostic notALibrary = VisualScriptTesting.Single(analysis, DiagnosticCode.NotALibrary);
		Assert.Equal((script, CrossGraphTesting.Door), (notALibrary.Node!.Value, notALibrary.Graph));
		Assert.Equal("Scripts/Door.sgraph is not a function library: only libraries' functions are called from other graphs", notALibrary.Message);
		Assert.All(analysis.Diagnostics, static diagnostic => Assert.Equal(DiagnosticSeverity.Error, diagnostic.Severity));
		// Their pins are what their links and inputs name, so nothing else is reported.
		Assert.Equal(3, analysis.Diagnostics.Count);
		Assert.Equal("Door", VisualScriptTesting.Node(analysis, script).Subtitle);
	}

	[Fact]
	public void AnUnknownFunctionOfALibrarySuggestsTheLibrarysFunctionsByTheirIds()
	{
		MotionLibrary library = new();
		DocumentBuilder document = new();
		uint call = document.Graph.Node("Function.Call", CrossGraphTesting.Call(library.Twice.Id + 100, CrossGraphTesting.Motion));

		Diagnostic diagnostic = VisualScriptTesting.Single(CrossGraphTesting.Analyze(document.Build(), library.Bundle()), DiagnosticCode.UnknownFunction);
		Assert.Equal(FormattableString.Invariant($"Scripts/Motion.sgraph has no function {library.Twice.Id + 100}"), diagnostic.Message);
		Assert.Equal((call, CrossGraphTesting.Motion), (diagnostic.Node!.Value, diagnostic.Graph));
		Assert.NotEmpty(diagnostic.Suggestions);
		Assert.All(diagnostic.Suggestions, static suggestion => Assert.True(uint.TryParse(suggestion, out _), suggestion));
	}

	[Fact]
	public void ACallOfALibraryWithAnErrorIsAReferenceError()
	{
		DocumentBuilder broken = new() { Kind = "Library" };
		FunctionBuilder run = broken.Function("Run");
		uint nothing = run.Graph.Node("Flow.Nothing");
		DocumentBuilder document = new();
		uint call = document.Graph.Node("Function.Call", CrossGraphTesting.Call(run, CrossGraphTesting.Motion));
		document.Graph.Flow(document.Graph.Node("Event.Create"), call);

		AnalyzedDocument analysis = CrossGraphTesting.Analyze(document.Build(),
			new BundleBuilder().Full(CrossGraphTesting.Motion, CrossGraphTesting.MotionName, broken).Build());
		Diagnostic diagnostic = VisualScriptTesting.Single(analysis, DiagnosticCode.ReferenceError);
		Assert.Equal(FormattableString.Invariant(
			$"Scripts/Motion.sgraph has 1 error, first: unknown node type \"Flow.Nothing\" (node {nothing}): fix the library"), diagnostic.Message);
		Assert.Equal((call, CrossGraphTesting.Motion, DiagnosticSeverity.Error), (diagnostic.Node!.Value, diagnostic.Graph, diagnostic.Severity));
		// The library's own findings stay in its analysis; the call still has the function's pins.
		Assert.Single(analysis.Diagnostics);
		Assert.Equal("Run", VisualScriptTesting.Node(analysis, call).Title);
		Assert.True(ProgramCompiler.Compile(analysis, TestRuntime.Name).IsBroken);
	}

	[Fact]
	public void ALibraryHasNoVariablesAndNoEventGraph()
	{
		DocumentBuilder document = new() { Kind = "Library" };
		uint speed = document.Variable("Speed", "float");
		uint update = document.Graph.Node("Event.Update");
		FunctionBuilder run = document.Function("Run");
		run.Graph.Flow(run.Entry, run.Graph.Write("ran"));

		AnalyzedDocument analysis = CrossGraphTesting.Analyze(document.Build(), new BundleBuilder().Build());
		Assert.Equal(DocumentKind.Library, analysis.Kind);
		Diagnostic variable = VisualScriptTesting.Single(analysis, DiagnosticCode.LibraryVariable);
		Assert.Equal((speed, (uint?)null, DiagnosticSeverity.Error), (variable.Variable!.Value, variable.Node, variable.Severity));
		Assert.Equal("Speed: a library has no variables: its functions run for the graph that calls them; use a local or an input", variable.Message);
		Diagnostic node = VisualScriptTesting.Single(analysis, DiagnosticCode.LibraryEventGraph);
		Assert.Equal((update, DiagnosticSeverity.Error), (node.Node!.Value, node.Severity));
		Assert.Equal("a library has no event graph: its functions run when graphs call them", node.Message);
		Assert.Equal(2, analysis.ErrorCount);

		// Comment boxes are no nodes: a library with only functions is clean.
		document = new DocumentBuilder { Kind = "Library" };
		run = document.Function("Run");
		run.Graph.Flow(run.Entry, run.Graph.Write("ran"));
		VisualScriptTesting.AssertNoErrors(CrossGraphTesting.Analyze(document.Build(), ""));
		Assert.Empty(CrossGraphTesting.Analyze(document.Build(), "").Diagnostics);
	}

	[Fact]
	public void TypedSendsAreCheckedAgainstTheTargetsCustomEventWithWarnings()
	{
		const string damage = """{ "Name": "Damage", "Type": "float" }""";
		(string Settings, ulong Graph, string Interface, DiagnosticCode Code, string Message)[] rows =
		[
			(CrossGraphTesting.Send("Hit", 42, damage), 42, "", DiagnosticCode.UncheckedGraph,
				"Graph: no visual script 42 to check Send Hit against; it sends Hit by name"),
			(CrossGraphTesting.Send("Hit", CrossGraphTesting.Motion, damage), CrossGraphTesting.Motion, new MotionLibrary().Build(), DiagnosticCode.UnknownEvent,
				"Scripts/Motion.sgraph is a function library: it has no events"),
			(CrossGraphTesting.Send("Hot", CrossGraphTesting.Door, damage), CrossGraphTesting.Door, CrossGraphTesting.DoorScript().Build(),
				DiagnosticCode.UnknownEvent, "Scripts/Door.sgraph has no custom event Hot"),
			(CrossGraphTesting.Send("Hit", CrossGraphTesting.Door, """{ "Name": "Damage", "Type": "int" }"""), CrossGraphTesting.Door,
				CrossGraphTesting.DoorScript().Build(), DiagnosticCode.EventSignatureMismatch, "Door's Hit takes (float Damage); this Send passes (int Damage)"),
			(CrossGraphTesting.Send("Hit", CrossGraphTesting.Door), CrossGraphTesting.Door, CrossGraphTesting.DoorScript().Build(),
				DiagnosticCode.EventSignatureMismatch, "Door's Hit takes (float Damage); this Send passes ()"),
			(CrossGraphTesting.Send("Hit", CrossGraphTesting.Door, damage), CrossGraphTesting.Door, CrossGraphTesting.DoorScript("Disabled").Build(),
				DiagnosticCode.DisabledTarget, "Door's Hit is disabled: it receives nothing"),
		];
		foreach ((string settings, ulong graph, string target, DiagnosticCode code, string message) in rows)
		{
			DocumentBuilder document = new();
			uint send = document.Graph.Node("Event.Send", settings);
			// A linked Target leaves only the finding about the event.
			document.Graph.Link(document.Graph.Node("Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)", inputs: """{ "Id": "8" }"""), "Result", send,
				"Target");
			BundleBuilder bundle = new();
			if (target.Length > 0)
			{
				bundle.Interface(graph, graph == CrossGraphTesting.Door ? CrossGraphTesting.DoorName : CrossGraphTesting.MotionName, target);
			}
			AnalyzedDocument analysis = CrossGraphTesting.Analyze(document.Build(), bundle.Build());
			Diagnostic diagnostic = Assert.Single(analysis.Diagnostics);
			Assert.Equal((code, message, DiagnosticSeverity.Warning), (diagnostic.Code, diagnostic.Message, diagnostic.Severity));
			Assert.Equal((send, graph), (diagnostic.Node!.Value, diagnostic.Graph));
			Assert.False(ProgramCompiler.Compile(analysis, TestRuntime.Name).IsBroken);
		}
	}

	[Fact]
	public void AnUnknownEventSuggestsTheTargetsNearestEventNames()
	{
		DocumentBuilder door = new();
		foreach (string name in (string[])["Open", "Opened", "Close", "Lock", "Unlock"])
		{
			door.Graph.Node("Event.Custom", $$"""{ "Name": "{{name}}" }""");
		}
		DocumentBuilder document = new();
		document.Graph.Node("Event.Send", CrossGraphTesting.Send("Opn", CrossGraphTesting.Door));
		Diagnostic diagnostic = VisualScriptTesting.Single(CrossGraphTesting.Analyze(document.Build(),
			new BundleBuilder().Interface(CrossGraphTesting.Door, CrossGraphTesting.DoorName, door).Build()), DiagnosticCode.UnknownEvent);
		Assert.Equal(["Open"], diagnostic.Suggestions);
	}

	[Fact]
	public void ASendWhoseTargetIsSelfWhileItNamesAnotherScriptIsAWarning()
	{
		string bundle = new BundleBuilder().Interface(CrossGraphTesting.Door, CrossGraphTesting.DoorName, CrossGraphTesting.DoorScript()).Build();
		DocumentBuilder document = new();
		uint send = document.Graph.Node("Event.Send", CrossGraphTesting.Send("Hit", CrossGraphTesting.Door, """{ "Name": "Damage", "Type": "float" }"""));

		Diagnostic diagnostic = Assert.Single(CrossGraphTesting.Analyze(document.Build(), bundle).Diagnostics);
		Assert.Equal((DiagnosticCode.TargetIsSelf, DiagnosticSeverity.Warning, send, CrossGraphTesting.Door),
			(diagnostic.Code, diagnostic.Severity, diagnostic.Node!.Value, diagnostic.Graph));
		Assert.Equal("Target: Self runs this graph, not Scripts/Door.sgraph: link the entity to send to", diagnostic.Message);
		// A load knows the graph's asset path.
		Assert.Equal("Target: Self runs Assets/Test.sgraph, not Scripts/Door.sgraph: link the entity to send to",
			VisualScriptTesting.Single(CrossGraphTesting.Analyze(document.Build(), bundle, name: TestRuntime.Name), DiagnosticCode.TargetIsSelf).Message);

		// In a library an unlinked Target is the entity whose execution called the function.
		DocumentBuilder library = new() { Kind = "Library" };
		FunctionBuilder hit = library.Function("Hit");
		hit.Graph.Flow(hit.Entry, hit.Graph.Node("Event.Send", CrossGraphTesting.Send("Hit", CrossGraphTesting.Door, """{ "Name": "Damage", "Type": "float" }""")));
		Assert.Empty(CrossGraphTesting.Analyze(library.Build(), bundle).Diagnostics);
	}

	[Fact]
	public void ASendNamingItsOwnGraphIsCheckedAgainstTheAnalyzedDocument()
	{
		// The bundle has no entry for the document itself: an event added in the same edit is found.
		DocumentBuilder document = new();
		document.Graph.Node("Event.Custom", """{ "Name": "Ping" }""");
		uint send = document.Graph.Node("Event.Send", CrossGraphTesting.Send("Ping", TestRuntime.Graph));
		uint unknown = document.Graph.Node("Event.Send", CrossGraphTesting.Send("Pong", TestRuntime.Graph));
		string bundle = new BundleBuilder().Interface(TestRuntime.Graph, "Scripts/Stale.sgraph", "{}").Build();

		AnalyzedDocument analysis = CrossGraphTesting.Analyze(document.Build(), bundle);
		Diagnostic diagnostic = Assert.Single(analysis.Diagnostics);
		Assert.Equal((DiagnosticCode.UnknownEvent, unknown, "this graph has no custom event Pong"), (diagnostic.Code, diagnostic.Node!.Value, diagnostic.Message));
		Assert.Equal(["Ping"], diagnostic.Suggestions);
		Assert.Equal(("Send Ping", "to this graph"), (VisualScriptTesting.Node(analysis, send).Title, VisualScriptTesting.Node(analysis, send).Subtitle));
	}

	[Fact]
	public void TypedSendsAndLibraryCallsHaveTheTitlesOfWhatTheyName()
	{
		MotionLibrary library = new();
		DocumentBuilder document = new();
		uint twice = document.Graph.Node("Function.Call", CrossGraphTesting.Call(library.Twice, CrossGraphTesting.Motion));
		uint pause = document.Graph.Node("Function.Call", CrossGraphTesting.Call(library.Pause, CrossGraphTesting.Motion));
		uint send = document.Graph.Node("Event.Send", CrossGraphTesting.Send("Hit", CrossGraphTesting.Door, """{ "Name": "Damage", "Type": "float" }"""));
		string bundle = new BundleBuilder().Full(CrossGraphTesting.Motion, CrossGraphTesting.MotionName, library.Document)
			.Interface(CrossGraphTesting.Door, CrossGraphTesting.DoorName, CrossGraphTesting.DoorScript()).Build();

		AnalyzedDocument analysis = CrossGraphTesting.Analyze(document.Build(), bundle);
		AnalyzedNode pure = VisualScriptTesting.Node(analysis, twice);
		Assert.Equal(("Twice", "Motion", true, false), (pure.Title, pure.Subtitle, pure.Pure, pure.Latent));
		Assert.Equal(["Value", "Result"], pure.Pins.Select(static pin => pin.Name));
		AnalyzedNode latent = VisualScriptTesting.Node(analysis, pause);
		Assert.Equal(("Pause", "Motion", false, true), (latent.Title, latent.Subtitle, latent.Pure, latent.Latent));
		Assert.Equal(["Execute", "Seconds", "Then"], latent.Pins.Select(static pin => pin.Name));
		Assert.Equal(("Send Hit", "to Door"), (VisualScriptTesting.Node(analysis, send).Title, VisualScriptTesting.Node(analysis, send).Subtitle));
		string json = Encoding.UTF8.GetString(AnalysisWriter.Write(analysis));
		Assert.Contains("\"Title\":\"Twice\",\"Subtitle\":\"Motion\"", json, StringComparison.Ordinal);
	}

	[Fact]
	public void ACallThatNamesAFormerParameterNameCompilesWithItsRenames()
	{
		MotionLibrary library = new();
		DocumentBuilder document = new();
		uint literal = document.Graph.Literal("int", "21");
		uint linked = document.Graph.Node("Function.Call", CrossGraphTesting.Call(library.Twice, CrossGraphTesting.Motion));
		document.Graph.Link(literal, "Value", linked, "X");
		uint stored = document.Graph.Node("Function.Call", CrossGraphTesting.Call(library.Twice, CrossGraphTesting.Motion), """{ "X": 5 }""");
		uint create = document.Graph.Node("Event.Create");
		uint first = document.Graph.Write(linked, "Result");
		uint second = document.Graph.Write(stored, "Result");
		document.Graph.Flow(create, first);
		document.Graph.Flow(first, second);

		AnalyzedDocument analysis = CrossGraphTesting.Analyze(document.Build(), library.Bundle());
		VisualScriptTesting.AssertNoErrors(analysis);
		foreach (uint call in (uint[])[linked, stored])
		{
			AnalyzedNode node = VisualScriptTesting.Node(analysis, call);
			Assert.Equal(NodeResolution.Exact, node.Resolution);
			Assert.Equal([new PinRename(PinDirection.In, "X", "Value")], node.PinRenames);
			Diagnostic updated = Assert.Single(analysis.Diagnostics, diagnostic => diagnostic.Node == call);
			Assert.Equal((DiagnosticCode.NodeUpdated, DiagnosticSeverity.Warning, CrossGraphTesting.Motion), (updated.Code, updated.Severity, updated.Graph));
			Assert.Equal("Twice's input X is now Value: the next save stores the new name", updated.Message);
		}
		// An exact node writes its renames without a canonical type.
		string json = Encoding.UTF8.GetString(AnalysisWriter.Write(analysis));
		Assert.Contains(FormattableString.Invariant(
			$"\"ID\":{linked},\"Function\":0,\"Type\":\"Function.Call\",\"Resolved\":\"Exact\",\"PinRenames\":[{{\"Direction\":\"In\",\"From\":\"X\",\"To\":\"Value\"}}],\"Title\":\"Twice\""),
			json, StringComparison.Ordinal);

		TestRuntime runtime = new();
		runtime.LoadWorking(document.Build(), references: library.Bundle());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		runtime.Invoke(Strada.Interop.ScriptEvent.Create);
		Assert.Equal(["42", "10"], runtime.Records);

		// A name that is neither a parameter nor a former name is no pin.
		document = new DocumentBuilder();
		uint unknown = document.Graph.Node("Function.Call", CrossGraphTesting.Call(library.Twice, CrossGraphTesting.Motion));
		document.Graph.Link(document.Graph.Literal("int", "1"), "Value", unknown, "Y");
		Diagnostic pin = VisualScriptTesting.Single(CrossGraphTesting.Analyze(document.Build(), library.Bundle()), DiagnosticCode.UnknownPin);
		Assert.Equal((unknown, "Y"), (pin.Node!.Value, pin.Pin));
		Assert.Empty(VisualScriptTesting.Node(CrossGraphTesting.Analyze(document.Build(), library.Bundle()), unknown).PinRenames);
	}

	// The texts the engine's reader is pinned to (spec-slice3 section 2.3, R6.3): one rule, both sides pinned with the same bytes.
	[Fact]
	public void RemoteFindingsAreWrittenAsTheEnginesReaderReadsThem()
	{
		Diagnostic remote = new(DiagnosticCode.UnknownVariable, 0, "Scripts/Door.sgraph has no variable 9")
		{
			Node = 7,
			Graph = 8146390251172604461,
			Suggestions = ["3", "4"],
		};
		Assert.Equal(
			"""[{"Severity":"Error","Code":"UnknownVariable","Function":0,"Node":7,"Graph":"8146390251172604461","Message":"Scripts/Door.sgraph has no variable 9","Suggestions":["3","4"]}]""",
			Encoding.UTF8.GetString(AnalysisWriter.WriteDiagnostics([remote])));
		Diagnostic full = new(DiagnosticCode.UnknownEvent, 3, "Scripts/Door.sgraph has no event Opn: did you mean Open?")
		{
			Node = 12,
			Variable = 4,
			Setting = "Name",
			Span = (0, 3),
			Graph = 8146390251172604461,
			Suggestions = ["Open"],
		};
		Assert.Equal(
			"""[{"Severity":"Warning","Code":"UnknownEvent","Function":3,"Node":12,"Variable":4,"Setting":"Name","Span":[0,3],"Graph":"8146390251172604461","Message":"Scripts/Door.sgraph has no event Opn: did you mean Open?","Suggestions":["Open"]}]""",
			Encoding.UTF8.GetString(AnalysisWriter.WriteDiagnostics([full])));
	}

	[Fact]
	public void TheBundleIsReadWhateverItHolds()
	{
		Assert.Equal(0, GraphReferences.Parse("not json"u8).Count);
		Assert.Equal(0UL, GraphReferences.Parse("""{ "Self": 7 }"""u8).Self);
		Assert.Equal(7UL, GraphReferences.Parse("""{ "Self": "007" }"""u8).Self);
		GraphReferences references = GraphReferences.Parse(Encoding.UTF8.GetBytes($$"""
			{ "Self": "7", "Graphs": [
				{ "Graph": "7", "Name": "Scripts/Self.sgraph", "Form": "Full", "Document": {} },
				{ "Graph": "8", "Name": "Scripts/A.sgraph", "Form": "Full", "Document": { "Kind": "Library" } },
				{ "Graph": "8", "Name": "Scripts/B.sgraph", "Form": "Full", "Document": {} },
				{ "Graph": "9", "Name": "Scripts/C.sgraph", "Form": "Partial", "Document": {} },
				{ "Graph": "10", "Name": "Scripts/D.sgraph", "Form": "Interface", "Document": [] },
				{ "Graph": "11", "Error": "gone" },
				{ "Graph": 12, "Name": "Scripts/E.sgraph", "Form": "Full", "Document": {} },
				{ "Graph": "13", "Form": "Interface", "Document": { "Kind": "Script" } }
			] }
			"""));
		Assert.Equal(7UL, references.Self);
		Assert.Null(references.Find(7));
		Assert.Equal(("Scripts/A.sgraph", "A", true), (references.Find(8)!.Name, references.Find(8)!.DisplayName, references.Find(8)!.IsLibrary));
		Assert.Null(references.Find(9));
		Assert.Null(references.Find(10)!.Document);
		Assert.NotNull(references.Find(10)!.Error);
		Assert.Equal("gone", references.Find(11)!.Error);
		Assert.Null(references.Find(12));
		Assert.Equal(("visual script 13", "visual script 13"), (references.Find(13)!.Name, references.Find(13)!.DisplayName));
		Assert.Equal(4, references.Count);
	}
}
