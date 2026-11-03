using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Text.Json.Nodes;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// A library edited while its dependents run (spec-slice4 section 9.4): each dependent is loaded again with its unchanged
// document, and keeps its own analysis exactly while the library's interface is unchanged. The oracle is a fresh runtime:
// diagnostics byte for byte, the compiled programs' dumps, and what an instance of each program writes.
public sealed class RootAnalysisTests
{
	internal const int Dependents = 20;

	private const int TimedRuns = 7;

	[Fact]
	public void ABodyEditReusesEveryDependentsAnalysis()
	{
		RootAnalysisWorld world = new(new MotionVariant());
		world.LoadAll();
		Assert.Equal(Dependents, world.Runtime.RootAnalyses.Count);
		Assert.Equal(0, world.Runtime.RootAnalyses.Hits);
		Assert.Equal(Dependents, world.Runtime.RootAnalyses.Misses);
		world.AssertMatchesAFreshRuntime();

		// Twice multiplies by 3 now, Quad calls it once more and Step waits: bodies only.
		world.Library = new MotionVariant { Factor = 3, ExtraAdd = true, QuadCallsTwiceThrice = true, StepWaits = false };
		world.LoadAll();
		Assert.Equal(Dependents, world.Runtime.RootAnalyses.Hits);
		Assert.Equal(Dependents, world.Runtime.RootAnalyses.Misses);
		world.AssertMatchesAFreshRuntime();
		// The reused analyses call the library's new functions: Quad(3) is 3 * 3 * 3 * 3 now.
		Assert.Equal(81 + RootAnalysisWorld.Additions, world.Run(5));
	}

	[Fact]
	public void AChangedInterfaceAnalyzesEveryDependentAgain()
	{
		RootAnalysisWorld world = new(new MotionVariant());
		world.LoadAll();
		(string Change, MotionVariant Library)[] changes =
		[
			("an added input", new MotionVariant { TwiceBias = true }),
			("a renamed input", new MotionVariant { RenameTwiceInput = true }),
			("another output type", new MotionVariant { TwiceOutputType = "long" }),
			("a latent Step", new MotionVariant { StepWaits = true }),
			("a renamed function", new MotionVariant { QuadName = "Fourfold" }),
			("a renamed input the calls still reach", new MotionVariant { QuadInput = "Amount:int:Value" }),
			("a broken library", new MotionVariant { Broken = true }),
		];
		foreach ((string change, MotionVariant library) in changes)
		{
			int hits = world.Runtime.RootAnalyses.Hits;
			world.Library = library;
			world.LoadAll();
			Assert.True(world.Runtime.RootAnalyses.Hits == hits, $"{change}: {world.Runtime.RootAnalyses.Hits - hits} dependents kept their analyses");
			world.AssertMatchesAFreshRuntime();
			// Back to the first library: its interface is the one the dependents read before the change, not now.
			world.Library = new MotionVariant();
			world.LoadAll();
			Assert.True(world.Runtime.RootAnalyses.Hits == hits, $"after {change}: the dependents kept analyses of another interface");
			world.AssertMatchesAFreshRuntime();
		}
	}

	[Fact]
	public void ADroppedFormerNameIsAChangedInterface()
	{
		RootAnalysisWorld world = new(new MotionVariant { QuadInput = "Amount:int:Value" });
		world.LoadAll();
		Assert.Equal(3 * 2 * 2 + RootAnalysisWorld.Additions, world.Run(0));
		world.Library = new MotionVariant { QuadInput = "Amount:int" };
		world.LoadAll();
		Assert.Equal(0, world.Runtime.RootAnalyses.Hits);
		world.AssertMatchesAFreshRuntime();
		// The calls' Value no longer reaches Amount, which takes its default.
		Assert.Equal(RootAnalysisWorld.Additions, world.Run(0));
	}

