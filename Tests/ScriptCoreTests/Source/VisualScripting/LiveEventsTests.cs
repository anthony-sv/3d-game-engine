using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The live log (spec-slice4 section 8.2): receipts of live edits and runtime problems for the editor, bounded, coalesced by
// key and read by serial through VisualScriptTakeLiveEvents (section 8.4).
public sealed class LiveEventsTests
{
	private static LiveProblemKey Key(uint node, ulong entity = TestRuntime.Hero, LiveSeverity severity = LiveSeverity.Error,
		ulong document = TestRuntime.Graph) => new(TestRuntime.Graph, document, node, entity, severity);

	private static void AddProblem(LiveEventLog log, LiveProblemKey key, string message = "integer division by zero") =>
		log.AddProblem(key, 0, "Divide", message, []);

	private static LiveEditReceipt Receipt(string name = TestRuntime.Name) =>
		new() { Graph = TestRuntime.Graph, Name = name, Outcome = LiveOutcome.Swapped };

	private static JsonElement Read(LiveEventLog log, ulong after)
	{
		byte[]? text = log.Write(after);
		Assert.NotNull(text);
		return VisualScriptTesting.ParseObject(text);
	}

	private static JsonElement[] Edits(JsonElement events) => [.. events.GetProperty("Edits").EnumerateArray()];

	private static JsonElement[] Problems(JsonElement events) => [.. events.GetProperty("Problems").EnumerateArray()];

	private static ulong Serial(JsonElement element) => element.GetProperty("Serial").GetUInt64();

	private static string Id(ulong id) => id.ToString(CultureInfo.InvariantCulture);

	[Fact]
	public void TheLogKeepsTheLast64ReceiptsAnd512Problems()
	{
		LiveEventLog log = new();
		for (int edit = 0; edit < 70; edit++)
		{
			log.AddEdit(Receipt(FormattableString.Invariant($"Assets/Graph{edit}.sgraph")));
		}
		for (uint node = 0; node < 520; node++)
		{
			AddProblem(log, Key(node));
		}
		// A problem the log has keeps being counted when the log is full.
		AddProblem(log, Key(3));

		JsonElement events = Read(log, 0);
		JsonElement[] edits = Edits(events);
		Assert.Equal(LiveEventLog.MaxEdits, edits.Length);
		Assert.Equal("Assets/Graph6.sgraph", edits[0].GetProperty("Name").GetString());
		Assert.Equal("Assets/Graph69.sgraph", edits[^1].GetProperty("Name").GetString());
		JsonElement[] problems = Problems(events);
		Assert.Equal(LiveEventLog.MaxProblems, problems.Length);
		Assert.Equal(8, events.GetProperty("Dropped").GetInt32());
		Assert.Equal(8, log.Dropped);
		Assert.DoesNotContain(problems, static problem => problem.GetProperty("Node").GetUInt32() >= LiveEventLog.MaxProblems);
		Assert.Equal(2, problems.Single(static problem => problem.GetProperty("Node").GetUInt32() == 3).GetProperty("Count").GetInt32());
	}

