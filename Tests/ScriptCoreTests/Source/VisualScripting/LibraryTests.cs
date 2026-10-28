using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Function libraries (spec-slice3 section 4.4): their functions run for the graphs that call them, compiled into the callers'
// programs; cycles between libraries are found from the files; latency follows the library's function; and the runtime
// analyzes each library once for every graph that calls it.
public sealed class LibraryTests
{
	[Fact]
	public void PureImpureAndLatentLibraryFunctionsRunForTheGraphThatCallsThem()
	{
		MotionLibrary library = new();
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint twice = graph.Node("Function.Call", CrossGraphTesting.Call(library.Twice, CrossGraphTesting.Motion), """{ "Value": 4 }""");
		uint report = graph.Node("Function.Call", CrossGraphTesting.Call(library.Report, CrossGraphTesting.Motion));
		uint text = graph.Node("Convert", """{ "To": "string" }""");
		graph.Link(twice, "Result", text, "Value");
		graph.Link(text, "Result", report, "Text");
		uint pause = graph.Node("Function.Call", CrossGraphTesting.Call(library.Pause, CrossGraphTesting.Motion), """{ "Seconds": 0.5 }""");
		graph.Flow(create, report);
		graph.Flow(report, pause);
		graph.Writes(pause, "Then", "after");
		TestRuntime runtime = new();
		runtime.LoadWorking(document.Build(), references: library.Bundle());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["8"], runtime.Records);
		// The caller waits in the library's Delay: a fiber of the caller's instance, resumed by its updates.
		Execution fiber = Assert.Single(runtime.Instance().Fibers);
		Assert.Equal(2, fiber.CallCount);
		Assert.Equal((CrossGraphTesting.Motion, library.Delay), (fiber.Current!.Owner.Graph, fiber.Current.Id));
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["8"], runtime.Records);
		runtime.Invoke(ScriptEvent.Update, 0.25f);
		Assert.Equal(["8", "resumed", "after"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void SelfInALibraryFunctionIsTheEntityWhoseExecutionCalledIt()
	{
		MotionLibrary library = new();
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		graph.Flow(graph.Node("Event.Create"), graph.Node("Function.Call", CrossGraphTesting.Call(library.Ping, CrossGraphTesting.Motion)));
		uint ping = graph.Node("Event.Custom", """{ "Name": "Ping" }""");
		graph.Flow(ping, graph.Write(graph.Node("Self"), "Self"));
		TestRuntime runtime = new();
		runtime.Environment.Add(8, "Sidekick");
		runtime.LoadWorking(document.Build(), references: library.Bundle());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(8));

		runtime.Invoke(ScriptEvent.Create, entity: 8);
		Assert.Equal(["Entity(8)"], runtime.Records);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["Entity(8)", "Entity(7)"], runtime.Records);
	}

	[Fact]
	public void LibrariesCallLibrariesAndRecurseWithinThemselves()
	{
		MotionLibrary motion = new();
		DocumentBuilder physics = new() { Kind = "Library" };
		// Quadruple(int Value) -> int Result: Twice(Twice(Value)), calls of another library.
		FunctionBuilder quadruple = physics.Function("Quadruple", pure: true, ["Value:int"], ["Result:int"]);
		uint inner = quadruple.Graph.Node("Function.Call", CrossGraphTesting.Call(motion.Twice, CrossGraphTesting.Motion));
		uint outer = quadruple.Graph.Node("Function.Call", CrossGraphTesting.Call(motion.Twice, CrossGraphTesting.Motion));
		uint result = quadruple.Graph.Node("Function.Return");
		quadruple.Graph.Link(quadruple.Entry, "Value", inner, "Value");
		quadruple.Graph.Link(inner, "Result", outer, "Value");
		quadruple.Graph.Link(outer, "Result", result, "Result");
		// Countdown(int From): records From, then calls itself with From - 1 while From is positive.
		FunctionBuilder countdown = physics.Function("Countdown", inputs: ["From:int"]);
		uint positive = countdown.Graph.Node("Operator.Greater", inputs: """{ "B": 0 }""");
		uint branch = countdown.Graph.Node("Flow.Branch");
		uint less = countdown.Graph.Node("Operator.Subtract", inputs: """{ "B": 1 }""");
		uint again = countdown.Graph.Node("Function.Call", countdown.Settings);
		countdown.Graph.Link(countdown.Entry, "From", positive, "A");
		countdown.Graph.Link(positive, "Result", branch, "Condition");
		countdown.Graph.Link(countdown.Entry, "From", less, "A");
		countdown.Graph.Link(less, "Result", again, "From");
		countdown.Graph.Flow(countdown.Entry, branch);
		uint write = countdown.Graph.Write(countdown.Entry, "From");
		countdown.Graph.Flow(branch, write, "True");
		countdown.Graph.Flow(write, again);
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint call = graph.Node("Function.Call", CrossGraphTesting.Call(quadruple, CrossGraphTesting.Physics), """{ "Value": 3 }""");
		uint written = graph.Write(call, "Result");
		uint count = graph.Node("Function.Call", CrossGraphTesting.Call(countdown, CrossGraphTesting.Physics), """{ "From": 3 }""");
		graph.Flow(create, written);
		graph.Flow(written, count);
		string bundle = new BundleBuilder().Full(CrossGraphTesting.Motion, CrossGraphTesting.MotionName, motion.Document)
			.Full(CrossGraphTesting.Physics, CrossGraphTesting.PhysicsName, physics).Build();
		TestRuntime runtime = new();
		runtime.LoadWorking(document.Build(), references: bundle);
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["12", "3", "2", "1"], runtime.Records);
		GraphProgram program = runtime.Instance().Program;
		Assert.NotNull(program.FindFunction(CrossGraphTesting.Motion, motion.Twice.Id));
		Assert.NotNull(program.FindFunction(CrossGraphTesting.Physics, countdown.Id));
		Assert.Equal(CrossGraphTesting.PhysicsName, program.FindFunction(CrossGraphTesting.Physics, countdown.Id)!.Body.Name);
	}

	[Fact]
	public void CyclesBetweenLibrariesAreFoundFromTheirFilesWhateverTheCacheHolds()
	{
		// Motion's Step calls Physics' Step; Physics' Step calls nothing, then Motion's.
		DocumentBuilder motion = new() { Kind = "Library" };
		FunctionBuilder motionStep = motion.Function("Step");
		uint motionCall = motionStep.Graph.Node("Function.Call", CrossGraphTesting.Call(motionStep.Id, CrossGraphTesting.Physics));
		motionStep.Graph.Flow(motionStep.Entry, motionCall);
		string Physics(bool callsBack)
		{
			DocumentBuilder physics = new() { Kind = "Library" };
			FunctionBuilder step = physics.Function("Step");
			if (callsBack)
			{
				step.Graph.Flow(step.Entry, step.Graph.Node("Function.Call", CrossGraphTesting.Call(motionStep.Id, CrossGraphTesting.Motion)));
			}
			return physics.Build();
		}
		DocumentBuilder document = new();
		uint call = document.Graph.Node("Function.Call", CrossGraphTesting.Call(motionStep.Id, CrossGraphTesting.Motion));
		document.Graph.Flow(document.Graph.Node("Event.Create"), call);
		string Bundle(bool callsBack) => new BundleBuilder().Full(CrossGraphTesting.Motion, CrossGraphTesting.MotionName, motion)
			.Full(CrossGraphTesting.Physics, CrossGraphTesting.PhysicsName, Physics(callsBack)).Build();
		LibraryAnalyses cache = new();

		Assert.False(VisualScriptTesting.Has(CrossGraphTesting.Analyze(document.Build(), Bundle(false), cache), DiagnosticCode.ReferenceError));
		Diagnostic broken = VisualScriptTesting.Single(CrossGraphTesting.Analyze(document.Build(), Bundle(true), cache), DiagnosticCode.ReferenceError);
		Assert.Equal(FormattableString.Invariant(
			$"Scripts/Motion.sgraph has 2 errors, first: Motion.sgraph calls Physics.sgraph, which calls Motion.sgraph: libraries cannot depend on themselves (node {motionCall}): fix the library"),
			broken.Message);
		Assert.False(VisualScriptTesting.Has(CrossGraphTesting.Analyze(document.Build(), Bundle(false), cache), DiagnosticCode.ReferenceError));

		// The analyzed document takes part as itself: the bundle has no entry for it.
		string bundle = new BundleBuilder(CrossGraphTesting.Motion).Full(CrossGraphTesting.Physics, CrossGraphTesting.PhysicsName, Physics(true)).Build();
		Diagnostic cycle = VisualScriptTesting.Single(CrossGraphTesting.Analyze(motion.Build(), bundle, cache), DiagnosticCode.LibraryCycle);
		Assert.Equal((motionCall, CrossGraphTesting.Physics, DiagnosticSeverity.Error), (cycle.Node!.Value, cycle.Graph, cycle.Severity));
		Assert.Equal("this library calls Physics.sgraph, which calls this library: libraries cannot depend on themselves", cycle.Message);
		Assert.Equal("Motion.sgraph calls Physics.sgraph, which calls Motion.sgraph: libraries cannot depend on themselves",
			VisualScriptTesting.Single(CrossGraphTesting.Analyze(motion.Build(), bundle, cache, CrossGraphTesting.MotionName), DiagnosticCode.LibraryCycle).Message);
		Assert.False(VisualScriptTesting.Has(CrossGraphTesting.Analyze(motion.Build(),
			new BundleBuilder(CrossGraphTesting.Motion).Full(CrossGraphTesting.Physics, CrossGraphTesting.PhysicsName, Physics(false)).Build(), cache),
			DiagnosticCode.LibraryCycle));
	}

	[Fact]
	public void ACallIsLatentWhenTheLibrarysFunctionIs()
	{
		MotionLibrary library = new();
		DocumentBuilder document = new();
		FunctionBuilder wait = document.Function("Wait");
		uint pause = wait.Graph.Node("Function.Call", CrossGraphTesting.Call(library.Pause, CrossGraphTesting.Motion));
		wait.Graph.Flow(wait.Entry, pause);
		FunctionBuilder pure = document.Function("Pure", pure: true);
		pure.Graph.Node("Function.Return");
		uint latent = pure.Graph.Node("Function.Call", CrossGraphTesting.Call(library.Pause, CrossGraphTesting.Motion));
		FunctionBuilder quick = document.Function("Quick");
		quick.Graph.Flow(quick.Entry, quick.Graph.Node("Function.Call", CrossGraphTesting.Call(library.Report, CrossGraphTesting.Motion)));

		AnalyzedDocument analysis = CrossGraphTesting.Analyze(document.Build(), library.Bundle());
		Assert.True(analysis.FindFunction(wait.Id)!.Latent);
		Assert.False(analysis.FindFunction(quick.Id)!.Latent);
		Assert.True(VisualScriptTesting.Node(analysis, pause).Latent);
		Assert.Equal(latent, VisualScriptTesting.Single(analysis, DiagnosticCode.LatentInPureContext).Node);
	}

	[Fact]
	public void TheRuntimeAnalyzesALibraryOnceUntilItOrALibraryItReadsChanges()
	{
		MotionLibrary motion = new();
		DocumentBuilder Physics(string text)
		{
			DocumentBuilder physics = new() { Kind = "Library" };
			FunctionBuilder say = physics.Function("Say");
			uint call = say.Graph.Node("Function.Call", CrossGraphTesting.Call(motion.Report, CrossGraphTesting.Motion),
				$$"""{ "Text": {{RuntimeTesting.Quote(text)}} }""");
			say.Graph.Flow(say.Entry, call);
			return physics;
		}
		DocumentBuilder document = new();
		uint call = document.Graph.Node("Function.Call", CrossGraphTesting.Call(1, CrossGraphTesting.Physics));
		document.Graph.Flow(document.Graph.Node("Event.Create"), call);
		string Bundle(DocumentBuilder physics, DocumentBuilder library) => new BundleBuilder()
			.Full(CrossGraphTesting.Motion, CrossGraphTesting.MotionName, library).Full(CrossGraphTesting.Physics, CrossGraphTesting.PhysicsName, physics).Build();
		TestRuntime runtime = new();
		LibraryAnalyses cache = runtime.Runtime.LibraryAnalyses;
		byte[] text = Encoding.UTF8.GetBytes(document.Build());
		byte[] bundle = Encoding.UTF8.GetBytes(Bundle(Physics("one"), motion.Document));

		byte[] first = runtime.Runtime.AnalyzeDocument(text, bundle).Text!;
		Assert.Equal((0, 2, 2), (cache.Hits, cache.Misses, cache.Count));
		// The same bundle again: Physics is found, and so is Motion, which it links.
		Assert.Equal(first, runtime.Runtime.AnalyzeDocument(text, bundle).Text!);
		Assert.Equal((2, 2), (cache.Hits, cache.Misses));
		Assert.Empty(runtime.Load(document.Build(), references: Encoding.UTF8.GetString(bundle)));
		Assert.Equal((4, 2), (cache.Hits, cache.Misses));

		// Physics changes: it is analyzed again, and Motion, which it calls, is not.
		runtime.Runtime.AnalyzeDocument(text, Encoding.UTF8.GetBytes(Bundle(Physics("two"), motion.Document)));
		Assert.Equal((5, 3), (cache.Hits, cache.Misses));
		// Motion changes: both are analyzed again, since Physics read the Motion it was analyzed with.
		MotionLibrary changed = new();
		changed.Report.Graph.Write("more");
		runtime.Runtime.AnalyzeDocument(text, Encoding.UTF8.GetBytes(Bundle(Physics("two"), changed.Document)));
		Assert.Equal((5, 5), (cache.Hits, cache.Misses));

		// The game assembly changed: the analyses go with the programs.
		runtime.Runtime.OnGameAssemblyLoaded();
		Assert.Equal(0, cache.Count);
	}

	[Fact]
	public void TheLibraryFixtureAnalyzesAsALibraryAndItsFunctionsRun()
	{
		using JsonDocument file = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(VisualScriptTesting.FixtureDirectory, "Library.sgraph")));
		string library = file.RootElement.GetProperty("VisualScript").GetRawText();
		const ulong handle = 5550000000000000009;
		AnalyzedDocument analysis = GraphAnalyzer.Analyze(FuzzLibrary.Library, Encoding.UTF8.GetBytes(library),
			GraphReferences.Parse(Encoding.UTF8.GetBytes(new BundleBuilder(handle).Build())), new LibraryAnalyses());
		VisualScriptTesting.AssertNoErrors(analysis);
		Assert.Equal(DocumentKind.Library, analysis.Kind);
		Assert.Equal(["Clamp01", "Report", "Pause"], analysis.Functions.Select(static function => function.Name));
		Assert.True(analysis.FindFunction(4)!.Latent);

		// A script calls Clamp01 by its input's former name X, Report and the latent Pause.
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint clamp = graph.Node("Function.Call", CrossGraphTesting.Call(1, handle), """{ "X": 7.5 }""");
		uint text = graph.Node("Convert", """{ "To": "string" }""");
		graph.Link(clamp, "Result", text, "Value");
		uint report = graph.Node("Function.Call", CrossGraphTesting.Call(2, handle));
		graph.Link(text, "Result", report, "Text");
		uint pause = graph.Node("Function.Call", CrossGraphTesting.Call(4, handle), """{ "Seconds": 0.5 }""");
		graph.Flow(create, report);
		graph.Flow(report, pause);
		string bundle = new BundleBuilder().Full(handle, "Scripts/Library.sgraph", library).Build();
		FuzzStandIns.Messages = [];
		try
		{
			FakeEnvironment environment = FuzzLibrary.CreateEnvironment();
			VisualScriptRuntime runtime = new(environment, () => FuzzLibrary.Library);
			IReadOnlyList<Diagnostic> diagnostics = runtime.Load(TestRuntime.Graph, TestRuntime.Name, Encoding.UTF8.GetBytes(document.Build()),
				Encoding.UTF8.GetBytes(bundle));
			Assert.DoesNotContain(diagnostics, static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error);
			Assert.Equal(VisualScriptStatus.Success, runtime.CreateInstance(FuzzLibrary.Hero, TestRuntime.Graph, []));
			Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(FuzzLibrary.Hero, ScriptEvent.Create, 0.0f, 0));
			Assert.Equal(["1"], FuzzStandIns.Messages);
			// Pause waits Clamp01(0.5) seconds, then reports "paused".
			Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(FuzzLibrary.Hero, ScriptEvent.Update, 0.5f, 0));
			Assert.Equal(["1", "paused"], FuzzStandIns.Messages);
			Assert.Empty(environment.Logs);
		}
		finally
		{
			FuzzStandIns.Messages = null;
		}
	}

	// A program is its document and the bundle it was compiled with (spec-slice3 section 4.4.4): after the game assembly
	// changed, the graph compiles again with the same bundle, so its calls still reach the library.
	[Fact]
	public void AGameAssemblyChangeCompilesTheGraphAgainWithTheBundleItWasLoadedWith()
	{
		MotionLibrary library = new();
		DocumentBuilder document = new();
		uint call = document.Graph.Node("Function.Call", CrossGraphTesting.Call(library.Report, CrossGraphTesting.Motion), """{ "Text": "again" }""");
		document.Graph.Flow(document.Graph.Node("Event.Create"), call);
		TestRuntime runtime = new();
		runtime.LoadWorking(document.Build(), references: library.Bundle());
		GraphProgram before = runtime.Runtime.FindEffectiveProgram(TestRuntime.Graph)!;

		runtime.Runtime.OnGameAssemblyUnloading();
		runtime.Runtime.OnGameAssemblyLoaded();
		Assert.Null(runtime.Runtime.FindEffectiveProgram(TestRuntime.Graph));
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		Assert.NotSame(before, runtime.Instance().Program);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["again"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	// Messages about a library's nodes name the library (spec-slice3 section 4.9), and node IDs, which collide across
	// documents, are told apart by it: each node warns once per instance.
	[Fact]
	public void MessagesAboutALibrarysNodesNameTheLibrary()
	{
		DocumentBuilder library = new() { Kind = "Library" };
		FunctionBuilder divide = library.Function("Divide", pure: true, ["A:int", "B:int"], ["Result:int"]);
		uint quotient = divide.Graph.Node("Operator.Divide");
		divide.Graph.Link(divide.Entry, "A", quotient, "A");
		divide.Graph.Link(divide.Entry, "B", quotient, "B");
		divide.Graph.Link(quotient, "Result", divide.Graph.Node("Function.Return"), "Result");
		DocumentBuilder document = new();
		uint call = document.Graph.Node("Function.Call", CrossGraphTesting.Call(divide, CrossGraphTesting.Motion), """{ "A": 1, "B": 0 }""");
		document.Graph.Flow(document.Graph.Node("Event.Create"), document.Graph.Write(call, "Result"));
		TestRuntime runtime = new();
		runtime.LoadWorking(document.Build(), references: new BundleBuilder().Full(CrossGraphTesting.Motion, CrossGraphTesting.MotionName, library).Build());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());

		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Create));
		string error = runtime.SingleError();
		Assert.StartsWith("Scripts/Motion.sgraph: ", error, StringComparison.Ordinal);
		Assert.EndsWith(FormattableString.Invariant($" (node {quotient}) on Hero (7): integer division by zero"), error, StringComparison.Ordinal);
	}

	[Fact]
	public void NodesOfALibraryAndOfTheCallerWithOneIdWarnApart()
	{
		const string none = """{ "Id": "0" }""";
		DocumentBuilder library = new() { Kind = "Library" };
		FunctionBuilder ping = library.Function("Ping");
		uint nobody = ping.Graph.Node("Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)", inputs: none);
		uint send = ping.Graph.Node("Event.Send", """{ "Name": "Ping" }""");
		ping.Graph.Link(nobody, "Result", send, "Target");
		ping.Graph.Flow(ping.Entry, send);
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint call = graph.Node("Function.Call", CrossGraphTesting.Call(ping, CrossGraphTesting.Motion));
		uint none2 = graph.Node("Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)", inputs: none);
		uint own = graph.Node("Event.Send", """{ "Name": "Ping" }""");
		graph.Link(none2, "Result", own, "Target");
		graph.Flow(create, call);
		graph.Flow(call, own);
		Assert.Equal(send, own);
		TestRuntime runtime = new();
		runtime.LoadWorking(document.Build(), references: new BundleBuilder().Full(CrossGraphTesting.Motion, CrossGraphTesting.MotionName, library).Build());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());

		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(
		[
			FormattableString.Invariant($"Scripts/Motion.sgraph: Send Ping (node {send}) on Hero (7): Target is None, so no graph receives Ping"),
			TestRuntime.On("Send Ping", own) + "Target is None, so no graph receives Ping",
		], runtime.Environment.Warnings);
	}
}
