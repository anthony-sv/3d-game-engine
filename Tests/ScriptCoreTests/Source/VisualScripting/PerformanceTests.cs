using System;
using System.Diagnostics;
using System.Linq;
using System.Reflection;
using System.Text;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The interpreter's hot loop (spec section 5.3): a tight loop of operators, a variable set and a ForLoop steps without
// allocating, and the test reports how many steps a second it runs (a Release build measures the number that matters).
public sealed class PerformanceTests
{
	// Steps per iteration of the loop below: the Set, its Get, Multiply and Add, and the loop's next iteration.
	private const int StepsPerIteration = 5;

	// Iterations per Update: just under the budget of one execution (2 steps more: the event and the loop).
	private const int Iterations = (VisualScriptRuntime.MaxSteps - 2) / StepsPerIteration;

	[Fact]
	public void ATightLoopStepsWithoutAllocating()
	{
		DocumentBuilder document = new();
		uint total = document.Variable("Total", "long", "\"0\"");
		GraphBuilder graph = document.Graph;
		uint loop = graph.Node("Flow.ForLoop", inputs: FormattableString.Invariant($$"""{ "First": 1, "Last": {{Iterations}} }"""));
		uint multiply = graph.Node("Operator.Multiply", inputs: """{ "B": 3 }""");
		uint add = graph.Node("Operator.Add");
		uint set = graph.Set(total);
		graph.Link(loop, "Index", multiply, "A");
		graph.Link(graph.Get(total), "Value", add, "A");
		graph.Link(multiply, "Result", add, "B");
		graph.Link(add, "Result", set, "Value");
		graph.Flow(graph.Node("Event.Update"), loop);
		graph.Flow(loop, set, "Body");
		TestRuntime runtime = TestRuntime.Start(document);
		// An optimized build runs long enough for the JIT to compile the interpreter fully (tiered compilation starts with
		// quick code): the steps a second of a game that has been running.
		bool optimized = typeof(VisualScriptRuntime).Assembly.GetCustomAttribute<DebuggableAttribute>()?.IsJITOptimizerDisabled != true;
		int warmUps = optimized ? 40 : 1;
		int updates = optimized ? 20 : 3;
		for (int update = 0; update < warmUps; update++)
		{
			Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update, 0.016f));
		}

		// A collection caused by another thread can count the unused rest of this thread's allocation buffer as allocated:
		// the Update that allocated least counts (a step that allocated would show in every one).
		long fewest = long.MaxValue;
		TimeSpan elapsed = TimeSpan.Zero;
		for (int update = 0; update < updates; update++)
		{
			long allocated = GC.GetAllocatedBytesForCurrentThread();
			long start = Stopwatch.GetTimestamp();
			runtime.Invoke(ScriptEvent.Update, 0.016f);
			elapsed += Stopwatch.GetElapsedTime(start);
			fewest = Math.Min(fewest, GC.GetAllocatedBytesForCurrentThread() - allocated);
		}

		Assert.Equal(0, fewest);
		Assert.Empty(runtime.Environment.Logs);
		// Every Update adds 3 (1 + 2 + ... + Iterations).
		long sum = (long)Iterations * (Iterations + 1) / 2 * 3;
		Assert.Equal((warmUps + updates) * sum, runtime.Variable("Total"));
		double steps = (double)updates * (2 + ((long)StepsPerIteration * Iterations));
		TestContext.Current.TestOutputHelper?.WriteLine(FormattableString.Invariant(
			$"{steps / elapsed.TotalSeconds / 1.0e6:F1} million steps per second ({elapsed.TotalMilliseconds / updates:F1} ms per Update of {steps / updates:F0} steps{(optimized ? string.Empty : ", unoptimized")})"));
	}

	// A formula is one node (spec-slice3 sections 3.2 and 3.3.8): ten additions cost one step against the ten its graph of
	// Add nodes costs, run as ten instructions without allocating. Only an optimized build with STRADA_PERFORMANCE_TESTS=1
	// measures the time: then the formula runs at least 3 times faster than the graph.
	[Fact]
	public void AFormulaIsOneStepAndRunsFasterThanItsGraph()
	{
		// Each iteration of the counting loop: the loop, the Set, its Get and the counter's computation; 2 steps more for the
		// event and the loop.
		// formula: null copies the counter (the loop's own cost), true adds through a formula, false through Add nodes.
		static TestRuntime Counting(int count, bool? formula, string name = "Event.Create")
		{
			DocumentBuilder document = new();
			uint counter = document.Variable("Counter", "int", "0");
			GraphBuilder graph = document.Graph;
			uint loop = graph.Node("Flow.ForLoop", inputs: FormattableString.Invariant($$"""{ "First": 1, "Last": {{count}} }"""));
			uint set = graph.Set(counter);
			uint get = graph.Get(counter);
			if (formula == null)
			{
				graph.Link(get, "Value", set, "Value");
			}
			else if (formula.Value)
			{
				uint add = graph.Node("Expression", """{ "Formula": "c + 1 + 0 + 0 + 0 + 0 + 0 + 0 + 0 + 0 + 0" }""");
				graph.Link(get, "Value", add, "c");
				graph.Link(add, "Result", set, "Value");
			}
			else
			{
				uint previous = get;
				for (int index = 0; index < 10; index++)
				{
					uint add = graph.Node("Operator.Add", inputs: index == 0 ? """{ "B": 1 }""" : """{ "B": 0 }""");
					graph.Link(previous, previous == get ? "Value" : "Result", add, "A");
					previous = add;
				}
				graph.Link(previous, "Result", set, "Value");
			}
			graph.Flow(graph.Node(name), loop);
			graph.Flow(loop, set, "Body");
			TestRuntime runtime = TestRuntime.Start(document);
			if (formula == true)
			{
				GraphProgram program = ProgramCompiler.Compile(GraphAnalyzer.Analyze(RuntimeTesting.Library, Encoding.UTF8.GetBytes(document.Build())), "Count");
				Assert.Equal(10, program.EventGraph.Nodes.Single(static node => node.Op == Opcode.Expression).Expression!.Code.Length);
			}
			return runtime;
		}
		Assert.Equal(VisualScriptStatus.Success, Counting(249_999, true).Invoke(ScriptEvent.Create));
		Assert.Equal(VisualScriptStatus.Failure, Counting(250_000, true).Invoke(ScriptEvent.Create));
		Assert.Equal(VisualScriptStatus.Success, Counting(76_922, false).Invoke(ScriptEvent.Create));
		Assert.Equal(VisualScriptStatus.Failure, Counting(76_923, false).Invoke(ScriptEvent.Create));

		TestRuntime formulas = Counting(10_000, true, "Event.Update");
		TestRuntime graphs = Counting(10_000, false, "Event.Update");
		TestRuntime copies = Counting(10_000, null, "Event.Update");
		formulas.Invoke(ScriptEvent.Update);
		Assert.Equal(0, formulas.FewestBytesPerUpdate(3));
		Assert.Empty(formulas.Environment.Logs);

		bool optimized = typeof(VisualScriptRuntime).Assembly.GetCustomAttribute<DebuggableAttribute>()?.IsJITOptimizerDisabled != true;
		if (!optimized || System.Environment.GetEnvironmentVariable("STRADA_PERFORMANCE_TESTS") != "1")
		{
			return;
		}
		static TimeSpan Fastest(TestRuntime runtime)
		{
			for (int update = 0; update < 40; update++)
			{
				runtime.Invoke(ScriptEvent.Update);
			}
			TimeSpan fastest = TimeSpan.MaxValue;
			for (int update = 0; update < 20; update++)
			{
				long start = Stopwatch.GetTimestamp();
				runtime.Invoke(ScriptEvent.Update);
				TimeSpan elapsed = Stopwatch.GetElapsedTime(start);
				fastest = elapsed < fastest ? elapsed : fastest;
			}
			return fastest;
		}
		// The evaluations alone: what the loop costs without them is subtracted.
		TimeSpan loop = Fastest(copies);
		TimeSpan formula = Fastest(formulas) - loop;
		TimeSpan graph = Fastest(graphs) - loop;
		TestContext.Current.TestOutputHelper?.WriteLine(FormattableString.Invariant(
			$"10 additions, 10,000 times: {formula.TotalMilliseconds:F3} ms as a formula, {graph.TotalMilliseconds:F3} ms as a graph"));
		Assert.True(formula.Ticks * 3 <= graph.Ticks, $"the formula took {formula.TotalMilliseconds:F3} ms and its graph {graph.TotalMilliseconds:F3} ms");
	}
}