	[Fact]
	public void AProblemThatRecursIsUpdatedInPlaceWithANewSerial()
	{
		LiveEventLog log = new();
		LiveCallFrame[] calls = [new LiveCallFrame(TestRuntime.Graph, 0, 22)];
		for (int abort = 0; abort < 1000; abort++)
		{
			log.AddProblem(Key(14), 0, "Divide", FormattableString.Invariant($"failure {abort}"), abort == 999 ? calls : []);
		}
		// Each part of the key tells problems apart: the node, the entity, the document and the severity.
		AddProblem(log, Key(14, entity: 8));
		AddProblem(log, Key(14, severity: LiveSeverity.Warning));
		AddProblem(log, Key(14, document: CrossGraphTesting.Motion));
		AddProblem(log, Key(15));

		JsonElement[] problems = Problems(Read(log, 0));
		Assert.Equal(5, problems.Length);
		JsonElement first = problems[0];
		Assert.Equal(1000, first.GetProperty("Count").GetInt32());
		Assert.Equal("failure 999", first.GetProperty("Message").GetString());
		Assert.Equal(1000ul, Serial(first));
		JsonElement call = Assert.Single(first.GetProperty("Calls").EnumerateArray());
		Assert.Equal((Id(TestRuntime.Graph), 0u, 22u), (call.GetProperty("Document").GetString(), call.GetProperty("Function").GetUInt32(), call.GetProperty("Node").GetUInt32()));
		Assert.All(problems.Skip(1), static problem => Assert.Equal(1, problem.GetProperty("Count").GetInt32()));
		Assert.Equal(["Error", "Error", "Warning", "Error", "Error"], problems.Select(static problem => problem.GetProperty("Severity").GetString()));
		Assert.Equal([Id(TestRuntime.Hero), Id(8), Id(TestRuntime.Hero), Id(TestRuntime.Hero), Id(TestRuntime.Hero)],
			problems.Select(static problem => problem.GetProperty("Entity").GetString()));

		// The recurrence takes a new serial, and is the only event after the ones taken.
		ulong last = log.LastSerial;
		AddProblem(log, Key(14), "failure 1000");
		JsonElement later = Assert.Single(Problems(Read(log, last)));
		Assert.Equal((last + 1, 1001), (Serial(later), later.GetProperty("Count").GetInt32()));
	}

	[Fact]
	public void SerialsIncreaseAndWriteGivesExactlyTheLaterEvents()
	{
		LiveEventLog log = new();
		Assert.Null(log.Write(0));
		log.AddEdit(Receipt());
		AddProblem(log, Key(1));
		log.AddEdit(Receipt());
		AddProblem(log, Key(2));
		AddProblem(log, Key(1));
		log.AddEdit(Receipt());
		Assert.Equal(6ul, log.LastSerial);

		for (ulong after = 0; after < log.LastSerial; after++)
		{
			JsonElement events = Read(log, after);
			Assert.Equal(6ul, events.GetProperty("Last").GetUInt64());
			ulong[] edits = [.. Edits(events).Select(Serial)];
			ulong[] problems = [.. Problems(events).Select(Serial)];
			Assert.Equal(((ulong[])[1, 3, 6]).Where(serial => serial > after), edits);
			// Problem 1 was updated at serial 5: its first serial is gone.
			Assert.Equal(((ulong[])[4, 5]).Where(serial => serial > after), problems);
		}
		Assert.Null(log.Write(6));
		Assert.Null(log.Write(7));
	}

	// A problem that finds the log full is a change: Dropped grows with a serial of its own, so a reader that took everything
	// reads the new count, with no edits and no problems.
	[Fact]
	public void ADroppedProblemTakesASerial()
	{
		LiveEventLog log = new();
		for (uint node = 0; node < LiveEventLog.MaxProblems; node++)
		{
			AddProblem(log, Key(node));
		}
		ulong last = Read(log, 0).GetProperty("Last").GetUInt64();
		Assert.Equal((ulong)LiveEventLog.MaxProblems, last);

		AddProblem(log, Key(LiveEventLog.MaxProblems));
		JsonElement events = Read(log, last);
		Assert.Equal((last + 1, 1), (events.GetProperty("Last").GetUInt64(), events.GetProperty("Dropped").GetInt32()));
		Assert.Empty(Edits(events));
		Assert.Empty(Problems(events));
		AddProblem(log, Key(LiveEventLog.MaxProblems));
		Assert.Equal(2, Read(log, last + 1).GetProperty("Dropped").GetInt32());
		Assert.Equal(LiveEventLog.MaxProblems, log.ProblemCount);
	}

