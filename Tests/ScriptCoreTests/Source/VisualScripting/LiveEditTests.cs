using System;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Live edit (spec section 5.6): a new working program of a graph replaces the program of its running instances, which keep
// their variables, node states, stored outputs and fibers as far as they still mean the same.
public sealed class LiveEditTests
{
	private static string Warning(uint latent, string latentTitle, string problem) =>
		FormattableString.Invariant($"{TestRuntime.Name} on Hero ({TestRuntime.Hero}): the execution waiting in {latentTitle} (node {latent}) is cancelled: {problem}");

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
		Assert.Same(runtime.Instance().Program.FindFunction(wait.Id)!.Graph, fiber.TopFrame.Graph);
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
}