	[Fact]
	public void CallsCompileByGraphAndFunctionId()
	{
		RootAnalysisWorld world = new(new MotionVariant());
		world.LoadAll();
		AnalyzedFunction before = world.LibraryAnalysis.Functions.Single(static function => function.Name == "Twice");
		world.Library = new MotionVariant { Factor = 5 };
		world.LoadAll();
		Assert.Equal(Dependents, world.Runtime.RootAnalyses.Hits);
		AnalyzedFunction after = world.LibraryAnalysis.Functions.Single(static function => function.Name == "Twice");
		Assert.NotSame(before, after);
		Assert.Equal(before.ID, after.ID);
		// The dependent's call nodes still hold the analysis it was made with; its program calls the library's new functions.
		GraphProgram program = world.Runtime.FindLatestProgram(RootAnalysisWorld.HandleOf(0))!;
		CompiledFunction quad = program.FindFunction(CrossGraphTesting.Motion, world.Library.QuadId)!;
		Assert.All(quad.Body.Nodes.Where(static node => node.Op == Opcode.PureCall),
			node => Assert.Same(program.FindFunction(CrossGraphTesting.Motion, after.ID), node.Function));
		Assert.Equal((3 * 5 * 5) + RootAnalysisWorld.Additions, world.Run(0));
	}

	[Fact]
	public void OtherGraphsTheDependentsReadAreComparedWhole()
	{
		// The dependents send to Door: a change of Door's document is a change of what they read.
		RootAnalysisWorld world = new(new MotionVariant(), sendsToDoor: true);
		world.LoadAll();
		world.Door = CrossGraphTesting.DoorScript(enabled: "Disabled").Build();
		world.LoadAll();
		Assert.Equal(0, world.Runtime.RootAnalyses.Hits);
		world.AssertMatchesAFreshRuntime();
		world.LoadAll();
		Assert.Equal(Dependents, world.Runtime.RootAnalyses.Hits);
	}

	[Fact]
	public void ADependentsOwnEditUnloadAndAGameAssemblyChangeDropItsAnalysis()
	{
		RootAnalysisWorld world = new(new MotionVariant());
		world.LoadAll();
		world.Load(0, RootAnalysisWorld.Dependent(world.Library, 0, extraAddition: true));
		Assert.Equal(0, world.Runtime.RootAnalyses.Hits);
		Assert.Equal(VisualScriptStatus.Success, world.Runtime.Unload(RootAnalysisWorld.HandleOf(1)));
		Assert.Equal(Dependents - 1, world.Runtime.RootAnalyses.Count);
		world.Runtime.OnGameAssemblyUnloading();
		Assert.Equal(0, world.Runtime.RootAnalyses.Count);
		world.LoadAll();
		Assert.Equal(0, world.Runtime.RootAnalyses.Hits);
		world.AssertMatchesAFreshRuntime();
	}

	[Fact]
	public void ADocumentThatLinksNoLibraryIsNotKept()
	{
		TestRuntime runtime = TestRuntime.Start(VisualScriptTesting.Document("""{ "ID": 1, "Type": "Event.Update" }"""));
		Assert.Equal(0, runtime.Runtime.RootAnalyses.Count);
		Assert.Equal(1, runtime.Runtime.RootAnalyses.Misses);
	}

	// The 20 dependents of 200 nodes loaded again after a library body edit (spec-slice4 section 21, the V2 row), with
	// their analyses reused and without: printed on request.
	[Fact]
	public void TwentyDependentsOf200NodesAfterABodyEditAreTimedOnRequest()
	{
		if (!PerformanceTesting.IsRequested)
		{
			return;
		}
		double[] reused = new double[TimedRuns];
		double[] analyzed = new double[TimedRuns];
		RootAnalysisWorld world = new(new MotionVariant(), additions: 197);
		world.LoadAll();
		for (int run = -3; run < TimedRuns; run++)
		{
			world.Library = new MotionVariant { Factor = run % 2 == 0 ? 3 : 2 };
			long start = Stopwatch.GetTimestamp();
			world.LoadAll();
			double reusedTime = Stopwatch.GetElapsedTime(start).TotalMilliseconds;
			RootAnalysisWorld fresh = new(world.Library, additions: 197);
			start = Stopwatch.GetTimestamp();
			fresh.LoadAll();
			double analyzedTime = Stopwatch.GetElapsedTime(start).TotalMilliseconds;
			if (run >= 0)
			{
				reused[run] = reusedTime;
				analyzed[run] = analyzedTime;
			}
		}
		Assert.Equal(Dependents * (TimedRuns + 3), world.Runtime.RootAnalyses.Hits);
		TestContext.Current.TestOutputHelper?.WriteLine(FormattableString.Invariant(
			$"20 dependents of 200 nodes loaded after a library body edit, medians of {TimedRuns}{(PerformanceTesting.IsOptimized ? string.Empty : ", unoptimized")}: {PerformanceTesting.Median(reused):F2} ms with their analyses reused, {PerformanceTesting.Median(analyzed):F2} ms analyzed again"));
	}
}

