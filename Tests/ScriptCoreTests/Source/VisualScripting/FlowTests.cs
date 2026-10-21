using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The flow nodes of spec section 4.2 (but the latent ones) and the flow stack of spec section 5.3.
public sealed class FlowTests
{
	[Fact]
	public void AnEventRunsItsChain()
	{
		DocumentBuilder document = new();
		uint score = document.Variable("Score", "int", "1");
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint set = graph.Set(score, "42");
		graph.Flow(create, set);
		graph.Writes(set, "Then", "done");
		TestRuntime runtime = TestRuntime.Start(document);
		Assert.Equal(1, runtime.Variable("Score"));

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(42, runtime.Variable("Score"));
		Assert.Equal(["done"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void SequenceRunsEachOutputAfterTheChainBeforeIt()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint sequence = graph.Node("Flow.Sequence", """{ "Count": 3 }""");
		uint inner = graph.Node("Flow.Sequence");
		graph.Flow(create, sequence);
		graph.Flow(sequence, inner, "Then0");
		graph.Writes(inner, "Then0", "A1", "A2");
		graph.Writes(inner, "Then1", "B");
		// Then1 of the outer sequence has no link: its chain ends at once.
		graph.Writes(sequence, "Then2", "C");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["A1", "A2", "B", "C"], runtime.Records);
	}

	[Fact]
	public void BranchFollowsItsCondition()
	{
		DocumentBuilder document = new();
		uint flag = document.Variable("Flag", "bool", "true");
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		uint branch = graph.Node("Flow.Branch");
		uint set = graph.Set(flag, "false");
		graph.Flow(update, branch);
		graph.Link(graph.Get(flag), "Value", branch, "Condition");
		graph.Flow(graph.Writes(branch, "True", "yes"), set);
		graph.Writes(branch, "False", "no");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Update, 0.5f);
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		Assert.Equal(["yes", "no"], runtime.Records);
	}

