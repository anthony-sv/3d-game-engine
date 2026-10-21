using System;
using System.Diagnostics;
using System.Reflection;
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
}
