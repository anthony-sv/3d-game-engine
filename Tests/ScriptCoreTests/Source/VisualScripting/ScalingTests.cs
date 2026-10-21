using System;
using System.Diagnostics;
using System.Linq;
using System.Text;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Work that grows with a graph costs time linear in it: an Update's pass over many fibers (spec section 5.2, step 4) and
// the analysis of a document with many links into one input (spec section 6.2). Each bound is several times what the case
// takes in an unoptimized build on a busy machine, and below what time quadratic in its size took there. The tests run
// alone, so other tests do not slow the times they measure.
[Collection(ProcessStaticsCollection.Name)]
public sealed class ScalingTests
{
	private static TimeSpan Time(Action action)
	{
		long start = Stopwatch.GetTimestamp();
		action();
		return Stopwatch.GetElapsedTime(start);
	}

	// Fibers waiting in WaitUntil, one per call of a function (a node inside a function is pending per call): a condition that
	// stays false keeps its fiber in place, and a condition that holds resumes it, each in constant time. Before, both moved
	// the fiber in a list: 40000 fibers took seconds per Update.
	[Fact]
	public void AnUpdateOfManyFibersTakesTimeLinearInTheirNumber()
	{
		const int fibers = 40000;
		DocumentBuilder document = new();
		uint go = document.Variable("Go", "bool", "false");
		FunctionBuilder wait = document.Function("Wait");
		uint until = wait.Graph.Node("Flow.WaitUntil");
		wait.Graph.Link(wait.Graph.Get(go), "Value", until, "Condition");
		wait.Graph.Flow(wait.Entry, until);
		GraphBuilder graph = document.Graph;
		uint loop = graph.Node("Flow.ForLoop", inputs: FormattableString.Invariant($$"""{ "First": 1, "Last": {{fibers}} }"""));
		graph.Flow(graph.Node("Event.Create"), loop);
		graph.Flow(loop, graph.Node("Event.Send", """{ "Name": "Wait" }"""), "Body");
		graph.Flow(graph.Node("Event.Custom", """{ "Name": "Wait" }"""), graph.Node("Function.Call", wait.Settings));
		TestRuntime runtime = TestRuntime.Start(document);
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(fibers, runtime.Instance().FiberCount);

		TimeSpan fewest = TimeSpan.MaxValue;
		for (int update = 0; update < 3; update++)
		{
			TimeSpan elapsed = Time(() => Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update, 0.016f)));
			fewest = fewest < elapsed ? fewest : elapsed;
		}
		// About 15 ms unoptimized; moving every fiber in a list took 11 s.
		TimeSpan bound = TimeSpan.FromSeconds(1.0);
		Assert.True(fewest < bound, $"an Update of {fibers} waiting fibers took {fewest.TotalMilliseconds:F0} ms");
		Assert.Equal(fibers, runtime.Instance().FiberCount);

		runtime.Runtime.SetVariable(TestRuntime.Hero, "Go", typeof(bool), true);
		TimeSpan resumed = Time(() => Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update, 0.016f)));
		Assert.True(resumed < bound, $"an Update resuming {fibers} fibers took {resumed.TotalMilliseconds:F0} ms");
		Assert.Equal(0, runtime.Instance().FiberCount);
		Assert.Empty(runtime.Environment.Logs);
	}

	// Damaged documents (a graph holds at most 10000 nodes and 40000 links, spec section 1.3). Reroutes with unconnected Ins
	// all linked into the head of one chain of Reroutes resolve through the link the head takes its value from: the chain is
	// walked once, not once for each of them. Links into one input are checked against what came before in constant time
	// each. Before, either document took seconds to analyze.
	[Fact]
	public void ManyLinksIntoOneInputAnalyzeInTimeLinearInTheirNumber()
	{
		const int sources = 5000;
		const int chain = 4990;
		StringBuilder nodes = new();
		StringBuilder links = new();
		for (int index = 1; index <= chain; index++)
		{
			nodes.Append(FormattableString.Invariant($$"""{ "ID": {{index}}, "Type": "Reroute" }, """));
			if (index > 1)
			{
				links.Append(FormattableString.Invariant($$"""{ "From": [{{index - 1}}, "Out"], "To": [{{index}}, "In"] }, """));
			}
		}
		const int branch = chain + 1;
		nodes.Append(FormattableString.Invariant($$"""{ "ID": {{branch}}, "Type": "Flow.Branch" }"""));
		links.Append(FormattableString.Invariant($$"""{ "From": [{{chain}}, "Out"], "To": [{{branch}}, "Condition"] }"""));
		for (int index = branch + 1; index <= branch + sources; index++)
		{
			nodes.Append(FormattableString.Invariant($$""", { "ID": {{index}}, "Type": "Reroute" }"""));
			links.Append(FormattableString.Invariant($$""", { "From": [{{index}}, "Out"], "To": [1, "In"] }"""));
		}
		AnalyzedDocument fan = null!;
		TimeSpan fanTime = Time(() => fan = VisualScriptTesting.Analyze(VisualScriptTesting.Document(nodes.ToString(), links.ToString())));

		Assert.Equal("bool", VisualScriptTesting.Pin(fan, 1, "Out").Type.Id);
		Assert.Equal("bool", VisualScriptTesting.Pin(fan, branch + 1, "In", PinDirection.In).Type.Id);
		// The other sources' only links are errors: nothing gives them a type.
		Assert.Equal(sources - 1, fan.Diagnostics.Count(static diagnostic => diagnostic.Code == DiagnosticCode.MultipleDataLinks));
		Assert.Equal(sources - 1, fan.Diagnostics.Count(static diagnostic => diagnostic.Code == DiagnosticCode.UnresolvedWildcard));
		Assert.Equal(2 * (sources - 1), fan.Diagnostics.Count);

		// 40000 links from the outputs of 10000 Break nodes, none of which converts to the bool input.
		const int breaks = 10000;
		nodes.Clear();
		links.Clear();
		nodes.Append("""{ "ID": 1, "Type": "Flow.Branch" }""");
		for (int index = 2; index < breaks + 2; index++)
		{
			nodes.Append(FormattableString.Invariant($$""", { "ID": {{index}}, "Type": "Break", "Settings": { "Type": "Strada.Vector4" } }"""));
			foreach (string output in new[] { "X", "Y", "Z", "W" })
			{
				links.Append(links.Length > 0 ? ", " : string.Empty)
					.Append(FormattableString.Invariant($$"""{ "From": [{{index}}, "{{output}}"], "To": [1, "Condition"] }"""));
			}
		}
		AnalyzedDocument mismatches = null!;
		TimeSpan mismatchTime = Time(() => mismatches = VisualScriptTesting.Analyze(VisualScriptTesting.Document(nodes.ToString(), links.ToString())));

		Assert.Equal(4 * breaks, mismatches.Diagnostics.Count(static diagnostic => diagnostic.Code == DiagnosticCode.TypeMismatch));
		Assert.Equal(4 * breaks, mismatches.Diagnostics.Count);
		// About 0.2 s and 0.6 s unoptimized; walking the chain for every source took 6 s, and every link looking through those
		// before it 8 s.
		Assert.True(fanTime < TimeSpan.FromSeconds(2.0), $"{sources} Reroutes into a chain of {chain} took {fanTime.TotalMilliseconds:F0} ms to analyze");
		Assert.True(mismatchTime < TimeSpan.FromSeconds(4.0), $"{4 * breaks} links into one input took {mismatchTime.TotalMilliseconds:F0} ms to analyze");
	}
}
