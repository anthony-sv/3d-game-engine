using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Programs and instances (spec section 5.1): loading documents for graph handles, broken and effective programs,
// creating, replacing and destroying instances.
public sealed class ProgramTests
{
	private static string Writing(string text)
	{
		DocumentBuilder document = new();
		document.Graph.Writes(document.Graph.Node("Event.Create"), "Then", text);
		return document.Build();
	}

	private const string Broken = """{ "Graph": { "Nodes": [{ "ID": 1, "Type": "Flow.Nothing" }] } }""";

	public static TheoryData<string> Fixtures => ["Events.json", "Flow.json", "Data.json", "Functions.json", "Variables.json", "Reflected.json"];

	[Theory]
	[MemberData(nameof(Fixtures))]
	public void EveryFixtureCompiles(string name)
	{
		TestRuntime runtime = new();
		runtime.LoadWorking(File.ReadAllText(Path.Combine(VisualScriptTesting.FixtureDirectory, "Documents", name)));
		Assert.False(runtime.Runtime.FindEffectiveProgram(TestRuntime.Graph)!.IsBroken);
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
	}

	[Fact]
	public void TheExampleCompiles()
	{
		using JsonDocument file = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(VisualScriptTesting.FixtureDirectory, "Example.sgraph")));
		TestRuntime runtime = new();
		runtime.LoadWorking(file.RootElement.GetProperty("VisualScript").GetRawText());
		GraphProgram program = runtime.Runtime.FindEffectiveProgram(TestRuntime.Graph)!;
		Assert.Equal(["Speed", "Score"], program.Variables.Select(static variable => variable.Name));
		Assert.Equal(1, program.InputEvents.Length + program.Events.Count(static node => node >= 0));
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		Assert.Equal(4.0f, runtime.Variable("Speed"));
	}

	[Fact]
	public void LoadReturnsTheDiagnosticsAndKeepsAProgramForAnyDocument()
	{
		TestRuntime runtime = new();
		Assert.Equal(DiagnosticCode.InvalidDocument, Assert.Single(runtime.Load("")).Code);
		Assert.True(runtime.Runtime.FindLatestProgram(TestRuntime.Graph)!.IsBroken);
		Assert.Null(runtime.Runtime.FindEffectiveProgram(TestRuntime.Graph));
		Assert.Equal(DiagnosticCode.InvalidDocument, Assert.Single(runtime.Load("[1, 2]")).Code);
		Assert.Contains(runtime.Load(Broken), static diagnostic => diagnostic.Code == DiagnosticCode.UnknownNodeType);
	}

	[Fact]
	public void AGraphWithoutAWorkingProgramCannotRun()
	{
		TestRuntime runtime = new();
		IReadOnlyList<Diagnostic> diagnostics = runtime.Load(Broken);
		Diagnostic first = diagnostics.First(static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error);

		Assert.Equal(VisualScriptStatus.Failure, runtime.Create());
		Assert.Equal(System.FormattableString.Invariant($"{TestRuntime.Name} cannot run: 1 error, first: {first.Message} (node 1)"), runtime.SingleError());
		Assert.Null(runtime.Runtime.FindInstance(TestRuntime.Hero));
	}

	[Fact]
	public void ABrokenDocumentKeepsTheWorkingProgramRunning()
	{
		TestRuntime runtime = TestRuntime.Start(Writing("first"));
		runtime.Load(Broken);
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(8));
		runtime.Environment.Add(8, "Villain");

		runtime.Invoke(ScriptEvent.Create, entity: 8);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["first", "first"], runtime.Records);
		Assert.True(runtime.Runtime.FindLatestProgram(TestRuntime.Graph)!.IsBroken);
	}

	[Fact]
	public void AWorkingDocumentGivesNewInstancesItsProgram()
	{
		TestRuntime runtime = TestRuntime.Start(Writing("first"));
		runtime.LoadWorking(Writing("second"));
		runtime.Create(8);

		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Create, entity: 8);
		Assert.Equal(["first", "second"], runtime.Records);
	}

	[Fact]
	public void UnloadForgetsTheHandleButNotItsInstances()
	{
		TestRuntime runtime = TestRuntime.Start(Writing("running"));
		Assert.True(runtime.Runtime.Unload(TestRuntime.Graph));
		Assert.False(runtime.Runtime.Unload(TestRuntime.Graph));

		Assert.Equal(VisualScriptStatus.NotFound, runtime.Create(8));
		Assert.Equal(VisualScriptStatus.NotFound, runtime.Create(8, 404));
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(["running"], runtime.Records);
	}

	[Fact]
	public void InstancesAreDestroyedWithoutEvents()
	{
		DocumentBuilder document = new();
		document.Graph.Writes(document.Graph.Node("Event.Destroy"), "Then", "destroyed");
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Create(8);
		runtime.Create(9);

		Assert.True(runtime.Runtime.DestroyInstance(TestRuntime.Hero));
		Assert.False(runtime.Runtime.DestroyInstance(TestRuntime.Hero));
		Assert.Equal(2, runtime.Runtime.InstanceCount);
		runtime.Runtime.DestroyAllInstances();
		Assert.Equal(0, runtime.Runtime.InstanceCount);
		Assert.Empty(runtime.Records);
		Assert.Equal(VisualScriptStatus.NotFound, runtime.Invoke(ScriptEvent.Destroy, entity: 8));
	}

	[Fact]
	public void InvokeWithoutAnInstanceIsNotFound()
	{
		TestRuntime runtime = new();
		Assert.Equal(VisualScriptStatus.NotFound, runtime.Invoke(ScriptEvent.Update, 0.1f));
		Assert.Empty(runtime.Environment.Logs);
	}
}
