using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Flow tracing (spec-slice4 section 6): what Record runs count in watched graphs (per exit and without one, in the order they
// first fired, pure evaluations included), merged by document and function, the trace block a take writes with its exact
// layout and caps, and graph.trace's recording with its filters and summary.
public sealed class DebugTraceTests
{
	private const ulong Villain = 8;
	private const ulong OtherGraph = 200;

	internal sealed record TraceStep(uint Node, string? Exit, uint Count, uint FirstOrder);

	internal sealed record TraceGraph(ulong Document, uint Function, TraceStep[] Steps);

	internal sealed record Trace(ulong StopRevision, uint Dropped, TraceGraph[] Graphs);

	// Reads a block of spec-slice4 section 6.2 the way the engine's reader validates it: the magic, the version, the reserved
	// word, every size against the length and every string index; the block holds exactly what its counts say.
	internal static Trace Read(byte[] block)
	{
		ReadOnlySpan<byte> span = block;
		Assert.True(block.Length >= 32);
		Assert.Equal("VSDT"u8.ToArray(), block[..4]);
		Assert.Equal(1u, BinaryPrimitives.ReadUInt32LittleEndian(span[4..]));
		ulong revision = BinaryPrimitives.ReadUInt64LittleEndian(span[8..]);
		uint graphCount = BinaryPrimitives.ReadUInt32LittleEndian(span[16..]);
		uint stringCount = BinaryPrimitives.ReadUInt32LittleEndian(span[20..]);
		uint dropped = BinaryPrimitives.ReadUInt32LittleEndian(span[24..]);
		Assert.Equal(0u, BinaryPrimitives.ReadUInt32LittleEndian(span[28..]));
		int offset = 32;
		List<(ulong, uint, List<(uint Node, uint Exit, uint Count, uint First)>)> graphs = [];
		for (uint graph = 0; graph < graphCount; graph++)
		{
			Assert.True(offset + 16 <= block.Length);
			ulong document = BinaryPrimitives.ReadUInt64LittleEndian(span[offset..]);
			uint function = BinaryPrimitives.ReadUInt32LittleEndian(span[(offset + 8)..]);
			uint count = BinaryPrimitives.ReadUInt32LittleEndian(span[(offset + 12)..]);
			offset += 16;
			Assert.True(offset + ((long)count * 16) <= block.Length);
			List<(uint, uint, uint, uint)> steps = [];
			for (uint step = 0; step < count; step++)
			{
				steps.Add((BinaryPrimitives.ReadUInt32LittleEndian(span[offset..]), BinaryPrimitives.ReadUInt32LittleEndian(span[(offset + 4)..]),
					BinaryPrimitives.ReadUInt32LittleEndian(span[(offset + 8)..]), BinaryPrimitives.ReadUInt32LittleEndian(span[(offset + 12)..])));
				offset += 16;
			}
			graphs.Add((document, function, steps));
		}
		List<string> strings = [];
		for (uint text = 0; text < stringCount; text++)
		{
			Assert.True(offset + 4 <= block.Length);
			int length = (int)BinaryPrimitives.ReadUInt32LittleEndian(span[offset..]);
			Assert.True(offset + 4 + length <= block.Length);
			strings.Add(Encoding.UTF8.GetString(span.Slice(offset + 4, length)));
			offset += 4 + length;
		}
		Assert.Equal(block.Length, offset);
		Assert.Equal(strings.Count, strings.Distinct(StringComparer.Ordinal).Count());
		TraceGraph[] read = [.. graphs.Select(graph => new TraceGraph(graph.Item1, graph.Item2, [.. graph.Item3.Select(step =>
		{
			Assert.True(step.Exit == 0xFFFFFFFF || step.Exit < strings.Count);
			return new TraceStep(step.Node, step.Exit == 0xFFFFFFFF ? null : strings[(int)step.Exit], step.Count, step.First);
		})]))];
		// Graphs by document and function, steps by first order.
		Assert.Equal(read.OrderBy(static graph => graph.Document).ThenBy(static graph => graph.Function), read);
		foreach (TraceGraph graph in read)
		{
			Assert.Equal(graph.Steps.OrderBy(static step => step.FirstOrder), graph.Steps);
		}
		return new Trace(revision, dropped, read);
	}

	private static Trace Take(TestRuntime runtime)
	{
		HostReply reply = runtime.Runtime.TakeTrace();
		Assert.Equal(VisualScriptStatus.Success, reply.Status);
		return Read(reply.Text!);
	}

	private static string Watch(ulong document, uint function = 0) => FormattableString.Invariant($$"""{ "Document": "{{document}}", "Function": {{function}} }""");

	// Update: a Branch on Flag (a variable), whose True writes "yes".
	private static (TestRuntime Runtime, uint Update, uint Get, uint Branch, uint Write) Branching()
	{
		DocumentBuilder document = new();
		uint flag = document.Variable("Flag", "bool", "true");
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		uint get = graph.Get(flag);
		uint branch = graph.Node("Flow.Branch");
		graph.Link(get, "Value", branch, "Condition");
		uint write = graph.Write("yes");
		graph.Flow(update, branch);
		graph.Flow(branch, write, "True");
		return (TestRuntime.Start(document), update, get, branch, write);
	}

	// Each exit counts how often it fired and when it first did, a pure evaluation counts without an exit, and a take starts
	// the next one empty.
	[Fact]
	public void RecordRunsCountEveryExitInTheOrderItFirstFired()
	{
		(TestRuntime runtime, uint update, uint get, uint branch, uint write) = Branching();
		DebugSession session = new(runtime);
		session.Configure(watches: [Watch(TestRuntime.Graph)]);
		for (int frame = 0; frame < 3; frame++)
		{
			runtime.Invoke(ScriptEvent.Update);
		}

		Trace trace = Take(runtime);
		Assert.Equal(session.StopRevision, trace.StopRevision);
		Assert.Equal(0u, trace.Dropped);
		TraceGraph graph = Assert.Single(trace.Graphs);
		Assert.Equal((TestRuntime.Graph, 0u), (graph.Document, graph.Function));
		Assert.Equal([new(update, "Then", 3, 1), new(get, null, 3, 2), new(branch, "True", 3, 3), new(write, "Then", 3, 4)], graph.Steps);
		Assert.Empty(Take(runtime).Graphs);

		runtime.Instance().Variables[0] = Slot.FromBool(false);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal([new(update, "Then", 1, 1), new(get, null, 1, 2), new(branch, "False", 1, 3)], Assert.Single(Take(runtime).Graphs).Steps);
	}

	// The block's layout byte by byte: the header, a graph, its steps and the string table of exit names.
	[Fact]
	public void TheBlockHasItsExactLayout()
	{
		(TestRuntime runtime, uint update, _, _, _) = Branching();
		DebugSession session = new(runtime);
		session.Configure(watches: [Watch(TestRuntime.Graph)]);
		runtime.Invoke(ScriptEvent.Update);
		byte[] block = runtime.Runtime.TakeTrace().Text!;

		// Header 32, one graph 16, four steps of 16, the strings "Then" and "True" of 4 + 4 bytes each.
		Assert.Equal(32 + 16 + (4 * 16) + (2 * 8), block.Length);
		Assert.Equal(1u, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(16)));
		Assert.Equal(2u, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(20)));
		Assert.Equal(TestRuntime.Graph, BinaryPrimitives.ReadUInt64LittleEndian(block.AsSpan(32)));
		Assert.Equal(0u, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(40)));
		Assert.Equal(4u, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(44)));
		Assert.Equal(update, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(48)));
		Assert.Equal(0u, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(52)));
		Assert.Equal(1u, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(56)));
		Assert.Equal(1u, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(60)));
		Assert.Equal(0xFFFFFFFFu, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(68)));
		Assert.Equal(4u, BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(112)));
		Assert.Equal("Then"u8.ToArray(), block[116..120]);
		Assert.Equal("True"u8.ToArray(), block[124..128]);
	}

	// Without a debugger there is nothing to take; an attached debugger with nothing watched takes empty blocks.
	[Fact]
	public void TakesNeedTheDebugger()
	{
		(TestRuntime runtime, _, _, _, _) = Branching();
		Assert.Equal(VisualScriptStatus.NotFound, runtime.Runtime.TakeTrace().Status);
		DebugSession session = new(runtime);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Empty(Take(runtime).Graphs);
		Assert.Null(runtime.Instance().Frame.Stamps);
		session.Send("""{ "Command": "Detach" }""");
		Assert.Equal(VisualScriptStatus.NotFound, runtime.Runtime.TakeTrace().Status);
	}

	// Under "any instance" every instance's flow adds up in its graph; an instance target counts only that instance.
	[Fact]
	public void InstancesOfAGraphAddUpUnlessOneIsTheTarget()
	{
		(TestRuntime runtime, uint update, _, _, _) = Branching();
		runtime.Environment.Add(Villain, "Villain");
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(Villain));
		DebugSession session = new(runtime);
		session.Configure(watches: [Watch(TestRuntime.Graph)]);
		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update, entity: Villain);
		Assert.Equal(2u, Assert.Single(Take(runtime).Graphs).Steps[0].Count);

		session.Configure(watches: [Watch(TestRuntime.Graph)], targets: [DebugSession.Target(TestRuntime.Graph, Villain)]);
		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update, entity: Villain);
		runtime.Invoke(ScriptEvent.Update, entity: Villain);
		TraceGraph graph = Assert.Single(Take(runtime).Graphs);
		Assert.Equal(new TraceStep(update, "Then", 2, 1), graph.Steps[0]);
	}

	// Only watched graphs count: an unwatched function of a watched program, another graph, until they are watched; what
	// was counted survives a configuration that keeps the graph watched.
	[Fact]
	public void OnlyWatchedGraphsCount()
	{
		DocumentBuilder document = new();
		FunctionBuilder greet = document.Function("Greet");
		uint inside = greet.Graph.Write("inside");
		greet.Graph.Flow(greet.Entry, inside);
		GraphBuilder graph = document.Graph;
		uint call = graph.Node("Function.Call", greet.Settings);
		graph.Flow(graph.Node("Event.Update"), call);
		TestRuntime runtime = TestRuntime.Start(document);
		DocumentBuilder other = new();
		other.Graph.Flow(other.Graph.Node("Event.Update"), other.Graph.Write("other"));
		runtime.LoadWorking(other.Build(), OtherGraph);
		runtime.Environment.Add(Villain, "Villain");
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(Villain, OtherGraph));
		DebugSession session = new(runtime);
		session.Configure(watches: [Watch(TestRuntime.Graph)]);
		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update, entity: Villain);
		Assert.Equal([(TestRuntime.Graph, 0u)], Take(runtime).Graphs.Select(static graph => (graph.Document, graph.Function)));

		runtime.Invoke(ScriptEvent.Update);
		session.Configure(watches: [Watch(TestRuntime.Graph), Watch(TestRuntime.Graph, greet.Id), Watch(OtherGraph)]);
		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update, entity: Villain);
		Trace trace = Take(runtime);
		Assert.Equal([(TestRuntime.Graph, 0u), (TestRuntime.Graph, greet.Id), (OtherGraph, 0u)], trace.Graphs.Select(static graph => (graph.Document, graph.Function)));
		// The event graph counted both Updates of Hero across the configuration.
		Assert.Equal(2u, trace.Graphs[0].Steps[0].Count);
		Assert.Equal([new TraceStep(greet.Entry, "Then", 1, 4), new TraceStep(inside, "Then", 1, 5)], trace.Graphs[1].Steps);
	}

	// A library's function is compiled into every program that calls it: its flow merges into one graph of the library's
	// document.
	[Fact]
	public void ALibrarysBodyMergesAcrossThePrograms()
	{
		MotionLibrary library = new();
		DocumentBuilder caller(string text)
		{
			DocumentBuilder document = new();
			uint report = document.Graph.Node("Function.Call", CrossGraphTesting.Call(library.Report, CrossGraphTesting.Motion),
				$$"""{ "Text": {{RuntimeTesting.Quote(text)}} }""");
			document.Graph.Flow(document.Graph.Node("Event.Update"), report);
			return document;
		}
		TestRuntime runtime = new();
		runtime.LoadWorking(caller("hero").Build(), references: library.Bundle());
		runtime.LoadWorking(caller("villain").Build(), OtherGraph,
			new BundleBuilder(OtherGraph).Full(CrossGraphTesting.Motion, CrossGraphTesting.MotionName, library.Document).Build());
		runtime.Environment.Add(Villain, "Villain");
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(Villain, OtherGraph));
		DebugSession session = new(runtime);
		session.Configure(watches: [Watch(CrossGraphTesting.Motion, library.Report.Id)]);
		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update, entity: Villain);
		runtime.Invoke(ScriptEvent.Update, entity: Villain);

		TraceGraph graph = Assert.Single(Take(runtime).Graphs);
		Assert.Equal((CrossGraphTesting.Motion, library.Report.Id), (graph.Document, graph.Function));
		Assert.Equal([3u, 3u], graph.Steps.Select(static step => step.Count));
		Assert.Equal(["hero", "villain", "villain"], runtime.Records);
	}

	// A graph's counters hold at most so many entries and a take at most so many in all; the steps beyond them are Dropped.
	[Fact]
	public void TheCapsDropWhatIsBeyondThem()
	{
		DocumentBuilder document = new();
		FunctionBuilder greet = document.Function("Greet");
		uint inside = greet.Graph.Write("inside");
		greet.Graph.Flow(greet.Entry, inside);
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		uint first = graph.Write("first");
		uint second = graph.Write("second");
		uint call = graph.Node("Function.Call", greet.Settings);
		graph.Flow(update, first);
		graph.Flow(first, second);
		graph.Flow(second, call);
		TestRuntime runtime = TestRuntime.Start(document);
		Assert.Equal(0, runtime.Runtime.AttachDebugger(new VisualScriptDebugger(runtime.Runtime, graphEntries: 2, takeEntries: 3)));
		DebugSession session = new(runtime);
		session.Configure(watches: [Watch(TestRuntime.Graph), Watch(TestRuntime.Graph, greet.Id)]);
		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update);

		Trace trace = Take(runtime);
		// The event graph keeps Update and the first write; the function's Entry is the take's third entry. Each Update dropped
		// four steps: the second write's, the call's two (entering Greet, and returning through Then) and the function's write.
		Assert.Equal([new TraceStep(update, "Then", 2, 1), new TraceStep(first, "Then", 2, 2)], trace.Graphs[0].Steps);
		Assert.Equal([new TraceStep(greet.Entry, "Then", 2, 3)], trace.Graphs[1].Steps);
		Assert.Equal(8u, trace.Dropped);
		Assert.Equal(0u, Take(runtime).Dropped);
	}

	// Update: a ForLoop of Index 0 to Frames (a variable the test raises), whose body writes the Index plus one.
	private static (TestRuntime Runtime, uint Loop, uint Add, uint Write) Looping()
	{
		DocumentBuilder document = new();
		uint frames = document.Variable("Frames", "int", "0", isPublic: true);
		GraphBuilder graph = document.Graph;
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 0 }""");
		graph.Link(graph.Get(frames), "Value", loop, "Last");
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
		graph.Link(loop, "Index", add, "A");
		uint write = graph.Write(add, "Result");
		graph.Flow(graph.Node("Event.Update"), loop);
		graph.Flow(loop, write, "Body");
		return (TestRuntime.Start(document), loop, add, write);
	}

	private static JsonElement Record(bool summary, string? nodes = null)
	{
		(TestRuntime runtime, _, _, _) = Looping();
		DebugSession session = new(runtime);
		session.StartRecording(documents: nodes != null ? FormattableString.Invariant($"[\"{TestRuntime.Graph}\"]") : null, nodes: nodes, summary: summary);
		for (int frame = 0; frame < 4; frame++)
		{
			runtime.Runtime.SetVariable(TestRuntime.Hero, "Frames", typeof(int), frame);
			session.RecordFrame(frame);
			runtime.Invoke(ScriptEvent.Update);
		}
		return session.StopRecording();
	}

	// The recording lists every step with its frame and every value it saw; a node filter keeps exactly the steps and values
	// of those nodes; the summary is what the full list adds up to.
	[Fact]
	public void TheSummaryAndTheNodeFilterAgreeWithTheFullRecording()
	{
		(_, uint loop, uint add, uint write) = Looping();
		JsonElement full = Record(summary: false);
		Assert.Equal(["Steps", "Values", "Truncated"], full.EnumerateObject().Select(static member => member.Name));
		Assert.Equal("""{"Steps":0,"Values":0}""", full.GetProperty("Truncated").GetRawText());
		JsonElement[] steps = [.. full.GetProperty("Steps").EnumerateArray()];
		JsonElement[] values = [.. full.GetProperty("Values").EnumerateArray()];
		Assert.Equal(["I", "Frame", "Execution", "Entity", "Graph", "Document", "Function", "Node", "Title", "Exit"], steps[0].EnumerateObject().Select(static member => member.Name));
		Assert.Equal(steps.Select(static step => step.GetProperty("I").GetInt64()).Order(), steps.Select(static step => step.GetProperty("I").GetInt64()));
		// Frame 3: the loop's Body four times and its Completed once.
		Assert.Equal(4, steps.Count(step => step.GetProperty("Frame").GetInt32() == 3 && step.GetProperty("Node").GetUInt32() == loop &&
			DebugSession.Text(step, "Exit") == "Body"));
		Assert.Equal(10, steps.Count(step => step.GetProperty("Node").GetUInt32() == write));
		Assert.Contains(values, value => value.GetProperty("Node").GetUInt32() == write && DebugSession.Text(value, "Direction") == "In" &&
			DebugSession.Text(value, "Pin") == "Text" && DebugSession.Text(value, "Value") == "4");

		JsonElement filtered = Record(summary: false, nodes: FormattableString.Invariant($"[{add}, {write}]"));
		Assert.Equal(steps.Where(step => step.GetProperty("Node").GetUInt32() == write).Select(static step => step.GetRawText()),
			filtered.GetProperty("Steps").EnumerateArray().Select(static step => step.GetRawText()));
		Assert.Equal(values.Where(value => value.GetProperty("Node").GetUInt32() is var node && (node == add || node == write)).Select(static value => value.GetRawText()),
			filtered.GetProperty("Values").EnumerateArray().Select(static value => value.GetRawText()));

		JsonElement summary = Record(summary: true);
		Assert.Equal(["Summary", "Truncated"], summary.EnumerateObject().Select(static member => member.Name));
		JsonElement graph = Assert.Single(summary.GetProperty("Summary").GetProperty("Graphs").EnumerateArray());
		Assert.Equal(["Graph", "Document", "Function", "Steps", "Values"], graph.EnumerateObject().Select(static member => member.Name));
		foreach (JsonElement entry in graph.GetProperty("Steps").EnumerateArray())
		{
			uint node = entry.GetProperty("Node").GetUInt32();
			string? exit = entry.GetProperty("Exit").ValueKind == JsonValueKind.Null ? null : DebugSession.Text(entry, "Exit");
			JsonElement[] matching = [.. steps.Where(step => step.GetProperty("Node").GetUInt32() == node &&
				(step.GetProperty("Exit").ValueKind == JsonValueKind.Null ? null : DebugSession.Text(step, "Exit")) == exit)];
			Assert.Equal(matching.Length, entry.GetProperty("Count").GetInt32());
			Assert.Equal(matching[0].GetProperty("Frame").GetInt32(), entry.GetProperty("FirstFrame").GetInt32());
			Assert.Equal(matching[^1].GetProperty("Frame").GetInt32(), entry.GetProperty("LastFrame").GetInt32());
			Assert.Equal(DebugSession.Text(matching[0], "Title"), DebugSession.Text(entry, "Title"));
		}
		Assert.Equal(steps.Select(static step => (step.GetProperty("Node").GetUInt32(), step.GetProperty("Exit").GetRawText())).Distinct().Count(),
			graph.GetProperty("Steps").GetArrayLength());
		foreach (JsonElement entry in graph.GetProperty("Values").EnumerateArray())
		{
			JsonElement[] matching = [.. values.Where(value => value.GetProperty("Node").GetUInt32() == entry.GetProperty("Node").GetUInt32() &&
				DebugSession.Text(value, "Pin") == DebugSession.Text(entry, "Pin") && DebugSession.Text(value, "Direction") == DebugSession.Text(entry, "Direction"))];
			Assert.Equal(matching.Length, entry.GetProperty("Count").GetInt32());
			Assert.Equal(matching[0].GetProperty("Value").GetRawText(), entry.GetProperty("First").GetProperty("Value").GetRawText());
			Assert.Equal(matching[^1].GetProperty("Value").GetRawText(), entry.GetProperty("Last").GetProperty("Value").GetRawText());
			if (DebugSession.Text(entry, "Type") is "int" or "float")
			{
				double[] numbers = [.. matching.Select(static value => value.GetProperty("Value").GetDouble())];
				Assert.Equal(numbers.Min(), entry.GetProperty("Min").GetProperty("Value").GetDouble());
				Assert.Equal(numbers.Max(), entry.GetProperty("Max").GetProperty("Value").GetDouble());
			}
			else
			{
				Assert.False(entry.TryGetProperty("Min", out _));
			}
		}
		Assert.Equal(values.Select(static value => (value.GetProperty("Node").GetUInt32(), DebugSession.Text(value, "Pin"), DebugSession.Text(value, "Direction"))).Distinct().Count(),
			graph.GetProperty("Values").GetArrayLength());
		Assert.Contains(graph.GetProperty("Values").EnumerateArray(), value => value.GetProperty("Node").GetUInt32() == add &&
			value.GetProperty("Max").GetProperty("Value").GetInt32() == 4);
	}

	// The caps of a list count what they leave out; an entity filter records only that instance.
	[Fact]
	public void TheRecordingsCapsAndEntityFilterHold()
	{
		(TestRuntime runtime, _, _, uint write) = Looping();
		runtime.Environment.Add(Villain, "Villain");
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(Villain));
		runtime.Runtime.SetVariable(TestRuntime.Hero, "Frames", typeof(int), 3);
		DebugSession session = new(runtime);
		session.StartRecording(entity: FormattableString.Invariant($"\"{Villain}\""), maxSteps: 2, maxValues: 1);
		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update, entity: Villain);
		JsonElement recorded = session.StopRecording();
		Assert.All(recorded.GetProperty("Steps").EnumerateArray(), static step => Assert.Equal("8", DebugSession.Text(step, "Entity")));
		Assert.Equal(2, recorded.GetProperty("Steps").GetArrayLength());
		Assert.Equal(1, recorded.GetProperty("Values").GetArrayLength());
		// Villain's Frames is 0: Update, the Body once, the write, Completed: 4 steps.
		Assert.Equal(2, recorded.GetProperty("Truncated").GetProperty("Steps").GetInt32());
		Assert.True(recorded.GetProperty("Truncated").GetProperty("Values").GetInt32() > 0);
		Assert.Equal(("InvalidState", "no recording runs: Record Start begins one"), session.Refused("""{ "Command": "Record", "Stop": true }"""));
		Assert.DoesNotContain(write, recorded.GetProperty("Steps").EnumerateArray().Select(static step => step.GetProperty("Node").GetUInt32()));
	}
}
