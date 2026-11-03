using System;
using System.Diagnostics;
using System.Linq;
using System.Reflection;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// What the debugger costs (spec-slice4 sections 7.3 and 21): the tight loop of PerformanceTests untraced, with a debugger
// attached that has nothing to debug in it, in Break and Record runs and in Values runs, and the snapshot of a 2,000-node
// instance. The untraced, idle and Break and Record runs allocate nothing per Update, always; the figures are printed, and
// only an optimized build with STRADA_PERFORMANCE_TESTS=1 asserts their bounds.
public sealed class DebugPerformanceTests
{
	// Steps per iteration of the loop: the Set, its Get, Multiply and Add, and the loop's next iteration.
	private const int StepsPerIteration = 5;

	// Iterations per Update: just under the budget of one execution (2 steps more: the event and the loop).
	private const int Iterations = (VisualScriptRuntime.MaxSteps - 2) / StepsPerIteration;

	private const double StepsPerUpdate = 2 + ((double)StepsPerIteration * Iterations);

	// The bounds asserted on an optimized build with STRADA_PERFORMANCE_TESTS=1 (spec-slice4 section 21): 3 times the time V1
	// measured, as a share of untraced speed for the run modes and in milliseconds for the snapshot.
	private const double BreakAndRecordBound = 0.26;
	private const double ValuesBound = 0.12;
	private const double SnapshotBound = 0.11;

	private static bool IsOptimized => typeof(VisualScriptRuntime).Assembly.GetCustomAttribute<DebuggableAttribute>()?.IsJITOptimizerDisabled != true;

	private static bool IsMeasured => IsOptimized && System.Environment.GetEnvironmentVariable("STRADA_PERFORMANCE_TESTS") == "1";

