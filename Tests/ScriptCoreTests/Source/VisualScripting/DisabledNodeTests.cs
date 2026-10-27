using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Nodes whose Enabled is "Disabled" compile as pass-throughs: the flow entering any exec input continues at Then, else
// Completed, else Default, else the first exec output that is not a Tick output; each data output takes the first data
// input of its type that no earlier output takes; disabled events start nothing and disabled latent nodes never wait.
// Function.Entry, Function.Return and pure nodes cannot be disabled (DisabledIgnored). A disabled node never makes a program
// broken: what is wrong with it is a Warning. "DevelopmentOnly" runs as "Enabled" (the engine disables it where it must
// not run), and a live edit that disables or enables a node removes it and adds a new one.
public sealed class DisabledNodeTests
{
	private const string Disabled = "Disabled";

	// type, settings, inputs, the exec input the flow enters, the expected pass-through exit and the node's other exits.
	public static TheoryData<string, string?, string?, string, string, string[]> PassThroughs => new()
	{
		{ RuntimeTesting.Write, null, """{ "Text": "skipped" }""", "Execute", "Then", [] },
		{ "Flow.Branch", null, """{ "Condition": false }""", "Execute", "True", ["False"] },
		{ "Flow.Sequence", """{ "Count": 3 }""", null, "Execute", "Then0", ["Then1", "Then2"] },
		{ "Flow.Switch", """{ "Type": "int", "Cases": [1, 2] }""", """{ "Selection": 1 }""", "Execute", "Default", ["Case:1", "Case:2"] },
		{ "Flow.ForLoop", null, """{ "First": 1, "Last": 3 }""", "Execute", "Completed", ["Body"] },
		{ "Flow.ForLoop", null, """{ "First": 1, "Last": 3 }""", "Break", "Completed", ["Body"] },
		{ "Flow.ForEach", null, null, "Execute", "Completed", ["Body"] },
		{ "Flow.While", null, """{ "Condition": true }""", "Execute", "Completed", ["Body"] },
		{ "Flow.DoOnce", """{ "StartClosed": true }""", null, "Execute", "Completed", [] },
		{ "Flow.DoN", null, """{ "N": 0 }""", "Reset", "Exit", [] },
		{ "Flow.FlipFlop", null, null, "Execute", "A", ["B"] },
		{ "Flow.Gate", """{ "StartOpen": false }""", null, "Enter", "Exit", [] },
		{ "Flow.Gate", null, null, "Close", "Exit", [] },
		{ "Flow.Delay", null, """{ "Duration": 0.25 }""", "Execute", "Completed", [] },
		{ "Flow.RetriggerableDelay", null, null, "Execute", "Completed", [] },
		{ "Flow.WaitFrames", null, """{ "Frames": 3 }""", "Execute", "Completed", [] },
		{ "Flow.WaitUntil", null, """{ "Condition": false }""", "Execute", "Completed", [] },
		{ "Flow.Tween", null, """{ "Duration": 1.0 }""", "Execute", "Completed", ["Update"] },
		{ "Flow.Tween", null, null, "Stop", "Completed", ["Update"] },
		{ "Flow.Interval", null, """{ "Interval": 0.1 }""", "Start", "Completed", ["Tick"] },
		{ "Flow.Interval", null, null, "Stop", "Completed", ["Tick"] },
		{ "Event.Send", """{ "Name": "Hit" }""", null, "Execute", "Then", [] },
	};

