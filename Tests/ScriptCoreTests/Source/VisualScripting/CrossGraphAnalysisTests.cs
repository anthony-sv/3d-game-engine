using System;
using System.Linq;
using System.Text;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// What the analysis finds about the other graphs a document's nodes name (spec-slice3 sections 4.1 to 4.8): the Graph
// setting, calls of function libraries, typed sends and broadcasts, variables of other graphs, RunsGraph, the rules of a
// library document, renamed library parameters, and how the findings are written.
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

		// A member name that is not text (an escaped surrogate without its pair) leaves its entry out, or the whole bundle when
		// it is the root's.
		references = GraphReferences.Parse("""
			{ "Self": "7", "Graphs": [ { "\uDC00Graph": "8", "Error": "gone" }, { "Graph": "9", "Name": "Scripts/A.sgraph", "Error": "gone", "\uD800": 1 }, { "Graph": "10", "Error": "gone" } ] }
			"""u8);
		Assert.Equal((7UL, 1, "gone"), (references.Self, references.Count, references.Find(10)!.Error));
		Assert.Equal(0, GraphReferences.Parse("""{ "\uDC00": 1, "Self": "7", "Graphs": [ { "Graph": "10", "Error": "gone" } ] }"""u8).Count);
	}

	// Spec-slice3 section 4.5's table: every finding of a remote Get is an Error about the other graph, located on the node.
	[Fact]
	public void RemoteVariablesAreCheckedAgainstTheTargetsVariables()
	{
		DocumentBuilder door = CrossGraphTesting.DoorWithVariables(out uint health, out uint secret);
		uint broken = door.Variable("Armor", "Game.Hp", isPublic: true);
		string bundle = new BundleBuilder().Interface(CrossGraphTesting.Door, CrossGraphTesting.DoorName, door)
			.Full(CrossGraphTesting.Motion, CrossGraphTesting.MotionName, new MotionLibrary().Document).Build();
		(ulong Graph, uint Variable, DiagnosticCode Code, string Message)[] rows =
		[
			(42, health, DiagnosticCode.UnknownGraph, "Graph: no visual script 42 (deleted, or not a visual script)"),
			(CrossGraphTesting.Motion, health, DiagnosticCode.UnknownVariable, "Scripts/Motion.sgraph is a function library: it has no variables"),
			(CrossGraphTesting.Door, 99, DiagnosticCode.UnknownVariable, "Scripts/Door.sgraph has no variable 99"),
			(CrossGraphTesting.Door, secret, DiagnosticCode.PrivateVariable,
				"Secret is private to Scripts/Door.sgraph: make it public there to use it from other graphs"),
			(CrossGraphTesting.Door, broken, DiagnosticCode.ReferenceError, "Door's Armor has the unknown type Game.Hp: fix Scripts/Door.sgraph"),
		];
		foreach ((ulong graph, uint variable, DiagnosticCode code, string message) in rows)
		{
			foreach (string type in (string[])["Variable.Get", "Variable.Set"])
			{
				DocumentBuilder document = new();
				uint node = document.Graph.Node(type, CrossGraphTesting.Variable(variable, graph));
				// A linked Target leaves only the finding about the variable.
				document.Graph.Link(CrossGraphTesting.EntityOf(document.Graph, 8), "Result", node, "Target");

				AnalyzedDocument analysis = CrossGraphTesting.Analyze(document.Build(), bundle);
				Diagnostic diagnostic = Assert.Single(analysis.Diagnostics);
				Assert.Equal((code, message, DiagnosticSeverity.Error), (diagnostic.Code, diagnostic.Message, diagnostic.Severity));
				Assert.Equal((node, graph, (uint?)null), (diagnostic.Node!.Value, diagnostic.Graph, diagnostic.Variable));
			}
		}

		// The suggestions of an unknown variable are the decimal IDs of the target's variables.
		DocumentBuilder unknown = new();
		unknown.Graph.Node("Variable.Get", CrossGraphTesting.Variable(health + 10, CrossGraphTesting.Door));
		Diagnostic suggested = VisualScriptTesting.Single(CrossGraphTesting.Analyze(unknown.Build(), bundle), DiagnosticCode.UnknownVariable);
		Assert.NotEmpty(suggested.Suggestions);
		Assert.All(suggested.Suggestions, static suggestion => Assert.True(uint.TryParse(suggestion, out _), suggestion));
	}

	[Fact]
	public void RemoteVariablesHaveATargetAndTheTypeOfTheVariable()
	{
		DocumentBuilder door = CrossGraphTesting.DoorWithVariables(out uint health, out _);
		string bundle = new BundleBuilder().Interface(CrossGraphTesting.Door, CrossGraphTesting.DoorName, door).Build();
		DocumentBuilder document = new();
		uint entity = CrossGraphTesting.EntityOf(document.Graph, 8);
		uint get = document.Graph.Node("Variable.Get", CrossGraphTesting.Variable(health, CrossGraphTesting.Door));
		uint set = document.Graph.Node("Variable.Set", CrossGraphTesting.Variable(health, CrossGraphTesting.Door), """{ "Value": 2.5 }""");
		document.Graph.Link(entity, "Result", get, "Target");
		document.Graph.Link(entity, "Result", set, "Target");
		document.Graph.Flow(document.Graph.Node("Event.Create"), set);
		document.Graph.Flow(set, document.Graph.Write(get, "Value"));

		AnalyzedDocument analysis = CrossGraphTesting.Analyze(document.Build(), bundle);
		Assert.Empty(analysis.Diagnostics);
		AnalyzedNode reader = VisualScriptTesting.Node(analysis, get);
		Assert.Equal(("Get Health", "of Door", true), (reader.Title, reader.Subtitle, reader.Pure));
		Assert.Equal(["Target", "Value"], reader.Pins.Select(static pin => pin.Name));
		Assert.Equal("float", VisualScriptTesting.Pin(analysis, get, "Value").Type.Id);
		AnalyzedNode writer = VisualScriptTesting.Node(analysis, set);
		Assert.Equal(("Set Health", "of Door", false), (writer.Title, writer.Subtitle, writer.Pure));
		Assert.Equal(["Execute", "Target", "Value", "Then", "Value"], writer.Pins.Select(static pin => pin.Name));
		Assert.False(ProgramCompiler.Compile(analysis, TestRuntime.Name).IsBroken);

		// An unlinked Target in a script is Self, which runs this script: an Error that says what to link.
		DocumentBuilder unlinked = new();
		uint read = unlinked.Graph.Node("Variable.Get", CrossGraphTesting.Variable(health, CrossGraphTesting.Door));
		uint write = unlinked.Graph.Node("Variable.Set", CrossGraphTesting.Variable(health, CrossGraphTesting.Door));
		AnalyzedDocument findings = CrossGraphTesting.Analyze(unlinked.Build(), bundle, name: TestRuntime.Name);
		Assert.Equal(
			[
				(read, "Target: Self runs Assets/Test.sgraph, not Scripts/Door.sgraph: link the entity to read from"),
				(write, "Target: Self runs Assets/Test.sgraph, not Scripts/Door.sgraph: link the entity to write to"),
			],
			findings.Diagnostics.Select(static diagnostic => (diagnostic.Node!.Value, diagnostic.Message)));
		Assert.All(findings.Diagnostics, static diagnostic =>
			Assert.Equal((DiagnosticCode.UnlinkedTarget, DiagnosticSeverity.Error, CrossGraphTesting.Door), (diagnostic.Code, diagnostic.Severity, diagnostic.Graph)));

		// In a library an unlinked Target is the entity whose execution called the function.
		DocumentBuilder library = new() { Kind = "Library" };
		FunctionBuilder heal = library.Function("Heal");
		heal.Graph.Flow(heal.Entry, heal.Graph.Node("Variable.Set", CrossGraphTesting.Variable(health, CrossGraphTesting.Door), """{ "Value": 10 }"""));
		Assert.Empty(CrossGraphTesting.Analyze(library.Build(), bundle).Diagnostics);
	}

	[Fact]
	public void TheGraphsOwnVariablesAreReadOnOtherEntitiesFromTheAnalyzedDocument()
	{
		// The bundle has no entry for the document itself: a variable that exists only in the analyzed document is found, and a
		// private one may be used.
		DocumentBuilder document = new();
		uint secret = document.Variable("Secret", "int", "3");
		uint get = document.Graph.Node("Variable.Get", CrossGraphTesting.Variable(secret, TestRuntime.Graph));
		uint unknown = document.Graph.Node("Variable.Get", CrossGraphTesting.Variable(secret + 100, TestRuntime.Graph));
		string bundle = new BundleBuilder().Interface(TestRuntime.Graph, "Scripts/Stale.sgraph", "{}").Build();

		AnalyzedDocument analysis = CrossGraphTesting.Analyze(document.Build(), bundle);
		Diagnostic diagnostic = Assert.Single(analysis.Diagnostics);
		Assert.Equal((DiagnosticCode.UnknownVariable, unknown, FormattableString.Invariant($"this graph has no variable {secret + 100}")),
			(diagnostic.Code, diagnostic.Node!.Value, diagnostic.Message));
		Assert.Equal(("Get Secret", "of another instance of this graph"), (VisualScriptTesting.Node(analysis, get).Title, VisualScriptTesting.Node(analysis, get).Subtitle));
		Assert.Equal("int", VisualScriptTesting.Pin(analysis, get, "Value").Type.Id);
		// A load knows the graph's asset path; an unlinked Target reads the instance's own variable, which is no finding.
		AnalyzedDocument loaded = CrossGraphTesting.Analyze(document.Build(), bundle, name: "Scripts/Enemy.sgraph");
		Assert.Equal("of another Enemy", VisualScriptTesting.Node(loaded, get).Subtitle);
		Assert.False(VisualScriptTesting.Has(loaded, DiagnosticCode.UnlinkedTarget));
	}

	// Spec-slice3 section 4.6: a Broadcast with Graph is verified as a typed Send is, with Warnings.
	[Fact]
	public void TypedBroadcastsAreCheckedAgainstTheTargetsCustomEventWithWarnings()
	{
		const string damage = """{ "Name": "Damage", "Type": "float" }""";
		(string Settings, ulong Graph, string Interface, DiagnosticCode Code, string Message)[] rows =
		[
			(CrossGraphTesting.Broadcast("Hit", 42, damage), 42, "", DiagnosticCode.UncheckedGraph,
				"Graph: no visual script 42 to check Broadcast Hit against; it broadcasts Hit by name"),
			(CrossGraphTesting.Broadcast("Hit", CrossGraphTesting.Motion, damage), CrossGraphTesting.Motion, new MotionLibrary().Build(),
				DiagnosticCode.UnknownEvent, "Scripts/Motion.sgraph is a function library: it has no events"),
			(CrossGraphTesting.Broadcast("Hot", CrossGraphTesting.Door, damage), CrossGraphTesting.Door, CrossGraphTesting.DoorScript().Build(),
				DiagnosticCode.UnknownEvent, "Scripts/Door.sgraph has no custom event Hot"),
			(CrossGraphTesting.Broadcast("Hit", CrossGraphTesting.Door, """{ "Name": "Damage", "Type": "int" }"""), CrossGraphTesting.Door,
				CrossGraphTesting.DoorScript().Build(), DiagnosticCode.EventSignatureMismatch, "Door's Hit takes (float Damage); this Broadcast passes (int Damage)"),
			(CrossGraphTesting.Broadcast("Hit", CrossGraphTesting.Door, damage), CrossGraphTesting.Door, CrossGraphTesting.DoorScript("Disabled").Build(),
				DiagnosticCode.DisabledTarget, "Door's Hit is disabled: it receives nothing"),
		];
		foreach ((string settings, ulong graph, string target, DiagnosticCode code, string message) in rows)
		{
			DocumentBuilder document = new();
			uint broadcast = document.Graph.Node("Event.Broadcast", settings);
			BundleBuilder bundle = new();
			if (target.Length > 0)
			{
				bundle.Interface(graph, graph == CrossGraphTesting.Door ? CrossGraphTesting.DoorName : CrossGraphTesting.MotionName, target);
			}
			AnalyzedDocument analysis = CrossGraphTesting.Analyze(document.Build(), bundle.Build());
			Diagnostic diagnostic = Assert.Single(analysis.Diagnostics);
			Assert.Equal((code, message, DiagnosticSeverity.Warning), (diagnostic.Code, diagnostic.Message, diagnostic.Severity));
			Assert.Equal((broadcast, graph), (diagnostic.Node!.Value, diagnostic.Graph));
			Assert.False(ProgramCompiler.Compile(analysis, TestRuntime.Name).IsBroken);
		}
	}

	[Fact]
	public void BroadcastsHaveTheirParametersAndCountAndNoTarget()
	{
		DocumentBuilder document = new();
		uint untyped = document.Graph.Node("Event.Broadcast", CrossGraphTesting.Broadcast("Hit", 0, """{ "Name": "Damage", "Type": "float" }"""));
		uint typed = document.Graph.Node("Event.Broadcast",
			CrossGraphTesting.Broadcast("Hit", CrossGraphTesting.Door, """{ "Name": "Damage", "Type": "float" }""", """, "IncludeSelf": false"""));
		uint own = document.Graph.Node("Event.Broadcast", CrossGraphTesting.Broadcast("Ping", TestRuntime.Graph));
		document.Graph.Node("Event.Custom", """{ "Name": "Ping" }""");
		uint invalid = document.Graph.Node("Event.Broadcast", CrossGraphTesting.Broadcast("Ping", 0, "", """, "IncludeSelf": 1"""));
		string bundle = new BundleBuilder().Interface(CrossGraphTesting.Door, CrossGraphTesting.DoorName, CrossGraphTesting.DoorScript()).Build();

		AnalyzedDocument analysis = CrossGraphTesting.Analyze(document.Build(), bundle);
		Diagnostic diagnostic = Assert.Single(analysis.Diagnostics);
		Assert.Equal((DiagnosticCode.InvalidSetting, invalid, "IncludeSelf: expected true or false, not 1"), (diagnostic.Code, diagnostic.Node!.Value, diagnostic.Message));
		AnalyzedNode node = VisualScriptTesting.Node(analysis, untyped);
		Assert.Equal(("Broadcast Hit", "to every script", false, NodeKind.Call), (node.Title, node.Subtitle, node.Pure, node.Kind));
		Assert.Equal(["Execute", "Damage", "Then", "Count"], node.Pins.Select(static pin => pin.Name));
		Assert.Equal("int", VisualScriptTesting.Pin(analysis, untyped, "Count").Type.Id);
		Assert.True(Assert.IsType<CustomEventBinding>(node.Binding).IncludeSelf);
		Assert.Equal("to every Door", VisualScriptTesting.Node(analysis, typed).Subtitle);
		Assert.False(Assert.IsType<CustomEventBinding>(VisualScriptTesting.Node(analysis, typed).Binding).IncludeSelf);
		Assert.Equal("to every instance of this graph", VisualScriptTesting.Node(analysis, own).Subtitle);
		Assert.Equal("to every Test", VisualScriptTesting.Node(CrossGraphTesting.Analyze(document.Build(), bundle, name: TestRuntime.Name), own).Subtitle);

		// A Broadcast runs other graphs' events, so a pure function holds none.
		DocumentBuilder pure = new();
		FunctionBuilder function = pure.Function("Shout", pure: true, outputs: ["Result:int"]);
		uint shout = function.Graph.Node("Event.Broadcast", CrossGraphTesting.Broadcast("Hit"));
		Diagnostic impure = VisualScriptTesting.Single(CrossGraphTesting.Analyze(pure.Build(), bundle), DiagnosticCode.ImpureInPureFunction);
		Assert.Equal(shout, impure.Node);
	}

	// Spec-slice3 section 4.7.
	[Fact]
	public void RunsGraphNamesAScriptAndWarnsWhenItIsAlwaysFalse()
	{
		string bundle = new BundleBuilder().Interface(CrossGraphTesting.Door, CrossGraphTesting.DoorName, CrossGraphTesting.DoorScript())
			.Full(CrossGraphTesting.Motion, CrossGraphTesting.MotionName, new MotionLibrary().Document).Build();
		(string Settings, DiagnosticCode Code, ulong Graph, string Message)[] rows =
		[
			("{}", DiagnosticCode.MissingSetting, 0, "Runs Graph needs the setting Graph: the decimal handle of a visual script, as asset.list gives it"),
			("""{ "Graph": 7 }""", DiagnosticCode.InvalidSetting, 0,
				"Graph: expected the decimal handle of a visual script, as asset.list gives it; remove the setting to refer to this script"),
			(CrossGraphTesting.Runs(42), DiagnosticCode.UnknownGraph, 42, "Graph: no visual script 42 (deleted, or not a visual script)"),
			(CrossGraphTesting.Runs(CrossGraphTesting.Motion), DiagnosticCode.InvalidSetting, CrossGraphTesting.Motion,
				"Graph: Motion is a function library: no entity runs it"),
		];
		foreach ((string settings, DiagnosticCode code, ulong graph, string message) in rows)
		{
			DocumentBuilder document = new();
			uint runs = document.Graph.Node("RunsGraph", settings);
			document.Graph.Link(CrossGraphTesting.EntityOf(document.Graph, 8), "Result", runs, "Target");
			Diagnostic diagnostic = Assert.Single(CrossGraphTesting.Analyze(document.Build(), bundle).Diagnostics);
			Assert.Equal((code, DiagnosticSeverity.Error, message), (diagnostic.Code, diagnostic.Severity, diagnostic.Message));
			Assert.Equal((runs, graph), (diagnostic.Node!.Value, diagnostic.Graph));
		}

		DocumentBuilder script = new();
		uint door = script.Graph.Node("RunsGraph", CrossGraphTesting.Runs(CrossGraphTesting.Door));
		uint own = script.Graph.Node("RunsGraph", CrossGraphTesting.Runs(TestRuntime.Graph));
		AnalyzedDocument analysis = CrossGraphTesting.Analyze(script.Build(), bundle, name: TestRuntime.Name);
		Diagnostic warning = Assert.Single(analysis.Diagnostics);
		Assert.Equal((DiagnosticCode.TargetIsSelf, DiagnosticSeverity.Warning, door, CrossGraphTesting.Door),
			(warning.Code, warning.Severity, warning.Node!.Value, warning.Graph));
		Assert.Equal("Target: Self runs Assets/Test.sgraph, so this is always false: link the entity to test", warning.Message);
		AnalyzedNode node = VisualScriptTesting.Node(analysis, door);
		Assert.Equal(("Runs Door", true, NodeKind.Data), (node.Title, node.Pure, node.Kind));
		Assert.Equal(["Target", "Result"], node.Pins.Select(static pin => pin.Name));
		Assert.Equal("Runs Test", VisualScriptTesting.Node(analysis, own).Title);
		Assert.False(ProgramCompiler.Compile(analysis, TestRuntime.Name).IsBroken);

		// In a library an unlinked Target is the calling entity: no Warning.
		DocumentBuilder library = new() { Kind = "Library" };
		FunctionBuilder check = library.Function("Check", pure: true, outputs: ["Result:bool"]);
		uint result = check.Graph.Node("Function.Return");
		check.Graph.Link(check.Graph.Node("RunsGraph", CrossGraphTesting.Runs(CrossGraphTesting.Door)), "Result", result, "Result");
		Assert.Empty(CrossGraphTesting.Analyze(library.Build(), bundle).Diagnostics);
	}

	// Spec-slice3 section 4.8: a remote UnknownVariable as the analysis writes it, with its graph and its ID suggestions and
	// no variable.
	[Fact]
	public void ARemoteUnknownVariableIsWrittenWithItsGraphAndIdSuggestions()
	{
		DocumentBuilder door = new();
		door.Variable("Health", "float", isPublic: true);
		door.Variable("Speed", "float", isPublic: true);
		DocumentBuilder document = new();
		document.Graph.Node("Variable.Get", CrossGraphTesting.Variable(3, CrossGraphTesting.Door));
		AnalyzedDocument analysis = CrossGraphTesting.Analyze(document.Build(),
			new BundleBuilder().Interface(CrossGraphTesting.Door, CrossGraphTesting.DoorName, door).Build());
		Assert.Equal(
			FormattableString.Invariant(
				$$"""[{"Severity":"Error","Code":"UnknownVariable","Function":0,"Node":1,"Graph":"{{CrossGraphTesting.Door}}","Message":"Scripts/Door.sgraph has no variable 3","Suggestions":["1","2"]}]"""),
			Encoding.UTF8.GetString(AnalysisWriter.WriteDiagnostics(analysis.Diagnostics)));
	}
}