	// The tight loop: Total += Index * 3 for Index 1 to Iterations, in every Update.
	private static TestRuntime TightLoop()
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
		return TestRuntime.Start(document);
	}

	// One Update's time and allocated bytes.
	private static (TimeSpan Elapsed, long Bytes) Measure(TestRuntime runtime)
	{
		long allocated = GC.GetAllocatedBytesForCurrentThread();
		long start = Stopwatch.GetTimestamp();
		VisualScriptStatus status = runtime.Invoke(ScriptEvent.Update, 0.016f);
		TimeSpan elapsed = Stopwatch.GetElapsedTime(start);
		long bytes = GC.GetAllocatedBytesForCurrentThread() - allocated;
		Assert.Equal(VisualScriptStatus.Success, status);
		return (elapsed, bytes);
	}

	// The fastest of the updates and the fewest bytes one allocated, after warm-up Updates (an optimized build runs long enough
	// for tiered compilation to compile the interpreter fully). A collection caused by another thread can count the unused rest
	// of this thread's allocation buffer as allocated: the Update that allocated least counts.
	private static (TimeSpan Fastest, long Fewest) Run(TestRuntime runtime, int updates)
	{
		TimeSpan fastest = TimeSpan.MaxValue;
		long fewest = long.MaxValue;
		for (int update = 0; update < updates; update++)
		{
			(TimeSpan elapsed, long bytes) = Measure(runtime);
			fastest = elapsed < fastest ? elapsed : fastest;
			fewest = Math.Min(fewest, bytes);
		}
		return (fastest, fewest);
	}

	private static double StepsPerSecond(TimeSpan elapsed) => StepsPerUpdate / elapsed.TotalSeconds;

	private static void AssertTotal(TestRuntime runtime, int updates)
	{
		long sum = (long)Iterations * (Iterations + 1) / 2 * 3;
		Assert.Equal(updates * sum, runtime.Variable("Total"));
	}

	[Fact]
	public void TheUntracedTightLoopStepsWithoutAllocating()
	{
		TestRuntime runtime = TightLoop();
		int warmUps = IsOptimized ? 40 : 1;
		int updates = IsOptimized ? 20 : 3;
		Run(runtime, warmUps);

		(TimeSpan fastest, long fewest) = Run(runtime, updates);
		Assert.Equal(0, fewest);
		Assert.Empty(runtime.Environment.Logs);
		AssertTotal(runtime, warmUps + updates);
		TestContext.Current.TestOutputHelper?.WriteLine(FormattableString.Invariant(
			$"untraced: {StepsPerSecond(fastest) / 1.0e6:F1} million steps per second ({fastest.TotalMilliseconds:F1} ms per Update{(IsOptimized ? string.Empty : ", unoptimized")})"));
	}

	// A debugger attached whose breakpoints and watches are all elsewhere decides every run untraced: one call per execution,
	// nothing per instruction.
	[Fact]
	public void AnAttachedIdleDebuggerCostsTheTightLoopNothingPerInstruction()
	{
		TestRuntime untraced = TightLoop();
		TestRuntime attached = TightLoop();
		DebugSession session = new(attached);
		const ulong elsewhere = 4242;
		session.Configure(
			[
				FormattableString.Invariant(
					$$"""{ "Document": "{{elsewhere}}", "Node": 3, "Type": "Flow.Branch", "Enabled": true, "Condition": "", "HitCount": 0 }"""),
			],
			watches: [FormattableString.Invariant($$"""{ "Document": "{{elsewhere}}", "Function": 0 }""")]);
		int warmUps = IsOptimized ? 40 : 1;
		int rounds = IsOptimized ? 20 : 3;
		Run(untraced, warmUps);
		Run(attached, warmUps);

		// Interleaved, so both see the same machine.
		TimeSpan fastestUntraced = TimeSpan.MaxValue;
		TimeSpan fastestAttached = TimeSpan.MaxValue;
		long fewest = long.MaxValue;
		for (int round = 0; round < rounds; round++)
		{
			(TimeSpan plain, _) = Measure(untraced);
			(TimeSpan traced, long bytes) = Measure(attached);
			fastestUntraced = plain < fastestUntraced ? plain : fastestUntraced;
			fastestAttached = traced < fastestAttached ? traced : fastestAttached;
			fewest = Math.Min(fewest, bytes);
		}
		Assert.Equal(0, fewest);
		Assert.Empty(attached.Environment.Logs);
		Assert.Empty(session.Stops());
		AssertTotal(attached, warmUps + rounds);
		Assert.Null(attached.Instance().Frame.Stamps);
		double ratio = fastestUntraced.TotalSeconds / fastestAttached.TotalSeconds;
		TestContext.Current.TestOutputHelper?.WriteLine(FormattableString.Invariant(
			$"attached and idle: {StepsPerSecond(fastestAttached) / 1.0e6:F1} million steps per second, {ratio * 100.0:F1} % of untraced ({StepsPerSecond(fastestUntraced) / 1.0e6:F1}){(IsOptimized ? string.Empty : ", unoptimized")}"));
		if (IsMeasured)
		{
			// The target is 95 %; the bound allows 3 times its slack's worth of noise on a shared machine.
			Assert.True(ratio >= 0.85, FormattableString.Invariant($"an idle debugger runs the loop at {ratio * 100.0:F1} % of untraced speed"));
		}
	}

	// The fastest Update of each runtime, measured in turns so both see the same machine, and the fewest bytes one of the
	// second's Updates allocated; between runs after each round, outside the measured time.
	private static (TimeSpan Plain, TimeSpan Traced, long Fewest) Interleave(TestRuntime plain, TestRuntime traced, int rounds, Action? between = null)
	{
		TimeSpan fastestPlain = TimeSpan.MaxValue;
		TimeSpan fastestTraced = TimeSpan.MaxValue;
		long fewest = long.MaxValue;
		for (int round = 0; round < rounds; round++)
		{
			(TimeSpan untraced, _) = Measure(plain);
			(TimeSpan measured, long bytes) = Measure(traced);
			between?.Invoke();
			fastestPlain = untraced < fastestPlain ? untraced : fastestPlain;
			fastestTraced = measured < fastestTraced ? measured : fastestTraced;
			fewest = Math.Min(fewest, bytes);
		}
		return (fastestPlain, fastestTraced, fewest);
	}

	private static void Report(string name, TimeSpan plain, TimeSpan traced)
	{
		double ratio = plain.TotalSeconds / traced.TotalSeconds;
		TestContext.Current.TestOutputHelper?.WriteLine(FormattableString.Invariant(
			$"{name}: {StepsPerSecond(traced) / 1.0e6:F1} million steps per second, {ratio * 100.0:F1} % of untraced ({StepsPerSecond(plain) / 1.0e6:F1}){(IsOptimized ? string.Empty : ", unoptimized")}"));
	}

	// Break and Record runs (spec-slice4 section 21): a breakpoint in the program on a node that never runs (a bit test per
	// instruction) and the event graph watched (a counter and a stamp per instruction), with a take after every Update.
	// After warm-up an Update allocates nothing.
	[Fact]
	public void BreakAndRecordRunsCountTheTightLoopWithoutAllocating()
	{
		TestRuntime untraced = TightLoop();
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
		uint never = graph.Write("never");
		graph.Flow(graph.Node("Event.FixedUpdate"), never);
		TestRuntime traced = TestRuntime.Start(document);
		DebugSession session = new(traced);
		session.Configure([session.Breakpoint(never)], watches: [FormattableString.Invariant($$"""{ "Document": "{{TestRuntime.Graph}}", "Function": 0 }""")]);
		int warmUps = IsOptimized ? 40 : 1;
		int rounds = IsOptimized ? 20 : 3;
		Run(untraced, warmUps);
		Run(traced, warmUps);
		traced.Runtime.TakeTrace();

		(TimeSpan plain, TimeSpan measured, long fewest) = Interleave(untraced, traced, rounds, () => traced.Runtime.TakeTrace());
		Assert.Equal(0, fewest);
		Assert.Empty(traced.Environment.Logs);
		Assert.Empty(session.Stops());
		AssertTotal(traced, warmUps + rounds);
		Assert.NotNull(traced.Instance().Frame.Stamps);
		Report("Break and Record runs", plain, measured);
		// The counts of one Update: the loop's Body once per iteration.
		traced.Invoke(ScriptEvent.Update, 0.016f);
		DebugTraceTests.TraceGraph counted = Assert.Single(DebugTraceTests.Read(traced.Runtime.TakeTrace().Text!).Graphs);
		Assert.Equal((uint)Iterations, counted.Steps.Single(step => step.Node == loop && step.Exit == "Body").Count);
		if (IsMeasured)
		{
			// Target: 65 % of untraced speed; the bound is 3 times the time V1 measured (spec-slice4 section 21).
			Assert.True(plain.TotalSeconds / measured.TotalSeconds >= BreakAndRecordBound,
				FormattableString.Invariant($"Break and Record runs reach {plain.TotalSeconds / measured.TotalSeconds * 100.0:F1} % of untraced speed"));
		}
	}

	// Values runs (graph.trace's recording, as its defaults record: a summary with values): every step and value goes to the
	// recording's summary.
	[Fact]
	public void ValuesRunsRecordTheTightLoop()
	{
		TestRuntime untraced = TightLoop();
		TestRuntime traced = TightLoop();
		DebugSession session = new(traced);
		int warmUps = IsOptimized ? 20 : 1;
		int rounds = IsOptimized ? 10 : 2;
		Run(untraced, warmUps);
		session.StartRecording(summary: true, maxSteps: 500, maxValues: 500);
		Run(traced, warmUps);

		(TimeSpan plain, TimeSpan measured, _) = Interleave(untraced, traced, rounds);
		JsonElement summary = session.StopRecording();
		JsonElement graph = Assert.Single(summary.GetProperty("Summary").GetProperty("Graphs").EnumerateArray());
		Assert.Contains(graph.GetProperty("Steps").EnumerateArray(), step => step.GetProperty("Count").GetInt64() == (long)Iterations * (warmUps + rounds));
		Assert.Empty(traced.Environment.Logs);
		Report("Values runs", plain, measured);
		if (IsMeasured)
		{
			// Target: 33 % of untraced speed; the bound is 3 times the time V1 measured (spec-slice4 section 21).
			Assert.True(plain.TotalSeconds / measured.TotalSeconds >= ValuesBound,
				FormattableString.Invariant($"Values runs reach {plain.TotalSeconds / measured.TotalSeconds * 100.0:F1} % of untraced speed"));
		}
	}

	// A 2,000-node instance's snapshot (spec-slice4 sections 5.1 and 21): 666 Variable.Set nodes in a chain, each fed by an
	// Operator.Add of a Variable.Get, stopped at the last one; the snapshot copies its frame, its stamps and its variables.
	[Fact]
	public void ASnapshotOfA2000NodeInstanceIsQuick()
	{
		DocumentBuilder document = new();
		uint total = document.Variable("Total", "int", "0");
		GraphBuilder graph = document.Graph;
		uint previous = graph.Node("Event.Update");
		for (int index = 0; index < 666; index++)
		{
			uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
			graph.Link(graph.Get(total), "Value", add, "A");
			uint set = graph.Set(total);
			graph.Link(add, "Result", set, "Value");
			graph.Flow(previous, set);
			previous = set;
		}
		TestRuntime runtime = TestRuntime.Start(document);
		Assert.Equal(1 + (666 * 3), runtime.Instance().Program.EventGraph.Nodes.Length);
		DebugSession session = new(runtime);
		session.Break(previous);
		runtime.Invoke(ScriptEvent.Update);
		Execution stopped = Assert.Single(runtime.Instance().DebugFibers);

		TimeSpan fastest = TimeSpan.MaxValue;
		DebugSnapshot? snapshot = null;
		for (int round = 0; round < (IsOptimized ? 50 : 3); round++)
		{
			long start = Stopwatch.GetTimestamp();
			snapshot = DebugSnapshot.Take(stopped, DebugPlace.Enter, [], -1);
			TimeSpan elapsed = Stopwatch.GetElapsedTime(start);
			fastest = elapsed < fastest ? elapsed : fastest;
		}
		DebugSnapshotFrame frame = Assert.Single(snapshot!.Members[0].Frames);
		Assert.Equal(runtime.Instance().Frame.Slots.Length, frame.Slots.Length);
		Assert.Equal(665, session.Describe(1).GetProperty("Variables")[0].GetProperty("Value").GetInt32());
		TestContext.Current.TestOutputHelper?.WriteLine(FormattableString.Invariant(
			$"a 2,000-node instance's snapshot: {fastest.TotalMilliseconds:F3} ms ({frame.Slots.Length} slots){(IsOptimized ? string.Empty : ", unoptimized")}"));
		if (IsMeasured)
		{
			// Target: 1 ms; the bound is 3 times the figure V1 measured (spec-slice4 section 21).
			Assert.True(fastest.TotalMilliseconds <= SnapshotBound, FormattableString.Invariant($"the snapshot took {fastest.TotalMilliseconds:F3} ms"));
		}
	}
}
