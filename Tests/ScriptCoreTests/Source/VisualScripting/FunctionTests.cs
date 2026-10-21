using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Functions (spec sections 4.3 and 5.3): pure and impure calls, Entry and Return, locals, call frames on the heap,
// recursion and the depth limit.
public sealed class FunctionTests
{
	[Fact]
	public void APureFunctionComputesItsOutputs()
	{
		DocumentBuilder document = new();
		FunctionBuilder midpoint = document.Function("Midpoint", pure: true, ["From:float", "To:float"], ["Middle:float"]);
		uint add = midpoint.Graph.Node("Operator.Add");
		uint half = midpoint.Graph.Node("Operator.Multiply", inputs: """{ "B": 0.5 }""");
		uint result = midpoint.Graph.Node("Function.Return");
		midpoint.Graph.Link(midpoint.Entry, "From", add, "A");
		midpoint.Graph.Link(midpoint.Entry, "To", add, "B");
		midpoint.Graph.Link(add, "Result", half, "A");
		midpoint.Graph.Link(half, "Result", result, "Middle");
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint call = graph.Node("Function.Call", midpoint.Settings, """{ "From": 2.0, "To": 10.0 }""");
		graph.Flow(create, graph.Write(call, "Middle"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["6"], runtime.Records);
	}

	[Fact]
	public void PureFunctionsCallPureFunctionsWithoutHostRecursion()
	{
		DocumentBuilder document = new();
		FunctionBuilder square = document.Function("Square", pure: true, ["X:int"], ["Y:int"]);
		uint times = square.Graph.Node("Operator.Multiply");
		uint squared = square.Graph.Node("Function.Return");
		square.Graph.Link(square.Entry, "X", times, "A");
		square.Graph.Link(square.Entry, "X", times, "B");
		square.Graph.Link(times, "Result", squared, "Y");
		FunctionBuilder fourth = document.Function("Fourth", pure: true, ["X:int"], ["Y:int"]);
		uint inner = fourth.Graph.Node("Function.Call", square.Settings);
		uint outer = fourth.Graph.Node("Function.Call", square.Settings);
		uint result = fourth.Graph.Node("Function.Return");
		fourth.Graph.Link(fourth.Entry, "X", inner, "X");
		fourth.Graph.Link(inner, "Y", outer, "X");
		fourth.Graph.Link(outer, "Y", result, "Y");
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint call = graph.Node("Function.Call", fourth.Settings, """{ "X": 3 }""");
		uint sum = graph.Node("Operator.Add");
		graph.Link(call, "Y", sum, "A");
		graph.Link(call, "Y", sum, "B");
		graph.Flow(create, graph.Write(sum, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["162"], runtime.Records);
	}

	[Fact]
	public void AnImpureFunctionRunsItsChainAndReturns()
	{
		DocumentBuilder document = new();
		FunctionBuilder greet = document.Function("Greet", inputs: ["Name:string"], outputs: ["Length:int"]);
		uint text = greet.Graph.Node("FormatString", """{ "Format": "hello {Name}" }""");
		greet.Graph.Link(greet.Entry, "Name", text, "Name");
		uint write = greet.Graph.Write(text, "Result");
		uint result = greet.Graph.Node("Function.Return", inputs: """{ "Length": 5 }""");
		greet.Graph.Flow(greet.Entry, write);
		greet.Graph.Flow(write, result);
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint call = graph.Node("Function.Call", greet.Settings, """{ "Name": "you" }""");
		graph.Flow(create, call);
		graph.Flow(call, graph.Write(call, "Length"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["hello you", "5"], runtime.Records);
	}

	[Fact]
	public void ReturnDiscardsTheCallsPendingContinuations()
	{
		DocumentBuilder document = new();
		FunctionBuilder early = document.Function("Early");
		uint sequence = early.Graph.Node("Flow.Sequence");
		uint loop = early.Graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 3 }""");
		early.Graph.Flow(early.Entry, sequence);
		early.Graph.Flow(sequence, loop, "Then0");
		uint index = early.Graph.Write(loop, "Index");
		early.Graph.Flow(loop, index, "Body");
		early.Graph.Flow(index, early.Graph.Node("Function.Return"));
		early.Graph.Writes(sequence, "Then1", "never");
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint outer = graph.Node("Flow.Sequence");
		uint call = graph.Node("Function.Call", early.Settings);
		graph.Flow(create, outer);
		graph.Flow(outer, call, "Then0");
		graph.Writes(call, "Then", "returned");
		graph.Writes(outer, "Then1", "caller continues");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["1", "returned", "caller continues"], runtime.Records);
	}

	[Fact]
	public void ACallWhoseChainEndsReturnsTheTypeDefaults()
	{
		DocumentBuilder document = new();
		uint flag = document.Variable("Flag", "bool", "true");
		FunctionBuilder maybe = document.Function("Maybe", outputs: ["Count:int", "Label:string"]);
		uint branch = maybe.Graph.Node("Flow.Branch");
		maybe.Graph.Flow(maybe.Entry, branch);
		maybe.Graph.Link(maybe.Graph.Get(flag), "Value", branch, "Condition");
		maybe.Graph.Flow(branch, maybe.Graph.Node("Function.Return", inputs: """{ "Count": 3, "Label": "set" }"""), "True");
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		uint call = graph.Node("Function.Call", maybe.Settings);
		uint text = graph.Node("FormatString", """{ "Format": "{Count} '{Label}'" }""");
		graph.Link(call, "Count", text, "Count");
		graph.Link(call, "Label", text, "Label");
		uint clear = graph.Set(flag, "false");
		graph.Flow(update, call);
		graph.Flow(call, clear);
		graph.Flow(clear, graph.Write(text, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal(["3 'set'", "0 ''"], runtime.Records);
	}

	[Fact]
	public void LocalsAndNodeStatesAreFreshOnEveryCall()
	{
		DocumentBuilder document = new();
		FunctionBuilder count = document.Function("Count");
		uint local = count.Local("Calls", "int", "10");
		uint add = count.Graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
		uint set = count.Graph.Set(local);
		uint once = count.Graph.Node("Flow.DoOnce");
		count.Graph.Link(count.Graph.Get(local), "Value", add, "A");
		count.Graph.Link(add, "Result", set, "Value");
		count.Graph.Flow(count.Entry, set);
		count.Graph.Flow(set, once);
		count.Graph.Flow(once, count.Graph.Write(set, "Value"), "Completed");
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		uint first = graph.Node("Function.Call", count.Settings);
		uint second = graph.Node("Function.Call", count.Settings);
		graph.Flow(update, first);
		graph.Flow(first, second);
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal(["11", "11", "11", "11"], runtime.Records);
	}

	[Fact]
	public void RecursionKeepsAFrameAndLoopPerCall()
	{
		// Total(N) = 1 + ... + N + Total(N - 1), accumulated in a local by a loop; Total(0) = 0.
		DocumentBuilder document = new();
		FunctionBuilder total = document.Function("Total", inputs: ["N:int"], outputs: ["Sum:int"]);
		GraphBuilder body = total.Graph;
		uint sum = total.Local("Accumulated", "int", "0");
		uint branch = body.Node("Flow.Branch");
		uint positive = body.Node("Operator.Greater", inputs: """{ "B": 0 }""");
		uint loop = body.Node("Flow.ForLoop", inputs: """{ "First": 1 }""");
		uint addIndex = body.Node("Operator.Add");
		uint accumulate = body.Set(sum);
		uint less = body.Node("Operator.Subtract", inputs: """{ "B": 1 }""");
		uint recurse = body.Node("Function.Call", total.Settings);
		uint addRest = body.Node("Operator.Add");
		uint finish = body.Set(sum);
		uint result = body.Node("Function.Return");
		uint zero = body.Node("Function.Return", inputs: """{ "Sum": 0 }""");
		body.Flow(total.Entry, branch);
		body.Link(total.Entry, "N", positive, "A");
		body.Link(positive, "Result", branch, "Condition");
		body.Flow(branch, loop, "True");
		body.Flow(branch, zero, "False");
		body.Link(total.Entry, "N", loop, "Last");
		body.Link(body.Get(sum), "Value", addIndex, "A");
		body.Link(loop, "Index", addIndex, "B");
		body.Link(addIndex, "Result", accumulate, "Value");
		body.Flow(loop, accumulate, "Body");
		body.Link(total.Entry, "N", less, "A");
		body.Link(less, "Result", recurse, "N");
		body.Flow(loop, recurse, "Completed");
		body.Link(body.Get(sum), "Value", addRest, "A");
		body.Link(recurse, "Sum", addRest, "B");
		body.Link(addRest, "Result", finish, "Value");
		body.Flow(recurse, finish);
		body.Link(finish, "Value", result, "Sum");
		body.Flow(finish, result);
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint call = graph.Node("Function.Call", total.Settings, """{ "N": 4 }""");
		graph.Flow(create, call);
		graph.Flow(call, graph.Write(call, "Sum"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["20"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Theory]
	[InlineData(62, true)]
	[InlineData(63, false)]
	public void RecursionPastTheDepthLimitIsANodeError(int depth, bool succeeds)
	{
		// Down(N) calls Down(N - 1) while N > 0: N + 1 frames, and the event graph's execution counts once.
		DocumentBuilder document = new();
		FunctionBuilder down = document.Function("Down", inputs: ["N:int"]);
		uint branch = down.Graph.Node("Flow.Branch");
		uint positive = down.Graph.Node("Operator.Greater", inputs: """{ "B": 0 }""");
		uint less = down.Graph.Node("Operator.Subtract", inputs: """{ "B": 1 }""");
		uint recurse = down.Graph.Node("Function.Call", down.Settings);
		down.Graph.Flow(down.Entry, branch);
		down.Graph.Link(down.Entry, "N", positive, "A");
		down.Graph.Link(positive, "Result", branch, "Condition");
		down.Graph.Link(down.Entry, "N", less, "A");
		down.Graph.Link(less, "Result", recurse, "N");
		down.Graph.Flow(branch, recurse, "True");
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint call = graph.Node("Function.Call", down.Settings, $$"""{ "N": {{depth}} }""");
		graph.Flow(create, call);
		graph.Writes(call, "Then", "returned");
		TestRuntime runtime = TestRuntime.Start(document);

		VisualScriptStatus status = runtime.Invoke(ScriptEvent.Create);
		if (succeeds)
		{
			Assert.Equal(VisualScriptStatus.Success, status);
			Assert.Equal(["returned"], runtime.Records);
			Assert.Empty(runtime.Environment.Logs);
			return;
		}
		Assert.Equal(VisualScriptStatus.Failure, status);
		Assert.Empty(runtime.Records);
		Assert.Equal(TestRuntime.On("Down", recurse) + "call depth exceeded 64", runtime.SingleError());
		Assert.False(runtime.Runtime.IsExecuting);
	}

	[Fact]
	public void ThePureCallOfAConsumerIsEvaluatedForEachConsumer()
	{
		DocumentBuilder document = new();
		FunctionBuilder roll = document.Function("Roll", pure: true, outputs: ["Value:int"]);
		uint next = roll.Graph.Node(RuntimeTesting.Next);
		uint result = roll.Graph.Node("Function.Return");
		roll.Graph.Link(next, "Result", result, "Value");
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint call = graph.Node("Function.Call", roll.Settings);
		uint pair = graph.Node("FormatString", """{ "Format": "{A} {B}" }""");
		graph.Link(call, "Value", pair, "A");
		graph.Link(call, "Value", pair, "B");
		uint first = graph.Write(pair, "Result");
		graph.Flow(create, first);
		graph.Flow(first, graph.Write(call, "Value"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		// Once per consumer evaluation: both placeholders share the first call, the second Write calls again.
		Assert.Equal(["1 1", "2"], runtime.Records);
	}

	[Fact]
	public void FunctionFramesComeBackToTheirPool()
	{
		DocumentBuilder document = new();
		FunctionBuilder noop = document.Function("Noop");
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		uint call = graph.Node("Function.Call", noop.Settings);
		graph.Flow(update, call);
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Update);
		// A call frame that did not come back would be made anew by every Update.
		Assert.Equal(0, runtime.FewestBytesPerUpdate());
		Assert.Empty(runtime.Instance().Fibers);
	}
}
