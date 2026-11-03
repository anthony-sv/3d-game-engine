using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Live edit while stopped (spec-slice4 sections 4.8 and 7.6): stopped executions migrate to the edited program as fibers do,
// by node ID (Enter and Continue positions, pending deliveries), or are cancelled with the existing Warning when the edit
// removed, retyped or disabled the node or changed a function they are in, or the event a waiting sender raises; edited
// literals of the stopped node are read from the current program, and the execution continues with them.
public sealed class DebugLiveEditTests
{
	private const ulong Villain = 8;
	private const ulong Third = 9;
	private const ulong ReceiverGraph = 200;

	private static string Cancelled(string title, uint node, string problem) =>
		string.Create(CultureInfo.InvariantCulture, $"{TestRuntime.Name} on Hero (7): the stopped execution at {title} (node {node}) is cancelled: {problem}");

	// Update writes "first", then the text into the stopped node (a Probe.Write), in a document built by build.
	private static string Writer(string text, string? enabled = null, bool changeType = false)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		uint first = graph.Write("first");
		uint stopped = changeType ? graph.Node("Call:Strada.ScriptCore.Tests.Probe.Throw(string)") : graph.Node(RuntimeTesting.Write,
			inputs: $$"""{ "Text": {{RuntimeTesting.Quote(text)}} }""", enabled: enabled);
		graph.Flow(update, first);
		graph.Flow(first, stopped);
		graph.Writes(stopped, "Then", "last");
		return document.Build();
	}

	// The writer's stopped node (the third node of the document).
	private const uint StoppedNode = 3;

	// The stopped execution migrates: its stop stays (Migrated, a new StopRevision), the stopped node's literal input reads
	// as edited from the current program, and Step Over runs the edited node.
	[Fact]
	public void AnEditedLiteralReachesTheStoppedExecution()
	{
		TestRuntime runtime = TestRuntime.Start(Writer("old"));
		DebugSession session = new(runtime);
		session.Break(StoppedNode);
		runtime.Invoke(ScriptEvent.Update);
		ulong revision = session.StopRevision;
		DebugStop stop = session.Debugger.Stops[0];
		Assert.Empty(DebugSnapshot.EditedInputs(stop.Leaf.Node!, stop.Execution!.TopFrame.Graph));

		runtime.LoadWorking(Writer("new"));
		JsonElement migrated = session.SingleStop();
		Assert.True(migrated.GetProperty("Migrated").GetBoolean());
		Assert.Equal(stop.Id, DebugSession.IdOf(migrated));
		Assert.True(session.StopRevision > revision);
		Assert.Empty(runtime.Environment.Warnings);
		Assert.Same(runtime.Instance().Program, stop.Execution!.Program);
		DebugEdit edit = Assert.Single(DebugSnapshot.EditedInputs(stop.Leaf.Node!, stop.Execution.TopFrame.Graph));
		Assert.Equal("Text", edit.Pin);
		Assert.Equal("old", edit.Before.Object);
		Assert.Equal("new", edit.After.Object);
		// Describe shows the current program's literal, marked Edited with the snapshot's as Before.
		Assert.Equal("""[{"Name":"Text","Type":"string","Value":"new","Currency":"Current","Edited":true,"Before":{"Value":"old"}}]""",
			session.Describe(stop.Id).GetProperty("Node").GetProperty("Inputs").GetRawText());
		Assert.Equal("Stopped", DebugSession.Text(session.Resume("StepOver"), "Outcome"));
		Assert.Equal(["first", "new"], runtime.Records);
		session.Continue();
		Assert.Equal(["first", "new", "last"], runtime.Records);
	}

	// A Continue position migrates by its flow entry: the Sequence continues with the edited program's next output.
	[Fact]
	public void AContinuePositionMigrates()
	{
		static string Sequenced(string then1)
		{
			DocumentBuilder document = new();
			GraphBuilder graph = document.Graph;
			uint sequence = graph.Node("Flow.Sequence", """{ "Count": 2 }""");
			graph.Flow(graph.Node("Event.Create"), sequence);
			graph.Writes(sequence, "Then0", "then0");
			graph.Writes(sequence, "Then1", then1);
			return document.Build();
		}

		TestRuntime runtime = TestRuntime.Start(Sequenced("old"));
		DebugSession session = new(runtime);
		session.Break(1);
		runtime.Invoke(ScriptEvent.Create);
		session.Resume("StepInto");
		session.Resume("StepInto");
		Assert.Equal("1 Continue next: Then1", session.Position());
		runtime.LoadWorking(Sequenced("new"));
		Assert.True(session.SingleStop().GetProperty("Migrated").GetBoolean());
		session.Continue();
		Assert.Equal(["then0", "new"], runtime.Records);
	}

	// The stopped node removed, retyped or disabled, or a function the execution is in changed its signature: the stopped
	// execution is cancelled with the Warning of fibers, and its stop ends.
	[Fact]
	public void AStoppedExecutionIsCancelledWhenTheEditRemovesWhatItHolds()
	{
		CompiledNode node;
		foreach ((string edited, string problem) in new[]
		{
			(Writer("old", enabled: "Disabled"), "the edit disabled {0}"),
			(Writer("old", changeType: true), "the edit removed or changed {0}"),
		})
		{
			TestRuntime runtime = TestRuntime.Start(Writer("old"));
			DebugSession session = new(runtime);
			session.Break(StoppedNode);
			runtime.Invoke(ScriptEvent.Update);
			node = session.Debugger.Stops[0].Leaf.Node!;
			ulong revision = session.StopRevision;
			runtime.LoadWorking(edited);
			Assert.Empty(session.Stops());
			Assert.True(session.StopRevision > revision);
			Assert.Empty(runtime.Instance().DebugFibers);
			string named = FormattableString.Invariant($"{node.Title} (node {StoppedNode})");
			Assert.Equal([Cancelled(node.Title, StoppedNode, string.Format(CultureInfo.InvariantCulture, problem, named))], runtime.Environment.Warnings);
			Assert.Equal(["first"], runtime.Records);
		}

		// A function's parameter changes type.
		static string Calling(string parameter)
		{
			DocumentBuilder document = new();
			FunctionBuilder greet = document.Function("Greet", inputs: [parameter]);
			greet.Graph.Flow(greet.Entry, greet.Graph.Write("inside"));
			GraphBuilder graph = document.Graph;
			graph.Flow(graph.Node("Event.Create"), graph.Node("Function.Call", greet.Settings));
			return document.Build();
		}

		TestRuntime calling = TestRuntime.Start(Calling("Amount:int"));
		DebugSession stopped = new(calling);
		stopped.Break(3);
		calling.Invoke(ScriptEvent.Create);
		node = stopped.Debugger.Stops[0].Leaf.Node!;
		calling.LoadWorking(Calling("Amount:float"));
		Assert.Empty(stopped.Stops());
		Assert.Equal([Cancelled(node.Title, 3, "the edit changed the function Greet")], calling.Environment.Warnings);
	}

	// A sender waiting in its Broadcast migrates when its node raises the same event in the same way, and is cancelled when
	// the edit renames the event, changes a parameter's type, its Graph or IncludeSelf: its receiver goes on alone.
	[Theory]
	[InlineData(null)]
	[InlineData("""{ "Name": "Hurt", "Parameters": [{ "Name": "Damage", "Type": "float" }] }""")]
	[InlineData("""{ "Name": "Hit", "Parameters": [{ "Name": "Damage", "Type": "int" }] }""")]
	[InlineData("""{ "Name": "Hit", "Parameters": [{ "Name": "Damage", "Type": "float" }], "Graph": "200" }""")]
	[InlineData("""{ "Name": "Hit", "Parameters": [{ "Name": "Damage", "Type": "float" }], "IncludeSelf": false }""")]
	public void AWaitingBroadcastMigratesOnlyWhileItRaisesTheSameEvent(string? edited)
	{
		const string hitSettings = """{ "Name": "Hit", "Parameters": [{ "Name": "Damage", "Type": "float" }] }""";
		static string Broadcasting(string settings)
		{
			DocumentBuilder document = new();
			GraphBuilder graph = document.Graph;
			uint broadcast = graph.Node("Event.Broadcast", settings, """{ "Damage": 2 }""");
			graph.Flow(graph.Node("Event.Create"), broadcast);
			graph.Writes(broadcast, "Then", "after");
			return document.Build();
		}

		DocumentBuilder receiver = new();
		uint hit = receiver.Graph.Node("Event.Custom", hitSettings);
		uint write = receiver.Graph.Write(hit, "Damage");
		receiver.Graph.Flow(hit, write);
		// A Broadcast with Graph names the receivers' graph in its bundle.
		string references = new BundleBuilder().Interface(ReceiverGraph, "Assets/Receiver.sgraph", receiver).Build();
		TestRuntime runtime = new();
		runtime.LoadWorking(Broadcasting(hitSettings), references: references);
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		runtime.LoadWorking(receiver.Build(), ReceiverGraph);
		foreach (ulong entity in (ulong[])[Villain, Third])
		{
			runtime.Environment.Add(entity, "Receiver");
			Assert.Equal(VisualScriptStatus.Success, runtime.Create(entity, ReceiverGraph));
		}
		DebugSession session = new(runtime);
		session.Configure([session.Breakpoint(write, ReceiverGraph)]);
		runtime.Invoke(ScriptEvent.Create);
		CompiledNode broadcastNode = runtime.Instance().Program.EventGraph.Nodes[runtime.Instance().Program.EventGraph.FindNode(1)];

		runtime.LoadWorking(Broadcasting(edited ?? hitSettings), references: references);
		if (edited == null)
		{
			Assert.Empty(runtime.Environment.Warnings);
			Assert.True(session.SingleStop().GetProperty("Migrated").GetBoolean());
			session.ContinueAll();
			session.ContinueAll();
			Assert.Equal(["2", "2", "after"], runtime.Records);
			return;
		}
		string named = FormattableString.Invariant($"{broadcastNode.Title} (node 1)");
		Assert.Equal([Cancelled(broadcastNode.Title, 1, $"the edit changed the event {named} raises")], runtime.Environment.Warnings);
		JsonElement stop = session.SingleStop();
		Assert.Equal("8", DebugSession.Text(stop, "Entity"));
		Assert.Equal("the sender was cancelled: this execution continues alone", DebugSession.Text(stop, "Message"));
		Assert.Equal("Ended", DebugSession.Text(session.Continue(), "Outcome"));
		Assert.Equal(["2"], runtime.Records);
		Assert.Empty(session.Stops());
	}

	// A chain whose members' graphs are both edited migrates member by member and continues with the edited programs.
	[Fact]
	public void AChainMigratesAcrossEditsOfItsGraphs()
	{
		static string Sender(string after)
		{
			DocumentBuilder document = new();
			GraphBuilder graph = document.Graph;
			uint send = graph.Node("Event.Send", """{ "Name": "Ping" }""");
			graph.Link(graph.Node("Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)", inputs: """{ "Id": "8" }"""), "Result", send, "Target");
			graph.Flow(graph.Node("Event.Create"), send);
			graph.Writes(send, "Then", after);
			return document.Build();
		}

		static string Receiver(string done)
		{
			DocumentBuilder document = new();
			GraphBuilder graph = document.Graph;
			uint ping = graph.Node("Event.Custom", """{ "Name": "Ping" }""");
			uint pinged = graph.Write("pinged");
			graph.Flow(ping, pinged);
			graph.Writes(pinged, "Then", done);
			return document.Build();
		}

		TestRuntime runtime = TestRuntime.Start(Sender("after"));
		runtime.Environment.Add(Villain, "Villain");
		runtime.LoadWorking(Receiver("done"), ReceiverGraph);
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(Villain, ReceiverGraph));
		DebugSession session = new(runtime);
		session.Configure([session.Breakpoint(2, ReceiverGraph)]);
		runtime.Invoke(ScriptEvent.Create);
		Execution sender = runtime.Instance().DebugFibers[0];
		GraphProgram before = sender.Program;

		runtime.LoadWorking(Sender("edited after"));
		runtime.LoadWorking(Receiver("edited done"), ReceiverGraph);
		Assert.Empty(runtime.Environment.Warnings);
		Assert.NotSame(before, sender.Program);
		Assert.Same(sender, runtime.Instance().DebugFibers[0]);
		JsonElement stop = session.SingleStop();
		Assert.True(stop.GetProperty("Migrated").GetBoolean());
		Assert.Equal(2, stop.GetProperty("Frames").GetArrayLength());
		session.Continue();
		Assert.Equal(["pinged", "edited done", "edited after"], runtime.Records);
	}
}
