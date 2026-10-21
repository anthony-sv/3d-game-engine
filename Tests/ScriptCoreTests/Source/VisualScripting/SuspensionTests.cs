using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The coroutine model (spec section 5.3, Suspension): an execution that reaches a latent node suspends whole, with its
// position, flow stack and call frames, into a fiber of its instance, and continues exactly there when it resumes. When
// latent nodes resume is theirs to decide; these tests resume fibers themselves.
public sealed class SuspensionTests
{
	private static Execution SingleFiber(TestRuntime runtime, ulong entity = TestRuntime.Hero) => Assert.Single(runtime.Instance(entity).Fibers);

	[Fact]
	public void ASuspendedExecutionKeepsItsLoopsAndCallFrames()
	{
		DocumentBuilder document = new();
		FunctionBuilder wait = document.Function("Wait", inputs: ["Round:int"]);
		uint delay = wait.Graph.Node("Flow.Delay");
		uint text = wait.Graph.Node("FormatString", """{ "Format": "resumed {Round}" }""");
		wait.Graph.Link(wait.Entry, "Round", text, "Round");
		wait.Graph.Flow(wait.Entry, delay);
		wait.Graph.Flow(delay, wait.Graph.Write(text, "Result"), "Completed");
		GraphBuilder graph = document.Graph;
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 2 }""");
		uint call = graph.Node("Function.Call", wait.Settings);
		uint after = graph.Node("FormatString", """{ "Format": "after {Index}" }""");
		graph.Link(loop, "Index", call, "Round");
		graph.Link(loop, "Index", after, "Index");
		graph.Flow(graph.Node("Event.Create"), loop);
		graph.Flow(loop, call, "Body");
		graph.Flow(call, graph.Write(after, "Result"));
		graph.Writes(loop, "Completed", "done");
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		Assert.Empty(runtime.Records);
		Execution fiber = SingleFiber(runtime);
		Assert.Equal((delay, "Completed"), fiber.SuspendedPosition);
		// The event graph's frame and the call's frame, and the loop's entry.
		Assert.Equal(2, fiber.CallCount);
		Assert.Equal(1, fiber.FlowCount);
		Assert.False(runtime.Runtime.IsExecuting);

		Assert.Equal(VisualScriptStatus.Success, runtime.Runtime.Resume(fiber));
		Assert.Equal(["resumed 1", "after 1"], runtime.Records);
		Assert.Equal(VisualScriptStatus.Success, runtime.Runtime.Resume(SingleFiber(runtime)));
		Assert.Equal(["resumed 1", "after 1", "resumed 2", "after 2", "done"], runtime.Records);
		Assert.Empty(runtime.Instance().Fibers);
		Assert.Equal(VisualScriptStatus.NotFound, runtime.Runtime.Resume(fiber));
	}

	[Fact]
	public void NothingPushedOnTheStackRunsBeforeTheFiberResumes()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint sequence = graph.Node("Flow.Sequence");
		uint delay = graph.Node("Flow.Delay");
		graph.Flow(graph.Node("Event.Create"), sequence);
		graph.Flow(sequence, delay, "Then0");
		graph.Writes(delay, "Completed", "A");
		graph.Writes(sequence, "Then1", "B");
		graph.Writes(graph.Node("Event.Update"), "Then", "update");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update);
		runtime.Runtime.Resume(SingleFiber(runtime));
		Assert.Equal(["update", "A", "B"], runtime.Records);
	}

	[Fact]
	public void TheSuspendedPartOfASentEventBelongsToTheTarget()
	{
		DocumentBuilder hero = new();
		uint send = hero.Graph.Node("Event.Send", """{ "Name": "Wait" }""");
		hero.Graph.Flow(hero.Graph.Node("Event.Create"), send);
		hero.Graph.Link(hero.Graph.Node("Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)", inputs: """{ "Id": "8" }"""), "Result", send, "Target");
		hero.Graph.Writes(send, "Then", "sent");
		DocumentBuilder villain = new();
		uint delay = villain.Graph.Node("Flow.Delay");
		villain.Graph.Flow(villain.Graph.Node("Event.Custom", """{ "Name": "Wait" }"""), delay);
		villain.Graph.Writes(delay, "Completed", "waited");
		TestRuntime runtime = TestRuntime.Start(hero);
		runtime.Environment.Add(8, "Villain");
		runtime.LoadWorking(villain.Build(), 101);
		runtime.Create(8, 101);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["sent"], runtime.Records);
		Assert.Empty(runtime.Instance().Fibers);
		Execution fiber = SingleFiber(runtime, 8);
		runtime.Runtime.Resume(fiber);
		Assert.Equal(["sent", "waited"], runtime.Records);
	}

	[Fact]
	public void DestroyReachingALatentNodeEndsTheChainWithAWarning()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint delay = graph.Node("Flow.Delay");
		graph.Flow(graph.Node("Event.Destroy"), delay);
		graph.Writes(delay, "Completed", "never");
		TestRuntime runtime = TestRuntime.Start(document);
		GraphInstance instance = runtime.Instance();

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Destroy));
		Assert.Empty(runtime.Records);
		Assert.Empty(instance.Fibers);
		Assert.Equal([TestRuntime.On("Delay", delay) + "the graph's instance was destroyed, so nothing can wait here: the chain ends"],
			runtime.Environment.Warnings);
	}

	[Fact]
	public void FibersDieWithTheirInstance()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint delay = graph.Node("Flow.Delay");
		graph.Flow(graph.Node("Event.Create"), delay);
		graph.Writes(delay, "Completed", "never");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Execution replaced = SingleFiber(runtime);
		runtime.Create();
		Assert.Equal(VisualScriptStatus.NotFound, runtime.Runtime.Resume(replaced));
		runtime.Invoke(ScriptEvent.Create);
		Execution destroyed = SingleFiber(runtime);
		runtime.Runtime.DestroyInstance(TestRuntime.Hero);
		Assert.Equal(VisualScriptStatus.NotFound, runtime.Runtime.Resume(destroyed));
		Assert.Empty(runtime.Records);
	}

	[Fact]
	public void AResumedFiberIsOutermostWithABudgetOfItsOwn()
	{
		// 4 steps per iteration: the loops before and after the wait do not fit one budget together.
		DocumentBuilder document = new();
		uint counter = document.Variable("Counter", "int", "0");
		GraphBuilder graph = document.Graph;
		uint Loop()
		{
			uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 150000 }""");
			uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
			uint set = graph.Set(counter);
			graph.Link(graph.Get(counter), "Value", add, "A");
			graph.Link(add, "Result", set, "Value");
			graph.Flow(loop, set, "Body");
			return loop;
		}
		uint before = Loop();
		uint delay = graph.Node("Flow.Delay");
		uint after = Loop();
		graph.Flow(graph.Node("Event.Create"), before);
		graph.Flow(before, delay, "Completed");
		graph.Flow(delay, after, "Completed");
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(150_000, runtime.Variable("Counter"));
		Assert.Equal(VisualScriptStatus.Success, runtime.Runtime.Resume(SingleFiber(runtime)));
		Assert.Equal(300_000, runtime.Variable("Counter"));
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void EveryLatentNodeSuspends()
	{
		foreach (string type in new[] { "Flow.Delay", "Flow.RetriggerableDelay", "Flow.WaitFrames", "Flow.WaitUntil", "Flow.Tween", "Flow.Interval" })
		{
			DocumentBuilder document = new();
			GraphBuilder graph = document.Graph;
			uint latent = graph.Node(type);
			graph.Flow(graph.Node("Event.Create"), latent, "Then", type == "Flow.Interval" ? "Start" : "Execute");
			graph.Writes(latent, "Completed", "completed");
			TestRuntime runtime = TestRuntime.Start(document);

			runtime.Invoke(ScriptEvent.Create);
			Assert.Empty(runtime.Records);
			runtime.Runtime.Resume(SingleFiber(runtime));
			Assert.True(runtime.Records.Count == 1, $"{type} did not continue at Completed");
		}
	}
}