	// Clearing the log starts a play session: its receipts, problems and Dropped go, a problem that happens again counts from 1,
	// and serials go on from the last one, so the reader reads after the serial it took.
	[Fact]
	public void ClearingTheLogStartsTheCountsAgain()
	{
		LiveEventLog log = new();
		log.AddEdit(Receipt());
		for (uint node = 0; node <= LiveEventLog.MaxProblems; node++)
		{
			AddProblem(log, Key(node));
		}
		AddProblem(log, Key(1));
		ulong last = Read(log, 0).GetProperty("Last").GetUInt64();
		Assert.Equal(1, log.Dropped);

		log.Clear();
		Assert.Equal((0, 0, last), (log.ProblemCount, log.Dropped, log.LastSerial));
		Assert.Null(log.Write(last));
		// A reader from an earlier serial learns the last one and that nothing is left.
		JsonElement empty = Read(log, 0);
		Assert.Equal((last, 0), (empty.GetProperty("Last").GetUInt64(), empty.GetProperty("Dropped").GetInt32()));
		Assert.Empty(Edits(empty));
		Assert.Empty(Problems(empty));
		AddProblem(log, Key(1));
		JsonElement events = Read(log, last);
		JsonElement problem = Assert.Single(Problems(events));
		Assert.Equal((last + 1, 1, 0), (Serial(problem), problem.GetProperty("Count").GetInt32(), events.GetProperty("Dropped").GetInt32()));
		Assert.Empty(Edits(events));
	}

