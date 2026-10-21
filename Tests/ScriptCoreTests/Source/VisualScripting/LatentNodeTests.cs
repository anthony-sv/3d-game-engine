using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The latent nodes Delay, RetriggerableDelay, WaitFrames and WaitUntil and latent function calls (spec sections 4.2, 4.3
// and 5.3): what waits for them, when they resume, and what other executions that reach them do. Records list what ran,
// in order.
public sealed class LatentNodeTests
{
	[Fact]
	public void ASequenceRunsItsNextOutputOnlyAfterTheDelayBeforeIt()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint sequence = graph.Node("Flow.Sequence");
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.5 }""");
		graph.Flow(graph.Node("Event.Create"), sequence);
		graph.Flow(sequence, delay, "Then0");
		graph.Writes(delay, "Completed", "A");
		graph.Writes(sequence, "Then1", "B");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Empty(runtime.Records);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["A", "B"], runtime.Records);
		Assert.Empty(runtime.Instance().Fibers);
	}

	[Fact]
	public void EveryIterationOfALoopWaitsForTheDelayInItsBody()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 3 }""");
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.25 }""");
		uint text = graph.Node("FormatString", """{ "Format": "iteration {Index}" }""");
		graph.Link(loop, "Index", text, "Index");
		graph.Flow(graph.Node("Event.Create"), loop);
		graph.Flow(loop, delay, "Body");
		graph.Flow(delay, graph.Write(text, "Result"), "Completed");
		graph.Writes(loop, "Completed", "done");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Empty(runtime.Records);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["iteration 1"], runtime.Records);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["iteration 1", "iteration 2"], runtime.Records);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["iteration 1", "iteration 2", "iteration 3", "done"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void AWhileLoopChecksItsConditionAfterEachDelayedBody()
	{
		DocumentBuilder document = new();
		uint counter = document.Variable("Counter", "int", "0");
		GraphBuilder graph = document.Graph;
		uint loop = graph.Node("Flow.While");
		uint less = graph.Node("Operator.Less", inputs: """{ "B": 3 }""");
		graph.Link(graph.Get(counter), "Value", less, "A");
		graph.Link(less, "Result", loop, "Condition");
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.25 }""");
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
		graph.Link(graph.Get(counter), "Value", add, "A");
		uint set = graph.Set(counter);
		graph.Link(add, "Result", set, "Value");
		graph.Flow(graph.Node("Event.Create"), loop);
		graph.Flow(loop, delay, "Body");
		graph.Flow(delay, set, "Completed");
		graph.Flow(set, graph.Write(set, "Value"));
		graph.Writes(loop, "Completed", "done");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["1", "2"], runtime.Records);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["1", "2", "3", "done"], runtime.Records);
	}

	[Fact]
	public void BreakAfterADelayEndsTheLoopAndCompletedRuns()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 10 }""");
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.25 }""");
		uint text = graph.Node("FormatString", """{ "Format": "iteration {Index}" }""");
		graph.Link(loop, "Index", text, "Index");
		uint write = graph.Write(text, "Result");
		graph.Flow(graph.Node("Event.Create"), loop);
		graph.Flow(loop, delay, "Body");
		graph.Flow(delay, write, "Completed");
		graph.Flow(write, loop, "Then", "Break");
		graph.Writes(loop, "Completed", "completed");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["iteration 1", "completed"], runtime.Records);
		Assert.Empty(runtime.Instance().Fibers);
	}

	[Fact]
	public void AnotherExecutionReachingAPendingDelayEndsThereAndRunsItsOwnContinuations()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 1.0 }""");
		uint sequence = graph.Node("Flow.Sequence");
		graph.Flow(graph.Node("Event.Create"), delay);
		graph.Writes(delay, "Completed", "delayed");
		graph.Flow(graph.Node("Event.KeyPressed", """{ "Key": "Space" }"""), sequence);
		graph.Flow(sequence, delay, "Then0");
		graph.Writes(sequence, "Then1", "the key's own continuation");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		runtime.Environment.PressedKeys.Add(KeyCode.Space);
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		Assert.Equal(["the key's own continuation"], runtime.Records);
		Assert.Single(runtime.Instance().Fibers);
		runtime.Environment.PressedKeys.Clear();
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Equal(["the key's own continuation", "delayed"], runtime.Records);
	}

	[Fact]
	public void ARetriggerableDelayKeepsOnlyItsLatestTrigger()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint retrigger = graph.Node("Flow.RetriggerableDelay", inputs: """{ "Duration": 1.0 }""");
		uint sequence = graph.Node("Flow.Sequence");
		graph.Flow(graph.Node("Event.Create"), sequence);
		graph.Flow(sequence, retrigger, "Then0");
		graph.Writes(sequence, "Then1", "the first trigger's next step");
		graph.Flow(graph.Node("Event.KeyPressed", """{ "Key": "Space" }"""), retrigger);
		graph.Writes(retrigger, "Completed", "waited");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Execution first = Assert.Single(runtime.Instance().Fibers);
		runtime.Environment.PressedKeys.Add(KeyCode.Space);
		runtime.Invoke(ScriptEvent.Update, 0.75f);
		Execution latest = Assert.Single(runtime.Instance().Fibers);
		runtime.Environment.PressedKeys.Clear();
		// The timer restarted with the latest trigger: the earlier one would have finished now.
		runtime.Invoke(ScriptEvent.Update, 0.75f);
		Assert.Empty(runtime.Records);
		Assert.Same(latest, Assert.Single(runtime.Instance().Fibers));
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Equal(["waited"], runtime.Records);
		Assert.Equal(VisualScriptStatus.NotFound, runtime.Runtime.Resume(first));
	}

	[Theory]
	[InlineData(0, 1)]
	[InlineData(-3, 1)]
	[InlineData(1, 1)]
	[InlineData(3, 3)]
	public void WaitFramesResumesInTheNthLaterUpdate(int frames, int resumesIn)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint wait = graph.Node("Flow.WaitFrames", inputs: $$"""{ "Frames": {{frames}} }""");
		graph.Flow(graph.Node("Event.Create"), wait);
		graph.Writes(wait, "Completed", "resumed");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		for (int update = 1; update < resumesIn; update++)
		{
			runtime.Invoke(ScriptEvent.Update, 1.0f);
			Assert.Empty(runtime.Records);
		}
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Equal(["resumed"], runtime.Records);
	}

	[Fact]
	public void WaitUntilATrueConditionContinuesWithoutSuspending()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint wait = graph.Node("Flow.WaitUntil", inputs: """{ "Condition": true }""");
		uint sequence = graph.Node("Flow.Sequence");
		graph.Flow(graph.Node("Event.Create"), sequence);
		graph.Flow(sequence, wait, "Then0");
		graph.Writes(wait, "Completed", "at once");
		graph.Writes(sequence, "Then1", "then");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["at once", "then"], runtime.Records);
		Assert.Empty(runtime.Instance().Fibers);
	}

	[Fact]
	public void WaitUntilChecksItsConditionOncePerLaterUpdate()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint greater = graph.Node("Operator.Greater", inputs: """{ "B": 3 }""");
		graph.Link(graph.Node(RuntimeTesting.Next), "Result", greater, "A");
		uint wait = graph.Node("Flow.WaitUntil");
		graph.Link(greater, "Result", wait, "Condition");
		graph.Flow(graph.Node("Event.Create"), wait);
		graph.Writes(wait, "Completed", "done");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(1, Probe.Evaluations);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(3, Probe.Evaluations);
		Assert.Empty(runtime.Records);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["done"], runtime.Records);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(4, Probe.Evaluations);
	}

	[Fact]
	public void AnErrorInAWaitUntilConditionAbortsAndDiscardsTheFiber()
	{
		DocumentBuilder document = new();
		uint divisor = document.Variable("Divisor", "int", "1");
		GraphBuilder graph = document.Graph;
		uint divide = graph.Node("Operator.Divide", inputs: """{ "A": 10 }""");
		graph.Link(graph.Get(divisor), "Value", divide, "B");
		uint greater = graph.Node("Operator.Greater", inputs: """{ "B": 100 }""");
		graph.Link(divide, "Result", greater, "A");
		uint wait = graph.Node("Flow.WaitUntil");
		graph.Link(greater, "Result", wait, "Condition");
		graph.Flow(graph.Node("Event.Create"), wait);
		graph.Writes(wait, "Completed", "never");
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update, 0.25f));
		runtime.Runtime.SetVariable(TestRuntime.Hero, "Divisor", typeof(int), 0);
		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Update, 0.25f));
		Assert.Equal(TestRuntime.On("Divide", divide) + "integer division by zero", runtime.SingleError());
		Assert.Empty(runtime.Instance().Fibers);
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update, 0.25f));
		Assert.Empty(runtime.Records);
	}

	[Fact]
	public void AFiberNeverResumesInTheDispatchThatCreatedIt()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint once = graph.Node("Flow.DoOnce");
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.0 }""");
		graph.Flow(graph.Node("Event.Update"), once);
		graph.Flow(once, delay, "Completed");
		graph.Writes(delay, "Completed", "resumed");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Empty(runtime.Records);
		Assert.Single(runtime.Instance().Fibers);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Equal(["resumed"], runtime.Records);
	}

	[Theory]
	[InlineData(0.5f, new[] { 0.25f, 0.125f, 0.125f })]
	[InlineData(1.0f, new[] { 0.5f, 0.25f, 0.125f, 0.0625f, 0.0625f })]
	[InlineData(0.0f, new[] { 0.0f })]
	[InlineData(-1.0f, new[] { 0.0f })]
	public void ADelayResumesInTheFirstUpdateWhereItsTimeStepsReachTheDuration(float duration, float[] steps)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint delay = graph.Node("Flow.Delay", inputs: $$"""{ "Duration": {{duration.ToString(System.Globalization.CultureInfo.InvariantCulture)}} }""");
		graph.Flow(graph.Node("Event.Create"), delay);
		graph.Writes(delay, "Completed", "resumed");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		for (int update = 0; update < steps.Length; update++)
		{
			Assert.Empty(runtime.Records);
			runtime.Invoke(ScriptEvent.Update, steps[update]);
		}
		Assert.Equal(["resumed"], runtime.Records);
	}

	[Fact]
	public void ADelayReadsItsDurationOnceWhenTheFlowEntersIt()
	{
		DocumentBuilder document = new();
		uint seconds = document.Variable("Seconds", "float", "0.5");
		GraphBuilder graph = document.Graph;
		uint delay = graph.Node("Flow.Delay");
		graph.Link(graph.Get(seconds), "Value", delay, "Duration");
		graph.Flow(graph.Node("Event.Create"), delay);
		graph.Writes(delay, "Completed", "resumed");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		runtime.Runtime.SetVariable(TestRuntime.Hero, "Seconds", typeof(float), 10.0f);
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		Assert.Equal(["resumed"], runtime.Records);
	}

	[Fact]
	public void AWaitUntilInAFunctionChecksItsConditionInTheCallsFrame()
	{
		DocumentBuilder document = new();
		uint counter = document.Variable("Counter", "int", "0");
		FunctionBuilder waitFor = document.Function("WaitFor", inputs: ["Goal:int"]);
		uint reached = waitFor.Graph.Node("Operator.GreaterOrEqual");
		waitFor.Graph.Link(waitFor.Graph.Get(counter), "Value", reached, "A");
		waitFor.Graph.Link(waitFor.Entry, "Goal", reached, "B");
		uint wait = waitFor.Graph.Node("Flow.WaitUntil");
		waitFor.Graph.Link(reached, "Result", wait, "Condition");
		waitFor.Graph.Flow(waitFor.Entry, wait);
		GraphBuilder graph = document.Graph;
		uint call = graph.Node("Function.Call", waitFor.Settings, """{ "Goal": 2 }""");
		graph.Flow(graph.Node("Event.Create"), call);
		graph.Writes(call, "Then", "reached");
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
		graph.Link(graph.Get(counter), "Value", add, "A");
		uint set = graph.Set(counter);
		graph.Link(add, "Result", set, "Value");
		graph.Flow(graph.Node("Event.Update"), set);
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Empty(runtime.Records);
		// Event.Update runs before the check: Counter is 2 in the second update.
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["reached"], runtime.Records);
	}

	[Fact]
	public void TwoExecutionsOfOneLatentCallWaitInFramesOfTheirOwn()
	{
		DocumentBuilder document = new();
		FunctionBuilder wait = document.Function("Wait", inputs: ["Seconds:float"], outputs: ["Waited:float"]);
		uint delay = wait.Graph.Node("Flow.Delay");
		uint done = wait.Graph.Node("Function.Return");
		wait.Graph.Link(wait.Entry, "Seconds", delay, "Duration");
		wait.Graph.Link(wait.Entry, "Seconds", done, "Waited");
		wait.Graph.Flow(wait.Entry, delay);
		wait.Graph.Flow(delay, done, "Completed");
		GraphBuilder graph = document.Graph;
		uint call = graph.Node("Function.Call", wait.Settings, """{ "Seconds": 0.5 }""");
		graph.Flow(graph.Node("Event.Create"), call);
		graph.Flow(graph.Node("Event.KeyPressed", """{ "Key": "Space" }"""), call);
		uint text = graph.Node("FormatString", """{ "Format": "waited {Waited}" }""");
		graph.Link(call, "Waited", text, "Waited");
		graph.Flow(call, graph.Write(text, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		runtime.Environment.PressedKeys.Add(KeyCode.Space);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		runtime.Environment.PressedKeys.Clear();
		Assert.Equal(2, runtime.Instance().Fibers.Count);
		Assert.All(runtime.Instance().Fibers, static fiber => Assert.Equal(2, fiber.CallCount));
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["waited 0.5"], runtime.Records);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["waited 0.5", "waited 0.5"], runtime.Records);
		Assert.Empty(runtime.Instance().Fibers);
	}

	[Fact]
	public void AReturnAfterADelayDiscardsTheCallsPendingContinuations()
	{
		DocumentBuilder document = new();
		FunctionBuilder run = document.Function("Run");
		uint sequence = run.Graph.Node("Flow.Sequence");
		uint delay = run.Graph.Node("Flow.Delay", inputs: """{ "Duration": 0.25 }""");
		run.Graph.Flow(run.Entry, sequence);
		run.Graph.Flow(sequence, delay, "Then0");
		run.Graph.Flow(delay, run.Graph.Node("Function.Return"), "Completed");
		run.Graph.Writes(sequence, "Then1", "never");
		GraphBuilder graph = document.Graph;
		uint call = graph.Node("Function.Call", run.Settings);
		graph.Flow(graph.Node("Event.Create"), call);
		graph.Writes(call, "Then", "after");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["after"], runtime.Records);
	}

	[Fact]
	public void AFiberReadsTheEventParametersASendToSelfOverwroteWhileItWaited()
	{
		const string hitEvent = """{ "Name": "Hit", "Parameters": [{ "Name": "Damage", "Type": "float" }] }""";
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint hit = graph.Node("Event.Custom", hitEvent);
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.5 }""");
		graph.Flow(hit, delay);
		graph.Flow(delay, graph.Write(hit, "Damage"), "Completed");
		uint first = graph.Node("Event.Send", hitEvent, """{ "Damage": 1.0 }""");
		uint second = graph.Node("Event.Send", hitEvent, """{ "Damage": 5.0 }""");
		graph.Flow(graph.Node("Event.Create"), first);
		graph.Flow(first, second);
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Single(runtime.Instance().Fibers);
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		Assert.Equal(["5"], runtime.Records);
	}

	[Fact]
	public void AnExecutionEnteringASuspendedLoopRunsBesideIt()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 2 }""");
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 1.0 }""");
		uint text = graph.Node("FormatString", """{ "Format": "iteration {Index}" }""");
		graph.Link(loop, "Index", text, "Index");
		graph.Flow(graph.Node("Event.Create"), loop);
		graph.Flow(graph.Node("Event.KeyPressed", """{ "Key": "Space" }"""), loop);
		graph.Flow(loop, delay, "Body");
		graph.Flow(delay, graph.Write(text, "Result"), "Completed");
		graph.Writes(loop, "Completed", "completed");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		runtime.Environment.PressedKeys.Add(KeyCode.Space);
		// The key's execution finds the Delay pending in both of its iterations, so it completes at once.
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		Assert.Equal(["completed"], runtime.Records);
		runtime.Environment.PressedKeys.Clear();
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		Assert.Equal(["completed", "iteration 1"], runtime.Records);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Equal(["completed", "iteration 1", "iteration 2", "completed"], runtime.Records);
	}

	[Fact]
	public void DestroyCancelsTheFibersAndTheirLatentNodesAreNoLongerPending()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 1.0 }""");
		graph.Flow(graph.Node("Event.Create"), delay);
		graph.Writes(delay, "Completed", "never");
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		GraphInstance instance = runtime.Instance();
		Execution fiber = Assert.Single(instance.Fibers);

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Destroy));
		Assert.Empty(instance.Fibers);
		Assert.Equal(VisualScriptStatus.NotFound, runtime.Runtime.Resume(fiber));
		Assert.Null(instance.Frame.Slots[instance.Program.EventGraph.Nodes[instance.Program.EventGraph.FindNode(delay)].StateSlot].Object);
		Assert.Equal(VisualScriptStatus.NotFound, runtime.Invoke(ScriptEvent.Update, 1.0f));
		Assert.Empty(runtime.Records);
	}

	// Two fibers wait in the order their Sends started them: a WaitUntil that is still false in the first Update keeps its
	// place, so in the second both resume in that order.
	[Theory]
	[InlineData(true)]
	[InlineData(false)]
	public void AWaitUntilThatStaysFalseKeepsItsPlaceInThePendingOrder(bool watchFirst)
	{
		DocumentBuilder document = new();
		uint go = document.Variable("Go", "bool", "false");
		GraphBuilder graph = document.Graph;
		uint watch = graph.Node("Event.Custom", """{ "Name": "Watch" }""");
		uint until = graph.Node("Flow.WaitUntil");
		graph.Link(graph.Get(go), "Value", until, "Condition");
		graph.Flow(watch, until);
		graph.Writes(until, "Completed", "watch");
		uint count = graph.Node("Event.Custom", """{ "Name": "Count" }""");
		uint frames = graph.Node("Flow.WaitFrames", inputs: """{ "Frames": 2 }""");
		graph.Flow(count, frames);
		graph.Writes(frames, "Completed", "count");
		uint sequence = graph.Node("Flow.Sequence");
		graph.Flow(graph.Node("Event.Create"), sequence);
		graph.Flow(sequence, graph.Node("Event.Send", watchFirst ? """{ "Name": "Watch" }""" : """{ "Name": "Count" }"""), "Then0");
		graph.Flow(sequence, graph.Node("Event.Send", watchFirst ? """{ "Name": "Count" }""" : """{ "Name": "Watch" }"""), "Then1");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(2, runtime.Instance().Fibers.Count);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Empty(runtime.Records);
		runtime.Runtime.SetVariable(TestRuntime.Hero, "Go", typeof(bool), true);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(watchFirst ? ["watch", "count"] : ["count", "watch"], runtime.Records);
		Assert.Empty(runtime.Instance().Fibers);
	}

	// A Delay and a WaitUntil waiting on what the Delay sets: in pending order, the WaitUntil sees the change in the same
	// Update only when the Delay became pending first.
	[Theory]
	[InlineData(true, 1)]
	[InlineData(false, 2)]
	public void PendingLatentNodesAdvanceInTheOrderTheyBecamePending(bool delayFirst, int watchedIn)
	{
		DocumentBuilder document = new();
		uint go = document.Variable("Go", "bool", "false");
		GraphBuilder graph = document.Graph;
		uint watch = graph.Node("Event.Custom", """{ "Name": "Watch" }""");
		uint until = graph.Node("Flow.WaitUntil");
		graph.Link(graph.Get(go), "Value", until, "Condition");
		graph.Flow(watch, until);
		graph.Writes(until, "Completed", "watched");
		uint start = graph.Node("Event.Custom", """{ "Name": "Start" }""");
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.0 }""");
		uint set = graph.Set(go, "true");
		graph.Flow(start, delay);
		graph.Flow(delay, set, "Completed");
		graph.Writes(set, "Then", "started");
		uint sequence = graph.Node("Flow.Sequence");
		graph.Flow(graph.Node("Event.Create"), sequence);
		graph.Flow(sequence, graph.Node("Event.Send", delayFirst ? """{ "Name": "Start" }""" : """{ "Name": "Watch" }"""), "Then0");
		graph.Flow(sequence, graph.Node("Event.Send", delayFirst ? """{ "Name": "Watch" }""" : """{ "Name": "Start" }"""), "Then1");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(watchedIn == 1 ? ["started", "watched"] : ["started"], runtime.Records);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["started", "watched"], runtime.Records);
	}

	// The latest trigger of a RetriggerableDelay waits in the place of the one it replaced: when it and a Delay that became
	// pending after the first trigger are due in one Update, it resumes first.
	[Fact]
	public void ARetriggeredDelayKeepsTheFirstTriggersPlaceInThePendingOrder()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint retrigger = graph.Node("Event.Custom", """{ "Name": "Retrigger" }""");
		uint retriggerable = graph.Node("Flow.RetriggerableDelay", inputs: """{ "Duration": 1.0 }""");
		graph.Flow(retrigger, retriggerable);
		graph.Writes(retriggerable, "Completed", "retriggered");
		uint wait = graph.Node("Event.Custom", """{ "Name": "Wait" }""");
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 1.5 }""");
		graph.Flow(wait, delay);
		graph.Writes(delay, "Completed", "waited");
		uint sequence = graph.Node("Flow.Sequence");
		graph.Flow(graph.Node("Event.Create"), sequence);
		graph.Flow(sequence, graph.Node("Event.Send", """{ "Name": "Retrigger" }"""), "Then0");
		graph.Flow(sequence, graph.Node("Event.Send", """{ "Name": "Wait" }"""), "Then1");
		graph.Flow(graph.Node("Event.KeyPressed", """{ "Key": "Space" }"""), graph.Node("Event.Send", """{ "Name": "Retrigger" }"""));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		runtime.Environment.PressedKeys.Add(KeyCode.Space);
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		runtime.Environment.PressedKeys.Clear();
		Assert.Equal(2, runtime.Instance().Fibers.Count);
		runtime.Invoke(ScriptEvent.Update, 1.0f);
		Assert.Equal(["retriggered", "waited"], runtime.Records);
		Assert.Empty(runtime.Instance().Fibers);
	}

	[Fact]
	public void InputEventsRunBeforeTheUpdateEventAndTheLatentNodes()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.0 }""");
		graph.Flow(graph.Node("Event.Create"), delay);
		graph.Writes(delay, "Completed", "delay");
		graph.Writes(graph.Node("Event.Update"), "Then", "update");
		graph.Writes(graph.Node("Event.KeyPressed", """{ "Key": "Space" }"""), "Then", "key");
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Environment.PressedKeys.Add(KeyCode.Space);

		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["key", "update", "delay"], runtime.Records);
	}
}
