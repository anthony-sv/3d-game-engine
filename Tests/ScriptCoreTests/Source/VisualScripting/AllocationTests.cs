using System;
using System.Linq;
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

		Assert.Equal(0, runtime.FewestBytesPerUpdate());
		Assert.Empty(runtime.Environment.Logs);
		Assert.Equal(52 * 79.0f, runtime.Variable("Total"));
		Assert.Equal(40, runtime.Variable("Picks"));
		Assert.NotEqual(new Vector3(0.0f, 0.0f, 0.0f), runtime.Variable("Offset"));
	}

	// A node logs its Warning once per instance (spec section 5.3): when it runs into the problem again, nothing of the
	// message is built (no text, no engine call for a name), so it allocates nothing.
	[Fact]
	public void AWarningLoggedAlreadyAllocatesNothing()
	{
		DocumentBuilder document = new();
		uint none = document.Variable("None", "Strada.Entity");
		uint other = document.Variable("Other", "Strada.Entity", isPublic: true);
		uint number = document.Variable("Number", "int", "0");
		GraphBuilder graph = document.Graph;
		uint sequence = graph.Node("Flow.Sequence", """{ "Count": 4 }""");
		graph.Flow(graph.Node("Event.Update"), sequence);
		uint toNone = graph.Node("Event.Send", """{ "Name": "Ping" }""");
		graph.Link(graph.Get(none), "Value", toNone, "Target");
		graph.Flow(sequence, toNone, "Then0");
		uint toOther = graph.Node("Event.Send", """{ "Name": "Ping" }""");
		graph.Link(graph.Get(other), "Value", toOther, "Target");
		graph.Flow(sequence, toOther, "Then1");
		graph.Flow(sequence, graph.Node("Event.Send", """{ "Name": "Missing", "Parameters": [{ "Name": "Count", "Type": "int" }] }"""), "Then2");
		uint convert = graph.Node("Convert", """{ "To": "int" }""");
		graph.Link(graph.Literal("string", "\"many\""), "Value", convert, "Value");
		uint set = graph.Set(number);
		graph.Link(convert, "Result", set, "Value");
		graph.Flow(sequence, set, "Then3");
		uint interval = graph.Node("Flow.Interval", inputs: """{ "Interval": 0.000001 }""");
		graph.Flow(graph.Node("Event.Create"), interval, "Then", "Start");
		graph.Flow(graph.Node("Event.Custom", """{ "Name": "Ping" }"""), graph.Write("pinged"));
		TestRuntime runtime = new();
		// With a script instance, Self is that object: looking it up allocates nothing.
		runtime.Environment.Add(TestRuntime.Hero, "Hero", new Mover());
		runtime.Environment.Add(5, "Crate");
		runtime.LoadWorking(document.Build());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(variables: """{ "Other": { "Type": "Entity", "Value": "5" } }"""));
		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 0.016f);
		string[] warnings = [.. runtime.Environment.Warnings];
		Assert.Equal(5, warnings.Length);
		foreach (string message in new[]
		{
			"Target is None, so no graph receives Ping", "Crate (5) runs no visual script to receive Ping",
			"the graph of Hero (7) has no custom event Missing(int)", "\"many\" cannot be read as ",
			"more than 1000 ticks were due in one update: the time beyond them is dropped",
		})
		{
			Assert.Contains(warnings, warning => warning.Contains(message, StringComparison.Ordinal));
		}

		Assert.Equal(0, runtime.FewestBytesPerUpdate());
		Assert.Equal(5, runtime.Environment.Warnings.Count());
		Assert.Empty(runtime.Environment.Errors);
		Assert.Empty(runtime.Records);
	}

	// Element read outside an active iteration is its type default (spec section 5.3, State), which the program holds.
	[Fact]
	public void TheElementOfAForEachThatDoesNotRunAllocatesNothing()
	{
		DocumentBuilder document = new();
		uint point = document.Variable("Point", "Strada.Vector3", "[1, 2, 3]");
		GraphBuilder graph = document.Graph;
		uint each = graph.Node("Flow.ForEach");
		graph.Link(graph.Node("Call:Strada.ScriptCore.Tests.Probe.Points(int)", inputs: """{ "Count": 3 }"""), "Result", each, "Array");
		uint set = graph.Set(point);
		graph.Link(each, "Element", set, "Value");
		graph.Flow(graph.Node("Event.Update"), set);
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Update, 0.016f);
		Assert.Equal(new Vector3(0.0f, 0.0f, 0.0f), runtime.Variable("Point"));

		Assert.Equal(0, runtime.FewestBytesPerUpdate());
		Assert.Empty(runtime.Environment.Logs);
	}
}
