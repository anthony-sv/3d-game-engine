using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The managed analysis measured by phase (spec-slice4 section 9.0) on the grid of the performance guards: GridDocuments.Make
// is pinned to the engine tests' generator through Benchmarks/Grid-4x5.json, and the phases of an analysis and a load of the
// 2,000-node grid are printed on request (STRADA_PERFORMANCE_TESTS=1), medians of 7.
[Collection(ProcessStaticsCollection.Name)]
public sealed class AnalysisPerformanceTests
{
	private const int Runs = 7;

	// Runs before the measured ones, enough for tiered compilation to optimize the analyzer's hot methods.
	private const int WarmUps = 20;

	[Fact]
	public void TheGridIsTheDocumentOfItsFixture()
	{
		byte[] fixture = File.ReadAllBytes(Path.Combine(VisualScriptTesting.FixtureDirectory, "Benchmarks", "Grid-4x5.json"));
		byte[] generated = GridDocuments.MakeUtf8(4, 5, 2, 1);
		List<Diagnostic> fixtureDiagnostics = [];
		List<Diagnostic> generatedDiagnostics = [];
		GraphDocument? expected = DocumentReader.Read(fixture, fixtureDiagnostics);
		GraphDocument? actual = DocumentReader.Read(generated, generatedDiagnostics);
		Assert.NotNull(expected);
		Assert.NotNull(actual);
		Assert.Empty(fixtureDiagnostics);
		Assert.Empty(generatedDiagnostics);
		Assert.Equal(20, expected.Graph.Nodes.Count);
		Assert.Equal(40, expected.Graph.Links.Count);
		Assert.Equal(Describe(expected), Describe(actual));
	}

	[Fact]
	public void TheGridsAnalysisAndLoadAreTimedPhaseByPhase()
	{
		byte[] grid = GridDocuments.MakeUtf8(40, 50, 2, 20);
		VisualScriptRuntime runtime = new(new FakeEnvironment(), static () => VisualScriptTesting.Library);
		AnalysisTimings timings = new();
		runtime.Timings = timings;
		Assert.Equal(VisualScriptStatus.Success, runtime.AnalyzeDocument(grid).Status);
		Assert.Equal(1, timings.Analyses);
		for (AnalysisPhase phase = AnalysisPhase.Read; phase <= AnalysisPhase.Write; phase++)
		{
			Assert.True(timings[phase] > 0, $"{phase} was not timed");
		}
		Assert.Equal(0, timings.Compile);
		Assert.Equal(0, timings.Swap);

		timings.Reset();
		runtime.Load(1, "Grid", grid);
		Assert.Equal(1, timings.Analyses);
		Assert.True(timings.Analysis > 0 && timings.Compile > 0);
		Assert.Equal(0, timings.Write);
		// The grid links exec outputs into data inputs: a broken program, which swaps no instance.
		Assert.True(runtime.FindLatestProgram(1)!.IsBroken);
		Assert.Equal(0, timings.Swap);

		timings.Reset();
		runtime.Timings = null;
		runtime.AnalyzeDocument(grid);
		Assert.Equal(0, timings.Analyses);

		if (!PerformanceTesting.IsRequested)
		{
			return;
		}
		runtime.Timings = timings;
		List<double[]> analyses = Measure(runtime, timings, () => runtime.AnalyzeDocument(grid));
		List<double[]> loads = Measure(runtime, timings, () => runtime.Load(1, "Grid", grid));
		StringBuilder table = new();
		table.AppendLine(FormattableString.Invariant($"The 2,000-node grid (40 x 50, 2 links per node, 20 comments), medians of {Runs} ({(PerformanceTesting.IsOptimized ? "optimized" : "unoptimized")}):"));
		table.AppendLine("| Phase | VisualScriptAnalyze (ms) | VisualScriptLoad (ms) |");
		table.AppendLine("|---|---|---|");
		foreach (AnalysisPhase phase in Enum.GetValues<AnalysisPhase>())
		{
			table.AppendLine(FormattableString.Invariant($"| {phase} | {Median(analyses, (int)phase):F3} | {Median(loads, (int)phase):F3} |"));
		}
		int analysis = Enum.GetValues<AnalysisPhase>().Length;
		table.AppendLine(FormattableString.Invariant($"| Analysis (Read to Rules) | {Median(analyses, analysis):F3} | {Median(loads, analysis):F3} |"));
		table.AppendLine(FormattableString.Invariant($"| Allocated (MB) | {Median(analyses, analysis + 1):F2} | {Median(loads, analysis + 1):F2} |"));
		table.AppendLine(FormattableString.Invariant($"| Collections (generation 0) | {Median(analyses, analysis + 2):F0} | {Median(loads, analysis + 2):F0} |"));
		TestContext.Current.TestOutputHelper?.WriteLine(table.ToString());
	}

