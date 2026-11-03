using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Steps, Break and Run to Node (spec-slice4 sections 2.5, 4.5 and 4.6): what Step Into, Over and Out do across each construct
// (Positions are "<node> <place>[ <note>]" of each stop reached in turn), Break with and without a graph, the one-shot Run to
// Node, and the step request's changes of StopRevision.
public sealed class DebugStepTests
{
	private const ulong Villain = 8;
	private const ulong Window = 9;
	private const ulong ReceiverGraph = 200;

	private static string At(uint node, string place = "Enter", string note = "") =>
		string.Create(CultureInfo.InvariantCulture, $"{node} {place}{(note.Length > 0 ? " " + note : "")}");

	// Create runs A, B, C; the session stops at the node first.
	private static (TestRuntime Runtime, DebugSession Session, uint[] Nodes) Plain(int first = 0)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint a = graph.Write("a");
		uint b = graph.Write("b");
		uint c = graph.Write("c");
		graph.Flow(graph.Node("Event.Create"), a);
		graph.Flow(a, b);
		graph.Flow(b, c);
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		uint[] nodes = [a, b, c];
		session.Break(nodes[first]);
		runtime.Invoke(ScriptEvent.Create);
		return (runtime, session, nodes);
	}

	// Plain impure nodes: each step runs one and stops at the next; the last one ends the execution, and the step with it.
	[Theory]
	[InlineData("StepInto")]
	[InlineData("StepOver")]
	public void APlainNodeRunsAndTheStepStopsWhereItsExitLeads(string action)
	{
		(TestRuntime runtime, DebugSession session, uint[] nodes) = Plain();
		(List<string> positions, string outcome) = session.Walk(action);
		Assert.Equal([At(nodes[0]), At(nodes[1]), At(nodes[2])], positions);
		Assert.Equal("Ended", outcome);
		Assert.Equal(["a", "b", "c"], runtime.Records);
		Assert.Equal(JsonValueKind.Null, session.State().GetProperty("Stepping").ValueKind);
		Assert.Empty(session.Stops());
	}

	// A stop reached by a step is a new stop of reason Step, current; Step Out of the event graph runs the execution to its end.
	[Fact]
	public void AStepMakesANewStopAndStepOutAtTheTopRunsToTheEnd()
	{
		(TestRuntime runtime, DebugSession session, uint[] nodes) = Plain();
		int first = DebugSession.IdOf(session.SingleStop());
		JsonElement resumed = session.Resume("StepOver");
		JsonElement stop = session.SingleStop();
		Assert.Equal("Stopped", DebugSession.Text(resumed, "Outcome"));
		Assert.Equal("Step", DebugSession.Text(stop, "Reason"));
		Assert.NotEqual(first, DebugSession.IdOf(stop));
		Assert.Equal(DebugSession.IdOf(stop), session.State().GetProperty("Current").GetInt32());
		Assert.Equal(JsonValueKind.Null, stop.GetProperty("Breakpoint").ValueKind);
		Assert.Equal(nodes[1], DebugSession.NodeOf(stop));
		Assert.Equal("Ended", DebugSession.Text(session.Resume("StepOut"), "Outcome"));
		Assert.Equal(["a", "b", "c"], runtime.Records);
	}

	// Function calls: Into enters the callee at its Entry, Over runs the body and stops after Then, Out from inside runs to the
	// return and stops at the caller's next stop point; an explicit and an implicit return go back to the caller.
	[Fact]
	public void FunctionCallsAreEnteredRunOverAndReturnedFrom()
	{
		DocumentBuilder document = new();
		FunctionBuilder greet = document.Function("Greet");
		uint inside = greet.Graph.Write("inside");
		uint returned = greet.Graph.Node("Function.Return");
		greet.Graph.Flow(greet.Entry, inside);
		greet.Graph.Flow(inside, returned);
		FunctionBuilder quiet = document.Function("Quiet");
		uint whisper = quiet.Graph.Write("whisper");
		quiet.Graph.Flow(quiet.Entry, whisper);
		GraphBuilder graph = document.Graph;
		uint call = graph.Node("Function.Call", greet.Settings);
		uint after = graph.Write("after");
		uint second = graph.Node("Function.Call", quiet.Settings);
		uint last = graph.Write("last");
		graph.Flow(graph.Node("Event.Create"), call);
		graph.Flow(call, after);
		graph.Flow(after, second);
		graph.Flow(second, last);

		(TestRuntime runtime, DebugSession session) = Started(document, call);
		AssertWalk(session, "StepInto", [At(call), At(greet.Entry), At(inside), At(returned), At(after), At(second), At(quiet.Entry), At(whisper), At(last)], "Ended");
		Assert.Equal("Call", DebugSession.Text(DebugSession.Frame(Stopped(document, call, "StepInto").SingleStop()), "Kind"));

		(runtime, session) = Started(document, call);
		AssertWalk(session, "StepOver", [At(call), At(after), At(second), At(last)], "Ended");
		Assert.Equal(["inside", "after", "whisper", "last"], runtime.Records);

		(_, session) = Started(document, inside, whisper);
		Assert.Equal("Stopped", DebugSession.Text(session.Resume("StepOut"), "Outcome"));
		Assert.Equal(At(after), session.Position());
		session.Resume("StepOver");
		session.Resume("StepOver");
		Assert.Equal(At(whisper), session.Position());
		// The implicit return of Quiet: the caller's next stop point.
		Assert.Equal("Stopped", DebugSession.Text(session.Resume("StepInto"), "Outcome"));
		Assert.Equal(At(last), session.Position());
	}

	// A library's function is entered like the graph's own: Into stops at its Entry, in a frame of the library's document.
	[Fact]
	public void StepIntoEntersALibrarysFunction()
	{
		MotionLibrary library = new();
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint call = graph.Node("Function.Call", CrossGraphTesting.Call(library.Report, CrossGraphTesting.Motion), """{ "Text": "hello" }""");
		uint after = graph.Write("after");
		graph.Flow(graph.Node("Event.Create"), call);
		graph.Flow(call, after);
		TestRuntime runtime = new();
		runtime.LoadWorking(document.Build(), references: library.Bundle());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		DebugSession session = new(runtime);
		session.Break(call);
		runtime.Invoke(ScriptEvent.Create);

		session.Resume("StepInto");
		JsonElement frame = DebugSession.Frame(session.SingleStop());
		Assert.Equal(library.Report.Entry, frame.GetProperty("Node").GetUInt32());
		Assert.Equal(CrossGraphTesting.Motion.ToString(CultureInfo.InvariantCulture), DebugSession.Text(frame, "Document"));
		AssertWalk(session, "StepOver", [At(library.Report.Entry), At(library.Report.Entry + 1), At(after)], "Ended");
		Assert.Equal(["hello", "after"], runtime.Records);
	}

	// Sequence and the loops: the body's nodes, then the Continue position where the next output, iteration or check comes,
	// and Completed; a loop's Break input ends its body.
	[Fact]
	public void SequencesAndLoopsStopAtTheirContinuePositions()
	{
		DocumentBuilder document = new();
		uint counter = document.Variable("Counter", "int", "0");
		GraphBuilder graph = document.Graph;
		uint sequence = graph.Node("Flow.Sequence", """{ "Count": 2 }""");
		uint then0 = graph.Write("then0");
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 0, "Last": 1 }""");
		uint body = graph.Write("body");
		uint each = graph.Node("Flow.ForEach");
		graph.Link(graph.Node("Call:Strada.ScriptCore.Tests.Probe.Numbers(int)", inputs: """{ "Count": 2 }"""), "Result", each, "Array");
		uint element = graph.Write(each, "Element");
		uint loopWhile = graph.Node("Flow.While");
		uint less = graph.Node("Operator.Less", inputs: """{ "B": 2 }""");
		graph.Link(graph.Get(counter), "Value", less, "A");
		graph.Link(less, "Result", loopWhile, "Condition");
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
		graph.Link(graph.Get(counter), "Value", add, "A");
		uint set = graph.Set(counter);
		graph.Link(add, "Result", set, "Value");
		uint broken = graph.Node("Flow.ForLoop", inputs: """{ "First": 0, "Last": 5 }""");
		uint once = graph.Write("once");
		uint done = graph.Write("done");
		graph.Flow(graph.Node("Event.Create"), sequence);
		graph.Flow(sequence, then0, "Then0");
		graph.Flow(sequence, loop, "Then1");
		graph.Flow(loop, body, "Body");
		graph.Flow(loop, each, "Completed");
		graph.Flow(each, element, "Body");
		graph.Flow(each, loopWhile, "Completed");
		graph.Flow(loopWhile, set, "Body");
		graph.Flow(loopWhile, broken, "Completed");
		graph.Flow(broken, once, "Body");
		graph.Flow(once, broken, "Then", "Break");
		graph.Flow(broken, done, "Completed");

		(TestRuntime runtime, DebugSession session) = Started(document, sequence);
		(List<string> positions, string outcome) = session.Walk("StepInto");
		Assert.Equal(
		[
			At(sequence), At(then0), At(sequence, "Continue", "next: Then1"),
			At(loop), At(body), At(loop, "Continue", "next iteration: Index 1"), At(body), At(loop, "Continue", "done: Completed"),
			At(each), At(element), At(each, "Continue", "next iteration: Index 1"), At(element), At(each, "Continue", "done: Completed"),
			At(loopWhile), At(set), At(loopWhile, "Continue", "checks its condition"), At(set), At(loopWhile, "Continue", "checks its condition"),
			At(broken), At(once), At(broken), At(broken, "Continue", "done: Completed"), At(done),
		], positions);
		Assert.Equal("Ended", outcome);
		Assert.Equal(["then0", "body", "body", "0", "1", "once", "done"], runtime.Records);
		// Step Over walks the same: none of them calls a function.
		(_, session) = Started(document, sequence);
		Assert.Equal(positions, session.Walk("StepOver").Positions);
		// The Break input entered the loop: its pin is Break.
		(_, session) = Started(document, once);
		session.Resume("StepInto");
		Assert.Equal("Break", DebugSession.Text(DebugSession.Frame(session.SingleStop()), "Pin"));
	}

	// Step Over a latent node: the root suspends (Waiting), the step waits in its fiber, and stops at the first stop point after
	// it resumes; a fiber discarded meanwhile ends the step. Each change of the step request changes StopRevision.
	[Fact]
	public void AStepOverALatentNodeWaitsForItsFiber()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.5 }""");
		uint after = graph.Write("after");
		graph.Flow(graph.Node("Event.Create"), delay);
		graph.Flow(delay, after, "Completed");
		(TestRuntime runtime, DebugSession session) = Started(document, delay);
		int stop = DebugSession.IdOf(session.SingleStop());
		ulong revision = session.StopRevision;

		JsonElement resumed = session.Resume("StepOver");
		Assert.Equal("Waiting", DebugSession.Text(resumed, "Outcome"));
		Assert.Empty(session.Stops());
		Assert.True(session.StopRevision > revision);
		JsonElement stepping = session.State().GetProperty("Stepping");
		Assert.Equal(stop, stepping.GetProperty("Stop").GetInt32());
		Assert.Equal("StepOver", DebugSession.Text(stepping, "Action"));
		JsonElement waits = stepping.GetProperty("WaitsIn");
		Assert.Equal(delay, waits.GetProperty("Node").GetUInt32());
		Assert.Equal("100", DebugSession.Text(waits, "Document"));
		Assert.Equal("7", DebugSession.Text(waits, "Entity"));
		Assert.Equal(0u, waits.GetProperty("Function").GetUInt32());
		revision = session.StopRevision;
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Empty(session.Stops());
		Assert.Equal(revision, session.StopRevision);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		JsonElement reached = session.SingleStop();
		Assert.Equal("Step", DebugSession.Text(reached, "Reason"));
		Assert.Equal(after, DebugSession.NodeOf(reached));
		Assert.Equal(JsonValueKind.Null, session.State().GetProperty("Stepping").ValueKind);
		session.Continue();
		Assert.Equal(["after"], runtime.Records);

		// A RetriggerableDelay's next trigger discards the waiting fiber: the step ends.
		document = new();
		graph = document.Graph;
		uint retriggered = graph.Node("Flow.RetriggerableDelay", inputs: """{ "Duration": 0.5 }""");
		graph.Flow(graph.Node("Event.Create"), retriggered);
		graph.Flow(graph.Node("Event.FixedUpdate"), retriggered);
		graph.Writes(retriggered, "Completed", "after");
		(runtime, session) = Started(document, retriggered);
		Assert.Equal("Waiting", DebugSession.Text(session.Resume("StepOver"), "Outcome"));
		session.Configure();
		revision = session.StopRevision;
		runtime.Invoke(ScriptEvent.FixedUpdate, 0.02f);
		Assert.True(session.StopRevision > revision);
		Assert.Equal(JsonValueKind.Null, session.State().GetProperty("Stepping").ValueKind);
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		Assert.Empty(session.Stops());
		Assert.Equal(["after"], runtime.Records);

		// Break's cancel cancels a waiting step; the instance's death ends one.
		(runtime, session) = Started(document, retriggered);
		session.Resume("StepOver");
		revision = session.StopRevision;
		session.CancelBreak();
		Assert.True(session.StopRevision > revision);
		Assert.Equal(JsonValueKind.Null, session.State().GetProperty("Stepping").ValueKind);
		(runtime, session) = Started(document, retriggered);
		session.Resume("StepOver");
		revision = session.StopRevision;
		Assert.Equal(VisualScriptStatus.Success, runtime.Runtime.DestroyInstance(TestRuntime.Hero));
		Assert.True(session.StopRevision > revision);
		Assert.Equal(JsonValueKind.Null, session.State().GetProperty("Stepping").ValueKind);
	}

	// A receiver that suspends ends its synchronous part: the step goes on in its sender, where Over stops after the Send.
	// A Tween's ticks run as executions of their own, never followed.
	[Fact]
	public void ALatentReceiverReturnsToItsSenderAndTicksAreNotFollowed()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint send = graph.Node("Event.Send", """{ "Name": "Ping" }""");
		uint after = graph.Write("after");
		graph.Flow(graph.Node("Event.Create"), send);
		graph.Flow(send, after);
		uint ping = graph.Node("Event.Custom", """{ "Name": "Ping" }""");
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.5 }""");
		graph.Flow(ping, delay);
		graph.Writes(delay, "Completed", "resumed");
		(TestRuntime runtime, DebugSession session) = Started(document, delay);
		Assert.Equal("Stopped", DebugSession.Text(session.Resume("StepOver"), "Outcome"));
		Assert.Equal(At(after), session.Position());
		session.Continue();
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		Assert.Equal(["after", "resumed"], runtime.Records);
		Assert.Empty(session.Stops());

		document = new();
		graph = document.Graph;
		uint tween = graph.Node("Flow.Tween", inputs: """{ "Duration": 0.5 }""");
		uint ticked = graph.Write("tick");
		uint completed = graph.Write("completed");
		graph.Flow(graph.Node("Event.Create"), tween);
		graph.Flow(tween, ticked, "Update");
		graph.Flow(tween, completed, "Completed");
		(runtime, session) = Started(document, tween);
		Assert.Equal("Waiting", DebugSession.Text(session.Resume("StepInto"), "Outcome"));
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Empty(session.Stops());
		Assert.Equal(["tick"], runtime.Records);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(At(completed), session.Position());
		Assert.Equal(["tick", "tick"], runtime.Records);
	}

	// Executions a member starts through native code run through as part of the step, their breakpoints passed with a
	// snapshot; an instruction that aborts ends the chain with its error.
	[Fact]
	public void NativeNestedExecutionsRunThroughAndAnAbortEndsTheChain()
	{
		DocumentBuilder document = new();
		uint divisor = document.Variable("Divisor", "int", "0");
		GraphBuilder graph = document.Graph;
		uint reenter = graph.Node("Call:Strada.ScriptCore.Tests.Probe.Reenter(ulong,int)", inputs: """{ "Entity": "8", "ScriptEvent": 0 }""");
		uint after = graph.Write("after");
		graph.Flow(graph.Node("Event.Update"), reenter);
		graph.Flow(reenter, after);
		uint created = graph.Write("created");
		graph.Flow(graph.Node("Event.Create"), created);
		uint divide = graph.Node("Operator.Divide", inputs: """{ "A": 10 }""");
		graph.Link(graph.Get(divisor), "Value", divide, "B");
		uint failing = graph.Write(divide, "Result");
		graph.Flow(graph.Node("Event.FixedUpdate"), failing);
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Environment.Add(Villain, "Villain");
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(Villain));
		Probe.Runtime = runtime.Runtime;
		DebugSession session = new(runtime);
		session.Break(reenter, created, failing);
		runtime.Invoke(ScriptEvent.Update);

		Assert.Equal("Stopped", DebugSession.Text(session.Resume("StepInto"), "Outcome"));
		JsonElement[] stops = session.Stops();
		Assert.Equal(["Passed", "Step"], [DebugSession.Text(stops[0], "Reason"), DebugSession.Text(stops[1], "Reason")]);
		Assert.Equal(created, DebugSession.NodeOf(stops[0]));
		Assert.Equal(after, DebugSession.NodeOf(stops[1]));
		session.ContinueAll();
		Assert.Equal(["created", "after"], runtime.Records);

		runtime.Invoke(ScriptEvent.FixedUpdate);
		JsonElement resumed = session.Resume("StepInto");
		Assert.Equal("Ended", DebugSession.Text(resumed, "Outcome"));
		Assert.True(resumed.GetProperty("Failed").GetBoolean());
		Assert.Equal(TestRuntime.On("Divide", divide) + "integer division by zero", runtime.SingleError());
		Assert.Empty(session.Stops());
		Assert.Equal(JsonValueKind.Null, session.State().GetProperty("Stepping").ValueKind);
	}

	// Event.Send: Into stops at the receiver's Event.Custom, tracing it although its instance is not a target; Over runs the
	// receiver and stops after Then; Out from the receiver runs the rest of it and stops at the sender's next stop point.
	[Fact]
	public void StepsFollowASendIntoItsReceiverAndBack()
	{
		DocumentBuilder sender = new();
		uint send = sender.Graph.Node("Event.Send", """{ "Name": "Ping" }""");
		sender.Graph.Link(sender.Graph.Node("Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)", inputs: """{ "Id": "8" }"""), "Result", send, "Target");
		uint after = sender.Graph.Write("after");
		sender.Graph.Flow(sender.Graph.Node("Event.Create"), send);
		sender.Graph.Flow(send, after);
		DocumentBuilder receiver = new();
		uint ping = receiver.Graph.Node("Event.Custom", """{ "Name": "Ping" }""");
		uint pinged = receiver.Graph.Write("pinged");
		uint done = receiver.Graph.Write("done");
		receiver.Graph.Flow(ping, pinged);
		receiver.Graph.Flow(pinged, done);

		(TestRuntime, DebugSession) Start()
		{
			TestRuntime runtime = TestRuntime.Start(sender);
			runtime.Environment.Add(Villain, "Villain");
			runtime.LoadWorking(receiver.Build(), ReceiverGraph);
			Assert.Equal(VisualScriptStatus.Success, runtime.Create(Villain, ReceiverGraph));
			DebugSession session = new(runtime);
			// Villain is no target: only entity 99 runs the receiver's breakpoints.
			session.Configure([session.Breakpoint(send)], [DebugSession.Target(ReceiverGraph, 99)]);
			runtime.Invoke(ScriptEvent.Create);
			return (runtime, session);
		}

		(TestRuntime runtime, DebugSession session) = Start();
		AssertWalk(session, "StepInto", [At(send), At(ping), At(pinged), At(done), At(after)], "Ended");
		Assert.Equal(["pinged", "done", "after"], runtime.Records);
		(_, session) = Start();
		AssertWalk(session, "StepOver", [At(send), At(after)], "Ended");
		(_, session) = Start();
		session.Resume("StepInto");
		Assert.Equal("8", DebugSession.Text(session.SingleStop(), "Entity"));
		AssertWalk(session, "StepOut", [At(ping), At(after)], "Ended");
	}

	// Event.Broadcast: Into stops at each receiver's Event.Custom in creation order, then at the broadcaster's next stop point;
	// Over runs every receiver; Out from a receiver runs the remaining ones.
	[Fact]
	public void StepsFollowABroadcastThroughEachReceiver()
	{
		DocumentBuilder sender = new();
		uint broadcast = sender.Graph.Node("Event.Broadcast", """{ "Name": "Hit" }""");
		uint after = sender.Graph.Write("after");
		sender.Graph.Flow(sender.Graph.Node("Event.Create"), broadcast);
		sender.Graph.Flow(broadcast, after);
		DocumentBuilder receiver = new();
		uint hit = receiver.Graph.Node("Event.Custom", """{ "Name": "Hit" }""");
		uint write = receiver.Graph.Write("hit");
		receiver.Graph.Flow(hit, write);

		DebugSession Start()
		{
			TestRuntime runtime = TestRuntime.Start(sender);
			runtime.LoadWorking(receiver.Build(), ReceiverGraph);
			foreach (ulong entity in (ulong[])[Villain, Window])
			{
				runtime.Environment.Add(entity, "Receiver");
				Assert.Equal(VisualScriptStatus.Success, runtime.Create(entity, ReceiverGraph));
			}
			DebugSession session = new(runtime);
			session.Break(broadcast);
			runtime.Invoke(ScriptEvent.Create);
			return session;
		}

		DebugSession session = Start();
		AssertWalk(session, "StepInto", [At(broadcast), At(hit), At(write), At(hit), At(write), At(after)], "Ended");
		session = Start();
		AssertWalk(session, "StepOver", [At(broadcast), At(after)], "Ended");
		session = Start();
		session.Resume("StepInto");
		Assert.Equal("8", DebugSession.Text(session.SingleStop(), "Entity"));
		AssertWalk(session, "StepOut", [At(hit), At(after)], "Ended");
	}

	// Break stops the first stop point of a targeted execution that can stop, once; with a script graph only executions of an
	// instance running it, with a library only stop points at its nodes; a cancel disarms it.
	[Fact]
	public void BreakStopsTheNextStopPointOfItsGraph()
	{
		DocumentBuilder document = new();
		uint update = document.Graph.Node("Event.Update");
		document.Graph.Flow(update, document.Graph.Write("update"));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Environment.Add(Villain, "Villain");
		runtime.LoadWorking(document.Build(), ReceiverGraph);
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(Villain, ReceiverGraph));
		DebugSession session = new(runtime);

		session.BreakNext();
		runtime.Invoke(ScriptEvent.Update);
		JsonElement stop = session.SingleStop();
		Assert.Equal("Break", DebugSession.Text(stop, "Reason"));
		Assert.Equal(update, DebugSession.NodeOf(stop));
		session.Continue();
		runtime.Invoke(ScriptEvent.Update);
		Assert.Empty(session.Stops());

		session.BreakNext(ReceiverGraph);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Empty(session.Stops());
		runtime.Invoke(ScriptEvent.Update, entity: Villain);
		Assert.Equal("8", DebugSession.Text(session.SingleStop(), "Entity"));
		session.Continue();

		session.BreakNext();
		session.CancelBreak();
		runtime.Invoke(ScriptEvent.Update);
		Assert.Empty(session.Stops());

		MotionLibrary library = new();
		DocumentBuilder caller = new();
		uint call = caller.Graph.Node("Function.Call", CrossGraphTesting.Call(library.Report, CrossGraphTesting.Motion), """{ "Text": "hello" }""");
		caller.Graph.Flow(caller.Graph.Node("Event.Create"), caller.Graph.Write("before"));
		caller.Graph.Flow(caller.Graph.Node("Event.Update"), call);
		TestRuntime calling = new();
		calling.LoadWorking(caller.Build(), references: library.Bundle());
		Assert.Equal(VisualScriptStatus.Success, calling.Create());
		DebugSession breaking = new(calling);
		breaking.BreakNext(CrossGraphTesting.Motion);
		calling.Invoke(ScriptEvent.Create);
		Assert.Empty(breaking.Stops());
		calling.Invoke(ScriptEvent.Update);
		JsonElement inLibrary = DebugSession.Frame(breaking.SingleStop());
		Assert.Equal(library.Report.Entry, inLibrary.GetProperty("Node").GetUInt32());
		Assert.Equal(CrossGraphTesting.Motion.ToString(CultureInfo.InvariantCulture), DebugSession.Text(inLibrary, "Document"));
	}

	// Run to Node: a one-shot breakpoint, removed at the next stop of any kind; on a pure node it stops the node that reads it.
	// A node no loaded program has is refused.
	[Fact]
	public void RunToNodeStopsOnceAtItsNode()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint a = graph.Write("a");
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 2 }""");
		graph.Link(graph.Literal("int", "1"), "Value", add, "A");
		uint b = graph.Write(add, "Result");
		uint c = graph.Write("c");
		graph.Flow(graph.Node("Event.Create"), a);
		graph.Flow(a, b);
		graph.Flow(b, c);
		(TestRuntime runtime, DebugSession session) = Started(document, a);

		session.RunTo(c);
		Assert.Equal("Stopped", DebugSession.Text(session.Continue(), "Outcome"));
		JsonElement stop = session.SingleStop();
		Assert.Equal("RunTo", DebugSession.Text(stop, "Reason"));
		Assert.Equal(c, stop.GetProperty("Breakpoint").GetProperty("Node").GetUInt32());
		Assert.Equal(c, DebugSession.NodeOf(stop));
		session.Continue();
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal("Ended", DebugSession.Text(session.Continue(), "Outcome"));

		runtime.Invoke(ScriptEvent.Create);
		session.RunTo(add);
		session.Continue();
		Assert.Equal(b, DebugSession.NodeOf(session.SingleStop()));
		// Any stop removes it: a step reaching its node first.
		session.RunTo(c);
		session.Resume("StepOver");
		Assert.Equal("Step", DebugSession.Text(session.SingleStop(), "Reason"));
		session.Continue();
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal("Ended", DebugSession.Text(session.Continue(), "Outcome"));
		Assert.Empty(session.Stops());
		Assert.Equal(("InvalidState", "Node: no loaded program has the node 999 of the document 100"),
			session.Refused("""{ "Command": "RunTo", "Document": "100", "Node": 999 }"""));
	}

	// The positions an action walks from the current stop, and the outcome it ends with.
	private static void AssertWalk(DebugSession session, string action, List<string> positions, string outcome)
	{
		(List<string> walked, string ended) = session.Walk(action);
		Assert.Equal(positions, walked);
		Assert.Equal(outcome, ended);
	}

	// A runtime running the document whose session stops at the first of the nodes when Create runs.
	private static (TestRuntime Runtime, DebugSession Session) Started(DocumentBuilder document, params uint[] breakpoints)
	{
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Break(breakpoints);
		runtime.Invoke(ScriptEvent.Create);
		Assert.NotEmpty(session.Stops());
		return (runtime, session);
	}

	// The session after one step of the action from the stop at the node.
	private static DebugSession Stopped(DocumentBuilder document, uint node, string action)
	{
		(_, DebugSession session) = Started(document, node);
		session.Resume(action);
		return session;
	}
}
