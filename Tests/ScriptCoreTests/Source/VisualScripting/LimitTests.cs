using System;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The budget, the depth limit and the aborting errors (spec section 5.3).
public sealed class LimitTests
{
	private const string Budget = "execution exceeded 1,000,000 steps (endless loop?)";

	// Create runs a ForLoop of count iterations whose body adds one to Counter: 4 steps per iteration (the Set, its Get and
	// Add, and the loop's next iteration) and 2 more (the event and the loop).
	private static (TestRuntime Runtime, uint Add) Counting(int count)
	{
		DocumentBuilder document = new();
		uint counter = document.Variable("Counter", "int", "0");
		GraphBuilder graph = document.Graph;
		uint loop = graph.Node("Flow.ForLoop", inputs: $$"""{ "First": 1, "Last": {{count}} }""");
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
		uint set = graph.Set(counter);
		graph.Link(graph.Get(counter), "Value", add, "A");
		graph.Link(add, "Result", set, "Value");
		graph.Flow(graph.Node("Event.Create"), loop);
		graph.Flow(loop, set, "Body");
		return (TestRuntime.Start(document), add);
	}

	[Fact]
	public void AnExecutionRunsAtMostAMillionSteps()
	{
		(TestRuntime fits, _) = Counting(249_999);
		Assert.Equal(VisualScriptStatus.Success, fits.Invoke(ScriptEvent.Create));
		Assert.Equal(249_999, fits.Variable("Counter"));

		(TestRuntime exceeds, uint add) = Counting(250_000);
		Assert.Equal(VisualScriptStatus.Failure, exceeds.Invoke(ScriptEvent.Create));
		Assert.Equal(TestRuntime.On("Add", add) + Budget, exceeds.SingleError());
		Assert.Equal(249_999, exceeds.Variable("Counter"));
		// Every outermost execution has a budget of its own.
		Assert.Equal(VisualScriptStatus.Failure, exceeds.Invoke(ScriptEvent.Create));
		Assert.Equal(2 * 249_999, exceeds.Variable("Counter"));
	}