	// Each phase of every run, then the analysis (Read to Rules), the megabytes the run allocated and the collections it
	// caused, after warm-up runs. A collection's time counts in the phase it interrupts.
	private static List<double[]> Measure(VisualScriptRuntime runtime, AnalysisTimings timings, Action run)
	{
		for (int warmUp = 0; warmUp < WarmUps; warmUp++)
		{
			run();
		}
		List<double[]> runs = [];
		AnalysisPhase[] phases = Enum.GetValues<AnalysisPhase>();
		for (int index = 0; index < Runs; index++)
		{
			// Every run starts after a full collection: what the run allocates itself decides where its collections land.
			GC.Collect();
			GC.WaitForPendingFinalizers();
			timings.Reset();
			long allocated = GC.GetAllocatedBytesForCurrentThread();
			int collections = GC.CollectionCount(0);
			run();
			double[] figures = new double[phases.Length + 3];
			foreach (AnalysisPhase phase in phases)
			{
				figures[(int)phase] = timings[phase];
			}
			figures[phases.Length] = timings.Analysis;
			figures[phases.Length + 1] = (GC.GetAllocatedBytesForCurrentThread() - allocated) / (1024.0 * 1024.0);
			figures[phases.Length + 2] = GC.CollectionCount(0) - collections;
			runs.Add(figures);
		}
		Assert.NotNull(runtime.Timings);
		return runs;
	}

	private static double Median(List<double[]> runs, int index) => PerformanceTesting.Median([.. runs.Select(run => run[index])]);

	// What the VM reads of a document, member by member.
	private static string Describe(GraphDocument document)
	{
		StringBuilder text = new();
		text.Append(document.Kind).Append('\n');
		foreach (DocumentVariable variable in document.Variables)
		{
			text.Append(FormattableString.Invariant($"variable {variable.ID} {variable.Name} {variable.Type} {variable.Value?.GetRawText()} {variable.Public}\n"));
		}
		foreach (DocumentFunction function in document.Functions)
		{
			text.Append(FormattableString.Invariant($"function {function.ID} {function.Name}\n"));
			Describe(text, function.Graph);
		}
		Describe(text, document.Graph);
		return text.ToString();
	}

	private static void Describe(StringBuilder text, DocumentGraph graph)
	{
		foreach (DocumentNode node in graph.Nodes)
		{
			text.Append(FormattableString.Invariant($"node {node.Index} {node.ID} {node.Type} disabled {node.Disabled}"));
			foreach (KeyValuePair<string, JsonElement> setting in node.Settings.Members)
			{
				text.Append(" setting ").Append(setting.Key).Append('=').Append(setting.Value.GetRawText());
			}
			foreach (KeyValuePair<string, JsonElement> input in node.Inputs.Members)
			{
				text.Append(" input ").Append(input.Key).Append('=').Append(input.Value.GetRawText());
			}
			text.Append(" split ").AppendJoin(',', node.Split.Inputs).Append('/').AppendJoin(',', node.Split.Outputs).Append('\n');
		}
		foreach (DocumentLink link in graph.Links)
		{
			text.Append(FormattableString.Invariant($"link {link.Index} {link.From.Node}.{link.From.Pin} -> {link.To.Node}.{link.To.Pin}\n"));
		}
	}
}
