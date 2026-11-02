using System;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Variables of another entity's graph (spec-slice3 section 4.5): a Get or Set with Graph reads or writes the variable of the
// Target entity's instance, which must run that graph; the variable is found in the program the instance runs now (by ID,
// else by name, with the node's type), and every step that cannot happen aborts the execution with its message.
public sealed class RemoteVariableTests
{
	private const ulong Window = 102;
	private const string WindowName = "Scripts/Window.sgraph";

	// Door's variables: the public float Health (ID 1, 10) and the private int Secret (ID 2, 3), or what a test gives.
	private static string Door(string variables = "") => $$"""{ "Variables": [{{(variables.Length > 0 ? variables : Health(1) + ", " + Secret)}}], "Functions": [], "Graph": { "Nodes": [], "Links": [], "Comments": [] } }""";

	private static string Health(uint id, string type = "float", string value = "10", bool isPublic = true) =>
		FormattableString.Invariant($$"""{ "ID": {{id}}, "Name": "Health", "Type": "{{type}}", "Value": {{value}}, "Public": {{(isPublic ? "true" : "false")}} }""");

	private const string Secret = """{ "ID": 2, "Name": "Secret", "Type": "int", "Value": 3 }""";

	private static string Bundle() => new BundleBuilder().Interface(CrossGraphTesting.Door, CrossGraphTesting.DoorName, Door()).Build();

