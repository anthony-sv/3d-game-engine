using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Live edit (spec section 5.6): a new working program of a graph replaces the program of its running instances, which keep
// their variables, node states, stored outputs and fibers as far as they still mean the same.
public sealed class LiveEditTests
{
	// library is the asset path of the library whose function holds the latent node, when one does.
	private static string Warning(uint latent, string latentTitle, string problem, string? library = null) => FormattableString.Invariant(
		$"{TestRuntime.Name} on Hero ({TestRuntime.Hero}): the execution waiting in {latentTitle} (node {latent}){(library != null ? " of " + library : string.Empty)} is cancelled: {problem}");

	// Event.Create plays the timeline (node 2) of the setting from its start; its Update writes its curve Lift.
	private static string TimelineDocument(string length, string lift, string enabled = "") => VisualScriptTesting.Document(
		$$"""{ "ID": 1, "Type": "Event.Create" }, { "ID": 2, "Type": "Flow.Timeline", "Settings": { "Timeline": { "Length": {{length}}, "Tracks": [{ "Name": "Lift", "Type": "float", "Keys": [{ "Time": 0, "Value": 0, "Interpolation": "Linear" }, {{lift}}] }] } }{{enabled}} }, { "ID": 3, "Type": "{{RuntimeTesting.Write}}" }""",
		"""{ "From": [1, "Then"], "To": [2, "PlayFromStart"] }, { "From": [2, "Update"], "To": [3, "Execute"] }, { "From": [2, "Lift"], "To": [3, "Text"] }""");

	// Editing a playing timeline's keys changes its motion at the next update without restarting it (spec-slice3 section
	// 3.8.6): its time and run carry over, its outputs follow the new curve.
	[Fact]
	public void EditingKeysKeepsAPlayingTimelinePlaying()
	{
		TestRuntime runtime = TestRuntime.Start(TimelineDocument("2", """{ "Time": 2, "Value": 2 }"""));
		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		Assert.Equal(["0.5"], runtime.Records);

		runtime.LoadWorking(TimelineDocument("2", """{ "Time": 2, "Value": 4 }"""));
		Assert.Single(runtime.Instance().Fibers);
		Assert.Equal(0.5f, TimelineTests.OutputOf(runtime, 2, "Time"));
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		Assert.Equal(["0.5", "2"], runtime.Records);
		Assert.Equal(true, TimelineTests.OutputOf(runtime, 2, "Playing"));
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void ShorteningClampsTheTime()
	{
		TestRuntime runtime = TestRuntime.Start(TimelineDocument("2", """{ "Time": 2, "Value": 2 }"""));
		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 1.5f);

		runtime.LoadWorking(TimelineDocument("1", """{ "Time": 1, "Value": 3 }"""));
		runtime.Invoke(ScriptEvent.Update, 0.0f);
		Assert.Equal(["1.5", "3"], runtime.Records);
		Assert.Equal(1.0f, TimelineTests.OutputOf(runtime, 2, "Time"));
		// At its end, so it finished.
		Assert.Equal(false, TimelineTests.OutputOf(runtime, 2, "Playing"));
		Assert.Empty(runtime.Instance().Fibers);
	}

	[Fact]
	public void DisablingAPlayingTimelineCancelsItsRun()
	{
		TestRuntime runtime = TestRuntime.Start(TimelineDocument("2", """{ "Time": 2, "Value": 2 }"""));
		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 0.5f);

