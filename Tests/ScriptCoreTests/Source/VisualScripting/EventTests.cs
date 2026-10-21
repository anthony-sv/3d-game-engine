using System;
using System.Linq;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Event dispatch (spec section 5.2) and Event.Send (spec section 4.1).
public sealed class EventTests
{
	private const string Hit = """{ "Name": "Hit", "Parameters": [{ "Name": "Damage", "Type": "float" }] }""";

	[Fact]
	public void UpdateRunsTheInputEventsInNodeOrderBeforeEventUpdate()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		graph.Writes(graph.Node("Event.KeyPressed", """{ "Key": "Space" }"""), "Then", "space");
		uint update = graph.Node("Event.Update");
		graph.Flow(update, graph.Write(update, "DeltaTime"));
		graph.Writes(graph.Node("Event.MouseButtonPressed", """{ "Button": "Left" }"""), "Then", "left");
		graph.Writes(graph.Node("Event.GamepadButtonPressed", """{ "Button": "A" }""", """{ "Gamepad": 1 }"""), "Then", "pad");
		graph.Writes(graph.Node("Event.KeyReleased", """{ "Key": "Escape" }"""), "Then", "escape");
		graph.Writes(graph.Node("Event.MouseButtonReleased", """{ "Button": "Right" }"""), "Then", "right");
		graph.Writes(graph.Node("Event.GamepadButtonReleased", """{ "Button": "B" }"""), "Then", "pad 0");
		TestRuntime runtime = TestRuntime.Start(document);
		FakeEnvironment environment = runtime.Environment;
		environment.PressedKeys.Add(KeyCode.Space);
		environment.PressedButtons.Add(MouseButton.Left);
		environment.PressedGamepadButtons.Add((GamepadButton.A, 1));
		environment.ReleasedKeys.Add(KeyCode.Escape);
		environment.ReleasedButtons.Add(MouseButton.Right);
		environment.ReleasedGamepadButtons.Add((GamepadButton.B, 0));

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update, 0.25f));
		Assert.Equal(0.25f, environment.DeltaTime);
		Assert.Equal(["space", "left", "pad", "escape", "right", "pad 0", "0.25"], runtime.Records);

		// Gamepad events poll their gamepad; nothing else is down now.
		runtime.Records.Clear();
		environment.PressedKeys.Clear();
		environment.PressedButtons.Clear();
		environment.ReleasedKeys.Clear();
		environment.ReleasedButtons.Clear();
		environment.ReleasedGamepadButtons.Clear();
		environment.PressedGamepadButtons.Clear();
		environment.PressedGamepadButtons.Add((GamepadButton.A, 0));
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		Assert.Equal(["0.5"], runtime.Records);
	}

	[Fact]
	public void EventsOutputWhatTheyWereGiven()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint fixedUpdate = graph.Node("Event.FixedUpdate");
		graph.Flow(fixedUpdate, graph.Write(fixedUpdate, "FixedDeltaTime"));
		foreach (string contact in new[] { "Event.CollisionEnter", "Event.CollisionExit", "Event.TriggerEnter", "Event.TriggerExit" })
		{
			uint node = graph.Node(contact);
			uint text = graph.Node("FormatString", $$"""{ "Format": "{{contact}} {Other} {Mover}" }""");
			uint mover = graph.Node("Convert", """{ "To": "Strada.ScriptCore.Tests.Mover" }""");
			graph.Link(node, "Other", text, "Other");
			graph.Link(node, "Other", mover, "Value");
			graph.Link(mover, "Result", text, "Mover");
			graph.Flow(node, graph.Write(text, "Result"));
		}
		uint click = graph.Node("Event.Click");
		graph.Flow(click, graph.Write(click, "Button"));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Environment.Add(9, "Crate", new Mover());
		runtime.Environment.Add(10, "Wall");

		runtime.Invoke(ScriptEvent.FixedUpdate, 0.02f);
		runtime.Invoke(ScriptEvent.CollisionEnter, other: 9);
		runtime.Invoke(ScriptEvent.CollisionExit, other: 10);
		runtime.Invoke(ScriptEvent.TriggerEnter, other: 9);
		runtime.Invoke(ScriptEvent.TriggerExit, other: 0);
		runtime.Invoke(ScriptEvent.Click, other: 10);
		Assert.Equal(
		[
			"0.02", "Event.CollisionEnter Entity(9) Entity(9)", "Event.CollisionExit Entity(10) None", "Event.TriggerEnter Entity(9) Entity(9)",
			"Event.TriggerExit None None", "Entity(10)",
		], runtime.Records);
	}

	[Fact]
	public void AMissingEventRunsNothing()
	{
		DocumentBuilder document = new();
		document.Graph.Writes(document.Graph.Node("Event.Create"), "Then", "created");
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update, 0.1f));
		Assert.Equal(VisualScriptStatus.Failure, runtime.Runtime.Invoke(TestRuntime.Hero, (ScriptEvent)99, 0.0f, 0));
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Destroy));
		Assert.Empty(runtime.Records);
		// Destroy removed the instance.
		Assert.Equal(VisualScriptStatus.NotFound, runtime.Invoke(ScriptEvent.Create));
	}

	[Fact]
	public void DestroyRemovesTheInstanceBeforeItsEventRuns()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint destroy = graph.Node("Event.Destroy");
		uint send = graph.Node("Event.Send", """{ "Name": "Ping" }""");
		graph.Flow(destroy, send);
		graph.Writes(send, "Then", "destroyed");
		graph.Writes(graph.Node("Event.Custom", """{ "Name": "Ping" }"""), "Then", "pinged");
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Destroy));
		Assert.Equal(["destroyed"], runtime.Records);
		Assert.Null(runtime.Runtime.FindInstance(TestRuntime.Hero));
		Assert.Equal(VisualScriptStatus.NotFound, runtime.Invoke(ScriptEvent.Update));
		Assert.Equal([TestRuntime.On("Send Ping", send) + "Hero (7) runs no visual script to receive Ping"], runtime.Environment.Warnings);
	}

	[Fact]
	public void SendRunsTheCustomEventOfTheTargetsInstance()
	{
		DocumentBuilder hero = new();
		uint send = hero.Graph.Node("Event.Send", Hit, """{ "Damage": 2.5 }""");
		hero.Graph.Flow(hero.Graph.Node("Event.Create"), send);
		hero.Graph.Link(hero.Graph.Node("Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)", inputs: """{ "Id": "8" }"""), "Result", send, "Target");
		hero.Graph.Writes(send, "Then", "sent");
		DocumentBuilder villain = new();
		uint hit = villain.Graph.Node("Event.Custom", Hit);
		uint text = villain.Graph.Node("FormatString", """{ "Format": "{Who} took {Damage}" }""");
		villain.Graph.Link(villain.Graph.Node("Self"), "Self", text, "Who");
		villain.Graph.Link(hit, "Damage", text, "Damage");
		villain.Graph.Flow(hit, villain.Graph.Write(text, "Result"));
		TestRuntime runtime = TestRuntime.Start(hero);
		runtime.Environment.Add(8, "Villain");
		runtime.LoadWorking(villain.Build(), 101);
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(8, 101));

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(["Entity(8) took 2.5", "sent"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void SendToSelfOverwritesTheEventsParameters()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint first = graph.Node("Event.Send", Hit, """{ "Damage": 1.0 }""");
		graph.Flow(graph.Node("Event.Create"), first);
		uint hit = graph.Node("Event.Custom", Hit);
		uint branch = graph.Node("Flow.Branch");
		uint small = graph.Node("Operator.Less", inputs: """{ "B": 2.0 }""");
		uint again = graph.Node("Event.Send", Hit, """{ "Damage": 5.0 }""");
		uint before = graph.Write(hit, "Damage");
		graph.Flow(hit, before);
		graph.Flow(before, branch);
		graph.Link(hit, "Damage", small, "A");
		graph.Link(small, "Result", branch, "Condition");
		graph.Flow(branch, again, "True");
		// The nested Send overwrote Damage: the outer execution reads the latest value.
		graph.Flow(again, graph.Write(hit, "Damage"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["1", "5", "5"], runtime.Records);
	}

	[Fact]
	public void SendWithoutAReceiverWarnsOncePerNodeAndContinues()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		uint none = graph.Node("Event.Send", Hit);
		uint stranger = graph.Node("Event.Send", Hit);
		uint unknown = graph.Node("Event.Send", """{ "Name": "Miss" }""");
		uint mistyped = graph.Node("Event.Send", """{ "Name": "Hit", "Parameters": [{ "Name": "Damage", "Type": "int" }] }""");
		graph.Link(graph.Node("Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)", inputs: """{ "Id": "0" }"""), "Result", none, "Target");
		graph.Link(graph.Node("Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)", inputs: """{ "Id": "99" }"""), "Result", stranger, "Target");
		graph.Flow(update, none);
		graph.Flow(none, stranger);
		graph.Flow(stranger, unknown);
		graph.Flow(unknown, mistyped);
		graph.Writes(mistyped, "Then", "after");
		graph.Writes(graph.Node("Event.Custom", Hit), "Then", "hit");
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Environment.Add(99, "Stranger");

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update));
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update));
		Assert.Equal(["after", "after"], runtime.Records);
		Assert.Equal(
		[
			TestRuntime.On("Send Hit", none) + "Target is None, so no graph receives Hit",
			TestRuntime.On("Send Hit", stranger) + "Stranger (99) runs no visual script to receive Hit",
			TestRuntime.On("Send Miss", unknown) + "the graph of Hero (7) has no custom event Miss()",
			TestRuntime.On("Send Hit", mistyped) + "the graph of Hero (7) has no custom event Hit(int)",
		], runtime.Environment.Warnings);
		Assert.Empty(runtime.Environment.Errors);
	}

	[Fact]
	public void AnAbortInASentEventAbortsTheSendingExecutionOnce()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint send = graph.Node("Event.Send", """{ "Name": "Boom" }""");
		graph.Flow(graph.Node("Event.Create"), send);
		graph.Writes(send, "Then", "after");
		uint boom = graph.Node("Event.Custom", """{ "Name": "Boom" }""");
		uint fail = graph.Node("Call:Strada.ScriptCore.Tests.Probe.Throw(string)", inputs: """{ "Message": "boom" }""");
		graph.Flow(boom, fail);
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Create));
		Assert.Empty(runtime.Records);
		Assert.Equal(TestRuntime.On("Throw", fail) + "InvalidOperationException: boom", runtime.SingleError());
		Assert.False(runtime.Runtime.IsExecuting);
	}

	[Fact]
	public void AnInvokeDuringAnExecutionReportsItsOwnFailure()
	{
		DocumentBuilder hero = new();
		uint reenter = hero.Graph.Node("Call:Strada.ScriptCore.Tests.Probe.Reenter(ulong,int)", inputs: """{ "Entity": "8", "ScriptEvent": 0 }""");
		hero.Graph.Flow(hero.Graph.Node("Event.Create"), reenter);
		hero.Graph.Flow(reenter, hero.Graph.Write(reenter, "Result"));
		DocumentBuilder villain = new();
		uint fail = villain.Graph.Node("Call:Strada.ScriptCore.Tests.Probe.Throw(string)", inputs: """{ "Message": "villain" }""");
		villain.Graph.Flow(villain.Graph.Node("Event.Create"), fail);
		TestRuntime runtime = TestRuntime.Start(hero);
		runtime.Environment.Add(8, "Villain");
		runtime.LoadWorking(villain.Build(), 101);
		runtime.Create(8, 101);
		Probe.Runtime = runtime.Runtime;

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(["1"], runtime.Records);
		Assert.Equal(FormattableString.Invariant($"{TestRuntime.Name}: Throw (node {fail}) on Villain (8): InvalidOperationException: villain"),
			runtime.SingleError());
	}

	[Fact]
	public void EveryInvokeOfAnEntityRunsOnItsCurrentInstance()
	{
		DocumentBuilder document = new();
		uint count = document.Variable("Count", "int", "0");
		GraphBuilder graph = document.Graph;
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
		uint set = graph.Set(count);
		graph.Link(graph.Get(count), "Value", add, "A");
		graph.Link(add, "Result", set, "Value");
		graph.Flow(graph.Node("Event.Update"), set);
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal(2, runtime.Variable("Count"));
		// A new instance replaces the old one and starts from the document's values.
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		Assert.Equal(0, runtime.Variable("Count"));
		Assert.Equal(1, runtime.Runtime.InstanceCount);
		Assert.Empty(runtime.Environment.Logs);
	}
}
