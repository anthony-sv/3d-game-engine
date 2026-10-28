using System;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Typed sends (spec-slice3 section 4.3): a Send with Graph reaches only a receiver running that graph and skips one running
// another graph with its node's once-per-instance warning; a Send without Graph reaches any graph with the event.
public sealed class TypedSendTests
{
	private const string Hit = """{ "Name": "Hit", "Parameters": [{ "Name": "Damage", "Type": "float" }] }""";
	private const ulong Window = 102;
	private const string WindowName = "Scripts/Window.sgraph";

	// Hero sends Hit (typed to Door, or untyped) to Door (entity 8, running Door) and to Window (entity 9, running another
	// graph with the same event) on every Update.
	private static TestRuntime Start(bool typed)
	{
		DocumentBuilder hero = new();
		string settings = typed ? CrossGraphTesting.Send("Hit", CrossGraphTesting.Door, """{ "Name": "Damage", "Type": "float" }""") : Hit;
		uint toDoor = hero.Graph.Node("Event.Send", settings, """{ "Damage": 1.5 }""");
		uint toWindow = hero.Graph.Node("Event.Send", settings, """{ "Damage": 2.5 }""");
		hero.Graph.Link(hero.Graph.Node("Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)", inputs: """{ "Id": "8" }"""), "Result", toDoor, "Target");
		hero.Graph.Link(hero.Graph.Node("Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)", inputs: """{ "Id": "9" }"""), "Result", toWindow, "Target");
		hero.Graph.Flow(hero.Graph.Node("Event.Update"), toDoor);
		hero.Graph.Flow(toDoor, toWindow);
		hero.Graph.Writes(toWindow, "Then", "sent");
		TestRuntime runtime = new();
		runtime.Environment.Add(8, "Door");
		runtime.Environment.Add(9, "Window");
		runtime.LoadWorking(CrossGraphTesting.DoorScript().Build(), CrossGraphTesting.Door, name: CrossGraphTesting.DoorName);
		runtime.LoadWorking(CrossGraphTesting.DoorScript().Build(), Window, name: WindowName);
		runtime.LoadWorking(hero.Build(), references: new BundleBuilder()
			.Interface(CrossGraphTesting.Door, CrossGraphTesting.DoorName, CrossGraphTesting.DoorScript()).Build());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(8, CrossGraphTesting.Door));
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(9, Window));
		return runtime;
	}

	[Fact]
	public void ATypedSendReachesOnlyReceiversRunningItsGraph()
	{
		TestRuntime runtime = Start(typed: true);
		uint toWindow = runtime.Instance().Program.EventGraph.Nodes[1].Id;

		runtime.Invoke(ScriptEvent.Update, 0.1f);
		runtime.Invoke(ScriptEvent.Update, 0.1f);
		Assert.Equal(["hit 1.5", "sent", "hit 1.5", "sent"], runtime.Records);
		// One warning for the node on Hero's instance, however often it skips.
		Assert.Equal([TestRuntime.On("Send Hit", toWindow) + "Hit was not sent: Window (9) runs Scripts/Window.sgraph, not Scripts/Door.sgraph"],
			runtime.Environment.Warnings);
	}

	[Fact]
	public void ASendWithoutGraphReachesEveryGraphWithTheEvent()
	{
		TestRuntime runtime = Start(typed: false);

		runtime.Invoke(ScriptEvent.Update, 0.1f);
		Assert.Equal(["hit 1.5", "hit 2.5", "sent"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void ATypedSendToItsOwnGraphReachesOtherInstancesOfIt()
	{
		DocumentBuilder document = new();
		uint send = document.Graph.Node("Event.Send", CrossGraphTesting.Send("Ping", TestRuntime.Graph));
		document.Graph.Link(document.Graph.Node("Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)", inputs: """{ "Id": "8" }"""), "Result", send,
			"Target");
		document.Graph.Flow(document.Graph.Node("Event.Create"), send);
		uint ping = document.Graph.Node("Event.Custom", """{ "Name": "Ping" }""");
		document.Graph.Flow(ping, document.Graph.Write(document.Graph.Node("Self"), "Self"));
		TestRuntime runtime = new();
		runtime.Environment.Add(8, "Twin");
		runtime.LoadWorking(document.Build(), references: new BundleBuilder().Build());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(8));

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["Entity(8)"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}
}