		runtime.LoadWorking(TimelineDocument("2", """{ "Time": 2, "Value": 2 }""", """, "Enabled": "Disabled" """));
		Assert.Empty(runtime.Instance().Fibers);
		Assert.Equal(Warning(2, "Timeline", "the edit disabled Timeline (node 2)"), Assert.Single(runtime.Environment.Warnings));
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		Assert.Equal(["0.5"], runtime.Records);
	}

	[Fact]
	public void AnUnrelatedEditKeepsAPendingDelayLoopRunning()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 3 }""");
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.25 }""");
		uint text = graph.Node("FormatString", """{ "Format": "iteration {Index}" }""");
		graph.Link(loop, "Index", text, "Index");
		graph.Flow(graph.Node("Event.Create"), loop);
		graph.Flow(loop, delay, "Body");
		graph.Flow(delay, graph.Write(text, "Result"), "Completed");
		graph.Writes(loop, "Completed", "done");
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		GraphProgram before = runtime.Instance().Program;

		graph.Node("Self");
		runtime.LoadWorking(document.Build());
		Assert.NotSame(before, runtime.Instance().Program);
		Execution fiber = Assert.Single(runtime.Instance().Fibers);
		Assert.Same(runtime.Instance().Program, fiber.Program);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["iteration 1", "iteration 2", "iteration 3", "done"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void NodeStatesAndStoredOutputsCarryOverWhileTheirTypesStayTheSame()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint hit = graph.Node("Event.Custom", """{ "Name": "Hit", "Parameters": [{ "Name": "Damage", "Type": "float" }] }""");
		uint once = graph.Node("Flow.DoOnce");
		graph.Flow(graph.Node("Event.Update"), once);
		graph.Flow(once, graph.Write(hit, "Damage"), "Completed");
		graph.Flow(graph.Node("Event.FixedUpdate"), graph.Write(hit, "Damage"));
		TestRuntime runtime = TestRuntime.Start(document);
		Assert.True(runtime.Runtime.SendEvent(TestRuntime.Hero, "Hit", [2.5f]));

		graph.Writes(graph.Node("Event.Create"), "Then", "an unrelated addition");
		runtime.LoadWorking(document.Build());
		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.FixedUpdate);
		// Damage was stored before the edit, and DoOnce stayed closed after its first run.
		Assert.Equal(["2.5", "2.5"], runtime.Records);

		runtime.LoadWorking(document.Build().Replace("\"Damage\", \"Type\": \"float\"", "\"Damage\", \"Type\": \"int\""));
		runtime.Invoke(ScriptEvent.FixedUpdate);
		runtime.Invoke(ScriptEvent.Update);
		// An output whose type changed starts at the type default; DoOnce's type did not change, so it stays closed.
		Assert.Equal(["2.5", "2.5", "0"], runtime.Records);
	}

	[Fact]
	public void VariablesCarryOverByIdThenByNameAndTypeAndTheOthersStartAtTheirValue()
	{
		DocumentBuilder document = new();
		uint score = document.Variable("Score", "int", "0");
		uint name = document.Variable("Name", "string", "\"\"");
		uint speed = document.Variable("Speed", "float", "1.0");
		GraphBuilder graph = document.Graph;
		uint setScore = graph.Set(score, "7");
		uint setName = graph.Set(name, "\"hero\"");
		uint setSpeed = graph.Set(speed, "2.5");
		graph.Flow(graph.Node("Event.Create"), setScore);
		graph.Flow(setScore, setName);
		graph.Flow(setName, setSpeed);
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);

		runtime.LoadWorking(FormattableString.Invariant($$"""
			{ "Variables": [
				{ "ID": {{score}}, "Name": "Points", "Type": "int", "Value": 100 },
				{ "ID": 50, "Name": "Name", "Type": "string", "Value": "villain" },
				{ "ID": {{speed}}, "Name": "Speed", "Type": "int", "Value": 4 },
				{ "ID": 51, "Name": "Extra", "Type": "bool", "Value": true },
				{ "ID": 52, "Name": "Tint", "Type": "Strada.Color" }
			], "Functions": [], "Graph": { "Nodes": [], "Links": [], "Comments": [] } }
			"""));
		// Score kept its ID under a new name, Name its name and type under a new ID: they keep their values, whatever Value the
		// edit gives them. Speed changed its type, and Extra and Tint are new: they start as in a new instance, at their Value,
		// else at the type default.
		Assert.Equal(7, runtime.Variable("Points"));
		Assert.Equal("hero", runtime.Variable("Name"));
		Assert.Equal(4, runtime.Variable("Speed"));
		Assert.Equal(true, runtime.Variable("Extra"));
		Assert.Equal(new Color(1.0f, 1.0f, 1.0f, 1.0f), runtime.Variable("Tint"));
		// A new instance starts at the Values.
		runtime.Create(8);
		Assert.Equal(100, runtime.Variable("Points", 8));
		Assert.Equal("villain", runtime.Variable("Name", 8));
		Assert.Equal(4, runtime.Variable("Speed", 8));
		Assert.Equal(true, runtime.Variable("Extra", 8));
		Assert.Equal(new Color(1.0f, 1.0f, 1.0f, 1.0f), runtime.Variable("Tint", 8));
	}

	[Fact]
	public void ASwapRequestedWhileTheOldProgramRunsWaitsForItsOutermostExecution()
	{
		DocumentBuilder document = new();
		document.Variable("Flag", "bool");
		uint count = document.Variable("Count", "int", "0");
		// The new program has only Count: what the old one writes after the load must reach it through the swap.
		string next = FormattableString.Invariant($$"""
			{ "Variables": [{ "ID": {{count}}, "Name": "Count", "Type": "int" }], "Functions": [],
			  "Graph": { "Nodes": [{ "ID": 900, "Type": "Event.Create" }, { "ID": 901, "Type": {{RuntimeTesting.Quote(RuntimeTesting.Write)}}, "Inputs": { "Text": "the new program" } }],
			  "Links": [{ "From": [900, "Then"], "To": [901, "Execute"] }], "Comments": [] } }
			""");
		GraphBuilder graph = document.Graph;
		uint reload = graph.Node("Call:Strada.ScriptCore.Tests.Probe.Reload(string)", inputs: $$"""{ "Document": {{RuntimeTesting.Quote(next)}} }""");
		uint send = graph.Node("Event.Send", """{ "Name": "Nested" }""");
		uint set = graph.Set(count, "5");
		graph.Flow(graph.Node("Event.Create"), send);
		graph.Flow(send, set);
		graph.Writes(set, "Then", "the old program after the load");
		graph.Flow(graph.Node("Event.Custom", """{ "Name": "Nested" }"""), reload);
		graph.Writes(reload, "Then", "the nested execution after the load");
		TestRuntime runtime = TestRuntime.Start(document);
		Probe.Runtime = runtime.Runtime;
		GraphProgram old = runtime.Instance().Program;

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(["the nested execution after the load", "the old program after the load"], runtime.Records);
		Assert.NotSame(old, runtime.Instance().Program);
		Assert.Equal(5, runtime.Variable("Count"));
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal("the new program", runtime.Records[^1]);
		Assert.Equal(0, old.Activity);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void ABrokenDocumentLeavesRunningInstancesAndTheirFibersAlone()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.25 }""");
		graph.Flow(graph.Node("Event.Create"), delay);
		graph.Writes(delay, "Completed", "resumed");
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		GraphProgram program = runtime.Instance().Program;

		runtime.Load("""{ "Graph": { "Nodes": [{ "ID": 1, "Type": "Flow.Nothing" }] } }""");
		Assert.Same(program, runtime.Instance().Program);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["resumed"], runtime.Records);
	}

	[Fact]
	public void AFiberWhoseLatentNodeIsGoneIsCancelledWithOneLogLine()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.25 }""");
		graph.Flow(create, delay);
		graph.Writes(delay, "Completed", "never");
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);

		// The Delay's ID now names a WaitFrames.
		runtime.LoadWorking(FormattableString.Invariant(
			$$"""{ "Graph": { "Nodes": [{ "ID": {{create}}, "Type": "Event.Create" }, { "ID": {{delay}}, "Type": "Flow.WaitFrames" }], "Links": [], "Comments": [] } }"""));
		Assert.Empty(runtime.Instance().Fibers);
		Assert.Equal([Warning(delay, "Delay", FormattableString.Invariant($"the edit removed or changed Delay (node {delay})"))], runtime.Environment.Warnings);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Empty(runtime.Records);
	}

	[Fact]
	public void AFiberSurvivesWhileTheSequenceOutputItContinuesAtExists()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint sequence = graph.Node("Flow.Sequence", """{ "Count": 3 }""");
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.25 }""");
		graph.Flow(graph.Node("Event.Create"), sequence);
		graph.Flow(sequence, delay, "Then1");
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		string built = document.Build();

		// The fiber continues at Then2 after its Delay.
		runtime.LoadWorking(built.Replace("{ \"Count\": 3 }", "{ \"Count\": 4 }"));
		Assert.Single(runtime.Instance().Fibers);
		runtime.LoadWorking(built.Replace("{ \"Count\": 3 }", "{ \"Count\": 2 }"));
		Assert.Empty(runtime.Instance().Fibers);
		Assert.Equal([Warning(delay, "Delay", FormattableString.Invariant($"the edit removed or changed Sequence (node {sequence})"))],
			runtime.Environment.Warnings);
	}

	[Fact]
	public void AFiberInAFunctionKeepsItsFrameAndLocalsAcrossAnEdit()
	{
		DocumentBuilder document = new();
		FunctionBuilder wait = document.Function("Wait");
		uint count = wait.Local("Count", "int", "5");
		uint set = wait.Graph.Set(count, "9");
		uint delay = wait.Graph.Node("Flow.Delay", inputs: """{ "Duration": 0.25 }""");
		wait.Graph.Flow(wait.Entry, set);
		wait.Graph.Flow(set, delay);
		wait.Graph.Flow(delay, wait.Graph.Write(wait.Graph.Get(count), "Value"), "Completed");
		GraphBuilder graph = document.Graph;
		uint call = graph.Node("Function.Call", wait.Settings);
		graph.Flow(graph.Node("Event.Create"), call);
		graph.Writes(call, "Then", "returned");
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);

		graph.Node("Self");
		runtime.LoadWorking(document.Build());
		Execution fiber = Assert.Single(runtime.Instance().Fibers);
		Assert.Equal(2, fiber.CallCount);
		Assert.Same(runtime.Instance().Program.FindFunction(0, wait.Id)!.Body, fiber.TopFrame.Graph);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["9", "returned"], runtime.Records);
	}

	[Fact]
	public void AFiberInAFunctionWhoseLocalsChangedIsCancelled()
	{
		DocumentBuilder document = new();
		FunctionBuilder wait = document.Function("Wait");
		wait.Local("Count", "int", "5");
		uint delay = wait.Graph.Node("Flow.Delay", inputs: """{ "Duration": 0.25 }""");
		wait.Graph.Flow(wait.Entry, delay);
		GraphBuilder graph = document.Graph;
		uint call = graph.Node("Function.Call", wait.Settings);
		graph.Flow(graph.Node("Event.Create"), call);
		graph.Writes(call, "Then", "returned");
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);

		runtime.LoadWorking(document.Build().Replace("\"Count\", \"Type\": \"int\", \"Value\": 5", "\"Count\", \"Type\": \"float\", \"Value\": 5.0"));
		Assert.Empty(runtime.Instance().Fibers);
		Assert.Equal([Warning(delay, "Delay", "the edit changed the function Wait")], runtime.Environment.Warnings);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Empty(runtime.Records);
	}

	// A graph that calls a library runs the functions the library had when the graph was loaded; a change of the library
	// loads the graph again (spec-slice3 section 4.4.4), and its instances swap as for any edit: a fiber waiting in the
	// library's function keeps waiting while the nodes it holds remain, and the variables carry over.
	private static (TestRuntime Runtime, string Document, uint Count) StartWaitingInTheLibrary()
	{
		MotionLibrary library = new();
		DocumentBuilder document = new();
		uint count = document.Variable("Count", "int", "1");
		GraphBuilder graph = document.Graph;
		uint set = graph.Set(count, "5");
		uint pause = graph.Node("Function.Call", CrossGraphTesting.Call(library.Pause, CrossGraphTesting.Motion), """{ "Seconds": 0.5 }""");
		graph.Flow(graph.Node("Event.Create"), set);
		graph.Flow(set, pause);
		graph.Writes(pause, "Then", "after");
		TestRuntime runtime = new();
		runtime.LoadWorking(document.Build(), references: library.Bundle());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		runtime.Invoke(ScriptEvent.Create);
		Assert.Single(runtime.Instance().Fibers);
		return (runtime, document.Build(), count);
	}

	[Fact]
	public void AFiberWaitingInALibraryFunctionSurvivesAChangeOfTheLibrary()
	{
		(TestRuntime runtime, string document, _) = StartWaitingInTheLibrary();
		GraphProgram before = runtime.Instance().Program;
		MotionLibrary changed = new();
		changed.Report.Graph.Write("more");

		runtime.LoadWorking(document, references: changed.Bundle());
		Assert.NotSame(before, runtime.Instance().Program);
		Execution fiber = Assert.Single(runtime.Instance().Fibers);
		Assert.Same(runtime.Instance().Program.FindFunction(CrossGraphTesting.Motion, changed.Pause.Id)!.Body, fiber.TopFrame.Graph);
		Assert.Equal(5, runtime.Variable("Count"));
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		Assert.Equal(["resumed", "after"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void AFiberWhoseLatentNodeInALibraryIsGoneIsCancelledWithOneLogLine()
	{
		(TestRuntime runtime, string document, _) = StartWaitingInTheLibrary();
		MotionLibrary changed = new("Disabled");

		runtime.LoadWorking(document, references: changed.Bundle());
		Assert.Empty(runtime.Instance().Fibers);
		Assert.Equal(5, runtime.Variable("Count"));
		// The messages name the library: node IDs collide across documents.
		Assert.Equal(
			[
				Warning(changed.Delay, "Delay", FormattableString.Invariant($"the edit disabled Delay (node {changed.Delay}) of {CrossGraphTesting.MotionName}"),
					CrossGraphTesting.MotionName),
			],
			runtime.Environment.Warnings);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Empty(runtime.Records);
	}

	[Fact]
	public void AFiberWhoseLatentNodeInALibraryWasRemovedIsCancelledWithOneLogLine()
	{
		(TestRuntime runtime, string document, _) = StartWaitingInTheLibrary();
		uint removed = new MotionLibrary().Delay;
		MotionLibrary changed = new(replaceDelay: true);
		Assert.NotEqual(removed, changed.Delay);

		runtime.LoadWorking(document, references: changed.Bundle());
		Assert.Empty(runtime.Instance().Fibers);
		Assert.Equal(5, runtime.Variable("Count"));
		Assert.Equal(
			[
				Warning(removed, "Delay", FormattableString.Invariant($"the edit removed or changed Delay (node {removed}) of {CrossGraphTesting.MotionName}"),
					CrossGraphTesting.MotionName),
			],
			runtime.Environment.Warnings);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Empty(runtime.Records);
	}

	[Fact]
	public void ABrokenLibraryLeavesTheGraphsThatCallItOnTheirWorkingProgram()
	{
		(TestRuntime runtime, string document, _) = StartWaitingInTheLibrary();
		GraphProgram working = runtime.Instance().Program;
		MotionLibrary broken = new();
		broken.Pause.Graph.Node("Flow.Nothing");

		Diagnostic error = Assert.Single(runtime.Load(document, references: broken.Bundle()), static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error);
		Assert.Equal(DiagnosticCode.ReferenceError, error.Code);
		Assert.Same(working, runtime.Runtime.FindEffectiveProgram(TestRuntime.Graph));
		Assert.Same(working, runtime.Instance().Program);
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		Assert.Equal(["resumed", "after"], runtime.Records);
	}

	// Receipts (spec-slice4 sections 8.3 and 8.4): each load of a graph's document tells the live log what it did.

	// The receipts written after the serial.
	private static JsonElement[] ReceiptsAfter(TestRuntime runtime, ulong after)
	{
		byte[]? text = runtime.Runtime.LiveEvents.Write(after);
		Assert.NotNull(text);
		return [.. VisualScriptTesting.ParseObject(text).GetProperty("Edits").EnumerateArray()];
	}

	// The receipt of a working load made after the runtime's last event.
	private static JsonElement ReceiptOf(TestRuntime runtime, string document)
	{
		ulong last = runtime.Runtime.LiveEvents.LastSerial;
		runtime.LoadWorking(document);
		return Assert.Single(ReceiptsAfter(runtime, last));
	}

	private static string[] Names(JsonElement element, string member) => [.. element.GetProperty(member).EnumerateArray().Select(static name => name.GetString()!)];

	private static string Text(JsonElement element, string member) => element.GetProperty(member).GetString()!;

	private static JsonElement Note(JsonElement receipt) => Assert.Single(receipt.GetProperty("InstanceNotes").EnumerateArray());

	private static string Id(ulong id) => id.ToString(CultureInfo.InvariantCulture);

	[Fact]
	public void AReceiptNamesWhatTheEditDidToEachVariable()
	{
		DocumentBuilder document = new();
		uint score = document.Variable("Score", "int", "7");
		document.Variable("Name", "string", "\"\"");
		uint speed = document.Variable("Speed", "float", "1.0");
		uint lives = document.Variable("Lives", "int", "3");
		document.Variable("Old", "bool");
		TestRuntime runtime = TestRuntime.Start(document);

		JsonElement receipt = ReceiptOf(runtime, FormattableString.Invariant($$"""
			{ "Variables": [
				{ "ID": {{score}}, "Name": "Points", "Type": "int" },
				{ "ID": 50, "Name": "Name", "Type": "string" },
				{ "ID": {{speed}}, "Name": "Speed", "Type": "int" },
				{ "ID": {{lives}}, "Name": "Hearts", "Type": "int" },
				{ "ID": 51, "Name": "Lives", "Type": "int" },
				{ "ID": 52, "Name": "Extra", "Type": "bool" }
			], "Functions": [], "Graph": { "Nodes": [], "Links": [], "Comments": [] } }
			"""));
		Assert.Equal(("Swapped", false, 1, 0), (Text(receipt, "Outcome"), receipt.GetProperty("Deferred").GetBoolean(),
			receipt.GetProperty("Instances").GetInt32(), receipt.GetProperty("Pending").GetInt32()));
		Assert.Equal(Id(TestRuntime.Graph), Text(receipt, "Graph"));
		Assert.Equal(TestRuntime.Name, Text(receipt, "Name"));
		JsonElement variables = receipt.GetProperty("Variables");
		// Name kept its name under a new ID; Score and Lives kept their IDs under new names; Speed changed its type; the new Lives
		// has the name of a variable another continues; Extra is new and Old is gone.
		Assert.Equal(["Name"], Names(variables, "Kept"));
		Assert.Equal("""[{"Name":"Points","From":"Score"},{"Name":"Hearts","From":"Lives"}]""", variables.GetProperty("Matched").GetRawText());
		Assert.Equal("""[{"Name":"Speed","Reason":"TypeChanged","From":"float","To":"int"},{"Name":"Lives","Reason":"New"}]""",
			variables.GetProperty("Reset").GetRawText());
		Assert.Equal(["Extra"], Names(variables, "Added"));
		Assert.Equal(["Old"], Names(variables, "Removed"));
		// An instance without waits, stops or Timelines has nothing to note.
		Assert.Empty(receipt.GetProperty("InstanceNotes").EnumerateArray());
		Assert.Equal(7, runtime.Variable("Points"));
	}

	[Fact]
	public void AReceiptNamesTheStatesTheEditResets()
	{
		static string Document(string onceType, string gateEnabled, bool withFlipFlop) => VisualScriptTesting.Document(
			$$"""{ "ID": 1, "Type": "Event.Update" }, { "ID": 2, "Type": "{{onceType}}" }, { "ID": 3, "Type": "Flow.Gate"{{gateEnabled}} }"""
			+ (withFlipFlop ? """, { "ID": 4, "Type": "Flow.FlipFlop" }""" : string.Empty),
			"""{ "From": [1, "Then"], "To": [2, "Execute"] }""");
		TestRuntime runtime = TestRuntime.Start(Document("Flow.DoOnce", string.Empty, true));
		CompiledGraph before = runtime.Instance().Program.EventGraph;
		string Title(uint node) => before.Nodes[before.FindNode(node)].Title;

		JsonElement states = ReceiptOf(runtime, Document("Flow.DoOnce", string.Empty, true)).GetProperty("States");
		Assert.Equal(3, states.GetProperty("Kept").GetInt32());
		Assert.Empty(states.GetProperty("Reset").EnumerateArray());

		states = ReceiptOf(runtime, Document("Flow.FlipFlop", """, "Enabled": "Disabled" """, false)).GetProperty("States");
		Assert.Equal(0, states.GetProperty("Kept").GetInt32());
		string document = Id(TestRuntime.Graph);
		Assert.Equal(
			FormattableString.Invariant(
				$$"""[{"Document":"{{document}}","Function":0,"Node":2,"Title":"{{Title(2)}}","Reason":"Changed"},{"Document":"{{document}}","Function":0,"Node":3,"Title":"{{Title(3)}}","Reason":"Disabled"},{"Document":"{{document}}","Function":0,"Node":4,"Title":"{{Title(4)}}","Reason":"Removed"}]"""),
			states.GetProperty("Reset").GetRawText());
		// A disabled node has no state to lose: enabling the Gate again starts it as a new one.
		states = ReceiptOf(runtime, Document("Flow.FlipFlop", string.Empty, false)).GetProperty("States");
		Assert.Equal(1, states.GetProperty("Kept").GetInt32());
		Assert.Empty(states.GetProperty("Reset").EnumerateArray());
	}

	[Fact]
	public void AReceiptNamesTheWaitsTheEditKeepsAndCancels()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.25 }""");
		graph.Flow(create, delay);
		graph.Writes(delay, "Completed", "never");
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);

		graph.Node("Self");
		JsonElement kept = Note(ReceiptOf(runtime, document.Build()));
		Assert.Equal((Id(TestRuntime.Hero), 1, 0), (Text(kept, "Entity"), kept.GetProperty("WaitsKept").GetInt32(), kept.GetProperty("StopsKept").GetInt32()));
		Assert.Empty(kept.GetProperty("Cancelled").EnumerateArray());

		JsonElement cancelled = Note(ReceiptOf(runtime, FormattableString.Invariant(
			$$"""{ "Graph": { "Nodes": [{ "ID": {{create}}, "Type": "Event.Create" }, { "ID": {{delay}}, "Type": "Flow.WaitFrames" }], "Links": [], "Comments": [] } }""")));
		string reason = FormattableString.Invariant($"the edit removed or changed Delay (node {delay})");
		Assert.Equal(0, cancelled.GetProperty("WaitsKept").GetInt32());
		Assert.Equal(
			FormattableString.Invariant(
				$$"""[{"Document":"{{Id(TestRuntime.Graph)}}","Function":0,"Node":{{delay}},"Title":"Delay","Stopped":false,"Reason":"{{reason}}"}]"""),
			cancelled.GetProperty("Cancelled").GetRawText());
		// The Warning is the one a cancelled wait always logged; the receipt's reason is its reason.
		Assert.Equal([Warning(delay, "Delay", reason)], runtime.Environment.Warnings);
	}

	[Fact]
	public void AReceiptNamesTheStoppedExecutionsTheEditKeepsAndCancels()
	{
		// The stopped node 2 writes the text; null: the node has another type.
		static string Writer(string? text) => VisualScriptTesting.Document(
			text != null ? $$"""{ "ID": 1, "Type": "Event.Update" }, { "ID": 2, "Type": "{{RuntimeTesting.Write}}", "Inputs": { "Text": {{RuntimeTesting.Quote(text)}} } }"""
				: """{ "ID": 1, "Type": "Event.Update" }, { "ID": 2, "Type": "Call:Strada.ScriptCore.Tests.Probe.Throw(string)" }""",
			"""{ "From": [1, "Then"], "To": [2, "Execute"] }""");
		TestRuntime runtime = TestRuntime.Start(Writer("old"));
		DebugSession session = new(runtime);
		session.Break(2);
		runtime.Invoke(ScriptEvent.Update);
		string title = runtime.Instance().Program.EventGraph.Nodes[runtime.Instance().Program.EventGraph.FindNode(2)].Title;

		JsonElement kept = Note(ReceiptOf(runtime, Writer("new")));
		Assert.Equal((0, 1), (kept.GetProperty("WaitsKept").GetInt32(), kept.GetProperty("StopsKept").GetInt32()));

		JsonElement cancelled = Note(ReceiptOf(runtime, Writer(null)));
		JsonElement stopped = Assert.Single(cancelled.GetProperty("Cancelled").EnumerateArray());
		Assert.True(stopped.GetProperty("Stopped").GetBoolean());
		Assert.Equal((2u, title), (stopped.GetProperty("Node").GetUInt32(), Text(stopped, "Title")));
		Assert.Equal(FormattableString.Invariant($"the edit removed or changed {title} (node 2)"), Text(stopped, "Reason"));
		Assert.Equal(0, cancelled.GetProperty("StopsKept").GetInt32());
		Assert.Empty(session.Stops());
	}

	[Fact]
	public void AReceiptNamesEachTimelinesTimeAndWhereItWasClampedFrom()
	{
		TestRuntime runtime = TestRuntime.Start(TimelineDocument("2", """{ "Time": 2, "Value": 2 }"""));
		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 1.5f);

		JsonElement unclamped = Note(ReceiptOf(runtime, TimelineDocument("2", """{ "Time": 2, "Value": 4 }""")));
		Assert.Equal("""[{"Node":2,"Time":1.5,"ClampedFrom":null}]""", unclamped.GetProperty("Timelines").GetRawText());
		// The timeline's run is a wait that carries over.
		Assert.Equal(1, unclamped.GetProperty("WaitsKept").GetInt32());
		JsonElement clamped = Note(ReceiptOf(runtime, TimelineDocument("1", """{ "Time": 1, "Value": 3 }""")));
		Assert.Equal("""[{"Node":2,"Time":1,"ClampedFrom":1.5}]""", clamped.GetProperty("Timelines").GetRawText());
	}

	// A swap that waits for a running step of the old program: Pending at the load, then Swapped, Deferred, when it is done.
	[Fact]
	public void ADeferredSwapGivesAPendingReceiptThenADeferredOne()
	{
		DocumentBuilder document = new();
		uint count = document.Variable("Count", "int", "0");
		string next = FormattableString.Invariant(
			$$"""{ "Variables": [{ "ID": {{count}}, "Name": "Count", "Type": "int" }], "Functions": [], "Graph": { "Nodes": [], "Links": [], "Comments": [] } }""");
		GraphBuilder graph = document.Graph;
		uint reload = graph.Node("Call:Strada.ScriptCore.Tests.Probe.Reload(string)", inputs: $$"""{ "Document": {{RuntimeTesting.Quote(next)}} }""");
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 1 }""");
		graph.Flow(graph.Node("Event.Create"), delay);
		graph.Flow(graph.Node("Event.Update"), reload);
		TestRuntime runtime = TestRuntime.Start(document);
		Probe.Runtime = runtime.Runtime;
		runtime.Invoke(ScriptEvent.Create);
		ulong last = runtime.Runtime.LiveEvents.LastSerial;

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update));
		JsonElement[] receipts = ReceiptsAfter(runtime, last);
		Assert.Equal(2, receipts.Length);
		Assert.Equal(("Pending", false, 0, 1), (Text(receipts[0], "Outcome"), receipts[0].GetProperty("Deferred").GetBoolean(),
			receipts[0].GetProperty("Instances").GetInt32(), receipts[0].GetProperty("Pending").GetInt32()));
		Assert.Empty(receipts[0].GetProperty("InstanceNotes").EnumerateArray());
		Assert.Equal(("Swapped", true, 1, 0), (Text(receipts[1], "Outcome"), receipts[1].GetProperty("Deferred").GetBoolean(),
			receipts[1].GetProperty("Instances").GetInt32(), receipts[1].GetProperty("Pending").GetInt32()));
		Assert.Equal(["Count"], Names(receipts[1].GetProperty("Variables"), "Kept"));
		// The Delay is gone from the new program: the deferred receipt names its cancelled wait.
		Assert.Equal(delay, Note(receipts[1]).GetProperty("Cancelled")[0].GetProperty("Node").GetUInt32());
	}

	[Fact]
	public void ABrokenDocumentsReceiptKeepsTheRunningProgram()
	{
		TestRuntime runtime = TestRuntime.Start(VisualScriptTesting.Document("""{ "ID": 1, "Type": "Event.Create" }"""));
		ulong last = runtime.Runtime.LiveEvents.LastSerial;

		runtime.Load("""{ "Graph": { "Nodes": [{ "ID": 1, "Type": "Flow.Nothing" }] } }""");
		JsonElement receipt = Assert.Single(ReceiptsAfter(runtime, last));
		Assert.Equal(("Kept", 0, 0), (Text(receipt, "Outcome"), receipt.GetProperty("Instances").GetInt32(), receipt.GetProperty("Pending").GetInt32()));
		Assert.Matches(@"^(1 error|\d+ errors), first: .+ \(node 1\)$", Text(receipt, "Errors"));
	}

	// The first working program of a handle replaces none: every variable is added.
	[Fact]
	public void TheFirstWorkingProgramAddsEveryVariable()
	{
		DocumentBuilder document = new();
		document.Variable("Score", "int", "0");
		TestRuntime runtime = new();

		JsonElement receipt = ReceiptOf(runtime, document.Build());
		Assert.Equal(("Swapped", 0), (Text(receipt, "Outcome"), receipt.GetProperty("Instances").GetInt32()));
		Assert.Equal(["Score"], Names(receipt.GetProperty("Variables"), "Added"));
	}

	// Spec-slice4 section 21: one carry plan for 1,000 instances, and 64 of their notes; the time is recorded.
	[Fact]
	public void OnePlanServesAThousandInstances()
	{
		DocumentBuilder document = new();
		document.Variable("Score", "int", "5");
		uint once = document.Graph.Node("Flow.DoOnce");
		uint delay = document.Graph.Node("Flow.Delay", inputs: """{ "Duration": 1 }""");
		document.Graph.Flow(document.Graph.Node("Event.Create"), once);
		document.Graph.Flow(once, delay, "Completed");
		TestRuntime runtime = TestRuntime.Start(document);
		for (ulong entity = 1000; entity < 1999; entity++)
		{
			runtime.Environment.Add(entity, "Clone");
			Assert.Equal(VisualScriptStatus.Success, runtime.Create(entity));
		}
		foreach (GraphInstance instance in runtime.Runtime.Instances)
		{
			runtime.Invoke(ScriptEvent.Create, entity: instance.Entity);
		}
		int plans = runtime.Runtime.CarryPlansMade;
		document.Graph.Node("Self");
		long start = Stopwatch.GetTimestamp();

		JsonElement receipt = ReceiptOf(runtime, document.Build());
		TimeSpan elapsed = Stopwatch.GetElapsedTime(start);
		TestContext.Current.TestOutputHelper?.WriteLine(FormattableString.Invariant($"a load that migrates 1,000 instances: {elapsed.TotalMilliseconds:F2} ms"));
		Assert.Equal(plans + 1, runtime.Runtime.CarryPlansMade);
		Assert.Equal(1000, receipt.GetProperty("Instances").GetInt32());
		Assert.Equal(LiveEditReceipt.MaxInstanceNotes, receipt.GetProperty("InstanceNotes").GetArrayLength());
		Assert.Equal(1000 - LiveEditReceipt.MaxInstanceNotes, receipt.GetProperty("MoreInstances").GetInt32());
		Assert.Equal(1, receipt.GetProperty("States").GetProperty("Kept").GetInt32());
		Assert.All(runtime.Runtime.Instances, instance => Assert.Single(instance.Fibers));
	}

	// The property test of spec-slice4 section 20.2: over random pairs of programs (the shared fixtures, damaged and rewired by
	// the fuzz mutator, each way), the carry plan carries exactly what slice 3's per-instance carry (PerInstanceCarry, kept here
	// as the oracle) did, slot for slot: variables, the event frame and every function frame both programs have. The debugger's
	// Changed compare reads the edit by the same plan: perturbing every slot the oracle carried shows exactly those as changed.
	[Fact]
	public void TheCarryPlanCarriesWhatThePerInstanceCarryDid()
	{
		List<GraphProgram> programs = [];
		VisualScriptRuntime runtime = new(FuzzLibrary.CreateEnvironment(), () => FuzzLibrary.Library);
		DocumentMutator mutator = new(0x43415252595041u);
		int rounds = Fuzzing.GetRounds(80);
		for (int round = 0; round < rounds; round++)
		{
			(_, JsonNode fixture) = FuzzLibrary.Fixtures[round % FuzzLibrary.Fixtures.Count];
			foreach (JsonNode? document in (JsonNode?[])[fixture, mutator.Damage(fixture, round), mutator.Rewire(fixture, round)])
			{
				runtime.Load((ulong)programs.Count + 1, "Assets/Pair.sgraph", Encoding.UTF8.GetBytes(document?.ToJsonString() ?? "null"));
				if (runtime.FindLatestProgram((ulong)programs.Count + 1) is { IsBroken: false } program)
				{
					programs.Add(program);
				}
			}
		}
		FuzzRandom random = new(0x534c4f5453u);
		CarryTally tally = new();
		for (int pair = 0; pair + 1 < programs.Count; pair++)
		{
			// Neighbours are a fixture and its own mutations, or a fixture and the next one's.
			ComparePair(programs[pair], programs[pair + 1], random, tally);
			ComparePair(programs[pair + 1], programs[pair], random, tally);
		}
		TestContext.Current.TestOutputHelper?.WriteLine(FormattableString.Invariant(
			$"{tally.Pairs} pairs: {tally.Variables} variables, {tally.Slots} frame slots, {tally.Timelines} timelines and {tally.Functions} function frames carried"));
		Assert.True(tally.Pairs > 100, $"only {tally.Pairs} pairs compiled");
		Assert.True(tally.Variables > 0 && tally.Slots > 0 && tally.Timelines > 0 && tally.Functions > 0, $"a carry was never exercised: {tally}");
	}

	private sealed class CarryTally
	{
		internal int Pairs { get; set; }

		internal int Variables { get; set; }

		internal int Slots { get; set; }

		internal int Timelines { get; set; }

		internal int Functions { get; set; }

		public override string ToString() => FormattableString.Invariant($"{Variables} variables, {Slots} slots, {Timelines} timelines, {Functions} functions");
	}

	private static void ComparePair(GraphProgram from, GraphProgram to, FuzzRandom random, CarryTally tally)
	{
		tally.Pairs++;
		CarryPlan plan = CarryPlan.Make(from, to);
		Slot[] values = [.. from.Variables.Select(_ => RandomSlot(random))];
		Slot[] expected = PerInstanceCarry.CarryVariables(from, values, to);
		Slot[] carried = plan.CarryVariables(values);
		AssertSameSlots(expected, carried, "variables");
		tally.Variables += PerInstanceCarry.MatchVariables(from, to).Count(static source => source >= 0);

		(Frame oracleFrom, Frame planFrom) = RandomFrames(from.EventGraph, random);
		Frame oracleTo = new(to.EventGraph);
		Frame planTo = new(to.EventGraph);
		PerInstanceCarry.CarryNodes(oracleFrom, oracleTo);
		plan.Events.Apply(planFrom, planTo, null);
		tally.Slots += AssertSameFrames(oracleTo, planTo, oracleFrom, planFrom, "the event graph");
		tally.Timelines += plan.Events.Timelines.Count(timeline => oracleFrom.Slots[timeline.From].Object is TimelineState);
		foreach (CompiledFunction function in from.Functions)
		{
			if (to.FindFunction(function.Graph, function.Id) is not { } target)
			{
				continue;
			}
			(Frame functionOracle, Frame functionPlan) = RandomFrames(function.Body, random);
			Frame oracleCall = new(target.Body);
			Frame planCall = new(target.Body);
			PerInstanceCarry.CarryNodes(functionOracle, oracleCall);
			plan.ForFunction(function, target).Apply(functionPlan, planCall, null);
			AssertSameFrames(oracleCall, planCall, functionOracle, functionPlan, FormattableString.Invariant($"function {function.Id}"));
			tally.Functions++;
		}
		AssertTheDebuggerReadsThePlan(from, to, values, planFrom, plan, oracleTo);
	}

	// A frame of the graph with a distinct value in every slot, twice: one for the oracle and one for the plan, alike except that
	// each has its own TimelineState objects (the carry clamps them in place), whose times are often beyond the length.
	private static (Frame Oracle, Frame Plan) RandomFrames(CompiledGraph graph, FuzzRandom random)
	{
		Frame oracle = new(graph);
		Frame plan = new(graph);
		for (int slot = 0; slot < oracle.Slots.Length; slot++)
		{
			oracle.Slots[slot] = RandomSlot(random);
			plan.Slots[slot] = oracle.Slots[slot];
		}
		foreach (CompiledNode node in graph.Nodes)
		{
			if (node.TimelineSlot >= 0 && random.Pick(4) > 0)
			{
				double time = (random.Pick(400) - 50) / 100.0;
				oracle.Slots[node.TimelineSlot] = Slot.FromObject(new TimelineState { Time = time });
				plan.Slots[node.TimelineSlot] = Slot.FromObject(new TimelineState { Time = time });
			}
		}
		return (oracle, plan);
	}

	private static Slot RandomSlot(FuzzRandom random) => new() { Long = (long)random.Next(), Object = random.Pick(3) == 0 ? new object() : null };

	private static void AssertSameSlots(Slot[] expected, Slot[] actual, string what)
	{
		Assert.Equal(expected.Length, actual.Length);
		for (int index = 0; index < expected.Length; index++)
		{
			Assert.True(DebugSnapshot.SameSlot(expected[index], actual[index]), FormattableString.Invariant($"{what}: slot {index} differs"));
		}
	}

	// Answers how many slots the oracle carried (those that differ from the new frame's template).
	private static int AssertSameFrames(Frame oracle, Frame plan, Frame oracleFrom, Frame planFrom, string what)
	{
		int carried = 0;
		for (int index = 0; index < oracle.Slots.Length; index++)
		{
			Slot expected = oracle.Slots[index];
			Slot actual = plan.Slots[index];
			if (expected.Object is TimelineState oracleState)
			{
				// The same position's state of each frame, clamped alike.
				int from = Array.FindIndex(oracleFrom.Slots, slot => ReferenceEquals(slot.Object, oracleState));
				Assert.True(from >= 0 && ReferenceEquals(planFrom.Slots[from].Object, actual.Object), FormattableString.Invariant($"{what}: timeline slot {index} differs"));
				Assert.Equal(oracleState.Time, ((TimelineState)actual.Object!).Time);
				carried++;
				continue;
			}
			Assert.True(DebugSnapshot.SameSlot(expected, actual), FormattableString.Invariant($"{what}: slot {index} differs"));
			if (!DebugSnapshot.SameSlot(expected, oracle.Graph.Template[index]))
			{
				carried++;
			}
		}
		return carried;
	}

	// The debugger's Changed compare (spec-slice4 section 4.7) across the edit: with nothing changed since, none; with every
	// carried value changed, exactly the variables and stored outputs the oracle carried.
	private static void AssertTheDebuggerReadsThePlan(GraphProgram from, GraphProgram to, Slot[] values, Frame snapshotFrame, CarryPlan plan, Frame carried)
	{
		DebugSnapshotMember member = new()
		{
			Serial = 1,
			Entity = TestRuntime.Hero,
			Graph = 1,
			Program = from,
			Frames = [new DebugSnapshotFrame(from.EventGraph, (Slot[])snapshotFrame.Slots.Clone(), null, -1, 0)],
			Variables = values,
			Flow = [],
			Place = DebugPlace.Enter,
			Node = null,
			Entry = -1,
			Exit = -1,
			Start = default,
		};
		Frame frame = new(to.EventGraph);
		Array.Copy(carried.Slots, frame.Slots, frame.Slots.Length);
		GraphInstance instance = new(TestRuntime.Hero, 1, to);
		instance.Swap(to, plan.CarryVariables(values), frame);
		List<DebugChange> changes = [];
		int more = 0;
		DebugSnapshot.AddChanges(member, instance, changes, int.MaxValue, ref more);
		Assert.Empty(changes);

		int[] sources = PerInstanceCarry.MatchVariables(from, to);
		HashSet<DebugChange> expected = [];
		for (int index = 0; index < to.Variables.Length; index++)
		{
			instance.Variables[index] = new Slot { Object = new object() };
			if (sources[index] >= 0)
			{
				expected.Add(new DebugChange(TestRuntime.Hero, 0, to.Variables[index].Name));
			}
		}
		foreach ((uint node, int output) in PerInstanceCarry.CarriedOutputs(from.EventGraph, to.EventGraph))
		{
			CompiledNode carriedNode = to.EventGraph.Nodes[to.EventGraph.FindNode(node)];
			frame.Slots[carriedNode.OutputBase + output] = new Slot { Object = new object() };
			expected.Add(new DebugChange(TestRuntime.Hero, node, carriedNode.OutputPins[output]));
		}
		changes.Clear();
		DebugSnapshot.AddChanges(member, instance, changes, int.MaxValue, ref more);
		Assert.Equal(expected.Count, changes.Count);
		Assert.True(expected.SetEquals(changes), "the debugger's Changed compare reads another carry than the edit made");
	}

	// Slice 3's per-instance carry (LiveEdit.cs before spec-slice4 section 8.3), the oracle of the property test.
	private static class PerInstanceCarry
	{
		internal static Slot[] CarryVariables(GraphProgram source, Slot[] values, GraphProgram target)
		{
			int[] matches = MatchVariables(source, target);
			CompiledVariable[] variables = target.Variables;
			Slot[] carried = new Slot[variables.Length];
			for (int index = 0; index < variables.Length; index++)
			{
				carried[index] = matches[index] >= 0 ? values[matches[index]] : variables[index].Initial;
			}
			return carried;
		}

		internal static int[] MatchVariables(GraphProgram source, GraphProgram target)
		{
			Dictionary<uint, int> byId = [];
			Dictionary<string, int> byName = new(StringComparer.Ordinal);
			for (int index = 0; index < source.Variables.Length; index++)
			{
				byId[source.Variables[index].Id] = index;
				byName[source.Variables[index].Name] = index;
			}
			CompiledVariable[] variables = target.Variables;
			int[] matches = new int[variables.Length];
			bool[] taken = new bool[source.Variables.Length];
			for (int index = 0; index < variables.Length; index++)
			{
				matches[index] = byId.TryGetValue(variables[index].Id, out int old) && SameType(source.Variables[old], variables[index]) ? old : -1;
				if (matches[index] >= 0)
				{
					taken[matches[index]] = true;
				}
			}
			for (int index = 0; index < variables.Length; index++)
			{
				if (matches[index] < 0 && byName.TryGetValue(variables[index].Name, out int old) && !taken[old] &&
					SameType(source.Variables[old], variables[index]))
				{
					matches[index] = old;
					taken[old] = true;
				}
			}
			return matches;
		}

		internal static void CarryNodes(Frame from, Frame to)
		{
			CompiledGraph source = from.Graph;
			foreach (CompiledNode node in to.Graph.Nodes)
			{
				if (node.IsSplit)
				{
					continue;
				}
				if (Find(source, node) is not { } old)
				{
					continue;
				}
				if (node.Op != Opcode.Latent && node.StateSlot >= 0 && old.StateSlot >= 0)
				{
					to.Slots[node.StateSlot] = from.Slots[old.StateSlot];
				}
				if (node.TimelineSlot >= 0 && old.TimelineSlot >= 0 && from.Slots[old.TimelineSlot].Object is TimelineState state)
				{
					state.Time = Math.Clamp(state.Time, 0.0, node.Timeline!.Definition.Length);
					to.Slots[node.TimelineSlot] = Slot.FromObject(state);
				}
				if (node.Pure)
				{
					continue;
				}
				for (int output = 0; output < node.OutputCount; output++)
				{
					int match = MatchOutput(old, node, output);
					if (match >= 0)
					{
						to.Slots[node.OutputBase + output] = from.Slots[old.OutputBase + match];
					}
				}
			}
		}

		// The stored outputs CarryNodes carries: the new node's ID and output.
		internal static IEnumerable<(uint Node, int Output)> CarriedOutputs(CompiledGraph source, CompiledGraph target)
		{
			foreach (CompiledNode node in target.Nodes)
			{
				if (node.IsSplit || node.Pure || Find(source, node) is not { } old)
				{
					continue;
				}
				for (int output = 0; output < node.OutputCount; output++)
				{
					if (MatchOutput(old, node, output) >= 0)
					{
						yield return (node.Id, output);
					}
				}
			}
		}

		private static bool SameType(CompiledVariable a, CompiledVariable b) => a.Type.Id == b.Type.Id;

		private static int MatchOutput(CompiledNode old, CompiledNode node, int output)
		{
			int match = Array.IndexOf(old.OutputPins, node.OutputPins[output]);
			return match >= 0 && old.OutputTypes[match].Id == node.OutputTypes[output].Id ? match : -1;
		}

		private static CompiledNode? Find(CompiledGraph graph, CompiledNode old)
		{
			int index = graph.FindNode(old.Id);
			return index >= 0 && graph.Nodes[index].Type == old.Type && graph.Nodes[index].IsDisabled == old.IsDisabled ? graph.Nodes[index] : null;
		}
	}
}
