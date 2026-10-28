using System;
using System.Linq;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Event.Broadcast (spec-slice3 section 4.6): a snapshot of the instances whose graph has the event with exactly the
// parameter types (with Graph, of the instances running that graph), in the order they were created, each receiving the
// event as Send's receiver does; Count is how many received it.
public sealed class BroadcastTests
{
	private const ulong Window = 102;
	private const string WindowName = "Scripts/Window.sgraph";
	private const ulong Other = 103;
	private const ulong Quiet = 104;
	private const string Damage = """{ "Name": "Damage", "Type": "float" }""";

	// A receiver whose Hit(float Damage) records the entity it runs on, then runs then (a probe call's type, or nothing).
	private static string Receiver(string? then = null, string? inputs = null)
	{
		DocumentBuilder receiver = new();
		uint hit = receiver.Graph.Node("Event.Custom", $$"""{ "Name": "Hit", "Parameters": [{{Damage}}] }""");
		uint write = receiver.Graph.Write(receiver.Graph.Node("Self"), "Self");
		receiver.Graph.Flow(hit, write);
		if (then != null)
		{
			receiver.Graph.Flow(write, receiver.Graph.Node(then, inputs: inputs));
		}
		return receiver.Build();
	}

	// Hero broadcasts Hit (1.5) on Create with the settings, then records the Count.
	private static DocumentBuilder Sender(string settings, bool receives = false)
	{
		DocumentBuilder sender = new();
		uint broadcast = sender.Graph.Node("Event.Broadcast", settings, """{ "Damage": 1.5 }""");
		sender.Graph.Flow(sender.Graph.Node("Event.Create"), broadcast);
		uint count = sender.Graph.Node("FormatString", """{ "Format": "count {Count}" }""");
		sender.Graph.Link(broadcast, "Count", count, "Count");
		sender.Graph.Flow(broadcast, sender.Graph.Write(count, "Result"));
		if (receives)
		{
			uint hit = sender.Graph.Node("Event.Custom", $$"""{ "Name": "Hit", "Parameters": [{{Damage}}] }""");
			sender.Graph.Flow(hit, sender.Graph.Write(sender.Graph.Node("Self"), "Self"));
		}
		return sender;
	}

	// The runtime with Door and Window loaded (both with Hit), Other (whose Hit takes an int), Quiet (without events) and
	// Hero's sender; no instances.
	private static TestRuntime Load(DocumentBuilder sender, string? door = null, string? window = null)
	{
		TestRuntime runtime = new();
		Probe.Runtime = runtime.Runtime;
		foreach ((ulong id, string name) in (ReadOnlySpan<(ulong, string)>)[(8, "Door"), (9, "Window"), (10, "Other"), (11, "Quiet")])
		{
			runtime.Environment.Add(id, name);
		}
		runtime.LoadWorking(door ?? Receiver(), CrossGraphTesting.Door, name: CrossGraphTesting.DoorName);
		runtime.LoadWorking(window ?? Receiver(), Window, name: WindowName);
		runtime.LoadWorking("""{ "Graph": { "Nodes": [ { "ID": 1, "Type": "Event.Custom", "Settings": { "Name": "Hit", "Parameters": [ { "Name": "Damage", "Type": "int" } ] } } ] } }""",
			Other, name: "Scripts/Other.sgraph");
		runtime.LoadWorking("{}", Quiet, name: "Scripts/Quiet.sgraph");
		runtime.LoadWorking(sender.Build(), references: new BundleBuilder().Interface(CrossGraphTesting.Door, CrossGraphTesting.DoorName, door ?? Receiver()).Build());
		return runtime;
	}