	// An abort of a library's node is a problem of the document holding the node and of the graph the instance runs, with the
	// calls that led there; its message is the bare one the log line ends with.
	[Fact]
	public void ALibraryNodesProblemNamesItsDocumentAndItsCallers()
	{
		DocumentBuilder library = new() { Kind = "Library" };
		FunctionBuilder split = library.Function("Split", inputs: ["Divisor:int"]);
		uint divide = split.Graph.Node("Operator.Divide", inputs: """{ "A": 10 }""");
		split.Graph.Link(split.Entry, "Divisor", divide, "B");
		split.Graph.Flow(split.Entry, split.Graph.Write(divide, "Result"));
		DocumentBuilder document = new();
		uint call = document.Graph.Node("Function.Call", CrossGraphTesting.Call(split, CrossGraphTesting.Motion), """{ "Divisor": 0 }""");
		document.Graph.Flow(document.Graph.Node("Event.Create"), call);
		TestRuntime runtime = new();
		runtime.LoadWorking(document.Build(), references: new BundleBuilder().Full(CrossGraphTesting.Motion, CrossGraphTesting.MotionName, library).Build());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		ulong last = runtime.Runtime.LiveEvents.LastSerial;

		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Create));
		JsonElement problem = Assert.Single(Problems(Read(runtime.Runtime.LiveEvents, last)));
		Assert.Equal("Error", problem.GetProperty("Severity").GetString());
		Assert.Equal(Id(TestRuntime.Graph), problem.GetProperty("Graph").GetString());
		Assert.Equal(Id(CrossGraphTesting.Motion), problem.GetProperty("Document").GetString());
		Assert.Equal((split.Id, divide, "Divide"),
			(problem.GetProperty("Function").GetUInt32(), problem.GetProperty("Node").GetUInt32(), problem.GetProperty("Title").GetString()));
		Assert.Equal(Id(TestRuntime.Hero), problem.GetProperty("Entity").GetString());
		JsonElement caller = Assert.Single(problem.GetProperty("Calls").EnumerateArray());
		Assert.Equal((Id(TestRuntime.Graph), 0u, call),
			(caller.GetProperty("Document").GetString(), caller.GetProperty("Function").GetUInt32(), caller.GetProperty("Node").GetUInt32()));
		string message = problem.GetProperty("Message").GetString()!;
		Assert.Equal(FormattableString.Invariant($"{CrossGraphTesting.MotionName}: Divide (node {divide}) on Hero ({TestRuntime.Hero}): {message}"),
			runtime.SingleError());
	}

	// Calls are the innermost eight of a deeper stack, innermost last.
	[Fact]
	public void AProblemKeepsTheInnermostEightCalls()
	{
		DocumentBuilder document = new();
		uint depth = document.Variable("Depth", "int", "0");
		FunctionBuilder descend = document.Function("Descend");
		uint get = descend.Graph.Get(depth);
		uint add = descend.Graph.Node("Operator.Add", inputs: """{ "B": 1 }""");
		descend.Graph.Link(get, "Value", add, "A");
		uint set = descend.Graph.Set(depth);
		descend.Graph.Link(add, "Result", set, "Value");
		uint zero = document.Variable("Zero", "int", "0");
		uint deeper = descend.Graph.Node("Function.Call", descend.Settings);
		uint divide = descend.Graph.Node("Operator.Divide", inputs: """{ "A": 1 }""");
		descend.Graph.Link(descend.Graph.Get(zero), "Value", divide, "B");
		uint gate = descend.Graph.Node("Flow.Branch");
		uint less = descend.Graph.Node("Operator.Less", inputs: """{ "B": 12 }""");
		descend.Graph.Link(set, "Value", less, "A");
		descend.Graph.Link(less, "Result", gate, "Condition");
		descend.Graph.Flow(descend.Entry, set);
		descend.Graph.Flow(set, gate);
		descend.Graph.Flow(gate, deeper, "True");
		descend.Graph.Flow(gate, descend.Graph.Write(divide, "Result"), "False");
		uint outer = document.Graph.Node("Function.Call", descend.Settings);
		document.Graph.Flow(document.Graph.Node("Event.Create"), outer);
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Create));
		JsonElement problem = Assert.Single(Problems(Read(runtime.Runtime.LiveEvents, 0)));
		JsonElement[] calls = [.. problem.GetProperty("Calls").EnumerateArray()];
		Assert.Equal(ExecutionError.MaxCalls, calls.Length);
		// Twelve calls deep: the event graph's call is the outermost, left out with three of Descend's.
		Assert.All(calls, call => Assert.Equal((descend.Id, deeper), (call.GetProperty("Function").GetUInt32(), call.GetProperty("Node").GetUInt32())));
		Assert.Equal(12, runtime.Variable("Depth"));
		Assert.NotEqual(outer, calls[0].GetProperty("Node").GetUInt32());
	}

	// Warnings are recorded when they are logged, once per node per instance; an abort in an event C# sends is recorded once,
	// by the execution that logged it.
	[Fact]
	public void WarningsAndAbortsOfSentEventsAreRecordedOnce()
	{
		DocumentBuilder document = new();
		uint divisor = document.Variable("Divisor", "int", "0");
		GraphBuilder graph = document.Graph;
		uint send = graph.Node("Event.Send", """{ "Name": "Missing" }""");
		graph.Link(CrossGraphTesting.EntityOf(graph, 0), "Result", send, "Target");
		graph.Flow(graph.Node("Event.Update"), send);
		uint fail = graph.Node("Event.Custom", """{ "Name": "Fail" }""");
		uint divide = graph.Node("Operator.Divide", inputs: """{ "A": 10 }""");
		graph.Link(graph.Get(divisor), "Value", divide, "B");
		graph.Flow(fail, graph.Write(divide, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Throws<InvalidOperationException>(() => runtime.Runtime.SendEvent(TestRuntime.Hero, "Fail", []));
		Assert.Throws<InvalidOperationException>(() => runtime.Runtime.SendEvent(TestRuntime.Hero, "Fail", []));

		JsonElement[] problems = Problems(Read(runtime.Runtime.LiveEvents, 0));
		Assert.Equal([("Warning", send, 1), ("Error", divide, 2)],
			problems.Select(static problem => (problem.GetProperty("Severity").GetString(), problem.GetProperty("Node").GetUInt32(), problem.GetProperty("Count").GetInt32())));
		Assert.Equal(TestRuntime.On(problems[0].GetProperty("Title").GetString()!, send) + problems[0].GetProperty("Message").GetString(),
			Assert.Single(runtime.Environment.Warnings));
		Assert.Equal(2, runtime.Environment.Errors.Count());
	}

	// Destroying every instance is no boundary: a scene load during play destroys them and creates them again for the same
	// entities, and the session's problems count on. Only VisualScriptClearLiveEvents, when play starts, counts them anew.
	[Fact]
	public void ASceneLoadKeepsCountingAndClearingTheLiveEventsCountsAnew()
	{
		DocumentBuilder document = new();
		uint divisor = document.Variable("Divisor", "int", "0");
		uint divide = document.Graph.Node("Operator.Divide", inputs: """{ "A": 10 }""");
		document.Graph.Link(document.Graph.Get(divisor), "Value", divide, "B");
		document.Graph.Flow(document.Graph.Node("Event.Update"), document.Graph.Write(divide, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update);
		ulong last = Serial(Problems(Read(runtime.Runtime.LiveEvents, 0)).Single());

		runtime.Runtime.DestroyAllInstances();
		Assert.Equal(1, runtime.Runtime.LiveEvents.ProblemCount);
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		runtime.Invoke(ScriptEvent.Update);
		JsonElement problem = Assert.Single(Problems(Read(runtime.Runtime.LiveEvents, last)));
		Assert.Equal(3, problem.GetProperty("Count").GetInt32());
		last = Serial(problem);

		runtime.Runtime.DestroyAllInstances();
		runtime.Runtime.ClearLiveEvents();
		Assert.Equal(0, runtime.Runtime.LiveEvents.ProblemCount);
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		runtime.Invoke(ScriptEvent.Update);
		problem = Assert.Single(Problems(Read(runtime.Runtime.LiveEvents, last)));
		Assert.Equal((last + 1, 1), (Serial(problem), problem.GetProperty("Count").GetInt32()));
	}

	// What a receipt says reaches the JSON of section 8.4 member for member.
	[Fact]
	public void AReceiptIsWrittenMemberForMember()
	{
		LiveEventLog log = new();
		InstanceNote note = new(123) { WaitsKept = 1 };
		note.Cancelled.Add(new NodeNote(0, 0, 12, "Delay", "the edit removed or changed Delay (node 12)"));
		note.Cancelled.Add(new NodeNote(CrossGraphTesting.Motion, 4, 30, "Apply Damage", "the edit disabled Apply Damage (node 30)", Stopped: true));
		note.Timelines.Add(new TimelineNote(9, 1.25, null));
		note.Timelines.Add(new TimelineNote(10, 1.0, 1.75));
		note.Timelines.Add(new TimelineNote(11, double.NaN, null));
		log.AddEdit(new LiveEditReceipt
		{
			Graph = CrossGraphTesting.Door,
			Name = CrossGraphTesting.DoorName,
			Outcome = LiveOutcome.Pending,
			Instances = 3,
			Pending = 2,
			KeptStates = 4,
			ResetStates = [new NodeNote(0, 0, 7, "Do Once", StateReset.Changed)],
			InstanceNotes = [note],
			MoreInstances = 5,
		});
		log.AddEdit(new LiveEditReceipt { Graph = 1, Name = "Broken", Outcome = LiveOutcome.Kept, Errors = "1 error, first: no (node 3)" });

		JsonElement[] edits = Edits(Read(log, 0));
		JsonElement edit = edits[0];
		// One line in the JSON, written here a member group per line.
		string expected = """
			{"Serial":1,"Graph":"8146390251172604461","Name":"Scripts/Door.sgraph","Outcome":"Pending","Deferred":false,"Instances":3,"Pending":2,
			"Variables":{"Kept":[],"Matched":[],"Reset":[],"Added":[],"Removed":[]},
			"States":{"Kept":4,"Reset":[{"Document":"8146390251172604461","Function":0,"Node":7,"Title":"Do Once","Reason":"Changed"}]},
			"InstanceNotes":[{"Entity":"123","WaitsKept":1,"StopsKept":0,"Cancelled":[
			{"Document":"8146390251172604461","Function":0,"Node":12,"Title":"Delay","Stopped":false,"Reason":"the edit removed or changed Delay (node 12)"},
			{"Document":"5550000000000000001","Function":4,"Node":30,"Title":"Apply Damage","Stopped":true,"Reason":"the edit disabled Apply Damage (node 30)"}],
			"Timelines":[{"Node":9,"Time":1.25,"ClampedFrom":null},{"Node":10,"Time":1,"ClampedFrom":1.75},{"Node":11,"Time":null,"ClampedFrom":null}]}],
			"MoreInstances":5,"Errors":""}
			""";
		Assert.Equal(expected.ReplaceLineEndings(string.Empty), edit.GetRawText());
		Assert.Equal(("Kept", "1 error, first: no (node 3)"), (edits[1].GetProperty("Outcome").GetString(), edits[1].GetProperty("Errors").GetString()));
	}
}
