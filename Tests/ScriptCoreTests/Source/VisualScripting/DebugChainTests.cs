using System;
using System.Linq;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Chains (spec-slice4 sections 4.3 and 4.4): a receiver that Event.Send or Event.Broadcast runs nested stops with its sender,
// which waits inside its Send; continuing the chain finishes the receiver, then the sender; members die apart; depth and
// budget are what they are without the stop; an untraced sender waits for a traced receiver.
public sealed class DebugChainTests
{
	private const ulong Villain = 8;
	private const ulong Third = 9;
	private const ulong ReceiverGraph = 200;
	private const ulong RelayGraph = 300;

	// Hero's sender: Create writes "before", sends Ping to the entity, then writes "after".
	private static DocumentBuilder Sender(ulong to, out uint send)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint before = graph.Write("before");
		send = graph.Node("Event.Send", """{ "Name": "Ping" }""");
		graph.Link(graph.Node("Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)", inputs: $$"""{ "Id": "{{to}}" }"""), "Result", send, "Target");
		graph.Flow(graph.Node("Event.Create"), before);
		graph.Flow(before, send);
		graph.Writes(send, "Then", "after");
		return document;
	}

	// A receiver of Ping that writes "pinged" (the node returned) and "done".
	private static DocumentBuilder Receiver(out uint pinged)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint ping = graph.Node("Event.Custom", """{ "Name": "Ping" }""");
		pinged = graph.Write("pinged");
		graph.Flow(ping, pinged);
		graph.Writes(pinged, "Then", "done");
		return document;
	}

	// Hero sends Ping to Villain, whose receiver stops at "pinged".
	private static (TestRuntime Runtime, DebugSession Session, uint Send, uint Pinged) StoppedChain()
	{
		TestRuntime runtime = TestRuntime.Start(Sender(Villain, out uint send));
		runtime.Environment.Add(Villain, "Villain");
		runtime.LoadWorking(Receiver(out uint pinged).Build(), ReceiverGraph);
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(Villain, ReceiverGraph));
		DebugSession session = new(runtime);
		session.Configure([session.Breakpoint(pinged, ReceiverGraph)]);
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		return (runtime, session, send, pinged);
	}

	// The receiver stops; its sender waits in its Send below it, both debug fibers. Continue finishes the receiver, then the
	// sender after its Send.
	[Fact]
	public void TheSenderWaitsInItsSendAndContinueFinishesTheReceiverThenTheSender()
	{
		(TestRuntime runtime, DebugSession session, uint send, uint pinged) = StoppedChain();

		Assert.Equal(["before"], runtime.Records);
		JsonElement stop = session.SingleStop();
		Assert.Equal("Breakpoint", DebugSession.Text(stop, "Reason"));
		Assert.True(stop.GetProperty("Resumable").GetBoolean());
		Assert.Equal("", DebugSession.Text(stop, "Message"));
		Assert.Equal("8", DebugSession.Text(stop, "Entity"));
		JsonElement[] frames = [.. stop.GetProperty("Frames").EnumerateArray()];
		Assert.Equal([pinged, send], frames.Select(static frame => frame.GetProperty("Node").GetUInt32()));
		Assert.Equal(["Graph", "Delivery"], frames.Select(static frame => DebugSession.Text(frame, "Kind")));
		Assert.Equal(["8", "7"], frames.Select(static frame => DebugSession.Text(frame, "Entity")));
		Assert.Equal("Delivery", DebugSession.Text(stop.GetProperty("Start"), "Kind"));
		Execution sender = Assert.Single(runtime.Instance().DebugFibers);
		Execution receiver = Assert.Single(runtime.Instance(Villain).DebugFibers);
		Assert.Same(receiver, sender.Delivery!.Receiver);
		Assert.Same(sender, receiver.Parent);
		Assert.Equal(sender.Serial, receiver.RootSerial);

		JsonElement resumed = session.Continue();
		Assert.Equal("Ended", DebugSession.Text(resumed, "Outcome"));
		Assert.Equal(["before", "pinged", "done", "after"], runtime.Records);
		Assert.Empty(session.Stops());
		Assert.Empty(runtime.Instance().DebugFibers);
		Assert.Empty(runtime.Instance(Villain).DebugFibers);
		Assert.Empty(runtime.Environment.Logs);
	}

	// A stopped receiver of a Broadcast keeps the broadcast's receivers and arguments: continuing it delivers to the next
	// receiver, which stops in turn, and Count counts every receiver, the stopped ones included.
	[Fact]
	public void ABroadcastContinuesAtTheNextReceiverWithTheRightCount()
	{
		DocumentBuilder sender = new();
		uint broadcast = sender.Graph.Node("Event.Broadcast", """{ "Name": "Hit", "Parameters": [{ "Name": "Damage", "Type": "float" }] }""",
			"""{ "Damage": 1.5 }""");
		sender.Graph.Flow(sender.Graph.Node("Event.Create"), broadcast);
		uint count = sender.Graph.Node("FormatString", """{ "Format": "count {Count}" }""");
		sender.Graph.Link(broadcast, "Count", count, "Count");
		sender.Graph.Flow(broadcast, sender.Graph.Write(count, "Result"));
		DocumentBuilder receiver = new();
		uint hit = receiver.Graph.Node("Event.Custom", """{ "Name": "Hit", "Parameters": [{ "Name": "Damage", "Type": "float" }] }""");
		uint write = receiver.Graph.Write(hit, "Damage");
		receiver.Graph.Flow(hit, write);
		TestRuntime runtime = TestRuntime.Start(sender);
		runtime.LoadWorking(receiver.Build(), ReceiverGraph);
		foreach (ulong entity in (ulong[])[Villain, Third, 10])
		{
			runtime.Environment.Add(entity, FormattableString.Invariant($"Receiver {entity}"));
			Assert.Equal(VisualScriptStatus.Success, runtime.Create(entity, ReceiverGraph));
		}
		DebugSession session = new(runtime);
		session.Configure([session.Breakpoint(write, ReceiverGraph)]);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal("8", DebugSession.Text(session.SingleStop(), "Entity"));
		PendingDelivery delivery = runtime.Instance().DebugFibers[0].Delivery!;
		Assert.Equal(1, delivery.NextReceiver);
		Assert.Equal(1, delivery.Count);
		Assert.NotNull(delivery.Scratch);
		// The second receiver dies before its turn: it is not delivered to, and does not count.
		Assert.Equal(VisualScriptStatus.Success, runtime.Runtime.DestroyInstance(Third));
		JsonElement resumed = session.Continue();
		Assert.Equal("Stopped", DebugSession.Text(resumed, "Outcome"));
		JsonElement next = session.SingleStop();
		Assert.Equal("10", DebugSession.Text(next, "Entity"));
		Assert.Equal(DebugSession.IdOf(next), resumed.GetProperty("Stop").GetInt32());
		Assert.Equal(2, runtime.Instance().DebugFibers[0].Delivery!.Count);
		session.Continue();
		Assert.Equal(["1.5", "1.5", "count 2"], runtime.Records);
		Assert.Empty(session.Stops());
	}

	// A leaf whose instance dies is released; its sender's delivery counts as delivered, and the stop goes on with the sender,
	// which continues after its Send. In a Broadcast the dead receiver still counts.
	[Fact]
	public void ALeafThatDiesLeavesItsSenderToContinueAfterItsSend()
	{
		(TestRuntime runtime, DebugSession session, uint send, _) = StoppedChain();
		ulong revision = session.StopRevision;

		Assert.Equal(VisualScriptStatus.Success, runtime.Runtime.DestroyInstance(Villain));
		JsonElement stop = session.SingleStop();
		Assert.True(session.StopRevision > revision);
		Assert.True(stop.GetProperty("Resumable").GetBoolean());
		Assert.Equal("7", DebugSession.Text(stop, "Entity"));
		Assert.Equal("the receiver was destroyed: this execution continues after its delivery", DebugSession.Text(stop, "Message"));
		JsonElement frame = Assert.Single(stop.GetProperty("Frames").EnumerateArray());
		Assert.Equal(send, frame.GetProperty("Node").GetUInt32());
		Assert.Equal("Delivery", DebugSession.Text(frame, "Kind"));
		session.Continue();
		Assert.Equal(["before", "after"], runtime.Records);
		Assert.Empty(session.Stops());
		Assert.Empty(runtime.Instance().DebugFibers);
	}

	// A sender whose instance dies is released; its receiver becomes the root of its own chain and continues alone with the
	// chain's budget. When a middle member dies, the members below and above it go on as two stops.
	[Fact]
	public void AParentThatDiesLeavesItsReceiverToContinueAlone()
	{
		(TestRuntime runtime, DebugSession session, _, uint pinged) = StoppedChain();
		Execution receiver = runtime.Instance(Villain).DebugFibers[0];
		int steps = receiver.SavedSteps;

		Assert.Equal(VisualScriptStatus.Success, runtime.Runtime.DestroyInstance(TestRuntime.Hero));
		JsonElement stop = session.SingleStop();
		Assert.Equal("the sender was destroyed: this execution continues alone", DebugSession.Text(stop, "Message"));
		Assert.Equal(pinged, Assert.Single(stop.GetProperty("Frames").EnumerateArray().ToArray()).GetProperty("Node").GetUInt32());
		Assert.Null(receiver.Parent);
		Assert.Equal(receiver.Serial, receiver.RootSerial);
		Assert.Equal(steps, receiver.SavedSteps);
		session.Continue();
		Assert.Equal(["before", "pinged", "done"], runtime.Records);
		Assert.Empty(session.Stops());

		// Hero sends to Villain, a relay that sends on to Third; Third stops, and Villain dies.
		TestRuntime chain = TestRuntime.Start(Sender(Villain, out _));
		chain.Environment.Add(Villain, "Villain");
		chain.Environment.Add(Third, "Third");
		DocumentBuilder relay = new();
		uint ping = relay.Graph.Node("Event.Custom", """{ "Name": "Ping" }""");
		uint relayed = relay.Graph.Node("Event.Send", """{ "Name": "Ping" }""");
		relay.Graph.Link(relay.Graph.Node("Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)", inputs: $$"""{ "Id": "{{Third}}" }"""), "Result", relayed, "Target");
		relay.Graph.Flow(ping, relayed);
		chain.LoadWorking(relay.Build(), RelayGraph);
		Assert.Equal(VisualScriptStatus.Success, chain.Create(Villain, RelayGraph));
		chain.LoadWorking(Receiver(out uint thirdPinged).Build(), ReceiverGraph);
		Assert.Equal(VisualScriptStatus.Success, chain.Create(Third, ReceiverGraph));
		DebugSession chained = new(chain);
		chained.Configure([chained.Breakpoint(thirdPinged, ReceiverGraph)]);
		chain.Invoke(ScriptEvent.Create);
		Assert.Equal(3, chained.SingleStop().GetProperty("Frames").GetArrayLength());

		Assert.Equal(VisualScriptStatus.Success, chain.Runtime.DestroyInstance(Villain));
		JsonElement[] stops = chained.Stops();
		Assert.Equal(2, stops.Length);
		Assert.Equal("9", DebugSession.Text(stops[0], "Entity"));
		Assert.Equal("the sender was destroyed: this execution continues alone", DebugSession.Text(stops[0], "Message"));
		Assert.Equal("7", DebugSession.Text(stops[1], "Entity"));
		Assert.Equal("the receiver was destroyed: this execution continues after its delivery", DebugSession.Text(stops[1], "Message"));
		Assert.Equal("Ended", DebugSession.Text(chained.ContinueAll(), "Outcome"));
		Assert.Equal(["before", "pinged", "done", "after"], chain.Records);
		Assert.Empty(chained.Stops());
	}

	// A chain of 64 nested Sends (the depth limit) stops at its bottom and continues as it runs without the stop; one more level
	// fails at the same depth either way. A chain keeps its budget across stops: an endless loop after a breakpoint fails
	// after as many steps as without it.
	[Fact]
	public void ADeepSendChainKeepsItsDepthAndBudgetAcrossStops()
	{
		static bool SendDown(TestRuntime runtime)
		{
			try
			{
				return runtime.Runtime.SendEvent(TestRuntime.Hero, "Down", [1]);
			}
			catch (InvalidOperationException)
			{
				return false;
			}
		}

		foreach (int levels in (int[])[VisualScriptRuntime.MaxDepth, VisualScriptRuntime.MaxDepth + 1])
		{
			(TestRuntime plain, _) = DeepChain(levels);
			Assert.Equal(levels == VisualScriptRuntime.MaxDepth, SendDown(plain));
			(TestRuntime debugged, uint bottom) = DeepChain(levels);
			DebugSession session = new(debugged);
			session.Configure([session.Breakpoint(bottom)]);
			Assert.Equal(levels == VisualScriptRuntime.MaxDepth, SendDown(debugged));
			if (levels == VisualScriptRuntime.MaxDepth)
			{
				JsonElement stop = session.SingleStop();
				Assert.Equal(levels, stop.GetProperty("Frames").GetArrayLength());
				Assert.Empty(debugged.Records);
				Assert.Equal("Ended", DebugSession.Text(session.Continue(), "Outcome"));
			}
			else
			{
				Assert.Empty(session.Stops());
			}
			Assert.Equal(plain.Records, debugged.Records);
			Assert.Equal(plain.Environment.Logs, debugged.Environment.Logs);
		}

		int Loops(bool debugged)
		{
			DocumentBuilder document = new();
			uint count = document.Variable("Count", "int", "0");
			GraphBuilder graph = document.Graph;
			uint send = graph.Node("Event.Send", """{ "Name": "Spin" }""");
			graph.Flow(graph.Node("Event.Create"), send);
			uint spin = graph.Node("Event.Custom", """{ "Name": "Spin" }""");
			uint first = graph.Write("spinning");
			uint loop = graph.Node("Flow.While", inputs: """{ "Condition": true }""");
			uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
			graph.Link(graph.Get(count), "Value", add, "A");
			uint set = graph.Set(count);
			graph.Link(add, "Result", set, "Value");
			graph.Flow(spin, first);
			graph.Flow(first, loop);
			graph.Flow(loop, set, "Body");
			TestRuntime runtime = TestRuntime.Start(document);
			if (debugged)
			{
				DebugSession session = new(runtime);
				session.Configure([session.Breakpoint(first)]);
				Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
				Assert.Single(session.Stops());
				Assert.True(session.Continue().GetProperty("Failed").GetBoolean());
			}
			else
			{
				Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Create));
			}
			Assert.EndsWith("execution exceeded 1,000,000 steps (endless loop?)", runtime.SingleError(), StringComparison.Ordinal);
			return (int)runtime.Variable("Count")!;
		}

		Assert.Equal(Loops(debugged: false), Loops(debugged: true));
	}

	// Hero's graph sends Down to itself with Level + 1 while Level is below levels; the bottom writes "bottom".
	private static (TestRuntime Runtime, uint Bottom) DeepChain(int levels)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint down = graph.Node("Event.Custom", """{ "Name": "Down", "Parameters": [{ "Name": "Level", "Type": "int" }] }""");
		uint less = graph.Node("Operator.Less", inputs: FormattableString.Invariant($$"""{ "B": {{levels}} }"""));
		graph.Link(down, "Level", less, "A");
		uint branch = graph.Node("Flow.Branch");
		graph.Link(less, "Result", branch, "Condition");
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
		graph.Link(down, "Level", add, "A");
		uint send = graph.Node("Event.Send", """{ "Name": "Down", "Parameters": [{ "Name": "Level", "Type": "int" }] }""");
		graph.Link(add, "Result", send, "Level");
		uint bottom = graph.Write("bottom");
		graph.Flow(down, branch);
		graph.Flow(branch, send, "True");
		graph.Flow(branch, bottom, "False");
		return (TestRuntime.Start(document), bottom);
	}

	// An untraced sender (its graph has no breakpoint and no window watches it) under a traced receiver: the receiver stops, the
	// sender waits in its Send, and the sender's frames were never stamped, so its values are of unknown currency.
	[Fact]
	public void AnUntracedSenderWaitsForATracedReceiver()
	{
		(TestRuntime runtime, DebugSession session, uint send, _) = StoppedChain();

		Execution sender = runtime.Instance().DebugFibers[0];
		Assert.Equal(DebugMode.None, sender.Mode);
		Assert.NotEqual(DebugMode.None, runtime.Instance(Villain).DebugFibers[0].Mode);
		DebugStop stop = session.Debugger.Stops[0];
		Assert.Equal(2, stop.Snapshot.Members.Length);
		DebugSnapshotMember waiting = stop.Snapshot.Members[1];
		Assert.True(waiting.Waiting);
		Assert.Equal(send, waiting.Node!.Id);
		Assert.Null(waiting.Frames[0].Stamps);
		Assert.NotNull(stop.Snapshot.Members[0].Frames[0].Stamps);
		session.Continue();
		Assert.Equal(["before", "pinged", "done", "after"], runtime.Records);
	}
}