	[Fact]
	public void AnEndlessLoopAbortsAtItsNode()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint loop = graph.Node("Flow.While", inputs: """{ "Condition": true }""");
		graph.Flow(graph.Node("Event.Update"), loop);
		graph.Writes(loop, "Completed", "never");
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Update));
		Assert.Equal(TestRuntime.On("While", loop) + Budget, runtime.SingleError());
		Assert.Empty(runtime.Records);
		Assert.False(runtime.Runtime.IsExecuting);
	}

	[Fact]
	public void TheExecutionsOfOneInvokeHaveBudgetsOfTheirOwn()
	{
		DocumentBuilder document = new();
		uint counter = document.Variable("Counter", "int", "0");
		GraphBuilder graph = document.Graph;
		foreach (uint start in new[] { graph.Node("Event.KeyPressed", """{ "Key": "Space" }"""), graph.Node("Event.Update") })
		{
			uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 150000 }""");
			uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
			uint set = graph.Set(counter);
			graph.Link(graph.Get(counter), "Value", add, "A");
			graph.Link(add, "Result", set, "Value");
			graph.Flow(start, loop);
			graph.Flow(loop, set, "Body");
		}
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Environment.PressedKeys.Add(KeyCode.Space);

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update));
		Assert.Equal(300_000, runtime.Variable("Counter"));
	}

	[Fact]
	public void NestedSendsDrawFromTheOutermostBudget()
	{
		// Each iteration: the loop, the Send, the custom event and its Set with Get and Add: 6 steps, so only the sum of the
		// nested executions exceeds the budget.
		DocumentBuilder document = new();
		uint counter = document.Variable("Counter", "int", "0");
		GraphBuilder graph = document.Graph;
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 170000 }""");
		uint send = graph.Node("Event.Send", """{ "Name": "Tick" }""");
		uint tick = graph.Node("Event.Custom", """{ "Name": "Tick" }""");
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
		uint set = graph.Set(counter);
		graph.Link(graph.Get(counter), "Value", add, "A");
		graph.Link(add, "Result", set, "Value");
		graph.Flow(graph.Node("Event.Create"), loop);
		graph.Flow(loop, send, "Body");
		graph.Flow(tick, set);
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(TestRuntime.On("Set Counter", set) + Budget, runtime.SingleError());
		Assert.Equal(166_666, runtime.Variable("Counter"));
	}

	[Fact]
	public void AnInvokeDuringAnExecutionDrawsFromItsBudgetAndFailsOnItsOwn()
	{
		DocumentBuilder hero = new();
		uint loop = hero.Graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 170000 }""");
		uint reenter = hero.Graph.Node("Call:Strada.ScriptCore.Tests.Probe.Reenter(ulong,int)", inputs: """{ "Entity": "8", "ScriptEvent": 0 }""");
		hero.Graph.Flow(hero.Graph.Node("Event.Create"), loop);
		hero.Graph.Flow(loop, reenter, "Body");
		DocumentBuilder villain = new();
		uint counter = villain.Variable("Counter", "int", "0");
		uint add = villain.Graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
		uint set = villain.Graph.Set(counter);
		villain.Graph.Link(villain.Graph.Get(counter), "Value", add, "A");
		villain.Graph.Link(add, "Result", set, "Value");
		villain.Graph.Flow(villain.Graph.Node("Event.Create"), set);
		TestRuntime runtime = TestRuntime.Start(hero);
		runtime.Environment.Add(8, "Villain");
		runtime.LoadWorking(villain.Build(), 101);
		runtime.Create(8, 101);
		Probe.Runtime = runtime.Runtime;

		// The nested Invoke fails once the budget is spent (its own status), then the outer execution fails at its next step.
		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(
		[
			FormattableString.Invariant($"{TestRuntime.Name}: Set Counter (node {set}) on Villain (8): {Budget}"),
			TestRuntime.On("For Loop", loop) + Budget,
		], runtime.Environment.Errors);
		Assert.Equal(166_666, runtime.Variable("Counter", 8));
	}

	[Fact]
	public void SendingWithoutEndPassesTheDepthLimitWithoutOverflowingTheStack()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint again = graph.Node("Event.Custom", """{ "Name": "Again" }""");
		uint send = graph.Node("Event.Send", """{ "Name": "Again" }""");
		graph.Flow(again, send);
		uint first = graph.Node("Event.Send", """{ "Name": "Again" }""");
		graph.Flow(graph.Node("Event.Create"), first);
		graph.Writes(first, "Then", "after");
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(TestRuntime.On("Send Again", send) + "call depth exceeded 64", runtime.SingleError());
		Assert.Empty(runtime.Records);
		Assert.False(runtime.Runtime.IsExecuting);
	}

	[Fact]
	public void AnExceptionOfAMemberAbortsWithItsTypeAndMessage()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint fail = graph.Node("Call:Strada.Testing.Assert.IsTrue(bool,string)", inputs: """{ "Condition": false, "Message": "the door opens" }""");
		graph.Flow(graph.Node("Event.Create"), fail);
		graph.Writes(fail, "Then", "never");
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(TestRuntime.On("Is True", fail) + "AssertionException: expected true: the door opens", runtime.SingleError());
		Assert.Empty(runtime.Records);
	}

	[Fact]
	public void AComponentMemberFetchesTheComponentOfItsTarget()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint instance = graph.Node("Get:Strada.ScriptComponent.Instance");
		graph.Flow(graph.Node("Event.Update"), graph.Write(instance, "Value"));
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Update));
		Assert.Equal(TestRuntime.On("Get Instance", instance) + "Hero has no Script", runtime.SingleError());
		runtime.Environment[TestRuntime.Hero].Components.Add(typeof(ScriptComponent));
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update));
		Assert.Equal(["None"], runtime.Records);
		Assert.Equal([(TestRuntime.Hero, typeof(ScriptComponent)), (TestRuntime.Hero, typeof(ScriptComponent))], runtime.Environment.ComponentFetches);
		// The component of Self is fetched by the instance's entity ID: the runtime looks no entity object up for it (the
		// fake environment looks one up for the view it makes).
		Assert.Equal(1, runtime.Environment.EntityLookups);
	}

	[Theory]
	[InlineData("Call:Strada.ScriptCore.Tests.Holder.Increment()", "Target", "Call:Strada.ScriptCore.Tests.Probe.NoHolder()", "Increment", "Target is None")]
	[InlineData("Call:Strada.Entity.Destroy()", "Target", "Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)", "Destroy", "Target is None")]
	[InlineData("Flow.ForEach", "Array", "Call:Strada.ScriptCore.Tests.Probe.NoEntities()", "For Each", "Array is None")]
	public void NullTargetsAndArraysAbort(string type, string input, string source, string title, string message)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint node = graph.Node(type);
		graph.Link(graph.Node(source, inputs: source.Contains("ulong", StringComparison.Ordinal) ? """{ "Id": "0" }""" : null), "Result", node, input);
		graph.Flow(graph.Node("Event.Create"), node);
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(TestRuntime.On(title, node) + message, runtime.SingleError());
	}

	[Theory]
	[InlineData("Call:Strada.ScriptCore.Tests.Probe.NoEntities()", null, "Array is None")]
	[InlineData("Call:Strada.ScriptCore.Tests.Probe.Entities(int)", "5", "index 5 is outside the array (length 3)")]
	[InlineData("Call:Strada.ScriptCore.Tests.Probe.Entities(int)", "-1", "index -1 is outside the array (length 3)")]
	public void ArrayGetOutsideItsArrayAborts(string source, string? index, string message)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint get = graph.Node("Array.Get", inputs: index != null ? $$"""{ "Index": {{index}} }""" : null);
		graph.Link(graph.Node(source, inputs: source.EndsWith("(int)", StringComparison.Ordinal) ? """{ "Count": 3 }""" : null), "Result", get, "Array");
		graph.Flow(graph.Node("Event.Create"), graph.Write(get, "Element"));
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(TestRuntime.On("Get", get) + message, runtime.SingleError());
	}

	[Fact]
	public void AnAbortStopsOneExecutionOfAnInvokeAndTheOthersRun()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint fail = graph.Node("Call:Strada.ScriptCore.Tests.Probe.Throw(string)", inputs: """{ "Message": "key" }""");
		graph.Flow(graph.Node("Event.KeyPressed", """{ "Key": "Space" }"""), fail);
		graph.Writes(fail, "Then", "never");
		graph.Writes(graph.Node("Event.Update"), "Then", "update");
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Environment.PressedKeys.Add(KeyCode.Space);

		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Update));
		Assert.Equal(["update"], runtime.Records);
		Assert.Equal(TestRuntime.On("Throw", fail) + "InvalidOperationException: key", runtime.SingleError());
		// The instance keeps running.
		runtime.Environment.PressedKeys.Clear();
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update));
	}
}
