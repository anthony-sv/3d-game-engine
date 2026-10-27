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

	// Links that name field pins of a wildcard node wait until it is resolved and split (spec-slice2 section 3.3). Attaching
	// them keeps every list of links in link order at the cost of one sort, and a node that splits visits only the links that
	// wait for it. A split Reroute whose 19980 field links come before 19980 other links, and 5000 split Reroutes that resolve
	// before the one split Select that 19960 field links wait for. Before, each late link was inserted in order behind every
	// link after it, and each split visited every link still waiting: the documents took seconds to analyze.
	[Fact]
	public void LinksToFieldPinsOfLateSplitsAnalyzeInTimeLinearInTheirNumber()
	{
		const int makes = 9990;
		StringBuilder nodes = new();
		StringBuilder links = new();
		nodes.Append("""{ "ID": 1, "Type": "Literal", "Settings": { "Type": "Strada.Vector3" } }, """)
			.Append("""{ "ID": 2, "Type": "Reroute", "Split": { "Outputs": ["Out"] } }, """)
			.Append("""{ "ID": 3, "Type": "Literal", "Settings": { "Type": "float" } }""");
		for (int index = 4; index < makes + 4; index++)
		{
			nodes.Append(FormattableString.Invariant($$""", { "ID": {{index}}, "Type": "Make", "Settings": { "Type": "Strada.Vector4" } }"""));
			links.Append(FormattableString.Invariant($$"""{ "From": [2, "Out.X"], "To": [{{index}}, "X"] }, { "From": [2, "Out.Y"], "To": [{{index}}, "Y"] }, """));
		}
		for (int index = 4; index < makes + 4; index++)
		{
			links.Append(FormattableString.Invariant($$"""{ "From": [3, "Value"], "To": [{{index}}, "Z"] }, { "From": [3, "Value"], "To": [{{index}}, "W"] }, """));
		}
		links.Append("""{ "From": [1, "Value"], "To": [2, "In"] }""");
		AnalyzedDocument early = null!;
		TimeSpan earlyTime = Time(() => early = VisualScriptTesting.Analyze(VisualScriptTesting.Document(nodes.ToString(), links.ToString())));

		Assert.Empty(early.Diagnostics);
		Assert.Equal(Enumerable.Range(0, 4 * makes + 1), early.EventGraph.Links.Select(static link => link.Document.Index));
		Assert.Equal("Out.Y", VisualScriptTesting.Pin(early, makes + 3, "Y", PinDirection.In).Source!.From.Name);
		Assert.Equal(2 * makes, VisualScriptTesting.Node(early, 2).GetShownPins().Where(static pin => !pin.IsInput).Sum(static pin => pin.Links.Count));

		const int reroutes = 5000;
		const int targets = 4990;
		nodes.Clear();
		links.Clear();
		for (int index = 1; index <= reroutes; index++)
		{
			nodes.Append(FormattableString.Invariant($$"""{ "ID": {{index}}, "Type": "Reroute", "Split": { "Outputs": ["Out"] } }, """));
		}
		const int select = reroutes + 1;
		nodes.Append(FormattableString.Invariant($$"""{ "ID": {{select}}, "Type": "Select", "Inputs": { "Condition": true }, "Split": { "Outputs": ["Result"] } }, """))
			.Append(FormattableString.Invariant($$"""{ "ID": {{select + 1}}, "Type": "Literal", "Settings": { "Type": "Strada.Vector3" } }"""));
		for (int index = select + 2; index < select + 2 + targets; index++)
		{
			nodes.Append(FormattableString.Invariant($$""", { "ID": {{index}}, "Type": "Make", "Settings": { "Type": "Strada.Vector4" } }"""));
			foreach ((string field, string input) in new[] { ("X", "X"), ("Y", "Y"), ("Z", "Z"), ("X", "W") })
			{
				links.Append(FormattableString.Invariant($$"""{ "From": [{{select}}, "Result.{{field}}"], "To": [{{index}}, "{{input}}"] }, """));
			}
		}
		links.Append(FormattableString.Invariant($$"""{ "From": [{{select + 1}}, "Value"], "To": [{{select}}, "True"] }"""));
		AnalyzedDocument pending = null!;
		TimeSpan pendingTime = Time(() => pending = VisualScriptTesting.Analyze(VisualScriptTesting.Document(nodes.ToString(), links.ToString())));

		// Nothing types the Reroutes: their Ins are unresolved, and their entries split nothing.
		Assert.Equal(reroutes, pending.Diagnostics.Count(static diagnostic => diagnostic.Code == DiagnosticCode.UnusedSplit));
		Assert.Equal(reroutes, pending.Diagnostics.Count(static diagnostic => diagnostic.Code == DiagnosticCode.UnresolvedWildcard));
		Assert.Equal(2 * reroutes, pending.Diagnostics.Count);
		Assert.True(VisualScriptTesting.Pin(pending, select + 1 + targets, "W", PinDirection.In).IsConnected);
		// About 0.5 s and 0.3 s unoptimized; inserting each late link in order behind the links after it took 5 s, and visiting
		// every waiting link at each split 2.5 s.
		Assert.True(earlyTime < TimeSpan.FromSeconds(3.0), $"{2 * makes} late links before {2 * makes} others took {earlyTime.TotalMilliseconds:F0} ms to analyze");
		Assert.True(pendingTime < TimeSpan.FromSeconds(1.5), $"{reroutes} splits before {4 * targets} late links took {pendingTime.TotalMilliseconds:F0} ms to analyze");
	}
}
