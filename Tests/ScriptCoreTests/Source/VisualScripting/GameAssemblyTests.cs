using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Threading.Tasks;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Game-assembly reload (spec section 5.6) and the engine's node library (spec section 5.1): before the game assembly
// unloads, nothing of the runtime may reference its types; after it loaded or unloaded, programs compile again from their
// documents when next used.
public sealed class GameAssemblyTests
{
	// The test library without the game's script classes, as after the game assembly unloaded.
	private static readonly Lazy<NodeLibrary> s_WithoutGame = new(() => NodeLibraryBuilder.Build(new NodeLibrarySources
	{
		ApiTypes = [.. NodeLibrarySources.GetApiTypes(), typeof(Probe), typeof(Pair), typeof(Holder)],
	}));

	// A graph that needs the game: a Mover variable and Entity.As<Mover>.
	private static string MoverDocument()
	{
		DocumentBuilder document = new();
		uint mover = document.Variable("Mover", "Strada.ScriptCore.Tests.Mover");
		GraphBuilder graph = document.Graph;
		uint cast = graph.Node("Call:Strada.Entity.As<T>()", """{ "TypeArguments": ["Strada.ScriptCore.Tests.Mover"] }""");
		graph.Link(graph.Node("Self"), "Self", cast, "Target");
		uint set = graph.Set(mover);
		graph.Link(cast, "Result", set, "Value");
		graph.Flow(graph.Node("Event.Create"), set);
		graph.Writes(set, "Then", "created");
		return document.Build();
	}

	// A runtime whose library the test switches, as the engine's changes with the game assembly.
	private sealed class SwitchingRuntime
	{
		internal SwitchingRuntime()
		{
			Probe.Reset();
			Runtime = new VisualScriptRuntime(Environment, () => Library);
			Environment.Add(TestRuntime.Hero, "Hero");
		}

		internal NodeLibrary Library { get; set; } = RuntimeTesting.Library;

		internal FakeEnvironment Environment { get; } = new();

		internal VisualScriptRuntime Runtime { get; }

		internal VisualScriptStatus Create() => Runtime.CreateInstance(TestRuntime.Hero, TestRuntime.Graph, []);

		internal void Load(string document) => Runtime.Load(TestRuntime.Graph, TestRuntime.Name, System.Text.Encoding.UTF8.GetBytes(document));
	}