// The library edits of the fuzz test: bodies, signatures, latency and breakage, with damage now and then.
public sealed class RootAnalysisFuzzTests
{
	[Fact]
	public void ReusedAnalysesEqualAFreshRuntimesOverRandomLibraryEdits()
	{
		DocumentMutator mutator = new(11300);
		RootAnalysisWorld world = new(new MotionVariant(), additions: 3);
		world.LoadAll();
		int rounds = Fuzzing.GetRounds(100);
		int reusing = 0;
		int analyzing = 0;
		MotionVariant library = new();
		for (int round = 0; round < rounds; round++)
		{
			// One or two changes of the last variant a round: bodies, signatures, latency or breakage.
			int changes = mutator.Pick(3) == 0 ? 2 : 1;
			for (int change = 0; change < changes; change++)
			{
				library = Change(library, mutator);
			}
			string text = library.Build();
			switch (mutator.Pick(8))
			{
				case 0:
					text = Encoding.UTF8.GetString(mutator.Write(mutator.Rewire(JsonNode.Parse(text), round)));
					break;
				case 1:
					text = Encoding.UTF8.GetString(mutator.Write(mutator.Damage(JsonNode.Parse(text), round)));
					break;
			}
			int hits = world.Runtime.RootAnalyses.Hits;
			world.LibraryText = text;
			world.LoadAll();
			int reused = world.Runtime.RootAnalyses.Hits - hits;
			Assert.True(reused is 0 or RootAnalysisTests.Dependents, $"round {round}: {reused} of the dependents kept their analyses");
			if (reused > 0)
			{
				reusing++;
			}
			else
			{
				analyzing++;
			}
			world.AssertMatchesAFreshRuntime($"round {round}");
		}
		Assert.True(reusing > 0 && analyzing > 0, $"{reusing} rounds reused the analyses and {analyzing} analyzed them again");
	}

	private static MotionVariant Change(MotionVariant library, DocumentMutator mutator) => mutator.Pick(10) switch
	{
		0 => library with { Factor = 2 + mutator.Pick(3) },
		1 => library with { ExtraAdd = !library.ExtraAdd },
		2 => library with { QuadCallsTwiceThrice = !library.QuadCallsTwiceThrice },
		3 => library with { StepWaits = !library.StepWaits },
		4 => library with { TwiceBias = !library.TwiceBias },
		5 => library with { RenameTwiceInput = !library.RenameTwiceInput },
		6 => library with { TwiceOutputType = library.TwiceOutputType == "int" ? "long" : "int" },
		7 => library with { QuadName = library.QuadName == "Quad" ? "Fourfold" : "Quad" },
		// Renamed with the former name, then the former name dropped, then back.
		8 => library with
		{
			QuadInput = library.QuadInput switch
			{
				"Value:int" => "Amount:int:Value",
				"Amount:int:Value" => "Amount:int",
				_ => "Value:int",
			},
		},
		_ => library with { Broken = !library.Broken },
	};
}

// The function library Scripts/Motion.sgraph as the root-analysis tests edit it: the pure Twice(int Value) -> int Result
// (Value was X before), which multiplies by Factor; the pure Quad(int Value) -> int Result, which calls Twice of its own
// library twice (thrice when asked); and the impure Step(int Count), which waits 0.5 s when asked. The functions' IDs are the
// same in every variant: the functions are declared before any body node takes an ID.
internal sealed record MotionVariant
{
	internal int Factor { get; init; } = 2;

