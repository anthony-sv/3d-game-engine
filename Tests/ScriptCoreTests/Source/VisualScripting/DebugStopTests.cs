using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Where breakpoints stop and what stopping does (spec-slice4 sections 2.2, 2.4, 2.6, 3, 4.1, 4.2, 4.5, 4.7, 4.10 to 4.12, 5.1
// and 5.2): every kind of node a breakpoint can be on, the rest of the frame running past a stop, debug fibers, Continue,
// targets, the cap on stops, the instance's death, Destroy events and errors. Chains are DebugChainTests', live edits
// DebugLiveEditTests'.
public sealed class DebugStopTests
{
	private static JsonElement Frame(JsonElement stop, int index = 0) => stop.GetProperty("Frames")[index];

	private static int IdOf(JsonElement stop) => stop.GetProperty("ID").GetInt32();

	// An event's breakpoint stops at the start of each execution it starts, its outputs already set; Continue runs it on.
	[Fact]
	public void AnEventStopsAtTheStartOfItsExecutionWithItsOutputsSet()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		graph.Flow(update, graph.Write(update, "DeltaTime"));
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Break(update);

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update, 0.25f));
		Assert.Empty(runtime.Records);
		JsonElement stop = session.SingleStop();
		Assert.Equal("Breakpoint", DebugSession.Text(stop, "Reason"));
		Assert.True(stop.GetProperty("Resumable").GetBoolean());
		Assert.Equal("Enter", DebugSession.Text(stop, "Place"));
		Assert.Equal(update, DebugSession.NodeOf(stop));
		Assert.Equal("Graph", DebugSession.Text(Frame(stop), "Kind"));
		Assert.Equal("7", DebugSession.Text(stop, "Entity"));
		Assert.Equal("100", DebugSession.Text(stop, "Graph"));
		JsonElement start = stop.GetProperty("Start");
		Assert.Equal("Event", DebugSession.Text(start, "Kind"));
		Assert.Equal(update, start.GetProperty("Node").GetUInt32());
		Assert.Equal(IdOf(stop), session.State().GetProperty("Current").GetInt32());
		// The snapshot holds the event's output (DeltaTime) as the stop saw it.
		DebugStop held = session.Debugger.Stops[0];
		CompiledNode node = held.Leaf.Node!;
		Assert.Equal(0.25f, held.Leaf.Frames[0].Slots[node.OutputBase].Float);
		Assert.Single(runtime.Instance().DebugFibers);

		JsonElement resumed = session.Continue();
		Assert.Equal("Ended", DebugSession.Text(resumed, "Outcome"));
		Assert.False(resumed.GetProperty("Failed").GetBoolean());
		Assert.Equal(JsonValueKind.Null, resumed.GetProperty("Stop").ValueKind);
		Assert.Equal(["0.25"], runtime.Records);
		Assert.Empty(session.Stops());
		Assert.Empty(runtime.Instance().DebugFibers);
		Assert.Equal(JsonValueKind.Null, session.State().GetProperty("Current").ValueKind);
	}

	// An impure node stops before it runs, at whichever exec input the flow enters: Gate's Open, DoOnce's Reset, a loop's Break.
	[Fact]
	public void AnImpureNodeStopsBeforeItRunsAtTheInputTheFlowEnters()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint gate = graph.Node("Flow.Gate");
		uint once = graph.Node("Flow.DoOnce");
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 0, "Last": 3 }""");
		uint write = graph.Write("body");
		graph.Flow(create, gate, "Then", "Open");
		graph.Flow(graph.Node("Event.Update"), once, "Then", "Reset");
		graph.Flow(graph.Node("Event.FixedUpdate"), loop);
		graph.Flow(loop, write, "Body");
		graph.Flow(write, loop, "Then", "Break");
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Break(gate, once, write);

		runtime.Invoke(ScriptEvent.Create);
		JsonElement stop = session.SingleStop();
		Assert.Equal(gate, DebugSession.NodeOf(stop));
		Assert.Equal("Open", DebugSession.Text(Frame(stop), "Pin"));
		session.Continue();
		runtime.Invoke(ScriptEvent.Update);
		stop = session.SingleStop();
		Assert.Equal(once, DebugSession.NodeOf(stop));
		Assert.Equal("Reset", DebugSession.Text(Frame(stop), "Pin"));
		session.Continue();
		session.Break(loop);
		runtime.Invoke(ScriptEvent.FixedUpdate);
		Assert.Equal("Execute", DebugSession.Text(Frame(session.SingleStop()), "Pin"));
		session.Continue();
		// The body breaks the loop: the flow enters the loop again through Break.
		stop = session.SingleStop();
		Assert.Equal(loop, DebugSession.NodeOf(stop));
		Assert.Equal("Break", DebugSession.Text(Frame(stop), "Pin"));
		Assert.Equal(["body"], runtime.Records);
		session.Continue();
		Assert.Empty(session.Stops());
	}

	// A call stops before it calls; a function's Entry when a call enters it, its Return before it returns; a breakpoint inside a
	// function stops every call.
	[Fact]
	public void FunctionsStopAtTheirCallEntryReturnAndBody()
	{
		DocumentBuilder document = new();
		FunctionBuilder greet = document.Function("Greet");
		uint inside = greet.Graph.Write("inside");
		uint returned = greet.Graph.Node("Function.Return");
		greet.Graph.Flow(greet.Entry, inside);
		greet.Graph.Flow(inside, returned);
		GraphBuilder graph = document.Graph;
		uint call = graph.Node("Function.Call", greet.Settings);
		uint again = graph.Node("Function.Call", greet.Settings);
		graph.Flow(graph.Node("Event.Create"), call);
		graph.Flow(call, again);
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Break(call, greet.Entry, inside, returned);

		runtime.Invoke(ScriptEvent.Create);
		List<(uint Node, string Kind, uint Function)> stops = [];
		while (session.Stops() is [JsonElement stop])
		{
			JsonElement frame = Frame(stop);
			stops.Add((frame.GetProperty("Node").GetUInt32(), DebugSession.Text(frame, "Kind"), frame.GetProperty("Function").GetUInt32()));
			if (DebugSession.Text(frame, "Kind") == "Call")
			{
				// The caller's frame below shows the call.
				Assert.Equal("Greet", DebugSession.Text(frame, "FunctionName"));
				Assert.Contains(Frame(stop, 1).GetProperty("Node").GetUInt32(), new[] { call, again });
			}
			session.Continue();
		}
		// The second call has no breakpoint of its own; the function's nodes stop it as they stopped the first.
		Assert.Equal(
		[
			(call, "Graph", 0u), (greet.Entry, "Call", greet.Id), (inside, "Call", greet.Id), (returned, "Call", greet.Id),
			(greet.Entry, "Call", greet.Id), (inside, "Call", greet.Id), (returned, "Call", greet.Id),
		], stops);
		Assert.Equal(["inside", "inside"], runtime.Records);
	}

	// A latent node stops where the flow enters it, never when its fiber resumes.
	[Fact]
	public void ALatentNodeStopsOnlyWhereTheFlowEntersIt()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.25 }""");
		graph.Flow(graph.Node("Event.Create"), delay);
		graph.Writes(delay, "Completed", "resumed");
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Break(delay);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(delay, DebugSession.NodeOf(session.SingleStop()));
		Assert.Empty(runtime.Instance().Fibers);
		session.Continue();
		Assert.Single(runtime.Instance().Fibers);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["resumed"], runtime.Records);
		Assert.Empty(session.Stops());
	}

	// A disabled node stops: the flow enters it and passes through.
	[Fact]
	public void ADisabledNodeStops()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint disabled = graph.Node(RuntimeTesting.Write, inputs: """{ "Text": "skipped" }""", enabled: "Disabled");
		graph.Flow(graph.Node("Event.Create"), disabled);
		graph.Writes(disabled, "Then", "after");
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Configure([session.Breakpoint(disabled, type: RuntimeTesting.Write)]);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(disabled, DebugSession.NodeOf(session.SingleStop()));
		session.Continue();
		Assert.Equal(["after"], runtime.Records);
	}

	// A pure node's breakpoint stops the instruction that evaluates it, also through pure calls, and a While's condition at
	// Enter(While) and at each Continue(While).
	[Fact]
	public void PureBreakpointsStopTheirConsumers()
	{
		DocumentBuilder document = new();
		uint counter = document.Variable("Counter", "int", "0");
		FunctionBuilder twice = document.Function("Twice", pure: true, ["Value:int"], ["Result:int"]);
		uint times = twice.Graph.Node("Operator.Multiply", inputs: """{ "B": 2 }""");
		uint result = twice.Graph.Node("Function.Return");
		twice.Graph.Link(twice.Entry, "Value", times, "A");
		twice.Graph.Link(times, "Result", result, "Result");
		GraphBuilder graph = document.Graph;
		uint loop = graph.Node("Flow.While");
		uint less = graph.Node("Operator.Less", inputs: """{ "B": 2 }""");
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
		uint set = graph.Set(counter);
		uint call = graph.Node("Function.Call", twice.Settings);
		graph.Flow(graph.Node("Event.Create"), loop);
		graph.Link(graph.Get(counter), "Value", less, "A");
		graph.Link(less, "Result", loop, "Condition");
		graph.Link(graph.Get(counter), "Value", add, "A");
		graph.Link(add, "Result", set, "Value");
		graph.Flow(loop, set, "Body");
		graph.Link(graph.Get(counter), "Value", call, "Value");
		uint write = graph.Write(call, "Result");
		graph.Flow(loop, write, "Completed");
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Configure([session.Breakpoint(less), session.Breakpoint(times)]);

		runtime.Invoke(ScriptEvent.Create);
		List<(uint Node, string Place, string Note, uint Breakpoint)> stops = [];
		while (session.Stops() is [JsonElement stop])
		{
			stops.Add((DebugSession.NodeOf(stop), DebugSession.Text(stop, "Place"), DebugSession.Text(Frame(stop), "Note"),
				stop.GetProperty("Breakpoint").GetProperty("Node").GetUInt32()));
			session.Continue();
		}
		Assert.Equal(
		[
			(loop, "Enter", "", less), (loop, "Continue", "checks its condition", less), (loop, "Continue", "checks its condition", less),
			(write, "Enter", "", times),
		], stops);
		Assert.Equal(["4"], runtime.Records);
	}

	// A breakpoint on a library's node is keyed by the library's handle: it stops the calls of that function.
	[Fact]
	public void ALibrarysNodeStopsInsideTheCallOfItsFunction()
	{
		MotionLibrary library = new();
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint call = graph.Node("Function.Call", CrossGraphTesting.Call(library.Report, CrossGraphTesting.Motion), """{ "Text": "hello" }""");
		graph.Flow(graph.Node("Event.Create"), call);
		TestRuntime runtime = new();
		runtime.LoadWorking(document.Build(), references: library.Bundle());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		DebugSession session = new(runtime);
		uint write = library.Report.Entry + 1;
		// The caller's document has a node of that ID too: the breakpoint names the library.
		session.Configure([session.Breakpoint(write, CrossGraphTesting.Motion)]);

		runtime.Invoke(ScriptEvent.Create);
		JsonElement stop = session.SingleStop();
		JsonElement frame = Frame(stop);
		Assert.Equal(write, frame.GetProperty("Node").GetUInt32());
		Assert.Equal(CrossGraphTesting.Motion.ToString(CultureInfo.InvariantCulture), DebugSession.Text(frame, "Document"));
		Assert.Equal("100", DebugSession.Text(frame, "Graph"));
		Assert.Equal(library.Report.Id, frame.GetProperty("Function").GetUInt32());
		Assert.Equal(call, Frame(stop, 1).GetProperty("Node").GetUInt32());
		Assert.Equal(CrossGraphTesting.Motion.ToString(CultureInfo.InvariantCulture), DebugSession.Text(stop.GetProperty("Breakpoint"), "Document"));
		session.Continue();
		Assert.Equal(["hello"], runtime.Records);
	}

	// A breakpoint whose node took another Type under its ID never stops: it is inactive, not in the program; one of a graph no
	// instance runs is not running. Breakpoints of a broken document stop in the last working version that runs.
	[Fact]
	public void BreakpointsOfAnotherTypeOrNoRunningGraphAreInactive()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint write = graph.Write("ran");
		graph.Flow(create, write);
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Configure(
		[
			session.Breakpoint(write, type: "Flow.Branch"), session.Breakpoint(create),
			session.Breakpoint(write, document: 4242, type: RuntimeTesting.Write),
		]);
		// A broken document of the graph: the last working program keeps running, with its breakpoints.
		Assert.Contains(runtime.Load("""{ "Variables": [], "Functions": [], "Graph": { "Nodes": [{ "ID": 1, "Type": "No.Such" }], "Links": [], "Comments": [] } }"""),
			static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error);

		JsonElement[] breakpoints = [.. session.State().GetProperty("Breakpoints").EnumerateArray()];
		Assert.Equal(3, breakpoints.Length);
		Assert.False(breakpoints[0].GetProperty("Active").GetBoolean());
		Assert.Equal("NotInProgram", DebugSession.Text(breakpoints[0], "Reason"));
		Assert.True(breakpoints[1].GetProperty("Active").GetBoolean());
		Assert.Equal(string.Empty, DebugSession.Text(breakpoints[1], "Reason"));
		Assert.Equal("NotRunning", DebugSession.Text(breakpoints[2], "Reason"));
		Assert.Equal(JsonValueKind.Null, breakpoints[0].GetProperty("Error").ValueKind);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(create, DebugSession.NodeOf(session.SingleStop()));
		session.Continue();
		Assert.Equal(["ran"], runtime.Records);
		Assert.Empty(session.Stops());
		breakpoints = [.. session.State().GetProperty("Breakpoints").EnumerateArray()];
		Assert.Equal(0UL, breakpoints[0].GetProperty("Reached").GetUInt64());
		Assert.Equal(1UL, breakpoints[1].GetProperty("Hits").GetUInt64());
	}

	// Hit counts (spec-slice4 section 2.4): hits 1 to n - 1 run on, every hit from the n-th on stops; the counters survive a
	// configuration that keeps the breakpoint's hit count and restart with a new one.
	[Fact]
	public void AHitCountStopsFromItsHitOn()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 6 }""");
		uint write = graph.Node(RuntimeTesting.Write);
		graph.Flow(graph.Node("Event.Create"), loop);
		graph.Flow(loop, write, "Body");
		uint text = graph.Node("Convert", """{ "To": "string" }""");
		graph.Link(loop, "Index", text, "Value");
		graph.Link(text, "Result", write, "Text");
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Configure([session.Breakpoint(write, hitCount: 3)]);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["1", "2"], runtime.Records);
		JsonElement breakpoint = session.State().GetProperty("Breakpoints")[0];
		Assert.Equal(3UL, breakpoint.GetProperty("Hits").GetUInt64());
		Assert.Equal(3UL, breakpoint.GetProperty("Reached").GetUInt64());
		session.Continue();
		Assert.Equal(["1", "2", "3"], runtime.Records);
		Assert.Single(session.Stops());
		// Disabling it keeps its counters; a new hit count restarts them.
		session.Configure([session.Breakpoint(write, enabled: false, hitCount: 3)]);
		Assert.Equal(4UL, session.State().GetProperty("Breakpoints")[0].GetProperty("Hits").GetUInt64());
		session.Configure([session.Breakpoint(write, hitCount: 2)]);
		Assert.Equal(0UL, session.State().GetProperty("Breakpoints")[0].GetProperty("Hits").GetUInt64());
		// The resumed instruction is not hit again; the first hit after it runs on, the second stops.
		session.Continue();
		Assert.Equal(["1", "2", "3", "4", "5"], runtime.Records);
		Assert.Single(session.Stops());
		session.Configure();
		session.Continue();
		Assert.Equal(["1", "2", "3", "4", "5", "6"], runtime.Records);
	}

	// The rest of the frame runs past a stop (spec-slice4 section 4.2): the instance's Event.Update after its stopped input event,
	// its pending pass and the other instances; the debug fiber is never advanced by the pending pass.
	[Fact]
	public void TheRestOfTheFrameRunsPastAStop()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint space = graph.Node("Event.KeyPressed", """{ "Key": "Space" }""");
		uint pressed = graph.Write("pressed");
		graph.Flow(space, pressed);
		uint update = graph.Node("Event.Update");
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.0 }""");
		graph.Writes(update, "Then", "update");
		graph.Flow(graph.Node("Event.Create"), delay);
		graph.Writes(delay, "Completed", "delayed");
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Environment.Add(8, "Villain");
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(8));
		runtime.Invoke(ScriptEvent.Create);
		DebugSession session = new(runtime);
		session.Configure([session.Breakpoint(pressed)], [DebugSession.Target(TestRuntime.Graph, TestRuntime.Hero)]);
		runtime.Environment.PressedKeys.Add(KeyCode.Space);

		runtime.Invoke(ScriptEvent.Update, 0.1f);
		runtime.Invoke(ScriptEvent.Update, 0.1f, entity: 8);
		Assert.Equal(["update", "delayed", "pressed", "update"], runtime.Records);
		Execution stopped = Assert.Single(runtime.Instance().DebugFibers);
		int node = stopped.Node;
		// Later frames run the instance again; its stopped execution waits where it stopped.
		runtime.Environment.PressedKeys.Clear();
		runtime.Invoke(ScriptEvent.Update, 0.1f);
		Assert.Equal(["update", "delayed", "pressed", "update", "update"], runtime.Records);
		Assert.Same(stopped, Assert.Single(runtime.Instance().DebugFibers));
		Assert.Equal(node, stopped.Node);
		Assert.Empty(runtime.Instance().Fibers);
		session.Continue();
		Assert.Equal(["update", "delayed", "pressed", "update", "update", "pressed"], runtime.Records);
	}

	// Continue resumes the execution exactly where it stopped: its loop's position and its flow stack.
	[Fact]
	public void ContinueResumesAtTheExactPosition()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 0, "Last": 3 }""");
		uint sequence = graph.Node("Flow.Sequence");
		uint second = graph.Node(RuntimeTesting.Write);
		graph.Flow(graph.Node("Event.Create"), loop);
		graph.Flow(loop, sequence, "Body");
		uint first = graph.Write(loop, "Index");
		graph.Flow(sequence, first, "Then0");
		graph.Flow(sequence, second, "Then1");
		uint text = graph.Node("FormatString", """{ "Format": "then {Index}" }""");
		graph.Link(loop, "Index", text, "Index");
		graph.Link(text, "Result", second, "Text");
		graph.Writes(loop, "Completed", "done");
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Configure([session.Breakpoint(second, hitCount: 3)]);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["0", "then 0", "1", "then 1", "2"], runtime.Records);
		session.Configure();
		session.Continue();
		Assert.Equal(["0", "then 0", "1", "then 1", "2", "then 2", "3", "then 3", "done"], runtime.Records);
	}

	// A chain keeps its budget across stops (spec-slice4 section 4.12): an endless loop with a breakpoint fails after the same
	// number of steps as without one.
	[Fact]
	public void AStoppedExecutionContinuesWithTheBudgetItHadLeft()
	{
		static (TestRuntime Runtime, uint Set) Endless()
		{
			DocumentBuilder document = new();
			uint count = document.Variable("Count", "int", "0");
			GraphBuilder graph = document.Graph;
			uint loop = graph.Node("Flow.While", inputs: """{ "Condition": true }""");
			uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
			uint set = graph.Set(count);
			graph.Link(graph.Get(count), "Value", add, "A");
			graph.Link(add, "Result", set, "Value");
			graph.Flow(graph.Node("Event.Create"), loop);
			graph.Flow(loop, set, "Body");
			return (TestRuntime.Start(document), set);
		}
		(TestRuntime plain, _) = Endless();
		Assert.Equal(VisualScriptStatus.Failure, plain.Invoke(ScriptEvent.Create));
		(TestRuntime stopped, uint set) = Endless();
		DebugSession session = new(stopped);
		session.Configure([session.Breakpoint(set, hitCount: 1000)]);

		Assert.Equal(VisualScriptStatus.Success, stopped.Invoke(ScriptEvent.Create));
		Assert.Equal(999, stopped.Variable("Count"));
		session.Configure();
		JsonElement resumed = session.Continue();
		Assert.True(resumed.GetProperty("Failed").GetBoolean());
		Assert.Equal("Ended", DebugSession.Text(resumed, "Outcome"));
		Assert.Equal(plain.Variable("Count"), stopped.Variable("Count"));
		Assert.Equal(plain.SingleError(), stopped.SingleError());
	}

	// Time.DeltaTime as the stopped execution read it (spec-slice4 section 4.12): a stop in a TriggerEnter or FixedUpdate dispatch
	// continues with the delta the frame's Update set, not 0 or the fixed step, and the value is restored afterwards.
	[Theory]
	[InlineData((int)ScriptEvent.TriggerEnter, "Event.TriggerEnter")]
	[InlineData((int)ScriptEvent.FixedUpdate, "Event.FixedUpdate")]
	public void AStoppedExecutionContinuesWithTheDeltaTimeItRead(int scriptEvent, string eventType)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint dispatched = graph.Node(eventType);
		uint stop = graph.Write("stopped");
		graph.Flow(dispatched, stop);
		// Self asks the environment for the entity: FakeEnvironment notes Time.DeltaTime then.
		graph.Flow(stop, graph.Write(graph.Node("Self"), "Self"));
		graph.Flow(graph.Node("Event.Update"), graph.Write("update"));
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Break(stop);

		runtime.Invoke(ScriptEvent.Update, 0.016f);
		runtime.Invoke((ScriptEvent)scriptEvent, 0.02f, other: TestRuntime.Hero);
		Assert.Single(session.Stops());
		runtime.Environment.SetDeltaTime(0.5f);
		session.Continue();
		Assert.Equal(0.016f, runtime.Environment.DeltaTimeAtLookup);
		Assert.Equal(0.5f, runtime.Environment.DeltaTime);
		Assert.Equal(["update", "stopped", "Entity(7)"], runtime.Records);
	}

	// Targets (spec-slice4 section 3.1): a graph's breakpoints stop the instance of its target entity only; entity 0 or no entry
	// is any instance; a target of another graph changes nothing.
	[Fact]
	public void BreakpointsStopTheirGraphsTargetOrAnyInstance()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		graph.Flow(update, graph.Write("update"));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Environment.Add(8, "Villain");
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(8));
		DebugSession session = new(runtime);
		string breakpoint = session.Breakpoint(update);

		void Frame()
		{
			runtime.Invoke(ScriptEvent.Update);
			runtime.Invoke(ScriptEvent.Update, entity: 8);
		}
		string[] StoppedEntities() => [.. session.Stops().Select(static stop => DebugSession.Text(stop, "Entity"))];

		session.Configure([breakpoint], [DebugSession.Target(TestRuntime.Graph, 8)]);
		Frame();
		Assert.Equal(["8"], StoppedEntities());
		session.ContinueAll();
		session.Configure([breakpoint], [DebugSession.Target(TestRuntime.Graph, 0)]);
		Frame();
		Assert.Equal(["7", "8"], StoppedEntities());
		session.ContinueAll();
		session.Configure([breakpoint], [DebugSession.Target(4242, 8)]);
		Frame();
		Assert.Equal(["7", "8"], StoppedEntities());
		JsonElement all = session.ContinueAll();
		Assert.Equal("Ended", DebugSession.Text(all, "Outcome"));
		Assert.Equal(Enumerable.Repeat("update", 6), runtime.Records);
		// A target that is no instance's matches nothing.
		session.Configure([breakpoint], [DebugSession.Target(TestRuntime.Graph, 9)]);
		Frame();
		Assert.Empty(session.Stops());
	}

	// At most MaxStops stops at once (spec-slice4 section 4.7): later hits run on unsnapshotted and are counted until the next
	// resume.
	[Fact]
	public void BreakpointsBeyondMaxStopsRunOnCounted()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		uint write = graph.Write("update");
		graph.Flow(update, write);
		TestRuntime runtime = TestRuntime.Start(document);
		const int count = VisualScriptDebugger.MaxStops + 6;
		for (ulong entity = 1000; entity < 1000 + count - 1; entity++)
		{
			runtime.Environment.Add(entity, "Enemy");
			Assert.Equal(VisualScriptStatus.Success, runtime.Create(entity));
		}
		DebugSession session = new(runtime);
		session.Break(write);

		foreach (GraphInstance instance in runtime.Runtime.Instances.ToArray())
		{
			runtime.Invoke(ScriptEvent.Update, entity: instance.Entity);
		}
		JsonElement state = session.State();
		Assert.Equal(VisualScriptDebugger.MaxStops, state.GetProperty("Stops").GetArrayLength());
		Assert.Equal(6, state.GetProperty("Passed").GetInt32());
		Assert.Equal(6, runtime.Records.Count);
		Assert.Equal(IdOf(state.GetProperty("Stops")[0]), state.GetProperty("Current").GetInt32());
		JsonElement resumed = session.Continue();
		Assert.Equal(0, session.State().GetProperty("Passed").GetInt32());
		Assert.Equal("Ended", DebugSession.Text(resumed, "Outcome"));
		// The current stop became the oldest one left.
		Assert.Equal(IdOf(session.Stops()[0]), session.State().GetProperty("Current").GetInt32());
		session.ContinueAll();
		Assert.Equal(count, runtime.Records.Count);
	}

	// An instance that dies releases its debug fibers (spec-slice4 sections 4.3 and 4.11): the stop ends; its Event.Destroy passes
	// its breakpoints, read-only: a destroyed instance keeps no fiber.
	[Fact]
	public void KillingAnInstanceReleasesItsDebugFibersAndDestroyEventsPass()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		graph.Flow(update, graph.Write("update"));
		uint destroy = graph.Node("Event.Destroy");
		uint farewell = graph.Write("destroyed");
		graph.Flow(destroy, farewell);
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Break(update, farewell);

		runtime.Invoke(ScriptEvent.Update);
		ulong revision = session.StopRevision;
		GraphInstance instance = runtime.Instance();
		Assert.Single(instance.DebugFibers);
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Destroy));
		Assert.Empty(instance.DebugFibers);
		Assert.Equal(["destroyed"], runtime.Records);
		JsonElement stop = session.SingleStop();
		Assert.Equal("Passed", DebugSession.Text(stop, "Reason"));
		Assert.False(stop.GetProperty("Resumable").GetBoolean());
		Assert.Equal(farewell, DebugSession.NodeOf(stop));
		Assert.Equal("its instance was destroyed, so it cannot wait", DebugSession.Text(stop, "Message"));
		Assert.True(session.StopRevision > revision);
		Assert.Equal("Dismissed", DebugSession.Text(session.Continue(), "Outcome"));
		Assert.Empty(session.Stops());

		// A replaced instance releases them too.
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		runtime.Invoke(ScriptEvent.Update);
		Assert.Single(session.Stops());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		Assert.Empty(session.Stops());
		Assert.Empty(runtime.Environment.Logs);
	}

	// Executions that cannot stop pass their breakpoints with a snapshot (spec-slice4 section 4.4): one nested through native code
	// (a C# member dispatching an event while a graph runs) shows the frames below it with the native boundary between them.
	[Fact]
	public void AnExecutionNestedThroughNativeCodePassesWithTheFramesBelowIt()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint reenter = graph.Node("Call:Strada.ScriptCore.Tests.Probe.Reenter(ulong,int)", inputs: """{ "Entity": "8", "ScriptEvent": 0 }""");
		graph.Flow(graph.Node("Event.Update"), reenter);
		uint create = graph.Node("Event.Create");
		uint created = graph.Write("created");
		graph.Flow(create, created);
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Environment.Add(8, "Villain");
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(8));
		Probe.Runtime = runtime.Runtime;
		DebugSession session = new(runtime);
		session.Break(created);

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update));
		Assert.Equal(["created"], runtime.Records);
		JsonElement stop = session.SingleStop();
		Assert.Equal("Passed", DebugSession.Text(stop, "Reason"));
		Assert.Equal("it runs nested in engine code another execution called", DebugSession.Text(stop, "Message"));
		JsonElement[] frames = [.. stop.GetProperty("Frames").EnumerateArray()];
		Assert.Equal(3, frames.Length);
		Assert.Equal(created, frames[0].GetProperty("Node").GetUInt32());
		Assert.Equal("8", DebugSession.Text(frames[0], "Entity"));
		Assert.Equal("Native", DebugSession.Text(frames[1], "Kind"));
		Assert.Equal(["Kind", "Title"], frames[1].EnumerateObject().Select(static member => member.Name));
		Assert.Equal(reenter, frames[2].GetProperty("Node").GetUInt32());
		Assert.Equal("7", DebugSession.Text(frames[2], "Entity"));
		Assert.Equal(DebugSession.Text(frames[1], "Title"), runtime.Instance().Program.EventGraph.Nodes[runtime.Instance().Program.EventGraph.FindNode(reenter)].Title);
		// A read-only stop blocks nothing: Continue dismisses it.
		Assert.Equal("Dismissed", DebugSession.Text(session.ContinueAll(), "Outcome"));
		Assert.Empty(session.Stops());
	}

	// Break on errors (spec-slice4 section 4.10): an aborting error of a targeted execution makes one read-only error stop at the
	// failing node with its message, a nested one too; without it, none.
	[Theory]
	[InlineData(true)]
	[InlineData(false)]
	public void BreakOnErrorsMakesOneReadOnlyStopAtTheFailingNode(bool breakOnErrors)
	{
		DocumentBuilder document = new();
		uint divisor = document.Variable("Divisor", "int", "0");
		GraphBuilder graph = document.Graph;
		uint send = graph.Node("Event.Send", """{ "Name": "Fail" }""");
		graph.Flow(graph.Node("Event.Create"), send);
		uint fail = graph.Node("Event.Custom", """{ "Name": "Fail" }""");
		uint divide = graph.Node("Operator.Divide", inputs: """{ "A": 10 }""");
		graph.Link(graph.Get(divisor), "Value", divide, "B");
		uint write = graph.Write(divide, "Result");
		graph.Flow(fail, write);
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Configure(breakOnErrors: breakOnErrors);

		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Create));
		string error = runtime.SingleError();
		if (!breakOnErrors)
		{
			Assert.Empty(session.Stops());
			return;
		}
		JsonElement stop = session.SingleStop();
		Assert.Equal("Error", DebugSession.Text(stop, "Reason"));
		Assert.Equal("Error", DebugSession.Text(stop, "Place"));
		Assert.False(stop.GetProperty("Resumable").GetBoolean());
		Assert.Equal(error, DebugSession.Text(stop, "Message"));
		Assert.Equal(TestRuntime.On("Divide", divide) + "integer division by zero", error);
		JsonElement[] frames = [.. stop.GetProperty("Frames").EnumerateArray()];
		Assert.Equal([divide, send], frames.Select(static frame => frame.GetProperty("Node").GetUInt32()));
		Assert.Equal(JsonValueKind.Null, stop.GetProperty("Breakpoint").ValueKind);
		// An error of an execution the target excludes makes none.
		session.Configure(breakOnErrors: true, targets: [DebugSession.Target(TestRuntime.Graph, 8)]);
		Assert.Equal("Dismissed", DebugSession.Text(session.Continue(), "Outcome"));
		runtime.Invoke(ScriptEvent.Create);
		Assert.Empty(session.Stops());
	}

	// What changed while an execution was stopped (spec-slice4 section 4.7): its next stop lists the variables and event-graph
	// outputs something else wrote, never its own writes.
	[Fact]
	public void AStopListsWhatChangedElsewhereWhileItsExecutionWasStopped()
	{
		DocumentBuilder document = new();
		uint score = document.Variable("Score", "int", "0", isPublic: true);
		uint mine = document.Variable("Mine", "int", "0");
		GraphBuilder graph = document.Graph;
		uint hit = graph.Node("Event.Custom", """{ "Name": "Hit", "Parameters": [{ "Name": "Damage", "Type": "float" }] }""");
		uint first = graph.Write("first");
		uint own = graph.Set(mine, "5");
		uint second = graph.Write("second");
		graph.Flow(hit, first);
		graph.Flow(first, own);
		graph.Flow(own, second);
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Break(first, second);

		Assert.True(runtime.Runtime.SendEvent(TestRuntime.Hero, "Hit", [1.0f]));
		JsonElement stop = session.SingleStop();
		Assert.Empty(stop.GetProperty("Changed").EnumerateArray());
		// Something else writes Score and delivers Hit again, overwriting its parameter; that delivery stops too.
		runtime.Runtime.SetVariable(TestRuntime.Hero, "Score", typeof(int), 3);
		Assert.True(runtime.Runtime.SendEvent(TestRuntime.Hero, "Hit", [2.0f]));
		Assert.Equal(2, session.Stops().Length);
		session.Continue(IdOf(stop));
		JsonElement next = session.Stops().Single(stop => DebugSession.NodeOf(stop) == second);
		Assert.Equal([(0u, "Score"), (hit, "Damage")],
			next.GetProperty("Changed").EnumerateArray().Select(static change => (change.GetProperty("Node").GetUInt32(), DebugSession.Text(change, "Name"))));
		Assert.Equal(0, next.GetProperty("ChangedMore").GetInt32());
		Assert.Equal(IdOf(next), session.State().GetProperty("Current").GetInt32());
		session.ContinueAll();
		session.ContinueAll();
		Assert.Empty(session.Stops());
	}

	// Stamps (spec-slice4 section 5.2): a traced run stamps the nodes it evaluates in their frames with its serial; untraced runs
	// make no stamps.
	[Fact]
	public void TracedRunsStampTheNodesTheyEvaluate()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 2 }""");
		graph.Link(graph.Literal("int", "1"), "Value", add, "A");
		uint first = graph.Write(add, "Result");
		uint second = graph.Write("second");
		uint update = graph.Node("Event.Update");
		graph.Flow(update, first);
		graph.Flow(first, second);
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Null(runtime.Instance().Frame.Stamps);
		DebugSession session = new(runtime);
		session.Break(second);

		runtime.Invoke(ScriptEvent.Update);
		DebugStop stop = session.Debugger.Stops[0];
		ulong[] stamps = stop.Leaf.Frames[0].Stamps!;
		CompiledGraph eventGraph = runtime.Instance().Program.EventGraph;
		ulong serial = stop.Serial;
		Assert.Equal(serial, stamps[eventGraph.FindNode(update)]);
		Assert.Equal(serial, stamps[eventGraph.FindNode(add)]);
		Assert.Equal(serial, stamps[eventGraph.FindNode(first)]);
		// The stopped node has not run yet.
		Assert.Equal(0UL, stamps[eventGraph.FindNode(second)]);
		session.Continue();
		Assert.Equal(serial, runtime.Instance().Frame.Stamps![eventGraph.FindNode(second)]);
	}
}
