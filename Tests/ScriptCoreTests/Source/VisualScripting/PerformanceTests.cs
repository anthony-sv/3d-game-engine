using System;
using System.Diagnostics;
using System.Globalization;
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
		Assert.Equal(VisualScriptStatus.Success, Counting(249_999, true).Invoke(ScriptEvent.Create));
		Assert.Equal(VisualScriptStatus.Failure, Counting(250_000, true).Invoke(ScriptEvent.Create));
		Assert.Equal(VisualScriptStatus.Success, Counting(76_922, false).Invoke(ScriptEvent.Create));
		Assert.Equal(VisualScriptStatus.Failure, Counting(76_923, false).Invoke(ScriptEvent.Create));

		// The timed loops chain ChainLength formulas (or groups of ten Add nodes) per iteration, so that the evaluations are
		// most of what an Update does.
		TestRuntime formulas = Counting(TimedIterations, true, "Event.Update", ChainLength);
		TestRuntime graphs = Counting(TimedIterations, false, "Event.Update", ChainLength);
		formulas.Invoke(ScriptEvent.Update);
		graphs.Invoke(ScriptEvent.Update);
		Assert.Equal(0, formulas.FewestBytesPerUpdate(3));
		Assert.Equal(0, graphs.FewestBytesPerUpdate(3));
		Assert.Empty(formulas.Environment.Logs);
		Assert.Empty(graphs.Environment.Logs);
		Assert.Equal(formulas.Variable("Counter"), graphs.Variable("Counter"));

		if (!PerformanceTesting.IsRequested)
		{
			return;
		}
		FormulaFigures figures = MeasureFormulas(formulas, graphs);
		TestContext.Current.TestOutputHelper?.WriteLine(figures.Describe());
		if (PerformanceTesting.IsMeasured)
		{
			Assert.True(figures.Ratio >= 3.0, figures.Describe());
		}
	}

	// Iterations of the timed loops, and the formulas each iteration chains: a graph Update stays inside one execution's
	// budget (ChainLength * 10 additions, the loop, the Get and the Set per iteration).
	private const int TimedIterations = 2_000;
	private const int ChainLength = 10;

	// Updates of each runtime measured, after the warm-up that lets tiered compilation finish.
	private const int WarmUpdates = 60;
	private const int MeasuredUpdates = 101;

	// A counting loop over First 1 to count: each iteration reads the counter, adds 1 through chain formulas of
	// "c + 1 + 0 + ... + 0" (formula: true) or chain groups of ten Add nodes (false), and sets it.
	private static TestRuntime Counting(int count, bool formula, string name = "Event.Create", int chain = 1)
	{
		DocumentBuilder document = new();
		uint counter = document.Variable("Counter", "int", "0");
		GraphBuilder graph = document.Graph;
		uint loop = graph.Node("Flow.ForLoop", inputs: FormattableString.Invariant($$"""{ "First": 1, "Last": {{count}} }"""));
		uint set = graph.Set(counter);
		uint previous = graph.Get(counter);
		string output = "Value";
		for (int link = 0; link < chain; link++)
		{
			if (formula)
			{
				string first = link == 0 ? "1" : "0";
				uint add = graph.Node("Expression", $$"""{ "Formula": "c + {{first}} + 0 + 0 + 0 + 0 + 0 + 0 + 0 + 0 + 0" }""");
				graph.Link(previous, output, add, "c");
				previous = add;
				output = "Result";
				continue;
			}
			for (int index = 0; index < 10; index++)
			{
				uint add = graph.Node("Operator.Add", inputs: link == 0 && index == 0 ? """{ "B": 1 }""" : """{ "B": 0 }""");
				graph.Link(previous, output, add, "A");
				previous = add;
				output = "Result";
			}
		}
		graph.Link(previous, output, set, "Value");
		graph.Flow(graph.Node(name), loop);
		graph.Flow(loop, set, "Body");
		TestRuntime runtime = TestRuntime.Start(document);
		if (formula)
		{
			GraphProgram program = ProgramCompiler.Compile(GraphAnalyzer.Analyze(RuntimeTesting.Library, Encoding.UTF8.GetBytes(document.Build())), "Count");
			Assert.All(program.EventGraph.Nodes.Where(static node => node.Op == Opcode.Expression), static node => Assert.Equal(10, node.Expression!.Code.Length));
		}
		return runtime;
	}

	// The formula against its graph measured steadily (S4-R5.3): Updates of the two runtimes alternate, so that the machine's
	// state changes both alike, and each pair gives the ratio of the graph's Update to the formula's. Whole Updates are compared,
	// never a difference that the loop's own cost could make negative: the loop costs both the same, so the ratio of the
	// evaluations alone is at least the ratio reported.
	private static FormulaFigures MeasureFormulas(TestRuntime formulas, TestRuntime graphs)
	{
		for (int update = 0; update < WarmUpdates; update++)
		{
			formulas.Invoke(ScriptEvent.Update);
			graphs.Invoke(ScriptEvent.Update);
		}
		double[] formulaTimes = new double[MeasuredUpdates];
		double[] graphTimes = new double[MeasuredUpdates];
		double[] ratios = new double[MeasuredUpdates];
		for (int update = 0; update < MeasuredUpdates; update++)
		{
			long start = Stopwatch.GetTimestamp();
			formulas.Invoke(ScriptEvent.Update);
			formulaTimes[update] = Stopwatch.GetElapsedTime(start).TotalMilliseconds;
			start = Stopwatch.GetTimestamp();
			graphs.Invoke(ScriptEvent.Update);
			graphTimes[update] = Stopwatch.GetElapsedTime(start).TotalMilliseconds;
			ratios[update] = graphTimes[update] / formulaTimes[update];
		}
		return new FormulaFigures(formulaTimes, graphTimes, ratios);
	}

	private sealed record FormulaFigures(double[] Formulas, double[] Graphs, double[] Ratios)
	{
		// The median of the pairs' ratios.
		internal double Ratio => PerformanceTesting.Median(Ratios);

		internal string Describe()
		{
			static string Spread(double[] values, string format) => FormattableString.Invariant(
				$"median {PerformanceTesting.Median(values).ToString(format, CultureInfo.InvariantCulture)} (10th to 90th percentile {PerformanceTesting.Percentile(values, 0.1).ToString(format, CultureInfo.InvariantCulture)} to {PerformanceTesting.Percentile(values, 0.9).ToString(format, CultureInfo.InvariantCulture)})");
			return FormattableString.Invariant(
				$"{ChainLength * 10} additions per iteration, {TimedIterations:N0} iterations, {Ratios.Length} alternating Updates{(PerformanceTesting.IsOptimized ? string.Empty : ", unoptimized")}: as formulas {Spread(Formulas, "F3")} ms, as a graph {Spread(Graphs, "F3")} ms, the graph's Update over the formulas' {Spread(Ratios, "F2")}x");
		}
	}
}
