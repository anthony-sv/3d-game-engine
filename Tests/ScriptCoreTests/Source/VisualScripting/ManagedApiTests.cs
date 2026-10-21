using System;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The managed API C#'s VisualScriptComponent forwards to (spec section 5.7): custom events and variables of an entity's
// graph instance, with values converted as links convert them.
public sealed class ManagedApiTests
{
	private const string Hit = """
		{ "Name": "Hit", "Parameters": [{ "Name": "Damage", "Type": "float" }, { "Name": "Source", "Type": "Strada.Entity" },
		{ "Name": "Label", "Type": "string" }, { "Name": "Handle", "Type": "Strada.AssetHandle" }, { "Name": "Anything", "Type": "System.Object" }] }
		""";

	// Hero's graph: Hit writes its parameters; Boom throws; Wait waits for a Delay; Spin counts 100,000 times.
	private static TestRuntime StartEvents()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint hit = graph.Node("Event.Custom", Hit);
		uint text = graph.Node("FormatString", """{ "Format": "{Damage} {Source} {Label} {Handle} {Anything}" }""");
		foreach (string parameter in new[] { "Damage", "Source", "Label", "Handle", "Anything" })
		{
			graph.Link(hit, parameter, text, parameter);
		}
		graph.Flow(hit, graph.Write(text, "Result"));
		uint fail = graph.Node("Call:Strada.ScriptCore.Tests.Probe.Throw(string)", inputs: """{ "Message": "boom" }""");
		graph.Flow(graph.Node("Event.Custom", """{ "Name": "Boom" }"""), fail);
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 0.25 }""");
		graph.Flow(graph.Node("Event.Custom", """{ "Name": "Wait" }"""), delay);
		graph.Writes(delay, "Completed", "waited");
		uint spin = graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 100000 }""");
		graph.Flow(graph.Node("Event.Custom", """{ "Name": "Spin" }"""), spin);
		graph.Flow(spin, graph.Node("Flow.Sequence"), "Body");
		return TestRuntime.Start(document);
	}

	[Fact]
	public void SendEventRunsTheEventWithItsArgumentsConvertedAsLinksConvert()
	{
		TestRuntime runtime = StartEvents();
		Mover mover = new() { ID = 9 };

		Assert.True(runtime.Runtime.SendEvent(TestRuntime.Hero, "Hit", [3, mover, 12, Probe.MaterialOf(5), new Vector3(1.0f, 2.0f, 3.0f)]));
		Assert.True(runtime.Runtime.SendEvent(TestRuntime.Hero, "Hit", [0.5f, Probe.TransformOf(new Entity(4)), "text", new AssetHandle(6), null]));
		Assert.Equal(["3 Entity(9) 12 5 (1, 2, 3)", "0.5 Entity(4) text 6 None"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void SendEventDoesNotRunWhatCannotRunAndWarns()
	{
		TestRuntime runtime = StartEvents();
		string prefix = $"{TestRuntime.Name} on Hero ({TestRuntime.Hero}): ";

		Assert.False(runtime.Runtime.SendEvent(99, "Hit", []));
		Assert.False(runtime.Runtime.SendEvent(TestRuntime.Hero, "Miss", []));
		Assert.False(runtime.Runtime.SendEvent(TestRuntime.Hero, "Hit", [1.0f]));
		Assert.False(runtime.Runtime.SendEvent(TestRuntime.Hero, "Hit", ["fast", null, "", new AssetHandle(1), null]));
		Assert.False(runtime.Runtime.SendEvent(TestRuntime.Hero, "Hit", [null, null, "", new AssetHandle(1), null]));
		Assert.False(runtime.Runtime.SendEvent(TestRuntime.Hero, "Hit", [1.0, null, "", new AssetHandle(1), null]));
		Assert.Empty(runtime.Records);
		Assert.Equal(
		[
			"SendEvent Hit:  (99) runs no visual script",
			prefix + "SendEvent Miss: the graph has no custom event Miss",
			prefix + "SendEvent Hit: the event takes 5 arguments, not 1",
			prefix + "SendEvent Hit: argument 1 (Damage) is a System.String, which does not convert to float",
			prefix + "SendEvent Hit: argument 1 (Damage) is None, which does not convert to float",
			prefix + "SendEvent Hit: argument 1 (Damage) is a System.Double, which does not convert to float",
		], runtime.Environment.Warnings);
	}

	[Fact]
	public void AnAbortInTheEventIsLoggedOnceAndThrown()
	{
		TestRuntime runtime = StartEvents();

		InvalidOperationException exception = Assert.Throws<InvalidOperationException>(() => runtime.Runtime.SendEvent(TestRuntime.Hero, "Boom", []));
		Assert.Equal(runtime.SingleError(), exception.Message);
		Assert.EndsWith("InvalidOperationException: boom", exception.Message, StringComparison.Ordinal);
		Assert.False(runtime.Runtime.IsExecuting);
	}

	[Fact]
	public void AnEventThatSuspendsHasRunAndItsFiberResumesWithTheInstance()
	{
		TestRuntime runtime = StartEvents();

		Assert.True(runtime.Runtime.SendEvent(TestRuntime.Hero, "Wait", []));
		Assert.Single(runtime.Instance().Fibers);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["waited"], runtime.Records);
	}

	[Fact]
	public void SendEventDuringAnExecutionDrawsFromItsBudget()
	{
		TestRuntime runtime = StartEvents();
		Probe.Runtime = runtime.Runtime;
		// Alone, Spin fits a budget of its own.
		Assert.True(runtime.Runtime.SendEvent(TestRuntime.Hero, "Spin", []));

		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		// 3 steps per iteration: 750,000 steps here and 300,000 in Spin do not fit one budget.
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 1, "Last": 250000 }""");
		uint send = graph.Node("Call:Strada.ScriptCore.Tests.Probe.Send(ulong,string)", inputs: """{ "Entity": "7", "Name": "Spin" }""");
		graph.Flow(graph.Node("Event.Create"), loop);
		graph.Flow(loop, graph.Node("Flow.Sequence"), "Body");
		graph.Flow(loop, send, "Completed");
		runtime.Environment.Add(8, "Villain");
		runtime.LoadWorking(document.Build(), 101);
		runtime.Create(8, 101);

		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Create, entity: 8));
		string[] errors = [.. runtime.Environment.Errors];
		Assert.Equal(2, errors.Length);
		Assert.EndsWith("execution exceeded 1,000,000 steps (endless loop?)", errors[0], StringComparison.Ordinal);
		Assert.Equal(FormattableString.Invariant($"{TestRuntime.Name}: Send (node {send}) on Villain (8): InvalidOperationException: {errors[0]}"), errors[1]);
	}

	[Fact]
	public void HasVariableKnowsPublicAndPrivateVariables()
	{
		DocumentBuilder document = new();
		document.Variable("Speed", "float", "1.0", isPublic: true);
		document.Variable("Secret", "int", "3");
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.True(runtime.Runtime.HasVariable(TestRuntime.Hero, "Speed"));
		Assert.True(runtime.Runtime.HasVariable(TestRuntime.Hero, "Secret"));
		Assert.False(runtime.Runtime.HasVariable(TestRuntime.Hero, "speed"));
		Assert.False(runtime.Runtime.HasVariable(99, "Speed"));
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void GetVariableConvertsByIdentityWideningAndUpcasts()
	{
		DocumentBuilder document = new();
		document.Variable("Count", "int", "3");
		document.Variable("Target", "Strada.Entity");
		document.Variable("Mode", "Strada.ForceMode", "\"Impulse\"");
		document.Variable("Name", "string", "\"hero\"");
		TestRuntime runtime = TestRuntime.Start(document);
		VisualScriptRuntime api = runtime.Runtime;

		Assert.Equal(3, api.GetVariable(TestRuntime.Hero, "Count", typeof(int)));
		Assert.Equal(3L, api.GetVariable(TestRuntime.Hero, "Count", typeof(long)));
		Assert.Equal(3.0f, api.GetVariable(TestRuntime.Hero, "Count", typeof(float)));
		Assert.Equal(3.0, api.GetVariable(TestRuntime.Hero, "Count", typeof(double)));
		Assert.Equal(3, api.GetVariable(TestRuntime.Hero, "Count", typeof(object)));
		Assert.Equal(ForceMode.Impulse, api.GetVariable(TestRuntime.Hero, "Mode", typeof(ForceMode)));
		Assert.Equal("hero", api.GetVariable(TestRuntime.Hero, "Name", typeof(string)));
		// None reads as null for references.
		Assert.Null(api.GetVariable(TestRuntime.Hero, "Target", typeof(Mover)));
		Mover mover = new() { ID = 9 };
		api.SetVariable(TestRuntime.Hero, "Target", typeof(Mover), mover);
		Assert.Same(mover, api.GetVariable(TestRuntime.Hero, "Target", typeof(Mover)));
		Assert.Same(mover, api.GetVariable(TestRuntime.Hero, "Target", typeof(Script)));
		Assert.Same(mover, api.GetVariable(TestRuntime.Hero, "Target", typeof(Entity)));
	}

	[Fact]
	public void GetVariableThrowsWhatTheCallerGotWrong()
	{
		DocumentBuilder document = new();
		document.Variable("Count", "int", "3");
		document.Variable("Speed", "float", "1.5");
		document.Variable("Target", "Strada.Entity");
		TestRuntime runtime = TestRuntime.Start(document);
		VisualScriptRuntime api = runtime.Runtime;

		Assert.Throws<InvalidOperationException>(() => api.GetVariable(99, "Count", typeof(int)));
		Assert.Equal("name", Assert.Throws<ArgumentException>(() => api.GetVariable(TestRuntime.Hero, "Missing", typeof(int))).ParamName);
		Assert.Throws<InvalidCastException>(() => api.GetVariable(TestRuntime.Hero, "Count", typeof(string)));
		Assert.Throws<InvalidCastException>(() => api.GetVariable(TestRuntime.Hero, "Count", typeof(short)));
		Assert.Throws<InvalidCastException>(() => api.GetVariable(TestRuntime.Hero, "Speed", typeof(int)));
		Assert.Throws<InvalidCastException>(() => api.GetVariable(TestRuntime.Hero, "Target", typeof(int)));
		api.SetVariable(TestRuntime.Hero, "Target", typeof(Entity), new Entity(4));
		Assert.Throws<InvalidCastException>(() => api.GetVariable(TestRuntime.Hero, "Target", typeof(Mover)));
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void SetVariableTakesValuesOfTheVariablesTypeOrOnesThatConvert()
	{
		DocumentBuilder document = new();
		document.Variable("Speed", "float", "1.0");
		document.Variable("Target", "Strada.Entity");
		document.Variable("Name", "string", "\"hero\"");
		document.Variable("Anything", "System.Object");
		TestRuntime runtime = TestRuntime.Start(document);
		VisualScriptRuntime api = runtime.Runtime;

		api.SetVariable(TestRuntime.Hero, "Speed", typeof(int), 3);
		Assert.Equal(3.0f, runtime.Variable("Speed"));
		api.SetVariable(TestRuntime.Hero, "Speed", typeof(object), 2.5f);
		Assert.Equal(2.5f, runtime.Variable("Speed"));
		api.SetVariable(TestRuntime.Hero, "Target", typeof(TransformComponent), Probe.TransformOf(new Entity(4)));
		Assert.Equal(new Entity(4), runtime.Variable("Target"));
		api.SetVariable(TestRuntime.Hero, "Target", typeof(Entity), null);
		Assert.Null(runtime.Variable("Target"));
		api.SetVariable(TestRuntime.Hero, "Name", typeof(string), null);
		Assert.Equal(string.Empty, runtime.Variable("Name"));
		api.SetVariable(TestRuntime.Hero, "Anything", typeof(Vector3), new Vector3(1.0f, 2.0f, 3.0f));
		Assert.Equal(new Vector3(1.0f, 2.0f, 3.0f), runtime.Variable("Anything"));

		Assert.Throws<InvalidOperationException>(() => api.SetVariable(99, "Speed", typeof(float), 1.0f));
		Assert.Equal("name", Assert.Throws<ArgumentException>(() => api.SetVariable(TestRuntime.Hero, "Missing", typeof(float), 1.0f)).ParamName);
		Assert.Throws<InvalidCastException>(() => api.SetVariable(TestRuntime.Hero, "Speed", typeof(string), "fast"));
		Assert.Throws<InvalidCastException>(() => api.SetVariable(TestRuntime.Hero, "Speed", typeof(double), 2.0));
		Assert.Throws<InvalidCastException>(() => api.SetVariable(TestRuntime.Hero, "Speed", typeof(float?), null));
		Assert.Throws<InvalidCastException>(() => api.SetVariable(TestRuntime.Hero, "Target", typeof(Material), null));
		Assert.Throws<InvalidCastException>(() => api.SetVariable(TestRuntime.Hero, "Name", typeof(int), 5));
		Assert.Equal(new Vector3(1.0f, 2.0f, 3.0f), runtime.Variable("Anything"));
		Assert.Empty(runtime.Environment.Logs);
	}
}