	// Twice adds 0 after multiplying (another body).
	internal bool ExtraAdd { get; init; }

	internal bool QuadCallsTwiceThrice { get; init; }

	internal bool StepWaits { get; init; }

	// Signature changes: Twice takes a second input, or its input is Amount (Value before), or its output is of another type.
	internal bool TwiceBias { get; init; }

	internal bool RenameTwiceInput { get; init; }

	internal string TwiceOutputType { get; init; } = "int";

	internal string QuadName { get; init; } = "Quad";

	// Quad's input as DocumentBuilder declares it: the dependents' calls store their literal under "Value", which an input
	// renamed with the former name Value still takes.
	internal string QuadInput { get; init; } = "Value:int";

	// Step holds a node the library does not know: the library has an error.
	internal bool Broken { get; init; }

	internal uint TwiceId => 1;

	internal uint QuadId => 3;

	internal uint StepId => 5;

	internal string Build()
	{
		DocumentBuilder document = new() { Kind = "Library" };
		string input = RenameTwiceInput ? "Amount:int:Value,X" : "Value:int:X";
		FunctionBuilder twice = document.Function("Twice", pure: true, TwiceBias ? [input, "Bias:int"] : [input], [$"Result:{TwiceOutputType}"]);
		FunctionBuilder quad = document.Function(QuadName, pure: true, [QuadInput], ["Result:int"]);
		FunctionBuilder step = document.Function("Step", inputs: ["Count:int"]);
		if (twice.Id != TwiceId || quad.Id != QuadId || step.Id != StepId)
		{
			throw new InvalidOperationException("the library's functions took other IDs");
		}

		uint times = twice.Graph.Node("Operator.Multiply", inputs: FormattableString.Invariant($$"""{ "B": {{Factor}} }"""));
		twice.Graph.Link(twice.Entry, RenameTwiceInput ? "Amount" : "Value", times, "A");
		uint result = times;
		if (ExtraAdd)
		{
			result = twice.Graph.Node("Operator.Add", inputs: """{ "B": 0 }""");
			twice.Graph.Link(times, "Result", result, "A");
		}
		uint twiceReturn = twice.Graph.Node("Function.Return");
		twice.Graph.Link(result, "Result", twiceReturn, "Result");

		uint previous = quad.Entry;
		string output = QuadInput.Split(':')[0];
		for (int call = 0; call < (QuadCallsTwiceThrice ? 3 : 2); call++)
		{
			uint node = quad.Graph.Node("Function.Call", twice.Settings);
			quad.Graph.Link(previous, output, node, RenameTwiceInput ? "Amount" : "Value");
			previous = node;
			output = "Result";
		}
		uint quadReturn = quad.Graph.Node("Function.Return");
		quad.Graph.Link(previous, output, quadReturn, "Result");

		if (StepWaits)
		{
			uint delay = step.Graph.Node("Flow.Delay", inputs: """{ "Duration": 0.5 }""");
			step.Graph.Flow(step.Entry, delay);
		}
		if (Broken)
		{
			step.Graph.Node("Sample.NoSuchNode");
		}
		return document.Build();
	}
}

// Twenty dependents of the library (handles 900 + i), each with the public int Result: Update calls Step(i), then sets
// Result to Quad(3) plus 1 for each of its additions; optionally each also sends Hit to Door. Each has a pure function with
// a disabled call of Step, whose Warning depends on Step's latency. A runtime keeps them, and a
// fresh runtime loads the same documents and bundles as the oracle.
internal sealed class RootAnalysisWorld
{
	internal const int Additions = 10;
	private const ulong FirstHandle = 900;
	private const ulong FirstEntity = 5000;

	private readonly int m_Additions;
	private readonly bool m_SendsToDoor;
	private readonly string[] m_Dependents;
	private List<IReadOnlyList<Diagnostic>> m_Diagnostics = [];
	private MotionVariant m_Library;
	private string m_LibraryText;

