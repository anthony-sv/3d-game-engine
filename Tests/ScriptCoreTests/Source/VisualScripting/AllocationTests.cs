using System;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The interpreter's steps allocate nothing (spec section 5.3, Values): once a graph ran (plans computed, invokers built,
// executions and frames pooled), running it again allocates no memory unless its nodes create objects.
public sealed class AllocationTests
{
	[Fact]
	public void RunningAGraphAgainAllocatesNothing()
	{
		DocumentBuilder document = new();
		uint total = document.Variable("Total", "float", "0.0");
		uint offset = document.Variable("Offset", "Strada.Vector3");
		uint picks = document.Variable("Picks", "int", "0");
		FunctionBuilder scale = document.Function("Scale", pure: true, ["Value:float"], ["Scaled:float"]);
		uint clamp = scale.Graph.Node("Call:Strada.Mathf.Clamp(float,float,float)", inputs: """{ "Min": 0.0, "Max": 2.0 }""");
		uint scaled = scale.Graph.Node("Function.Return");
		scale.Graph.Link(scale.Entry, "Value", clamp, "Value");
		scale.Graph.Link(clamp, "Result", scaled, "Scaled");
		FunctionBuilder accumulate = document.Function("Accumulate", inputs: ["Amount:float"]);
		uint local = accumulate.Local("Before", "float");
		uint remember = accumulate.Graph.Set(local);
		uint add = accumulate.Graph.Node("Operator.Add");
		uint store = accumulate.Graph.Set(total);
		accumulate.Graph.Link(accumulate.Graph.Get(total), "Value", remember, "Value");
		accumulate.Graph.Link(accumulate.Graph.Get(local), "Value", add, "A");
		accumulate.Graph.Link(accumulate.Entry, "Amount", add, "B");
		accumulate.Graph.Link(add, "Result", store, "Value");
		accumulate.Graph.Flow(accumulate.Entry, remember);
		accumulate.Graph.Flow(remember, store);

		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 0, "Last": 40 }""");
		uint sequence = graph.Node("Flow.Sequence", """{ "Count": 3 }""");
		uint toFloat = graph.Node("Convert", """{ "To": "float" }""");
		uint call = graph.Node("Function.Call", scale.Settings);
		uint accumulateCall = graph.Node("Function.Call", accumulate.Settings);
		uint choose = graph.Node("Flow.Switch", """{ "Type": "int", "Cases": [0, 1, 2] }""");
		uint modulo = graph.Node("Operator.Modulo", inputs: """{ "B": 3 }""");
		uint branch = graph.Node("Flow.Branch");
		uint even = graph.Node("Operator.Equal", inputs: """{ "B": 0 }""");
		uint flip = graph.Node("Flow.FlipFlop");
		uint make = graph.Node("Make", """{ "Type": "Strada.Vector3" }""", """{ "Y": 1.0 }""");
		uint sum = graph.Node("Operator.Add");
		uint move = graph.Set(offset);
		uint select = graph.Node("Select");
		uint pick = graph.Set(picks);
		uint self = graph.Node("Self");
		uint valid = graph.Node("IsValid");
		graph.Flow(update, loop);
		graph.Flow(loop, sequence, "Body");
		graph.Link(loop, "Index", toFloat, "Value");
		graph.Link(toFloat, "Result", call, "Value");
		graph.Link(call, "Scaled", accumulateCall, "Amount");
		graph.Flow(sequence, accumulateCall, "Then0");
		graph.Link(loop, "Index", modulo, "A");
		graph.Link(modulo, "Result", choose, "Selection");
		graph.Flow(sequence, choose, "Then1");
		graph.Flow(choose, branch, "Case:1");
		graph.Link(modulo, "Result", even, "A");
		graph.Link(even, "Result", branch, "Condition");
		graph.Flow(branch, flip, "False");
		graph.Link(toFloat, "Result", make, "X");
		graph.Link(graph.Get(offset), "Value", sum, "A");
		graph.Link(make, "Value", sum, "B");
		graph.Link(sum, "Result", move, "Value");
		graph.Flow(flip, move, "A");
		graph.Link(self, "Self", valid, "Value");
		graph.Link(valid, "Result", select, "Condition");
		graph.Link(loop, "Index", select, "True");
		graph.Link(modulo, "Result", select, "False");
		graph.Link(select, "Result", pick, "Value");
		graph.Flow(sequence, pick, "Then2");
		TestRuntime runtime = new();
		// With a script instance, Self is that object: looking it up allocates nothing.
		runtime.Environment.Add(TestRuntime.Hero, "Hero", new Mover());
		runtime.LoadWorking(document.Build());
		runtime.Create();

		runtime.Invoke(ScriptEvent.Update, 0.016f);
		runtime.Invoke(ScriptEvent.Update, 0.016f);
		// A collection caused by another thread can count the unused rest of this thread's allocation buffer as allocated:
		// the frame that allocated least counts (an allocation of the graph would show in every frame).
		long fewest = long.MaxValue;
		for (int frame = 0; frame < 50; frame++)
		{
			long before = GC.GetAllocatedBytesForCurrentThread();
			runtime.Invoke(ScriptEvent.Update, 0.016f);
			fewest = Math.Min(fewest, GC.GetAllocatedBytesForCurrentThread() - before);
		}

		Assert.Equal(0, fewest);
		Assert.Empty(runtime.Environment.Logs);
		Assert.Equal(52 * 79.0f, runtime.Variable("Total"));
		Assert.Equal(40, runtime.Variable("Picks"));
		Assert.NotEqual(new Vector3(0.0f, 0.0f, 0.0f), runtime.Variable("Offset"));
	}
}
