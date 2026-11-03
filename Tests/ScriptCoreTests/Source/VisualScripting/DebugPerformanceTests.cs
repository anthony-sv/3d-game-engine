using System;
using System.Diagnostics;
using System.Reflection;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// What the debugger costs (spec-slice4 sections 7.3 and 21): the tight loop of PerformanceTests untraced, and with a debugger
// attached that has nothing to debug in it. Both allocate nothing per Update, always; their speeds are printed, and only an
// optimized build with STRADA_PERFORMANCE_TESTS=1 asserts the bound on the attached debugger (3 times the target's slack).
public sealed class DebugPerformanceTests
{
	// Steps per iteration of the loop: the Set, its Get, Multiply and Add, and the loop's next iteration.
	private const int StepsPerIteration = 5;

	// Iterations per Update: just under the budget of one execution (2 steps more: the event and the loop).
	private const int Iterations = (VisualScriptRuntime.MaxSteps - 2) / StepsPerIteration;

	private const double StepsPerUpdate = 2 + ((double)StepsPerIteration * Iterations);

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
}