	internal RootAnalysisWorld(MotionVariant library, int additions = Additions, bool sendsToDoor = false)
	{
		m_Library = library;
		m_LibraryText = library.Build();
		m_Additions = additions;
		m_SendsToDoor = sendsToDoor;
		m_Dependents = new string[RootAnalysisTests.Dependents];
		for (int index = 0; index < m_Dependents.Length; index++)
		{
			m_Dependents[index] = Dependent(library, index, additions: additions, sendsToDoor: sendsToDoor);
		}
		Runtime = MakeRuntime(Environment);
	}

	internal FakeEnvironment Environment { get; } = new();

	internal VisualScriptRuntime Runtime { get; }

	// The library as built (its function IDs are what the dependents call).
	internal MotionVariant Library
	{
		get => m_Library;
		set
		{
			m_Library = value;
			m_LibraryText = value.Build();
		}
	}

	// The library's text as sent (a variant, or a damaged one).
	internal string LibraryText
	{
		get => m_LibraryText;
		set => m_LibraryText = value;
	}

	internal string Door { get; set; } = CrossGraphTesting.DoorScript().Build();

	// The library's analysis the runtime keeps.
	internal AnalyzedDocument LibraryAnalysis => Runtime.LibraryAnalyses.Find(CrossGraphTesting.Motion)!.Analysis;

	internal static ulong HandleOf(int dependent) => FirstHandle + (ulong)dependent;

	internal static string NameOf(int dependent) => FormattableString.Invariant($"Scripts/Dependent{dependent}.sgraph");

	internal static string Dependent(MotionVariant library, int index, int additions = Additions, bool sendsToDoor = false, bool extraAddition = false)
	{
		DocumentBuilder document = new();
		uint result = document.Variable("Result", "int", "0", isPublic: true);
		// A pure function holding a disabled call of Step: its finding (a Warning) says whether Step waits.
		FunctionBuilder check = document.Function("Check", pure: true, outputs: ["Result:int"]);
		check.Graph.Node("Function.Return");
		check.Graph.Node("Function.Call", CrossGraphTesting.Call(library.StepId, CrossGraphTesting.Motion), enabled: "Disabled");
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		uint step = graph.Node("Function.Call", CrossGraphTesting.Call(library.StepId, CrossGraphTesting.Motion),
			FormattableString.Invariant($$"""{ "Count": {{index}} }"""));
		graph.Flow(update, step);
		uint set = graph.Set(result);
		uint last = step;
		if (sendsToDoor)
		{
			uint send = graph.Node("Event.Send", CrossGraphTesting.Send("Hit", CrossGraphTesting.Door, """{ "Name": "Damage", "Type": "float" }"""));
			graph.Flow(step, send);
			last = send;
		}
		graph.Flow(last, set);
		uint previous = graph.Node("Function.Call", CrossGraphTesting.Call(library.QuadId, CrossGraphTesting.Motion), """{ "Value": 3 }""");
		for (int addition = 0; addition < additions + (extraAddition ? 1 : 0); addition++)
		{
			uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
			graph.Link(previous, "Result", add, "A");
			previous = add;
		}
		graph.Link(previous, "Result", set, "Value");
		return document.Build();
	}

	internal string BundleOf(int dependent)
	{
		BundleBuilder bundle = new BundleBuilder(HandleOf(dependent)).Full(CrossGraphTesting.Motion, CrossGraphTesting.MotionName, m_LibraryText);
		if (m_SendsToDoor)
		{
			bundle.Full(CrossGraphTesting.Door, CrossGraphTesting.DoorName, Door);
		}
		return bundle.Build();
	}

	// Loads the dependent again with that document (its own edit).
	internal void Load(int dependent, string document)
	{
		m_Dependents[dependent] = document;
		m_Diagnostics[dependent] = Load(Runtime, dependent, document);
	}

	// Loads every dependent again with the library as it is now.
	internal void LoadAll()
	{
		m_Diagnostics = LoadAll(Runtime);
	}

	// The Result of a new instance of the dependent after one Update.
	internal object? Run(int dependent) => Run(Runtime, Environment, dependent);

