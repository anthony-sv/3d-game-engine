using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// RunsGraph (spec-slice3 section 4.7): whether the Target entity's instance runs the graph; the guard of remote variables,
// so a Target that runs nothing, or None, is false rather than an error.
public sealed class RunsGraphTests
{
	private const ulong Window = 102;

	[Fact]
	public void RunsGraphIsTrueOnlyForAnEntityRunningTheGraph()
	{
		DocumentBuilder document = new();
		uint previous = document.Graph.Node("Event.Create");
		// Door (8) runs Door, Window (9) another graph, Ghost (10) none, 0 is None; an unlinked Target is Self.
		foreach (ulong? entity in (ulong?[])[8, 9, 10, 0, null])
		{
			uint runs = document.Graph.Node("RunsGraph", CrossGraphTesting.Runs(entity == null ? TestRuntime.Graph : CrossGraphTesting.Door));
			if (entity is { } id)
			{
				document.Graph.Link(CrossGraphTesting.EntityOf(document.Graph, id), "Result", runs, "Target");
			}
			uint write = document.Graph.Write(runs, "Result");
			document.Graph.Flow(previous, write);
			previous = write;
		}
		TestRuntime runtime = new();
		runtime.Environment.Add(8, "Door");
		runtime.Environment.Add(9, "Window");
		runtime.Environment.Add(10, "Ghost");
		runtime.LoadWorking(CrossGraphTesting.DoorScript().Build(), CrossGraphTesting.Door, name: CrossGraphTesting.DoorName);
		runtime.LoadWorking(CrossGraphTesting.DoorScript().Build(), Window, name: "Scripts/Window.sgraph");
		runtime.LoadWorking(document.Build(),
			references: new BundleBuilder().Interface(CrossGraphTesting.Door, CrossGraphTesting.DoorName, CrossGraphTesting.DoorScript()).Build());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(8, CrossGraphTesting.Door));
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(9, Window));
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["True", "False", "False", "False", "True"], runtime.Records);

		// An entity whose instance is destroyed runs no graph any more.
		Assert.Equal(VisualScriptStatus.Success, runtime.Runtime.DestroyInstance(8));
		runtime.Records.Clear();
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["False", "False", "False", "False", "True"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void RunsGraphGuardsARemoteRead()
	{
		// Read Door's Health only from an entity that runs Door: Window is skipped instead of aborting the execution.
		DocumentBuilder door = CrossGraphTesting.DoorWithVariables(out uint health, out _);
		DocumentBuilder reader = new();
		uint previous = reader.Graph.Node("Event.Create");
		string exit = "Then";
		foreach (ulong entity in (ulong[])[9, 8])
		{
			uint target = CrossGraphTesting.EntityOf(reader.Graph, entity);
			uint runs = reader.Graph.Node("RunsGraph", CrossGraphTesting.Runs(CrossGraphTesting.Door));
			uint get = reader.Graph.Node("Variable.Get", CrossGraphTesting.Variable(health, CrossGraphTesting.Door));
			uint branch = reader.Graph.Node("Flow.Branch");
			uint write = reader.Graph.Write(get, "Value");
			reader.Graph.Link(target, "Result", runs, "Target");
			reader.Graph.Link(target, "Result", get, "Target");
			reader.Graph.Link(runs, "Result", branch, "Condition");
			reader.Graph.Flow(previous, branch, exit);
			reader.Graph.Flow(branch, write, "True");
			previous = branch;
			exit = "False";
			reader.Graph.Flow(write, reader.Graph.Write("read"));
		}
		TestRuntime runtime = new();
		runtime.Environment.Add(8, "Door");
		runtime.Environment.Add(9, "Window");
		runtime.LoadWorking(door.Build(), CrossGraphTesting.Door, name: CrossGraphTesting.DoorName);
		runtime.LoadWorking(door.Build(), Window, name: "Scripts/Window.sgraph");
		runtime.LoadWorking(reader.Build(), references: new BundleBuilder().Interface(CrossGraphTesting.Door, CrossGraphTesting.DoorName, door).Build());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(8, CrossGraphTesting.Door));
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(9, Window));
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(["10", "read"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}
}