	[Theory]
	[MemberData(nameof(PassThroughs))]
	public void TheFlowContinuesAtThePassThroughExitWithoutRunningTheNode(string type, string? settings, string? inputs, string entry, string exit,
		string[] others)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint node = graph.Node(type, settings, inputs, Disabled);
		graph.Flow(graph.Node("Event.Create"), node, "Then", entry);
		graph.Writes(node, exit, "passed");
		uint other = graph.Write("another exit");
		foreach (string output in others)
		{
			graph.Flow(node, other, output);
		}
		string built = document.Build();
		TestRuntime runtime = TestRuntime.Start(built);

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(["passed"], runtime.Records);
		// Nothing waits in it, ticks or resumes later.
		Assert.Empty(runtime.Instance().Fibers);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Equal(["passed"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
		Assert.Equal(exit, PassThroughOf(built, node));
	}

	[Fact]
	public void PassingThroughCostsOneStep()
	{
		// Each iteration: the disabled node, the Set, its Get and Add, and the loop's next iteration; 2 more for the event
		// and the loop. 199,999 iterations take 999,997 steps; one more exceeds the 1,000,000 an execution may take.
		static TestRuntime Counting(int count)
		{
			DocumentBuilder document = new();
			uint counter = document.Variable("Counter", "int", "0");
			GraphBuilder graph = document.Graph;
			uint loop = graph.Node("Flow.ForLoop", inputs: FormattableString.Invariant($$"""{ "First": 1, "Last": {{count}} }"""));
			uint skipped = graph.Node(RuntimeTesting.Write, inputs: """{ "Text": "skipped" }""", enabled: Disabled);
			uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
			uint set = graph.Set(counter);
			graph.Link(graph.Get(counter), "Value", add, "A");
			graph.Link(add, "Result", set, "Value");
			graph.Flow(graph.Node("Event.Create"), loop);
			graph.Flow(loop, skipped, "Body");
			graph.Flow(skipped, set);
			return TestRuntime.Start(document);
		}

		TestRuntime fits = Counting(199_999);
		Assert.Equal(VisualScriptStatus.Success, fits.Invoke(ScriptEvent.Create));
		Assert.Equal(199_999, fits.Variable("Counter"));
		TestRuntime exceeds = Counting(200_000);
		Assert.Equal(VisualScriptStatus.Failure, exceeds.Invoke(ScriptEvent.Create));
		Assert.Equal(199_999, exceeds.Variable("Counter"));
		Assert.Empty(fits.Records);
	}

	// Passing the flow and values through a disabled node allocates nothing.
	[Fact]
	public void PassingThroughAllocatesNothing()
	{
		DocumentBuilder document = new();
		uint total = document.Variable("Total", "Strada.Vector3");
		GraphBuilder graph = document.Graph;
		uint set = graph.Node("Variable.Set", FormattableString.Invariant($"{{ \"Variable\": {total} }}"), """{ "Value": [1.0, 2.0, 3.0] }""", Disabled);
		uint delay = graph.Node("Flow.Delay", enabled: Disabled);
		uint copy = graph.Set(total);
		graph.Link(set, "Value", copy, "Value");
		graph.Flow(graph.Node("Event.Update"), set);
		graph.Flow(set, delay);
		graph.Flow(delay, copy, "Completed");
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.Equal(0, runtime.FewestBytesPerUpdate());
		Assert.Equal(new Vector3(1.0f, 2.0f, 3.0f), runtime.Variable("Total"));
	}

	// A disabled node of a type the library does not have: its pins come from its links, typed by nothing, so it has no
	// exec output and its chain ends there; the analysis names no exit. Links into it and from it are Warnings.
	[Fact]
	public void TheChainEndsAtADisabledNodeWithoutAnExit()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint before = graph.Write("before");
		uint unknown = graph.Node("Flow.Nothing", enabled: Disabled);
		graph.Flow(graph.Node("Event.Create"), before);
		graph.Flow(before, unknown);
		graph.Writes(unknown, "Then", "after");
		uint update = graph.Node("Event.Update", enabled: Disabled);
		graph.Writes(update, "Then", "updated");
		string built = document.Build();

		AnalyzedDocument analyzed = VisualScriptTesting.Analyze(RuntimeTesting.Library, built);
		VisualScriptTesting.AssertNoErrors(analyzed);
		Assert.Equal(DiagnosticSeverity.Warning, VisualScriptTesting.Single(analyzed, DiagnosticCode.UnknownNodeType).Severity);
		Assert.Equal(2, analyzed.Diagnostics.Count(static diagnostic => diagnostic.Code == DiagnosticCode.TypeMismatch));
		Assert.Null(PassThroughOf(built, unknown));
		// A disabled event has no exec input: no flow enters it.
		Assert.Null(PassThroughOf(built, update));
		// Enabled nodes have none.
		Assert.Null(PassThroughOf(built, before));

		TestRuntime runtime = TestRuntime.Start(built);
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update));
		Assert.Equal(["before"], runtime.Records);

		// Enabled, the same node breaks the program.
		Assert.Equal(DiagnosticSeverity.Error,
			VisualScriptTesting.Single(VisualScriptTesting.Analyze(RuntimeTesting.Library, Enable(built)), DiagnosticCode.UnknownNodeType).Severity);
	}

	[Fact]
	public void AnExecRerouteContinuesAtItsOutput()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint reroute = graph.Node("Reroute", enabled: Disabled);
		graph.Flow(graph.Node("Event.Create"), reroute, "Then", "In");
		graph.Writes(reroute, "Out", "passed");
		string built = document.Build();
		TestRuntime runtime = TestRuntime.Start(built);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["passed"], runtime.Records);
		Assert.Equal("Out", PassThroughOf(built, reroute));
	}

	// Each data output takes the value of the first data input of its type that no earlier output takes, read when the flow
	// passes; only those inputs are evaluated.
	[Fact]
	public void DataOutputsTakeTheInputsOfTheirTypes()
	{
		DocumentBuilder document = new();
		uint score = document.Variable("Score", "int", "0");
		FunctionBuilder function = document.Function("Combine", inputs: ["A:int", "Text:string", "B:int"], outputs: ["X:int", "Y:int", "Z:int", "S:string"]);
		function.Graph.Writes(function.Entry, "Then", "in the function");
		GraphBuilder graph = document.Graph;
		// A disabled Variable.Set passes its Value on and sets nothing.
		uint set = graph.Node("Variable.Set", FormattableString.Invariant($"{{ \"Variable\": {score} }}"), enabled: Disabled);
		graph.Link(graph.Node(RuntimeTesting.Next), "Result", set, "Value");
		uint call = graph.Node("Function.Call", function.Settings, """{ "A": 5, "Text": "text" }""", Disabled);
		graph.Link(graph.Node(RuntimeTesting.Next), "Result", call, "B");
		uint format = graph.Node("FormatString", """{ "Format": "{X} {Y} {Z} {S}" }""");
		foreach (string output in (string[])["X", "Y", "Z", "S"])
		{
			graph.Link(call, output, format, output);
		}
		// Index takes First; Element and Index of a ForEach take their type defaults (it has no input of their types), and the
		// array it would iterate is not evaluated, nor is the text of a disabled Write.
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 3, "Last": 9 }""", enabled: Disabled);
		uint each = graph.Node("Flow.ForEach", enabled: Disabled);
		graph.Link(graph.Node("Call:Strada.ScriptCore.Tests.Probe.Entities(int)", inputs: """{ "Count": 2 }"""), "Result", each, "Array");
		uint silent = graph.Node(RuntimeTesting.Write, enabled: Disabled);
		graph.Link(graph.Node(RuntimeTesting.Next), "Result", silent, "Text");

		uint create = graph.Node("Event.Create");
		graph.Flow(create, set);
		uint writeValue = graph.Write(set, "Value");
		graph.Flow(set, writeValue);
		graph.Flow(writeValue, call);
		uint writeCall = graph.Write(format, "Result");
		graph.Flow(call, writeCall);
		graph.Flow(writeCall, loop);
		uint writeIndex = graph.Write(loop, "Index");
		graph.Flow(loop, writeIndex, "Completed");
		graph.Flow(writeIndex, each);
		uint writeElement = graph.Write(each, "Element");
		graph.Flow(each, writeElement, "Completed");
		uint writeEachIndex = graph.Write(each, "Index");
		graph.Flow(writeElement, writeEachIndex);
		graph.Flow(writeEachIndex, silent);
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(["1", "5 2 0 text", "3", "None", "0"], runtime.Records);
		Assert.Equal(0, runtime.Variable("Score"));
		// The two Next nodes the outputs take were evaluated; the one feeding the disabled Write was not.
		Assert.Equal(2, Probe.Evaluations);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void DisabledEventsStartNoExecution()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		graph.Writes(graph.Node("Event.Create", enabled: Disabled), "Then", "create");
		// A disabled event handles nothing, so an enabled one of the same kind is no duplicate, before it or after it.
		graph.Writes(graph.Node("Event.Update", enabled: Disabled), "Then", "disabled update");
		graph.Writes(graph.Node("Event.Update"), "Then", "update");
		graph.Writes(graph.Node("Event.FixedUpdate"), "Then", "fixed update");
		graph.Writes(graph.Node("Event.FixedUpdate", enabled: Disabled), "Then", "disabled fixed update");
		graph.Writes(graph.Node("Event.KeyPressed", """{ "Key": "Space" }""", enabled: Disabled), "Then", "space");
		graph.Writes(graph.Node("Event.Custom", """{ "Name": "Hit" }""", enabled: Disabled), "Then", "hit");
		graph.Writes(graph.Node("Event.Custom", """{ "Name": "Heal" }""", enabled: Disabled), "Then", "disabled heal");
		graph.Writes(graph.Node("Event.Custom", """{ "Name": "Heal" }"""), "Then", "heal");
		string built = document.Build();
		Assert.Empty(VisualScriptTesting.Analyze(RuntimeTesting.Library, built).Diagnostics);
		TestRuntime runtime = TestRuntime.Start(built);

		runtime.Environment.PressedKeys.Add(KeyCode.Space);
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update));
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.FixedUpdate));
		Assert.False(runtime.Runtime.SendEvent(TestRuntime.Hero, "Hit", []));
		Assert.True(runtime.Runtime.SendEvent(TestRuntime.Hero, "Heal", []));
		Assert.Equal(["update", "fixed update", "heal"], runtime.Records);
		Assert.Equal([$"{TestRuntime.Name} on Hero ({TestRuntime.Hero}): SendEvent Hit: the graph has no custom event Hit"], runtime.Environment.Warnings);
	}

	// A disabled latent node continues at once, so it makes no function latent.
	[Fact]
	public void ADisabledLatentNodeNeverWaitsAndMakesNoFunctionLatent()
	{
		DocumentBuilder document = new();
		FunctionBuilder function = document.Function("Pause");
		uint delay = function.Graph.Node("Flow.Delay", inputs: """{ "Duration": 0.5 }""", enabled: Disabled);
		function.Graph.Flow(function.Entry, delay);
		function.Graph.Writes(delay, "Completed", "in the function");
		GraphBuilder graph = document.Graph;
		uint call = graph.Node("Function.Call", function.Settings);
		graph.Flow(graph.Node("Event.Create"), call);
		graph.Writes(call, "Then", "after the call");
		string built = document.Build();
		Assert.False(VisualScriptTesting.Node(VisualScriptTesting.Analyze(RuntimeTesting.Library, built), call).Latent);
		TestRuntime runtime = TestRuntime.Start(built);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["in the function", "after the call"], runtime.Records);
		Assert.Empty(runtime.Instance().Fibers);
		Assert.True(VisualScriptTesting.Node(VisualScriptTesting.Analyze(RuntimeTesting.Library, Enable(built)), call).Latent);
	}

	[Fact]
	public void FunctionEntryFunctionReturnAndPureNodesCannotBeDisabledAndRun()
	{
		DocumentBuilder document = new();
		uint count = document.Variable("Count", "int", "41");
		FunctionBuilder pure = document.Function("AddOne", pure: true, inputs: ["A:int"], outputs: ["Result:int"]);
		uint add = pure.Graph.Node("Operator.Add", inputs: """{ "B": 1 }""", enabled: Disabled);
		uint pureReturn = pure.Graph.Node("Function.Return", enabled: Disabled);
		pure.Graph.Link(pure.Entry, "A", add, "A");
		pure.Graph.Link(add, "Result", pureReturn, "Result");
		FunctionBuilder impure = document.Function("Greet");
		uint impureReturn = impure.Graph.Node("Function.Return", enabled: Disabled);
		uint greet = impure.Graph.Write("in the function");
		impure.Graph.Flow(impure.Entry, greet);
		impure.Graph.Flow(greet, impureReturn);
		GraphBuilder graph = document.Graph;
		uint get = graph.Node("Variable.Get", FormattableString.Invariant($"{{ \"Variable\": {count} }}"), enabled: Disabled);
		uint addOne = graph.Node("Function.Call", pure.Settings, enabled: Disabled);
		graph.Link(get, "Value", addOne, "A");
		uint call = graph.Node("Function.Call", impure.Settings);
		graph.Flow(graph.Node("Event.Create"), call);
		graph.Flow(call, graph.Write(addOne, "Result"));
		string built = document.Build()
			.Replace(FormattableString.Invariant($$"""{ "ID": {{pure.Entry}}, "Type": "Function.Entry" }"""),
				FormattableString.Invariant($$"""{ "ID": {{pure.Entry}}, "Type": "Function.Entry", "Enabled": "Disabled" }"""), StringComparison.Ordinal)
			.Replace(FormattableString.Invariant($$"""{ "ID": {{impure.Entry}}, "Type": "Function.Entry" }"""),
				FormattableString.Invariant($$"""{ "ID": {{impure.Entry}}, "Type": "Function.Entry", "Enabled": "Disabled" }"""), StringComparison.Ordinal);

		AnalyzedDocument analyzed = VisualScriptTesting.Analyze(RuntimeTesting.Library, built);
		List<Diagnostic> ignored = [.. analyzed.Diagnostics.Where(static diagnostic => diagnostic.Code == DiagnosticCode.DisabledIgnored)];
		Assert.Equal(analyzed.Diagnostics.Count, ignored.Count);
		Assert.Equal(new uint[] { pure.Entry, add, pureReturn, impure.Entry, impureReturn, get, addOne }.Order(), ignored.Select(static diagnostic => diagnostic.Node!.Value).Order());
		Assert.All(ignored, static diagnostic => Assert.Equal(DiagnosticSeverity.Warning, diagnostic.Severity));
		Assert.All(analyzed.Graphs.SelectMany(static graph => graph.Nodes), static node => Assert.False(node.IsDisabled));
		Assert.Contains("is pure", ignored.Single(diagnostic => diagnostic.Node == add).Message, StringComparison.Ordinal);
		Assert.Contains("cannot be disabled", ignored.Single(diagnostic => diagnostic.Node == pureReturn).Message, StringComparison.Ordinal);
		TestRuntime runtime = TestRuntime.Start(built);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["in the function", "42"], runtime.Records);
	}

	// What is wrong with a disabled node, or with a link that ends at one, is a Warning: the program runs, and the flow passes
	// the node. Enabled, the same document is broken.
	[Fact]
	public void ErrorsOnDisabledNodesAreWarningsAndTheProgramRuns()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint first = graph.Write("first");
		uint sequence = graph.Node("Flow.Sequence", """{ "Count": 1 }""", enabled: Disabled);
		uint set = graph.Node("Variable.Set", """{ "Variable": 99 }""", enabled: Disabled);
		uint branch = graph.Node("Flow.Branch", enabled: Disabled);
		graph.Link(graph.Node("Self"), "Self", branch, "Condition");
		uint unknown = graph.Node("Flow.Nothing", enabled: Disabled);
		uint untyped = graph.Write(unknown, "Value");
		graph.Flow(graph.Node("Event.Create"), first);
		graph.Flow(first, sequence);
		graph.Flow(sequence, set, "Then0");
		graph.Flow(set, branch);
		graph.Flow(branch, untyped, "True");
		graph.Writes(untyped, "Then", "last");
		string built = document.Build();

		AnalyzedDocument analyzed = VisualScriptTesting.Analyze(RuntimeTesting.Library, built);
		Dictionary<DiagnosticCode, Diagnostic> found = analyzed.Diagnostics.ToDictionary(static diagnostic => diagnostic.Code);
		Assert.Equal(new[] { DiagnosticCode.TypeMismatch, DiagnosticCode.UnresolvedWildcard, DiagnosticCode.InvalidSetting, DiagnosticCode.UnknownNodeType,
			DiagnosticCode.UnknownVariable }.Order(), found.Keys.Order());
		Assert.All(analyzed.Diagnostics, static diagnostic => Assert.Equal(DiagnosticSeverity.Warning, diagnostic.Severity));
		Assert.Equal(sequence, found[DiagnosticCode.InvalidSetting].Node);
		Assert.Equal(set, found[DiagnosticCode.UnknownVariable].Node);
		Assert.Equal(unknown, found[DiagnosticCode.UnknownNodeType].Node);
		Assert.Equal(branch, found[DiagnosticCode.TypeMismatch].Link!.To.Node);
		// The output of the unknown node has no type: the Write takes no value from it.
		Assert.Equal((unknown, untyped), (found[DiagnosticCode.UnresolvedWildcard].Link!.From.Node, found[DiagnosticCode.UnresolvedWildcard].Link!.To.Node));
		Assert.Contains("is disabled", found[DiagnosticCode.UnresolvedWildcard].Message, StringComparison.Ordinal);
		// The diagnostics JSON says Warning too.
		JsonElement diagnostics = VisualScriptTesting.ParseObject(AnalysisWriter.Write(analyzed)).GetProperty("Diagnostics");
		Assert.All(diagnostics.EnumerateArray(), static diagnostic => Assert.Equal("Warning", diagnostic.GetProperty("Severity").GetString()));

		TestRuntime runtime = new();
		IReadOnlyList<Diagnostic> loaded = runtime.Load(built);
		Assert.Equal(analyzed.Diagnostics.Select(static diagnostic => (diagnostic.Code, diagnostic.Severity)), loaded.Select(static diagnostic => (diagnostic.Code, diagnostic.Severity)));
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(["first", "", "last"], runtime.Records);

		AnalyzedDocument enabled = VisualScriptTesting.Analyze(RuntimeTesting.Library, Enable(built));
		Assert.Equal(new[] { DiagnosticCode.TypeMismatch, DiagnosticCode.InvalidSetting, DiagnosticCode.UnknownNodeType, DiagnosticCode.UnknownVariable }.Order(),
			VisualScriptTesting.Errors(enabled).Select(static diagnostic => diagnostic.Code).Distinct().Order());
	}

	// A wildcard input of an enabled node takes no type from an output of a disabled node that it refuses (an int into For
	// Each's Array, Length's Array or Is Valid's Value): the link is a Warning, and the input is unconnected with no type, an
	// Error on the enabled node. Typed by the refused link, it would run on a value it cannot take. Enabled, the link is the
	// Error, and the input keeps the type it gives.
	[Theory]
	[InlineData("Flow.ForEach", "Array")]
	[InlineData("Array.Length", "Array")]
	[InlineData("IsValid", "Value")]
	public void AWildcardInputTakesNoTypeFromADisabledNodesOutputItRefuses(string type, string input)
	{
		DocumentBuilder document = new();
		uint count = document.Variable("Count", "int");
		GraphBuilder graph = document.Graph;
		uint set = graph.Node("Variable.Set", FormattableString.Invariant($"{{ \"Variable\": {count} }}"), """{ "Value": 3 }""", Disabled);
		uint reader = graph.Node(type);
		graph.Link(set, "Value", reader, input);
		graph.Flow(graph.Node("Event.Create"), set);
		string built = document.Build();

		AnalyzedDocument analyzed = VisualScriptTesting.Analyze(RuntimeTesting.Library, built);
		Assert.Equal([(DiagnosticCode.TypeMismatch, DiagnosticSeverity.Warning), (DiagnosticCode.UnresolvedWildcard, DiagnosticSeverity.Error)],
			analyzed.Diagnostics.Select(static diagnostic => (diagnostic.Code, diagnostic.Severity)));
		Assert.Equal((set, reader), (analyzed.Diagnostics[0].Link!.From.Node, analyzed.Diagnostics[0].Link!.To.Node));
		Diagnostic unresolved = analyzed.Diagnostics[1];
		Assert.Equal((reader, input, PinDirection.In), (unresolved.Node!.Value, unresolved.Pin!, unresolved.Direction!.Value));
		Assert.Equal($"the type of {input} is unknown: none of its links gives it a value", unresolved.Message);
		AnalyzedPin pin = VisualScriptTesting.Pin(analyzed, reader, input, PinDirection.In);
		Assert.False(pin.IsConnected);
		Assert.False(pin.Type.HasValues);
		Assert.Equal(DiagnosticCode.UnresolvedWildcard, Assert.Single(new TestRuntime().Load(built), static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error).Code);

		AnalyzedDocument enabled = VisualScriptTesting.Analyze(RuntimeTesting.Library, Enable(built));
		Assert.Equal(DiagnosticCode.TypeMismatch, Assert.Single(VisualScriptTesting.Errors(enabled)).Code);
		Assert.Equal("int", VisualScriptTesting.Pin(enabled, reader, input, PinDirection.In).Type.Id);
	}

	[Fact]
	public void DevelopmentOnlyNodesRunAsEnabledNodes()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint development = graph.Node(RuntimeTesting.Write, inputs: """{ "Text": "development" }""", enabled: "DevelopmentOnly");
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.25 }""", enabled: "DevelopmentOnly");
		uint pure = graph.Node(RuntimeTesting.Next, enabled: "DevelopmentOnly");
		graph.Flow(graph.Node("Event.Create", enabled: "DevelopmentOnly"), development);
		graph.Flow(development, delay);
		graph.Flow(delay, graph.Write(pure, "Result"), "Completed");
		string built = document.Build();
		Assert.Empty(VisualScriptTesting.Analyze(RuntimeTesting.Library, built).Diagnostics);
		Assert.Null(PassThroughOf(built, development));
		TestRuntime runtime = TestRuntime.Start(built);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Single(runtime.Instance().Fibers);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["development", "1"], runtime.Records);
	}

	// Enabled is read on every node; a value that is no state is an Error InvalidDocument naming its path, and the node stays
	// enabled. Node comments and collapsed states are not read, so not even malformed ones are reported.
	[Fact]
	public void EnabledIsReadAndCommentsAndCollapsedStatesAreNot()
	{
		foreach (string state in (string[])["\"Off\"", "\"disabled\"", "false", "0", "null", "[\"Disabled\"]"])
		{
			AnalyzedDocument analyzed = VisualScriptTesting.Analyze(VisualScriptTesting.Document($$"""{ "ID": 1, "Type": "Flow.Branch", "Enabled": {{state}} }"""));
			Diagnostic diagnostic = Assert.Single(analyzed.Diagnostics);
			Assert.Equal(DiagnosticCode.InvalidDocument, diagnostic.Code);
			Assert.Equal(DiagnosticSeverity.Error, diagnostic.Severity);
			Assert.StartsWith("Graph.Nodes[0].Enabled: ", diagnostic.Message, StringComparison.Ordinal);
			Assert.False(VisualScriptTesting.Node(analyzed, 1).IsDisabled);
		}
		foreach (string keys in (string[])["\"Comment\": \"Tuned by hand\", \"Collapsed\": true", "\"Comment\": 5, \"Collapsed\": \"yes\"", "\"Comment\": null, \"Collapsed\": []"])
		{
			Assert.Empty(VisualScriptTesting.Analyze(VisualScriptTesting.Document($$"""{ "ID": 1, "Type": "Flow.Branch", {{keys}} }""")).Diagnostics);
		}
		Assert.True(VisualScriptTesting.Node(VisualScriptTesting.Analyze(VisualScriptTesting.Document("""{ "ID": 1, "Type": "Flow.Branch", "Enabled": "Disabled" }""")), 1).IsDisabled);
	}

	// A live edit that disables a node removes it: a fiber waiting in it is cancelled with one log line, and nothing of it
	// resumes later.
	[Fact]
	public void DisablingAWaitingDelayCancelsItsFiber()
	{
		(string Enabled, string Disabled, uint Delay) documents = Documents(static (graph, state) =>
		{
			uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.25 }""", enabled: state);
			graph.Flow(graph.Node("Event.Create"), delay);
			graph.Writes(delay, "Completed", "resumed");
			return delay;
		});
		TestRuntime runtime = TestRuntime.Start(documents.Enabled);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Single(runtime.Instance().Fibers);

		runtime.LoadWorking(documents.Disabled);
		Assert.Empty(runtime.Instance().Fibers);
		Assert.Equal([Cancelled(documents.Delay, "Delay", FormattableString.Invariant($"the edit disabled Delay (node {documents.Delay})"))],
			runtime.Environment.Warnings);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Empty(runtime.Records);
		// Now it passes the flow on.
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["resumed"], runtime.Records);
	}

	[Fact]
	public void DisablingARunningTweenStopsItWithoutCompleted()
	{
		(string Enabled, string Disabled, uint Tween) documents = Documents(static (graph, state) =>
		{
			uint tween = graph.Node("Flow.Tween", inputs: """{ "Duration": 1.0 }""", enabled: state);
			graph.Flow(graph.Node("Event.Create"), tween);
			graph.Flow(tween, graph.Write(tween, "Alpha"), "Update");
			graph.Writes(tween, "Completed", "completed");
			return tween;
		});
		TestRuntime runtime = TestRuntime.Start(documents.Enabled);
		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["0.25"], runtime.Records);

		runtime.LoadWorking(documents.Disabled);
		Assert.Single(runtime.Environment.Warnings);
		for (int update = 0; update < 6; update++)
		{
			runtime.Invoke(ScriptEvent.Update, 0.25f);
		}
		Assert.Equal(["0.25"], runtime.Records);
		Assert.Empty(runtime.Instance().Fibers);
	}

	// A fiber holding the node in its flow stack is cancelled too.
	[Fact]
	public void DisablingANodeAFiberWillContinueAtCancelsTheFiber()
	{
		(string Enabled, string Disabled, uint Sequence) documents = Documents(static (graph, state) =>
		{
			uint sequence = graph.Node("Flow.Sequence", enabled: state);
			uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.25 }""");
			graph.Flow(graph.Node("Event.Create"), sequence);
			graph.Flow(sequence, delay, "Then0");
			graph.Writes(delay, "Completed", "then 0");
			graph.Writes(sequence, "Then1", "then 1");
			return sequence;
		});
		TestRuntime runtime = TestRuntime.Start(documents.Enabled);
		runtime.Invoke(ScriptEvent.Create);
		Execution fiber = Assert.Single(runtime.Instance().Fibers);
		uint delay = fiber.Current!.Id;

		runtime.LoadWorking(documents.Disabled);
		Assert.Equal([Cancelled(delay, "Delay", FormattableString.Invariant($"the edit disabled Sequence (node {documents.Sequence})"))],
			runtime.Environment.Warnings);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Empty(runtime.Records);
	}

	// Disabling a node and enabling it again gives it fresh state; switching between Enabled and DevelopmentOnly changes
	// nothing for the VM.
	[Fact]
	public void TogglingEnabledResetsTheNodesStateAndDevelopmentOnlyKeepsIt()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint once = graph.Node("Flow.DoOnce");
		graph.Flow(graph.Node("Event.Update"), once);
		graph.Writes(once, "Completed", "once");
		string enabled = document.Build();
		string Set(string state) => enabled.Replace(FormattableString.Invariant($$"""{ "ID": {{once}}, "Type": "Flow.DoOnce" }"""),
			FormattableString.Invariant($$"""{ "ID": {{once}}, "Type": "Flow.DoOnce", "Enabled": "{{state}}" }"""), StringComparison.Ordinal);
		TestRuntime runtime = TestRuntime.Start(enabled);
		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal(["once"], runtime.Records);

		runtime.LoadWorking(Set("DevelopmentOnly"));
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal(["once"], runtime.Records);

		runtime.LoadWorking(Set(Disabled));
		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal(["once", "once", "once"], runtime.Records);

		runtime.LoadWorking(enabled);
		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal(["once", "once", "once", "once"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	// The analysis JSON's PassThrough of a node: its exit, or null when the member is absent.
	private static string? PassThroughOf(string document, uint node)
	{
		JsonElement root = VisualScriptTesting.ParseObject(AnalysisWriter.Write(VisualScriptTesting.Analyze(RuntimeTesting.Library, document)));
		JsonElement analyzed = root.GetProperty("Nodes").EnumerateArray().Single(element => element.GetProperty("ID").GetUInt32() == node);
		return analyzed.TryGetProperty("PassThrough", out JsonElement exit) ? exit.GetString() : null;
	}

	// The document with every node enabled.
	private static string Enable(string document) => document.Replace(", \"Enabled\": \"Disabled\"", "", StringComparison.Ordinal);

	// A document build makes, and the same document with the node it gives the Enabled state disabled (IDs are given in
	// order, so both builds give the same ones).
	private static (string Enabled, string Disabled, uint Node) Documents(Func<GraphBuilder, string?, uint> build)
	{
		DocumentBuilder enabled = new();
		uint node = build(enabled.Graph, null);
		DocumentBuilder disabled = new();
		Assert.Equal(node, build(disabled.Graph, Disabled));
		return (enabled.Build(), disabled.Build(), node);
	}

	private static string Cancelled(uint latent, string latentTitle, string problem) =>
		FormattableString.Invariant($"{TestRuntime.Name} on Hero ({TestRuntime.Hero}): the execution waiting in {latentTitle} (node {latent}) is cancelled: {problem}");
}