	// Door (entity 8) runs Door, Window (entity 9) runs another graph, Ghost (entity 10) runs none; Hero runs reader.
	private static TestRuntime Start(DocumentBuilder reader, string? references = null)
	{
		TestRuntime runtime = new();
		runtime.Environment.Add(8, "Door");
		runtime.Environment.Add(9, "Window");
		runtime.Environment.Add(10, "Ghost");
		runtime.LoadWorking(Door(), CrossGraphTesting.Door, name: CrossGraphTesting.DoorName);
		runtime.LoadWorking(Door(), Window, name: WindowName);
		runtime.LoadWorking(reader.Build(), references: references ?? Bundle());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(8, CrossGraphTesting.Door));
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(9, Window));
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		return runtime;
	}

	// A reader that writes the Health of the entity on every Update; get is its Get node.
	private static DocumentBuilder Reader(ulong entity, out uint get)
	{
		DocumentBuilder reader = new();
		get = reader.Graph.Node("Variable.Get", CrossGraphTesting.Variable(1, CrossGraphTesting.Door));
		reader.Graph.Link(CrossGraphTesting.EntityOf(reader.Graph, entity), "Result", get, "Target");
		uint write = reader.Graph.Write(get, "Value");
		reader.Graph.Flow(reader.Graph.Node("Event.Update"), write);
		reader.Graph.Writes(write, "Then", "read");
		return reader;
	}

	[Fact]
	public void AGetReadsAndASetWritesTheVariableOfTheTargetsInstance()
	{
		DocumentBuilder reader = new();
		uint entity = CrossGraphTesting.EntityOf(reader.Graph, 8);
		uint before = reader.Graph.Node("Variable.Get", CrossGraphTesting.Variable(1, CrossGraphTesting.Door));
		uint set = reader.Graph.Node("Variable.Set", CrossGraphTesting.Variable(1, CrossGraphTesting.Door), """{ "Value": 25 }""");
		uint after = reader.Graph.Node("Variable.Get", CrossGraphTesting.Variable(1, CrossGraphTesting.Door));
		reader.Graph.Link(entity, "Result", before, "Target");
		reader.Graph.Link(entity, "Result", set, "Target");
		reader.Graph.Link(entity, "Result", after, "Target");
		uint first = reader.Graph.Write(before, "Value");
		reader.Graph.Flow(reader.Graph.Node("Event.Create"), first);
		reader.Graph.Flow(first, set);
		uint written = reader.Graph.Write(set, "Value");
		reader.Graph.Flow(set, written);
		reader.Graph.Flow(written, reader.Graph.Write(after, "Value"));
		TestRuntime runtime = Start(reader);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["10", "25", "25"], runtime.Records);
		Assert.Equal(25.0f, runtime.Variable("Health", 8));
		// Only Door's instance changed.
		Assert.Equal(10.0f, runtime.Variable("Health", 9));
		Assert.Empty(runtime.Environment.Logs);
	}

	[Theory]
	[InlineData(0UL, "Target is None")]
	[InlineData(10UL, "Ghost (10) runs no visual script")]
	[InlineData(9UL, "Window (9) runs Scripts/Window.sgraph, not Scripts/Door.sgraph")]
	public void ATargetThatDoesNotRunTheGraphAbortsTheExecution(ulong entity, string message)
	{
		TestRuntime runtime = Start(Reader(entity, out uint get));

		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Update));
		Assert.Empty(runtime.Records);
		Assert.Equal(TestRuntime.On("Get Health", get) + message, runtime.SingleError());
	}

	[Fact]
	public void ASetWritesAnotherEntityRunningTheOwnGraphPrivateVariablesIncluded()
	{
		// Hero and its twin (entity 8) run the same graph: its private Secret may be written on the twin.
		DocumentBuilder document = new();
		uint secret = document.Variable("Secret", "int", "3");
		uint set = document.Graph.Node("Variable.Set", CrossGraphTesting.Variable(secret, TestRuntime.Graph), """{ "Value": 9 }""");
		uint get = document.Graph.Node("Variable.Get", CrossGraphTesting.Variable(secret, TestRuntime.Graph));
		uint twin = CrossGraphTesting.EntityOf(document.Graph, 8);
		document.Graph.Link(twin, "Result", set, "Target");
		document.Graph.Link(twin, "Result", get, "Target");
		document.Graph.Flow(document.Graph.Node("Event.Create"), set);
		document.Graph.Flow(set, document.Graph.Write(get, "Value"));
		TestRuntime runtime = new();
		runtime.Environment.Add(8, "Twin");
		runtime.LoadWorking(document.Build(), references: new BundleBuilder().Build());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(8));
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["9"], runtime.Records);
		Assert.Equal(9, runtime.Variable("Secret", 8));
		Assert.Equal(3, runtime.Variable("Secret"));
		Assert.Empty(runtime.Environment.Logs);
	}

	// A live edit of Door that changes the variable the reader names: the reader's program is unchanged, and the node looks
	// the variable up again in the program Door's instance runs now.
	[Theory]
	[InlineData("""{ "ID": 2, "Name": "Secret", "Type": "int" }""", "Scripts/Door.sgraph has no variable Health any more")]
	[InlineData("""{ "ID": 1, "Name": "Health", "Type": "string", "Public": true }""", "Door's Health is a string now, not a float")]
	[InlineData("""{ "ID": 1, "Name": "Health", "Type": "float" }""", "Door's Health is private now")]
	[InlineData("""{ "ID": 5, "Name": "Health", "Type": "int", "Public": true }""", "Door's Health is an int now, not a float")]
	public void AVariableThatChangedSinceTheReaderCompiledAbortsTheRead(string variables, string message)
	{
		TestRuntime runtime = Start(Reader(8, out uint get));
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal(["10", "read"], runtime.Records);

		runtime.LoadWorking(Door(variables), CrossGraphTesting.Door, name: CrossGraphTesting.DoorName);
		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Update));
		Assert.Equal(["10", "read"], runtime.Records);
		Assert.Equal(TestRuntime.On("Get Health", get) + message, runtime.SingleError());
	}

	[Fact]
	public void AVariableWhoseIdChangedIsFoundByItsNameAndTheIndexFollowsLiveEdits()
	{
		TestRuntime runtime = Start(Reader(8, out uint get));
		runtime.Invoke(ScriptEvent.Update);
		CompiledNode node = runtime.Instance().Program.EventGraph.Nodes[runtime.Instance().Program.EventGraph.FindNode(get)];
		GraphProgram first = runtime.Instance(8).Program;
		Assert.Same(first, node.RemoteProgram);
		Assert.Equal(0, node.RemoteIndex);

		// Health gets another ID behind a new float variable: the live edit carries its value by name and type, and the reader
		// finds it by name at its new index.
		runtime.Instance(8).Variables[0] = Slot.FromFloat(25.0f);
		runtime.LoadWorking(Door("""{ "ID": 6, "Name": "Armor", "Type": "float", "Value": 99 }, """ + Health(7, value: "1")),
			CrossGraphTesting.Door, name: CrossGraphTesting.DoorName);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal(["10", "read", "25", "read"], runtime.Records);
		Assert.NotSame(first, node.RemoteProgram);
		Assert.Same(runtime.Instance(8).Program, node.RemoteProgram);
		Assert.Equal(1, node.RemoteIndex);
		Assert.Empty(runtime.Environment.Logs);
	}
}
