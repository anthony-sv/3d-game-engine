using System.Globalization;
using System.Linq;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Tween and Interval (spec section 4.2): latent nodes whose Tick outputs start an execution of their own each time they
// fire while the node is pending, before the suspended execution resumes at Completed.
public sealed class TweenIntervalTests
{
	// The values are the easing formulas of spec section 4.2 at t = 0.25, 0.5, 0.75 and 1, computed independently.
	public static TheoryData<string, float[]> Easings => new()
	{
		{ "Linear", [0.25f, 0.5f, 0.75f, 1.0f] },
		{ "EaseIn", [0.0625f, 0.25f, 0.5625f, 1.0f] },
		{ "EaseOut", [0.4375f, 0.75f, 0.9375f, 1.0f] },
		{ "EaseInOut", [0.125f, 0.5f, 0.875f, 1.0f] },
		{ "EaseInCubic", [0.015625f, 0.125f, 0.421875f, 1.0f] },
		{ "EaseOutCubic", [0.578125f, 0.875f, 0.984375f, 1.0f] },
		{ "EaseInOutCubic", [0.0625f, 0.5f, 0.9375f, 1.0f] },
		{ "EaseOutBack", [0.817409694f, 1.08769751f, 1.06413651f, 1.0f] },
		{ "EaseOutBounce", [0.47265625f, 0.765625f, 0.97265625f, 1.0f] },
		{ "EaseOutElastic", [0.911611676f, 1.015625f, 1.00552428f, 1.0f] },
	};