	[Theory]
	[InlineData("1", "one")]
	[InlineData("-3", "minus three")]
	[InlineData("7", "default")]
	public void SwitchOnAnIntegerTakesTheMatchingCase(string selection, string expected)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint choose = graph.Node("Flow.Switch", """{ "Type": "int", "Cases": [1, -3] }""", $$"""{ "Selection": {{selection}} }""");
		graph.Flow(create, choose);
		graph.Writes(choose, "Case:1", "one");
		graph.Writes(choose, "Case:-3", "minus three");
		graph.Writes(choose, "Default", "default");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal([expected], runtime.Records);
	}

	[Fact]
	public void SwitchOnTextComparesOrdinally()
	{
		DocumentBuilder document = new();
		uint word = document.Variable("Word", "string", "\"open\"");
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		uint choose = graph.Node("Flow.Switch", """{ "Type": "string", "Cases": ["open", "close"] }""");
		graph.Flow(update, choose);
		graph.Link(graph.Get(word), "Value", choose, "Selection");
		graph.Flow(graph.Writes(choose, "Case:open", "opened"), graph.Set(word, "\"Close\""));
		graph.Writes(choose, "Case:close", "closed");
		graph.Writes(choose, "Default", "unknown");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal(["opened", "unknown"], runtime.Records);
	}

	[Fact]
	public void SwitchOnAnEnumHasACaseForEveryEnumeratorWithoutCases()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint choose = graph.Node("Flow.Switch", """{ "Type": "Strada.ForceMode" }""", """{ "Selection": "Acceleration" }""");
		graph.Flow(create, choose);
		graph.Writes(choose, "Case:Force", "force");
		graph.Writes(choose, "Case:Acceleration", "acceleration");
		graph.Writes(choose, "Default", "default");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["acceleration"], runtime.Records);
	}

	[Fact]
	public void ForLoopRunsItsBodyForEveryIndexThenCompleted()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 3 }""");
		graph.Flow(create, loop);
		graph.Flow(loop, graph.Write(loop, "Index"), "Body");
		// Index outside an active iteration reads the type default.
		graph.Flow(graph.Writes(loop, "Completed", "done"), graph.Write(loop, "Index"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["1", "2", "3", "done", "0"], runtime.Records);
	}

	[Theory]
	[InlineData("5", "4", new string[] { "done" })]
	[InlineData("2147483646", "2147483647", new[] { "2147483646", "2147483647", "done" })]
	[InlineData("-2147483648", "-2147483648", new[] { "-2147483648", "done" })]
	public void ForLoopReadsItsBoundsOnceAndStopsAtLast(string first, string last, string[] expected)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint loop = graph.Node("Flow.ForLoop", inputs: $$"""{ "First": {{first}}, "Last": {{last}} }""");
		graph.Flow(create, loop);
		graph.Flow(loop, graph.Write(loop, "Index"), "Body");
		graph.Writes(loop, "Completed", "done");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(expected, runtime.Records);
	}

	[Fact]
	public void BreakExitsTheLoopAfterTheCurrentBody()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 5 }""");
		uint body = graph.Node("Flow.Sequence");
		uint branch = graph.Node("Flow.Branch");
		uint equal = graph.Node("Operator.Equal", inputs: """{ "B": 2 }""");
		graph.Flow(create, loop);
		graph.Flow(loop, body, "Body");
		graph.Flow(body, graph.Write(loop, "Index"), "Then0");
		graph.Flow(body, branch, "Then1");
		graph.Link(loop, "Index", equal, "A");
		graph.Link(equal, "Result", branch, "Condition");
		graph.Flow(graph.Writes(branch, "True", "break"), loop, "Then", "Break");
		graph.Writes(loop, "Completed", "done");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["1", "2", "break", "done"], runtime.Records);
	}

	[Fact]
	public void BreakOutsideItsLoopEndsTheChainAndChangesNothing()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		uint create = graph.Node("Event.Create");
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 2 }""");
		graph.Flow(graph.Writes(update, "Then", "update"), loop, "Then", "Break");
		graph.Flow(create, loop);
		graph.Flow(loop, graph.Write(loop, "Index"), "Body");
		graph.Writes(loop, "Completed", "done");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["update", "1", "2", "done"], runtime.Records);
	}

	[Fact]
	public void AReenteredLoopKeepsBothActivations()
	{
		DocumentBuilder document = new();
		uint depth = document.Variable("Depth", "int", "0");
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 2 }""");
		uint branch = graph.Node("Flow.Branch");
		uint outer = graph.Node("Operator.Equal", inputs: """{ "B": 0 }""");
		graph.Flow(create, loop);
		uint index = graph.Write(loop, "Index");
		graph.Flow(loop, index, "Body");
		graph.Flow(index, branch);
		graph.Link(graph.Get(depth), "Value", outer, "A");
		graph.Link(outer, "Result", branch, "Condition");
		uint set = graph.Set(depth, "1");
		graph.Flow(branch, set, "True");
		// The body enters the loop again: the inner activation runs to Completed, then the outer one continues.
		graph.Flow(set, loop);
		graph.Writes(loop, "Completed", "done");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["1", "1", "2", "done", "2", "done"], runtime.Records);
	}

	[Fact]
	public void NestedLoopsReadTheirOwnIndexes()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint rows = graph.Node("Flow.ForLoop", inputs: """{ "First": 0, "Last": 1 }""");
		uint columns = graph.Node("Flow.ForLoop", inputs: """{ "First": 0, "Last": 2 }""");
		uint cell = graph.Node("FormatString", """{ "Format": "{Row}{Column}" }""");
		graph.Flow(create, rows);
		graph.Flow(rows, columns, "Body");
		graph.Link(rows, "Index", cell, "Row");
		graph.Link(columns, "Index", cell, "Column");
		graph.Flow(columns, graph.Write(cell, "Result"), "Body");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["00", "01", "02", "10", "11", "12"], runtime.Records);
	}

	[Fact]
	public void ForEachRunsOverASnapshotOfItsArray()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint each = graph.Node("Flow.ForEach");
		uint entities = graph.Node("Call:Strada.ScriptCore.Tests.Probe.Entities(int)", inputs: """{ "Count": 3 }""");
		uint text = graph.Node("FormatString", """{ "Format": "{Index}: {Element}" }""");
		graph.Flow(create, each);
		graph.Link(entities, "Result", each, "Array");
		graph.Link(each, "Index", text, "Index");
		graph.Link(each, "Element", text, "Element");
		graph.Flow(each, graph.Write(text, "Result"), "Body");
		graph.Flow(graph.Writes(each, "Completed", "done"), graph.Write(text, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["0: Entity(1)", "1: Entity(2)", "2: Entity(3)", "done", "0: None"], runtime.Records);
	}

	[Fact]
	public void ForEachOverValuesAndBreak()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint each = graph.Node("Flow.ForEach");
		uint points = graph.Node("Call:Strada.ScriptCore.Tests.Probe.Points(int)", inputs: """{ "Count": 4 }""");
		uint branch = graph.Node("Flow.Branch");
		uint last = graph.Node("Operator.GreaterOrEqual", inputs: """{ "B": 1 }""");
		graph.Flow(create, each);
		graph.Link(points, "Result", each, "Array");
		uint write = graph.Write(each, "Element");
		graph.Flow(each, write, "Body");
		graph.Flow(write, branch);
		graph.Link(each, "Index", last, "A");
		graph.Link(last, "Result", branch, "Condition");
		graph.Flow(branch, each, "True", "Break");
		graph.Writes(each, "Completed", "done");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["(0, 0, 0)", "(1, 2, 3)", "done"], runtime.Records);
	}

	[Fact]
	public void ForEachOverAnEmptyArrayCompletesAtOnce()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint each = graph.Node("Flow.ForEach");
		uint entities = graph.Node("Call:Strada.ScriptCore.Tests.Probe.Entities(int)", inputs: """{ "Count": 0 }""");
		graph.Flow(create, each);
		graph.Link(entities, "Result", each, "Array");
		graph.Writes(each, "Body", "body");
		graph.Writes(each, "Completed", "done");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["done"], runtime.Records);
	}

	[Fact]
	public void WhileChecksItsConditionBeforeEveryIteration()
	{
		DocumentBuilder document = new();
		uint counter = document.Variable("Counter", "int", "0");
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint loop = graph.Node("Flow.While");
		uint less = graph.Node("Operator.Less", inputs: """{ "B": 3 }""");
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
		uint set = graph.Set(counter);
		graph.Flow(create, loop);
		graph.Link(graph.Get(counter), "Value", less, "A");
		graph.Link(less, "Result", loop, "Condition");
		graph.Link(graph.Get(counter), "Value", add, "A");
		graph.Link(add, "Result", set, "Value");
		graph.Flow(loop, set, "Body");
		graph.Flow(set, graph.Write(set, "Value"));
		graph.Writes(loop, "Completed", "done");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["1", "2", "3", "done"], runtime.Records);
		Assert.Equal(3, runtime.Variable("Counter"));
	}

	[Theory]
	[InlineData(false, new[] { "once", "reset", "once" })]
	[InlineData(true, new[] { "reset", "once" })]
	public void DoOnceRunsUntilReset(bool startClosed, string[] expected)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		uint fixedUpdate = graph.Node("Event.FixedUpdate");
		uint once = graph.Node("Flow.DoOnce", startClosed ? """{ "StartClosed": true }""" : null);
		graph.Flow(update, once);
		graph.Writes(once, "Completed", "once");
		graph.Flow(graph.Writes(fixedUpdate, "Then", "reset"), once, "Then", "Reset");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.FixedUpdate);
		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal(expected, runtime.Records);
	}

	[Fact]
	public void DoNCountsItsExitsUntilReset()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		uint fixedUpdate = graph.Node("Event.FixedUpdate");
		uint times = graph.Node("Flow.DoN", inputs: """{ "N": 2 }""");
		graph.Flow(update, times);
		graph.Flow(times, graph.Write(times, "Counter"), "Exit");
		graph.Flow(fixedUpdate, times, "Then", "Reset");
		TestRuntime runtime = TestRuntime.Start(document);

		for (int frame = 0; frame < 3; frame++)
		{
			runtime.Invoke(ScriptEvent.Update);
		}
		runtime.Invoke(ScriptEvent.FixedUpdate);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal(["1", "2", "1"], runtime.Records);
	}

	[Fact]
	public void FlipFlopAlternatesStartingWithA()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		uint flip = graph.Node("Flow.FlipFlop");
		uint state = graph.Node("FormatString", """{ "Format": "{Path} {IsA}" }""");
		graph.Flow(update, flip);
		uint a = graph.Write(state, "Result");
		uint b = graph.Write(state, "Result");
		graph.Flow(flip, a, "A");
		graph.Flow(flip, b, "B");
		graph.Link(flip, "IsA", state, "IsA");
		graph.Link(graph.Literal("string", "\"went\""), "Value", state, "Path");
		TestRuntime runtime = TestRuntime.Start(document);

		for (int frame = 0; frame < 3; frame++)
		{
			runtime.Invoke(ScriptEvent.Update);
		}
		Assert.Equal(["went True", "went False", "went True"], runtime.Records);
	}

	[Fact]
	public void GateLetsTheFlowThroughWhileOpen()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		uint open = graph.Node("Event.KeyPressed", """{ "Key": "O" }""");
		uint close = graph.Node("Event.KeyPressed", """{ "Key": "C" }""");
		uint toggle = graph.Node("Event.KeyPressed", """{ "Key": "T" }""");
		uint gate = graph.Node("Flow.Gate", """{ "StartOpen": false }""");
		graph.Flow(update, gate, "Then", "Enter");
		graph.Flow(open, gate, "Then", "Open");
		graph.Flow(close, gate, "Then", "Close");
		graph.Flow(toggle, gate, "Then", "Toggle");
		graph.Writes(gate, "Exit", "through");
		TestRuntime runtime = TestRuntime.Start(document);

		void Frame(params KeyCode[] keys)
		{
			runtime.Environment.PressedKeys.Clear();
			runtime.Environment.PressedKeys.UnionWith(keys);
			runtime.Invoke(ScriptEvent.Update);
			runtime.Records.Add("|");
		}
		Frame();
		Frame(KeyCode.O);
		Frame();
		Frame(KeyCode.T);
		Frame(KeyCode.T);
		Frame(KeyCode.C);
		Assert.Equal(["|", "through", "|", "through", "|", "|", "through", "|", "|"], runtime.Records);
	}

	[Fact]
	public void RerouteNodesPassTheFlowAndValuesOn()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint flow = graph.Node("Reroute");
		uint value = graph.Node("Reroute");
		uint write = graph.Node(RuntimeTesting.Write);
		graph.Link(create, "Then", flow, "In");
		graph.Link(flow, "Out", write, "Execute");
		graph.Link(graph.Literal("string", "\"routed\""), "Value", value, "In");
		graph.Link(value, "Out", write, "Text");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["routed"], runtime.Records);
	}

	[Fact]
	public void LoopIndexesBelongToTheirExecution()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 2 }""");
		uint send = graph.Node("Event.Send", """{ "Name": "Peek" }""");
		uint peek = graph.Node("Event.Custom", """{ "Name": "Peek" }""");
		graph.Flow(create, loop);
		graph.Flow(loop, send, "Body");
		graph.Flow(send, graph.Write(loop, "Index"));
		// The custom event runs as a nested execution of its own: the loop is not active in it.
		graph.Flow(peek, graph.Write(loop, "Index"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["0", "1", "0", "2"], runtime.Records);
	}
}
