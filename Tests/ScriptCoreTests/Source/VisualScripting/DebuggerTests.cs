using System;
using System.Collections.Generic;
using System.Globalization;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The debug hook, version 2 (spec section 5.5, spec-slice4 section 7.4): a debugger decides how each run of an execution is
// traced (here by its instance's entity), and Record runs report every instruction and every value of a pure node they
// compute, and their call frames before they return to their pool.
public sealed class DebuggerTests
{
	// "<entity>: <node> <exit taken>" per step, "<entity>: <node>.<pin> = <value>" per value; a node of a library's function is
	// "<library handle>/<node>".
	private sealed class RecordingDebugger : IVisualScriptDebugger
	{
		internal HashSet<ulong> Targets { get; } = [];

		internal List<ulong> Asked { get; } = [];

		internal List<string> Steps { get; } = [];

		internal List<string> Values { get; } = [];

		internal List<string> Released { get; } = [];

		internal List<string> Aborted { get; } = [];

		public DebugMode Attach(Execution execution)
		{
			Asked.Add(execution.Instance.Entity);
			return Targets.Contains(execution.Instance.Entity) ? DebugMode.Record : DebugMode.None;
		}

		public bool Armed => false;

		public bool ShouldStop(Execution execution) => throw new InvalidOperationException("a Record run checks no stop point");

		public void OnStep(Execution execution, CompiledNode node, int exit)
		{
			string exitPin = exit >= 0 ? node.ExitPins[exit] : "-";
			Steps.Add(string.Create(CultureInfo.InvariantCulture, $"{execution.Instance.Entity}: {NodeName(node)} {exitPin}"));
		}

		public void OnEvaluated(Execution execution, CompiledNode node, Slot[] slots)
		{
			for (int output = 0; output < node.OutputCount; output++)
			{
				object? value = SlotValues.ToValue(slots[node.OutputBase + output], node.OutputTypes[output]);
				Values.Add(string.Create(CultureInfo.InvariantCulture, $"{execution.Instance.Entity}: {NodeName(node)}.{node.OutputPins[output]} = {value}"));
			}
		}

		public void OnFrameReleasing(Execution execution, Frame frame)
		{
			Released.Add(frame.Graph.Function!.Name);
		}

		public void OnAborted(Execution execution)
		{
			Aborted.Add(execution.Error!.Text);
		}

		public void OnStoppedReleased(Execution execution, string reason)
		{
			throw new InvalidOperationException("nothing stops for a debugger that never stops");
		}

		public void OnStoppedMigrated(Execution execution)
		{
			throw new InvalidOperationException("nothing stops for a debugger that never stops");
		}

		public void OnParked(Execution execution)
		{
			// Steps follow suspended executions; this debugger takes none.
		}

		public void OnReleased(Execution execution)
		{
			// As OnParked.
		}

		private static string NodeName(CompiledNode node) => node.Owner.Graph == 0 ? node.Id.ToString(CultureInfo.InvariantCulture)
			: string.Create(CultureInfo.InvariantCulture, $"{node.Owner.Graph}/{node.Id}");
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
		// The call's frame returns to its pool at the Return.
		Assert.Equal(["Greet"], debugger.Released);
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
		// A pure call's frame returns to its pool when its plan is done.
		Assert.Equal(["Twice"], debugger.Released);
	}

	// Every aborting error is reported while a debugger is attached, traced or not, its frames intact; a receiver's error
	// is reported again by the sender it aborts, with the same error object.
	[Fact]
	public void AbortsAreReportedWhetherTheExecutionIsTracedOrNot()
	{
		DocumentBuilder document = new();
		uint divisor = document.Variable("Divisor", "int", "0");
		GraphBuilder graph = document.Graph;
		uint send = graph.Node("Event.Send", """{ "Name": "Fail" }""");
		graph.Flow(graph.Node("Event.Create"), send);
		uint fail = graph.Node("Event.Custom", """{ "Name": "Fail" }""");
		uint divide = graph.Node("Operator.Divide", inputs: """{ "A": 10 }""");
		graph.Link(graph.Get(divisor), "Value", divide, "B");
		graph.Flow(fail, graph.Write(divide, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);
		RecordingDebugger debugger = new();
		runtime.Runtime.AttachDebugger(debugger);

		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Create));
		string error = runtime.SingleError();
		Assert.Equal([error, error], debugger.Aborted);
		Assert.Empty(debugger.Steps);
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

	// A library's node 7 and the caller's node 7 are told apart: steps in a library's function name the library.
	[Fact]
	public void StepsInALibrarysFunctionNameTheLibrary()
	{
		MotionLibrary library = new();
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint call = graph.Node("Function.Call", CrossGraphTesting.Call(library.Report, CrossGraphTesting.Motion), """{ "Text": "hello" }""");
		graph.Flow(create, call);
		TestRuntime runtime = new();
		runtime.LoadWorking(document.Build(), references: library.Bundle());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		RecordingDebugger debugger = new();
		debugger.Targets.Add(TestRuntime.Hero);
		runtime.Runtime.AttachDebugger(debugger);

		runtime.Invoke(ScriptEvent.Create);
		uint write = library.Report.Entry + 1;
		Assert.Equal(
		[
			Step(create, "Then"), Step(call, null), string.Create(CultureInfo.InvariantCulture, $"7: {CrossGraphTesting.Motion}/{library.Report.Entry} Then"),
			string.Create(CultureInfo.InvariantCulture, $"7: {CrossGraphTesting.Motion}/{write} Then"), Step(call, "Then"),
		], debugger.Steps);
		Assert.Equal(["hello"], runtime.Records);
	}

	// The values of a library's pure nodes name the library too, beside the caller's own values (graph 0).
	[Fact]
	public void ValuesInALibrarysFunctionNameTheLibrary()
	{
		MotionLibrary library = new();
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint literal = graph.Literal("int", "3");
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 4 }""");
		uint call = graph.Node("Function.Call", CrossGraphTesting.Call(library.Twice, CrossGraphTesting.Motion));
		graph.Link(literal, "Value", add, "A");
		graph.Link(add, "Result", call, "Value");
		graph.Flow(graph.Node("Event.Create"), graph.Write(call, "Result"));
		TestRuntime runtime = new();
		runtime.LoadWorking(document.Build(), references: library.Bundle());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		RecordingDebugger debugger = new();
		debugger.Targets.Add(TestRuntime.Hero);
		runtime.Runtime.AttachDebugger(debugger);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["14"], runtime.Records);
		// Twice's Operator.Multiply follows its entry.
		uint multiply = library.Twice.Entry + 1;
		Assert.Equal(
		[
			$"7: {literal}.Value = 3", $"7: {add}.Result = 7", string.Create(CultureInfo.InvariantCulture, $"7: {CrossGraphTesting.Motion}/{multiply}.Result = 14"),
			$"7: {call}.Result = 14",
		], debugger.Values);
	}
}