	// A Tween started by Event.Create whose Update writes Alpha and whose Completed writes "completed".
	private static (TestRuntime Runtime, uint Tween) StartTween(string easing, float duration, bool stopOnSpace = false)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint tween = graph.Node("Flow.Tween", $$"""{ "Easing": "{{easing}}" }""",
			$$"""{ "Duration": {{duration.ToString(CultureInfo.InvariantCulture)}} }""");
		graph.Flow(graph.Node("Event.Create"), tween);
		graph.Flow(tween, graph.Write(tween, "Alpha"), "Update");
		graph.Writes(tween, "Completed", "completed");
		if (stopOnSpace)
		{
			graph.Flow(graph.Node("Event.KeyPressed", """{ "Key": "Space" }"""), tween, "Then", "Stop");
		}
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		return (runtime, tween);
	}

	// An Interval started by Event.Create whose Tick writes Index and whose Completed writes "completed".
	private static (TestRuntime Runtime, uint Interval) StartInterval(float interval, int count)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint node = graph.Node("Flow.Interval",
			inputs: $$"""{ "Interval": {{interval.ToString(CultureInfo.InvariantCulture)}}, "Count": {{count}} }""");
		graph.Flow(graph.Node("Event.Create"), node, "Then", "Start");
		graph.Flow(node, graph.Write(node, "Index"), "Tick");
		graph.Writes(node, "Completed", "completed");
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		return (runtime, node);
	}

	[Theory]
	[MemberData(nameof(Easings))]
	public void ATweenTicksItsEasedAlphaEveryUpdateThenCompletes(string easing, float[] alphas)
	{
		(TestRuntime runtime, _) = StartTween(easing, 1.0f);

		for (int update = 0; update < 4; update++)
		{
			Assert.Equal(update, runtime.Records.Count);
			runtime.Invoke(ScriptEvent.Update, 0.25f);
		}
		Assert.Equal(5, runtime.Records.Count);
		for (int index = 0; index < alphas.Length; index++)
		{
			Assert.Equal(alphas[index], float.Parse(runtime.Records[index], CultureInfo.InvariantCulture), 1e-6f);
		}
		Assert.Equal("completed", runtime.Records[4]);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(5, runtime.Records.Count);
	}

	[Theory]
	[InlineData(0.0f)]
	[InlineData(-2.0f)]
	public void ATweenWithoutDurationTicksOnceWithAlphaOneThenCompletes(float duration)
	{
		(TestRuntime runtime, _) = StartTween("EaseIn", duration);

		runtime.Invoke(ScriptEvent.Update, 0.0f);
		Assert.Equal(["1", "completed"], runtime.Records);
	}

	[Fact]
	public void StopCancelsAPendingTweenWithoutCompleting()
	{
		(TestRuntime runtime, _) = StartTween("Linear", 1.0f, stopOnSpace: true);

		runtime.Invoke(ScriptEvent.Update, 0.25f);
		runtime.Environment.PressedKeys.Add(KeyCode.Space);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		runtime.Environment.PressedKeys.Clear();
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Equal(["0.25"], runtime.Records);
		Assert.Empty(runtime.Instance().Fibers);
		// Stopped, the Tween can start again.
		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Equal(["0.25", "1", "completed"], runtime.Records);
	}

	[Fact]
	public void ATweenReachedWhilePendingKeepsItsRun()
	{
		(TestRuntime runtime, _) = StartTween("Linear", 1.0f);

		runtime.Invoke(ScriptEvent.Update, 0.5f);
		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		Assert.Equal(["0.5", "1", "completed"], runtime.Records);
	}

	[Fact]
	public void AnIntervalTicksEveryIntervalCountTimesThenCompletes()
	{
		(TestRuntime runtime, _) = StartInterval(0.5f, 3);

		// The first tick fires an interval after Start.
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Empty(runtime.Records);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["0"], runtime.Records);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Equal(["0", "1", "2", "completed"], runtime.Records);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Equal(4, runtime.Records.Count);
		Assert.Empty(runtime.Instance().Fibers);
	}

	[Fact]
	public void StartingAPendingIntervalAgainChangesNothing()
	{
		(TestRuntime runtime, _) = StartInterval(0.5f, 2);

		runtime.Invoke(ScriptEvent.Update, 0.5f);
		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		Assert.Equal(["0", "1", "completed"], runtime.Records);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Equal(3, runtime.Records.Count);
	}

	[Fact]
	public void AnIntervalOfZeroTicksOncePerUpdateEndlessly()
	{
		(TestRuntime runtime, _) = StartInterval(0.0f, 0);

		for (int update = 0; update < 5; update++)
		{
			runtime.Invoke(ScriptEvent.Update, update * 10.0f);
		}
		Assert.Equal(["0", "1", "2", "3", "4"], runtime.Records);
		Assert.Single(runtime.Instance().Fibers);
	}

	[Fact]
	public void AnIntervalFiresAtMostAThousandTicksPerUpdateAndDropsTheRest()
	{
		(TestRuntime runtime, uint interval) = StartInterval(0.25f, 0);

		runtime.Invoke(ScriptEvent.Update, 300.0f);
		Assert.Equal(VisualScriptRuntime.MaxTicksPerUpdate, runtime.Records.Count);
		Assert.Equal("999", runtime.Records[^1]);
		string warning = TestRuntime.On("Interval", interval) + "more than 1000 ticks were due in one update: the time beyond them is dropped";
		Assert.Equal([warning], runtime.Environment.Warnings);
		runtime.Invoke(ScriptEvent.Update, 0.125f);
		Assert.Equal(1000, runtime.Records.Count);
		runtime.Invoke(ScriptEvent.Update, 0.125f);
		Assert.Equal("1000", runtime.Records[^1]);
		// The Warning is logged once per node and instance.
		runtime.Invoke(ScriptEvent.Update, 300.0f);
		Assert.Equal(2001, runtime.Records.Count);
		Assert.Equal([warning], runtime.Environment.Warnings);
	}

	[Fact]
	public void StopFromATickCancelsTheIntervalAtOnce()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint interval = graph.Node("Flow.Interval", inputs: """{ "Interval": 0.25, "Count": 0 }""");
		uint write = graph.Write(interval, "Index");
		uint second = graph.Node("Operator.Equal", inputs: """{ "B": 1 }""");
		graph.Link(interval, "Index", second, "A");
		uint branch = graph.Node("Flow.Branch");
		graph.Link(second, "Result", branch, "Condition");
		graph.Flow(graph.Node("Event.Create"), interval, "Then", "Start");
		graph.Flow(interval, write, "Tick");
		graph.Flow(write, branch);
		graph.Flow(branch, interval, "True", "Stop");
		graph.Writes(interval, "Completed", "completed");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Equal(["0", "1"], runtime.Records);
		Assert.Empty(runtime.Instance().Fibers);
	}

	[Fact]
	public void TicksRunWithBudgetsOfTheirOwn()
	{
		// 4 steps per iteration: one tick takes 600,000 steps, so two in one update would not fit one budget.
		DocumentBuilder document = new();
		uint counter = document.Variable("Counter", "int", "0");
		GraphBuilder graph = document.Graph;
		uint interval = graph.Node("Flow.Interval", inputs: """{ "Interval": 0.5, "Count": 2 }""");
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 150000 }""");
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
		uint set = graph.Set(counter);
		graph.Link(graph.Get(counter), "Value", add, "A");
		graph.Link(add, "Result", set, "Value");
		graph.Flow(graph.Node("Event.Create"), interval, "Then", "Start");
		graph.Flow(interval, loop, "Tick");
		graph.Flow(loop, set, "Body");
		graph.Writes(interval, "Completed", "completed");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update, 1.0f));
		Assert.Equal(300_000, runtime.Variable("Counter"));
		Assert.Equal(["completed"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void ADelayInATickWaitsForALaterUpdate()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint interval = graph.Node("Flow.Interval", inputs: """{ "Interval": 0.5, "Count": 1 }""");
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.0 }""");
		graph.Flow(graph.Node("Event.Create"), interval, "Then", "Start");
		graph.Flow(interval, delay, "Tick");
		graph.Writes(delay, "Completed", "the tick's delay");
		graph.Writes(interval, "Completed", "completed");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		Assert.Equal(["completed"], runtime.Records);
		runtime.Invoke(ScriptEvent.Update, 0.0f);
		Assert.Equal(["completed", "the tick's delay"], runtime.Records);
	}

	[Fact]
	public void AnAbortingTickFailsItsUpdateAndTheIntervalGoesOn()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint interval = graph.Node("Flow.Interval", inputs: """{ "Interval": 0.5, "Count": 2 }""");
		uint fail = graph.Node("Call:Strada.ScriptCore.Tests.Probe.Throw(string)", inputs: """{ "Message": "tick" }""");
		graph.Flow(graph.Node("Event.Create"), interval, "Then", "Start");
		graph.Flow(interval, fail, "Tick");
		graph.Writes(interval, "Completed", "completed");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Update, 1.0f));
		Assert.Equal(2, runtime.Environment.Errors.Count());
		Assert.Equal(["completed"], runtime.Records);
	}
}