	[Fact]
	public void UnloadingDropsTheInstancesAndProgramsWhichCompileAgainWhenNextUsed()
	{
		SwitchingRuntime runtime = new();
		runtime.Load(MoverDocument());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		Assert.Equal(VisualScriptStatus.Success, runtime.Runtime.Invoke(TestRuntime.Hero, ScriptEvent.Create, 0.0f, 0));

		runtime.Runtime.OnGameAssemblyUnloading();
		Assert.Equal(0, runtime.Runtime.InstanceCount);
		Assert.Null(runtime.Runtime.FindLatestProgram(TestRuntime.Graph));
		Assert.Null(runtime.Runtime.FindEffectiveProgram(TestRuntime.Graph));

		runtime.Runtime.OnGameAssemblyLoaded();
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		Assert.False(runtime.Runtime.FindEffectiveProgram(TestRuntime.Graph)!.IsBroken);
		Assert.Equal(VisualScriptStatus.Success, runtime.Runtime.Invoke(TestRuntime.Hero, ScriptEvent.Create, 0.0f, 0));
		Assert.Equal(["created", "created"], Probe.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void AGraphThatNeededTheGamesTypesWorksOnceTheyAreLoaded()
	{
		SwitchingRuntime runtime = new() { Library = s_WithoutGame.Value };
		runtime.Load(MoverDocument());
		Assert.Equal(VisualScriptStatus.Failure, runtime.Create());

		runtime.Library = RuntimeTesting.Library;
		runtime.Runtime.OnGameAssemblyLoaded();
		runtime.Environment.Logs.Clear();
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void AProgramThatNoLongerCompilesLogsItsErrorsOnce()
	{
		SwitchingRuntime runtime = new();
		runtime.Load(MoverDocument());

		runtime.Runtime.OnGameAssemblyUnloading();
		runtime.Library = s_WithoutGame.Value;
		Assert.Equal(VisualScriptStatus.Failure, runtime.Create());
		Assert.Equal(VisualScriptStatus.Failure, runtime.Create());
		string[] errors = [.. runtime.Environment.Errors];
		Assert.Equal(3, errors.Length);
		Assert.StartsWith($"{TestRuntime.Name} was compiled again for the changed game assembly: ", errors[0], StringComparison.Ordinal);
		Assert.StartsWith($"{TestRuntime.Name} cannot run: ", errors[1], StringComparison.Ordinal);
		Assert.Equal(errors[1], errors[2]);
		Assert.True(runtime.Runtime.FindLatestProgram(TestRuntime.Graph)!.IsBroken);
		Assert.Null(runtime.Runtime.FindEffectiveProgram(TestRuntime.Graph));
	}

	[Fact]
	public void ABrokenLoadAfterAChangeKeepsTheWorkingDocumentThatStillCompiles()
	{
		DocumentBuilder working = new();
		working.Graph.Writes(working.Graph.Node("Event.Create"), "Then", "working");
		const string broken = """{ "Graph": { "Nodes": [{ "ID": 1, "Type": "Flow.Nothing" }] } }""";
		SwitchingRuntime runtime = new();
		runtime.Load(working.Build());
		runtime.Load(broken);

		runtime.Runtime.OnGameAssemblyLoaded();
		runtime.Load(broken);
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		runtime.Runtime.Invoke(TestRuntime.Hero, ScriptEvent.Create, 0.0f, 0);
		Assert.Equal(["working"], Probe.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void NothingOfTheDroppedProgramsStaysReachable()
	{
		SwitchingRuntime runtime = new();
		(WeakReference program, WeakReference instance) = RunMover(runtime);

		runtime.Runtime.OnGameAssemblyUnloading();
		for (int attempt = 0; attempt < 10 && (program.IsAlive || instance.IsAlive); attempt++)
		{
			GC.Collect();
			GC.WaitForPendingFinalizers();
		}
		Assert.False(program.IsAlive);
		Assert.False(instance.IsAlive);
	}

	// A separate method so that no local of the test keeps the program or the instance alive.
	[MethodImpl(MethodImplOptions.NoInlining)]
	private static (WeakReference Program, WeakReference Instance) RunMover(SwitchingRuntime runtime)
	{
		runtime.Load(MoverDocument());
		runtime.Create();
		runtime.Runtime.Invoke(TestRuntime.Hero, ScriptEvent.Create, 0.0f, 0);
		GraphInstance instance = runtime.Runtime.FindInstance(TestRuntime.Hero)!;
		return (new WeakReference(instance.Program), new WeakReference(instance));
	}

	// A graph that calls a member of the game and holds an array of a game enum (spec-slice3 section 3.7.5): unloading the
	// game assembly leaves nothing of it reachable, the array included, and the members' invokers go with the programs.
	[Fact]
	public void GameMembersAndArraysOfGameTypesAreDroppedOnUnload()
	{
		SwitchingRuntime runtime = new() { Library = GameTesting.Library };
		TestGame.Probe.Reset();
		(WeakReference program, WeakReference instance, WeakReference elements) = RunGameMembers(runtime);
		Assert.Equal(["Record Fire,Earth"], TestGame.Probe.Calls);
		Assert.Equal(1, runtime.Runtime.InvokerCount);

		runtime.Runtime.OnGameAssemblyUnloading();
		Assert.Equal(0, runtime.Runtime.InstanceCount);
		Assert.Equal(0, runtime.Runtime.InvokerCount);
		Assert.Null(runtime.Runtime.FindEffectiveProgram(TestRuntime.Graph));
		for (int attempt = 0; attempt < 10 && (program.IsAlive || instance.IsAlive || elements.IsAlive); attempt++)
		{
			GC.Collect();
			GC.WaitForPendingFinalizers();
		}
		Assert.False(program.IsAlive);
		Assert.False(instance.IsAlive);
		Assert.False(elements.IsAlive);
	}

	[MethodImpl(MethodImplOptions.NoInlining)]
	private static (WeakReference Program, WeakReference Instance, WeakReference Elements) RunGameMembers(SwitchingRuntime runtime)
	{
		DocumentBuilder document = new();
		uint elements = document.Variable("Elements", "TestGame.Element[]", """["Fire", "Earth"]""");
		GraphBuilder graph = document.Graph;
		uint record = graph.Node("Call:TestGame.Probe.Record(TestGame.Element[])");
		graph.Link(graph.Get(elements), "Value", record, "Elements");
		graph.Flow(graph.Node("Event.Create"), record);
		runtime.Load(document.Build());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		Assert.Equal(VisualScriptStatus.Success, runtime.Runtime.Invoke(TestRuntime.Hero, ScriptEvent.Create, 0.0f, 0));
		GraphInstance instance = runtime.Runtime.FindInstance(TestRuntime.Hero)!;
		object array = instance.ReadVariable(instance.FindVariable("Elements"))!;
		Assert.Equal(new[] { TestGame.Element.Fire, TestGame.Element.Earth }, array);
		return (new WeakReference(instance.Program), new WeakReference(instance), new WeakReference(array));
	}
}

// The engine's node library is built once per set of assemblies, on whichever thread asks first.
[Collection(ProcessStaticsCollection.Name)]
public sealed class EngineLibraryTests
{
	[Fact]
	public void TheLibraryIsBuiltOnceUntilDropped()
	{
		EngineLibrary.Drop();
		ConcurrentBag<NodeLibrary> libraries = [];
		Parallel.For(0, 8, _ => libraries.Add(EngineLibrary.Get()));
		NodeLibrary first = libraries.First();
		Assert.All(libraries, library => Assert.Same(first, library));
		Assert.Same(first, EngineLibrary.Get());

		EngineLibrary.Drop();
		NodeLibrary second = EngineLibrary.Get();
		Assert.NotSame(first, second);
		Assert.Equal(LibraryWriter.Write(first), LibraryWriter.Write(second));
	}

	// Each build reports the notes of the game's [VisualNode] members once (spec-slice3 section 3.7.2): analyzing with the
	// library reports nothing again; the next build, after a game assembly reloaded, reports them again.
	[Fact]
	public void EachBuildLogsTheNotesOnce()
	{
		List<string> warnings = [];
		NodeLibrary library = EngineLibrary.Build(GameTesting.Sources(), warnings.Add);
		Assert.NotEmpty(library.Notes);
		Assert.Equal(library.Notes.Select(static note => note.Message), warnings);

		VisualScriptTesting.Analyze(library, VisualScriptTesting.Document("""{ "ID": 1, "Type": "Call:TestGame.Probe.Swap(int,int)" }"""));
		Assert.Equal(library.Notes.Count, warnings.Count);

		EngineLibrary.Build(GameTesting.Sources(), warnings.Add);
		Assert.Equal(2 * library.Notes.Count, warnings.Count);
	}
}