	// The oracle: a fresh runtime loading the same documents gives the diagnostics the last loads gave, the same programs and
	// the same runs (with the same messages logged).
	internal void AssertMatchesAFreshRuntime(string context = "")
	{
		FakeEnvironment environment = new();
		VisualScriptRuntime fresh = MakeRuntime(environment);
		List<IReadOnlyList<Diagnostic>> expected = LoadAll(fresh);
		int freshLogs = environment.Logs.Count;
		int keptLogs = Environment.Logs.Count;
		for (int index = 0; index < m_Dependents.Length; index++)
		{
			Assert.True(Encoding.UTF8.GetString(AnalysisWriter.WriteDiagnostics(expected[index])) ==
				Encoding.UTF8.GetString(AnalysisWriter.WriteDiagnostics(m_Diagnostics[index])), $"{context}: dependent {index}'s diagnostics");
			GraphProgram expectedProgram = fresh.FindLatestProgram(HandleOf(index))!;
			GraphProgram actualProgram = Runtime.FindLatestProgram(HandleOf(index))!;
			string expectedDump = ProgramDump.Of(expectedProgram);
			string actualDump = ProgramDump.Of(actualProgram);
			if (expectedDump != actualDump)
			{
				Assert.Fail($"{context}: dependent {index}'s program:\n{FirstDifference(expectedDump, actualDump)}");
			}
			if (!expectedProgram.IsBroken)
			{
				Assert.True(Equals(Run(fresh, environment, index), Run(Runtime, Environment, index)), $"{context}: dependent {index}'s run");
			}
		}
		Assert.Equal(environment.Logs.Skip(freshLogs), Environment.Logs.Skip(keptLogs));
	}

	private static VisualScriptRuntime MakeRuntime(FakeEnvironment environment)
	{
		for (int index = 0; index < RootAnalysisTests.Dependents; index++)
		{
			environment.Add(FirstEntity + (ulong)index, FormattableString.Invariant($"Dependent {index}"));
		}
		return new VisualScriptRuntime(environment, static () => RuntimeTesting.Library);
	}

	private IReadOnlyList<Diagnostic> Load(VisualScriptRuntime runtime, int dependent, string document) =>
		runtime.Load(HandleOf(dependent), NameOf(dependent), Encoding.UTF8.GetBytes(document), Encoding.UTF8.GetBytes(BundleOf(dependent)));

	private List<IReadOnlyList<Diagnostic>> LoadAll(VisualScriptRuntime runtime)
	{
		List<IReadOnlyList<Diagnostic>> diagnostics = [];
		for (int index = 0; index < m_Dependents.Length; index++)
		{
			diagnostics.Add(Load(runtime, index, m_Dependents[index]));
		}
		return diagnostics;
	}

	private static object? Run(VisualScriptRuntime runtime, FakeEnvironment environment, int dependent)
	{
		ulong entity = FirstEntity + (ulong)dependent;
		Assert.Equal(VisualScriptStatus.Success, runtime.CreateInstance(entity, HandleOf(dependent), []));
		runtime.Invoke(entity, ScriptEvent.Update, 1.0f, 0);
		runtime.Invoke(entity, ScriptEvent.Update, 1.0f, 0);
		GraphInstance instance = runtime.FindInstance(entity)!;
		object? result = instance.ReadVariable(instance.FindVariable("Result"));
		Assert.Equal(VisualScriptStatus.Success, runtime.DestroyInstance(entity));
		return result;
	}

	private static string FirstDifference(string expected, string actual)
	{
		string[] expectedLines = expected.Split('\n');
		string[] actualLines = actual.Split('\n');
		for (int line = 0; line < Math.Max(expectedLines.Length, actualLines.Length); line++)
		{
			string left = line < expectedLines.Length ? expectedLines[line] : "(none)";
			string right = line < actualLines.Length ? actualLines[line] : "(none)";
			if (left != right)
			{
				return string.Create(CultureInfo.InvariantCulture, $"line {line}:\nexpected {left}\nactual   {right}");
			}
		}
		return "equal";
	}
}