	private static void Create(TestRuntime runtime, ulong entity, ulong graph)
	{
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(entity, graph));
	}

	[Fact]
	public void AnUntypedBroadcastReachesEveryGraphWithTheEventInCreationOrder()
	{
		TestRuntime runtime = Load(Sender(CrossGraphTesting.Broadcast("Hit", 0, Damage), receives: true));
		Create(runtime, 9, Window);
		Create(runtime, TestRuntime.Hero, TestRuntime.Graph);
		Create(runtime, 8, CrossGraphTesting.Door);
		// Other's Hit takes an int, and Quiet's graph has no Hit: neither receives it.
		Create(runtime, 10, Other);
		Create(runtime, 11, Quiet);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["Entity(9)", "Entity(7)", "Entity(8)", "count 3"], runtime.Records);

		// An instance created again is the newest.
		Create(runtime, 9, Window);
		runtime.Records.Clear();
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["Entity(7)", "Entity(8)", "Entity(9)", "count 3"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void ATypedBroadcastReachesOnlyTheEntitiesRunningItsGraph()
	{
		TestRuntime runtime = Load(Sender(CrossGraphTesting.Broadcast("Hit", CrossGraphTesting.Door, Damage), receives: true));
		Create(runtime, 8, CrossGraphTesting.Door);
		Create(runtime, 9, Window);
		Create(runtime, 11, CrossGraphTesting.Door);
		Create(runtime, TestRuntime.Hero, TestRuntime.Graph);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["Entity(8)", "Entity(11)", "count 2"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void IncludeSelfDecidesWhetherTheBroadcastingInstanceReceivesIt()
	{
		TestRuntime runtime = Load(Sender(CrossGraphTesting.Broadcast("Hit", 0, Damage, """, "IncludeSelf": false"""), receives: true));
		Create(runtime, TestRuntime.Hero, TestRuntime.Graph);
		Create(runtime, 8, CrossGraphTesting.Door);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["Entity(8)", "count 1"], runtime.Records);

		// Nobody else: none receives it, which is no warning.
		Assert.Equal(VisualScriptStatus.Success, runtime.Runtime.DestroyInstance(8));
		runtime.Records.Clear();
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["count 0"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void AReceiverThatDestroysALaterReceiverSkipsIt()
	{
		// Door destroys Window (Probe.Reenter dispatches Destroy, 3) while it receives Hit.
		TestRuntime runtime = Load(Sender(CrossGraphTesting.Broadcast("Hit", 0, Damage)),
			door: Receiver("Call:Strada.ScriptCore.Tests.Probe.Reenter(ulong,int)", """{ "Entity": "9", "ScriptEvent": 3 }"""));
		Create(runtime, 8, CrossGraphTesting.Door);
		Create(runtime, 9, Window);
		Create(runtime, TestRuntime.Hero, TestRuntime.Graph);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["Entity(8)", "count 1"], runtime.Records);
		Assert.Null(runtime.Runtime.FindInstance(9));
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void AnInstanceCreatedDuringTheBroadcastIsNotReached()
	{
		// Door starts Window on entity 9 the first time it receives Hit.
		DocumentBuilder door = new();
		uint hit = door.Graph.Node("Event.Custom", $$"""{ "Name": "Hit", "Parameters": [{{Damage}}] }""");
		uint write = door.Graph.Write(door.Graph.Node("Self"), "Self");
		uint once = door.Graph.Node("Flow.DoOnce");
		door.Graph.Flow(hit, write);
		door.Graph.Flow(write, once);
		door.Graph.Flow(once,
			door.Graph.Node("Call:Strada.ScriptCore.Tests.Probe.Create(ulong,ulong)", inputs: FormattableString.Invariant($$"""{ "Entity": "9", "Graph": "{{Window}}" }""")),
			"Completed");
		TestRuntime runtime = Load(Sender(CrossGraphTesting.Broadcast("Hit", 0, Damage)), door.Build());
		Create(runtime, 8, CrossGraphTesting.Door);
		Create(runtime, TestRuntime.Hero, TestRuntime.Graph);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["Entity(8)", "count 1"], runtime.Records);
		Assert.NotNull(runtime.Runtime.FindInstance(9));
		// The next broadcast reaches it.
		runtime.Records.Clear();
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["Entity(8)", "Entity(9)", "count 2"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void AReceiverMayBroadcastAgain()
	{
		// Door broadcasts Ping while it receives Hit; Window receives both.
		DocumentBuilder door = new();
		uint hit = door.Graph.Node("Event.Custom", $$"""{ "Name": "Hit", "Parameters": [{{Damage}}] }""");
		uint ping = door.Graph.Node("Event.Broadcast", CrossGraphTesting.Broadcast("Ping"));
		door.Graph.Flow(hit, ping);
		door.Graph.Writes(ping, "Then", "pinged");
		DocumentBuilder window = new();
		uint windowHit = window.Graph.Node("Event.Custom", $$"""{ "Name": "Hit", "Parameters": [{{Damage}}] }""");
		window.Graph.Writes(windowHit, "Then", "window hit");
		window.Graph.Writes(window.Graph.Node("Event.Custom", """{ "Name": "Ping" }"""), "Then", "window ping");
		TestRuntime runtime = Load(Sender(CrossGraphTesting.Broadcast("Hit", 0, Damage)), door.Build(), window.Build());
		Create(runtime, 8, CrossGraphTesting.Door);
		Create(runtime, 9, Window);
		Create(runtime, TestRuntime.Hero, TestRuntime.Graph);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["window ping", "pinged", "window hit", "count 2"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void AnAbortInAReceiverAbortsTheBroadcast()
	{
		TestRuntime runtime = Load(Sender(CrossGraphTesting.Broadcast("Hit", 0, Damage)),
			door: Receiver("Call:Strada.ScriptCore.Tests.Probe.Throw(string)", """{ "Message": "boom" }"""));
		Create(runtime, 8, CrossGraphTesting.Door);
		Create(runtime, 9, Window);
		Create(runtime, TestRuntime.Hero, TestRuntime.Graph);

		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Create));
		// Window, after Door, receives nothing, and Hero does not continue.
		Assert.Equal(["Entity(8)"], runtime.Records);
		string error = runtime.SingleError();
		Assert.StartsWith("Scripts/Door.sgraph: Throw (node ", error, StringComparison.Ordinal);
		Assert.EndsWith("on Door (8): InvalidOperationException: boom", error, StringComparison.Ordinal);
	}

	[Fact]
	public void AReceiverWhoseEventWaitsCounts()
	{
		DocumentBuilder door = new();
		uint hit = door.Graph.Node("Event.Custom", $$"""{ "Name": "Hit", "Parameters": [{{Damage}}] }""");
		uint delay = door.Graph.Node("Flow.Delay", inputs: """{ "Duration": 0.5 }""");
		door.Graph.Flow(hit, delay);
		door.Graph.Writes(delay, "Completed", "door done");
		TestRuntime runtime = Load(Sender(CrossGraphTesting.Broadcast("Hit", 0, Damage)), door.Build());
		Create(runtime, 8, CrossGraphTesting.Door);
		Create(runtime, TestRuntime.Hero, TestRuntime.Graph);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["count 1"], runtime.Records);
		Assert.Equal(1, runtime.Instance(8).FiberCount);
		runtime.Invoke(ScriptEvent.Update, 1.0f, entity: 8);
		Assert.Equal(["count 1", "door done"], runtime.Records);
	}

	// One step for the node and one per instance examined: a hundred broadcasts over 10,000 instances exhaust the budget.
	[Fact]
	public void EveryInstanceExaminedCostsAStep()
	{
		DocumentBuilder sender = new();
		uint loop = sender.Graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 110 }""");
		uint broadcast = sender.Graph.Node("Event.Broadcast", CrossGraphTesting.Broadcast("Hit", 0, Damage));
		sender.Graph.Flow(sender.Graph.Node("Event.Update"), loop);
		sender.Graph.Flow(loop, broadcast, "Body");
		uint once = sender.Graph.Node("Event.Broadcast", CrossGraphTesting.Broadcast("Hit", 0, Damage));
		sender.Graph.Flow(sender.Graph.Node("Event.Create"), once);
		uint onceCount = sender.Graph.Node("FormatString", """{ "Format": "count {Count}" }""");
		sender.Graph.Link(once, "Count", onceCount, "Count");
		sender.Graph.Flow(once, sender.Graph.Write(onceCount, "Result"));
		TestRuntime runtime = Load(sender);
		const ulong first = 1000;
		for (ulong entity = first; entity < first + 10_000; entity++)
		{
			Create(runtime, entity, Other);
		}
		Create(runtime, 8, CrossGraphTesting.Door);
		Create(runtime, TestRuntime.Hero, TestRuntime.Graph);

		// One broadcast examines 10,002 instances.
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["Entity(8)", "count 1"], runtime.Records);
		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Update));
		Assert.EndsWith("execution exceeded 1,000,000 steps (endless loop?)", runtime.SingleError(), StringComparison.Ordinal);
		// Door received the broadcasts made before the budget ran out: fewer than a hundred.
		int received = runtime.Records.Count(static record => record == "Entity(8)") - 1;
		Assert.InRange(received, 90, 99);
	}
}
