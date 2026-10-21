using System.Collections.Generic;
using System.Globalization;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The debug hook (spec section 5.5): a debugger picks the executions it traces by entity and hears of every instruction
// and every value of a pure node they compute.
public sealed class DebuggerTests
{
	// "<entity>: <node> <link taken>" per step, "<entity>: <node>.<pin> = <value>" per value.
	private sealed class RecordingDebugger : IVisualScriptDebugger
	{
		internal HashSet<ulong> Targets { get; } = [];

		internal List<ulong> Asked { get; } = [];

		internal List<string> Steps { get; } = [];

		internal List<string> Values { get; } = [];

		public bool IsTarget(ulong entity)
		{
			Asked.Add(entity);
			return Targets.Contains(entity);
		}

		public void OnStep(GraphInstance instance, uint node, string? linkTaken)
		{
			Steps.Add(string.Create(CultureInfo.InvariantCulture, $"{instance.Entity}: {node} {linkTaken ?? "-"}"));
		}

		public void OnValue(GraphInstance instance, uint node, string pin, object? value)
		{
			Values.Add(string.Create(CultureInfo.InvariantCulture, $"{instance.Entity}: {node}.{pin} = {value}"));
		}
	}

	private static string Step(uint node, string? link) => string.Create(CultureInfo.InvariantCulture, $"7: {node} {link ?? "-"}");

	[Fact]
	public void StepsNameEachInstructionsNodeAndTheOutputTheFlowLeftThrough()
	{
		DocumentBuilder document = new();
		FunctionBuilder function = document.Function("Greet");
		uint greet = function.Graph.Write("in the function");
		function.Graph.Flow(function.Entry, greet);
		// The Return is reported as the call continuing at Then.
		function.Graph.Flow(greet, function.Graph.Node("Function.Return"));
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint sequence = graph.Node("Flow.Sequence");
		uint call = graph.Node("Function.Call", function.Settings);
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.25 }""");
		uint once = graph.Node("Flow.DoOnce", """{ "StartClosed": true }""");
		uint after = graph.Write("after");
		graph.Flow(create, sequence);
		graph.Flow(sequence, call, "Then0");
		graph.Flow(sequence, delay, "Then1");
		graph.Flow(delay, after, "Completed");
		graph.Flow(after, once);
		TestRuntime runtime = TestRuntime.Start(document);
		RecordingDebugger debugger = new();
		debugger.Targets.Add(TestRuntime.Hero);
		runtime.Runtime.AttachDebugger(debugger);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(
		[
			Step(create, "Then"), Step(sequence, "Then0"), Step(call, null), Step(function.Entry, "Then"), Step(greet, "Then"),
			Step(call, "Then"), Step(sequence, "Then1"), Step(delay, null),
		], debugger.Steps);
		debugger.Steps.Clear();
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal([Step(delay, "Completed"), Step(after, "Then"), Step(once, null)], debugger.Steps);
	}

	[Fact]
	public void TracedExecutionsReportTheValuesOfPureNodes()
	{
		DocumentBuilder document = new();
		FunctionBuilder twice = document.Function("Twice", pure: true, ["Value:int"], ["Result:int"]);
		uint multiply = twice.Graph.Node("Operator.Multiply", inputs: """{ "B": 2 }""");
		uint result = twice.Graph.Node("Function.Return");
		twice.Graph.Link(twice.Entry, "Value", multiply, "A");
		twice.Graph.Link(multiply, "Result", result, "Result");
		GraphBuilder graph = document.Graph;
		uint literal = graph.Literal("int", "3");
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 4 }""");
		uint call = graph.Node("Function.Call", twice.Settings);
		graph.Link(literal, "Value", add, "A");
		graph.Link(add, "Result", call, "Value");
		uint write = graph.Write(call, "Result");
		graph.Flow(graph.Node("Event.Create"), write);
		TestRuntime runtime = TestRuntime.Start(document);
		RecordingDebugger debugger = new();
		debugger.Targets.Add(TestRuntime.Hero);
		runtime.Runtime.AttachDebugger(debugger);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["14"], runtime.Records);
		Assert.Equal(
		[
			$"7: {literal}.Value = 3", $"7: {add}.Result = 7", $"7: {multiply}.Result = 14", $"7: {call}.Result = 14",
		], debugger.Values);
	}

	[Fact]
	public void TheDebuggerChoosesTheExecutionsItTracesByEntity()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint send = graph.Node("Event.Send", """{ "Name": "Ping" }""");
		graph.Link(graph.Node("Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)", inputs: """{ "Id": "8" }"""), "Result", send, "Target");
		graph.Flow(graph.Node("Event.Create"), send);
		uint ping = graph.Node("Event.Custom", """{ "Name": "Ping" }""");
		graph.Flow(ping, graph.Write("pinged"));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Environment.Add(8, "Villain");
		runtime.Create(8);
		RecordingDebugger debugger = new();
		debugger.Targets.Add(8);
		runtime.Runtime.AttachDebugger(debugger);

		runtime.Invoke(ScriptEvent.Create);
		// Hero's execution is not traced; the Send runs Villain's event in an execution of its own, which is.
		Assert.Equal([TestRuntime.Hero, 8UL], debugger.Asked);
		Assert.Equal([$"8: {ping} Then", "8: " + (ping + 1) + " Then"], debugger.Steps);
		Assert.Empty(debugger.Values);
	}

	[Fact]
	public void NothingIsReportedWithoutATargetOrADebugger()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
		graph.Link(graph.Literal("int", "1"), "Value", add, "A");
		graph.Flow(graph.Node("Event.Update"), graph.Write(add, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);
		RecordingDebugger debugger = new();
		runtime.Runtime.AttachDebugger(debugger);

		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal([TestRuntime.Hero], debugger.Asked);
		Assert.Empty(debugger.Steps);
		Assert.Empty(debugger.Values);
		runtime.Runtime.AttachDebugger(null);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Single(debugger.Asked);
		Assert.Equal(["2", "2"], runtime.Records);
	}

	[Fact]
	public void ResumedFibersAndTicksAskAgainAndReportWhereTheyStart()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.5 }""");
		uint resumed = graph.Write("resumed");
		uint interval = graph.Node("Flow.Interval", inputs: """{ "Interval": 0.5, "Count": 1 }""");
		uint ticked = graph.Write("ticked");
		graph.Flow(graph.Node("Event.Create"), delay);
		graph.Flow(graph.Node("Event.FixedUpdate"), interval, "Then", "Start");
		graph.Flow(delay, resumed, "Completed");
		graph.Flow(interval, ticked, "Tick");
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.FixedUpdate, 0.02f);
		// Attached after the executions suspended: the fibers are traced once they resume.
		RecordingDebugger debugger = new();
		debugger.Targets.Add(TestRuntime.Hero);
		runtime.Runtime.AttachDebugger(debugger);

		runtime.Invoke(ScriptEvent.Update, 0.5f);
		Assert.Equal(["resumed", "ticked"], runtime.Records);
		// The resumed Delay, the Tick execution, then the Interval's resumed fiber.
		Assert.Equal([TestRuntime.Hero, TestRuntime.Hero, TestRuntime.Hero], debugger.Asked);
		Assert.Equal(
		[
			Step(delay, "Completed"), Step(resumed, "Then"), Step(interval, "Tick"), Step(ticked, "Then"), Step(interval, "Completed"),
		], debugger.Steps);
	}
}
