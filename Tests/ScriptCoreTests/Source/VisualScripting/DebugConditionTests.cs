using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Breakpoint conditions (spec-slice4 sections 2.3 and 2.4): the scope a condition reads (the loops around its node, innermost
// first, the locals, the one start's outputs, the variables), identifier forms, ambiguities and suggestions, the comparisons
// only conditions have, refused types, errors that never stop and failures that do, hit and reached counts, CheckCondition
// and Evaluate against a snapshot.
public sealed class DebugConditionTests
{
	// How many times the runtime stops while run runs and every stop is continued.
	private static int StopsOf(DebugSession session, Action run)
	{
		run();
		int stops = 0;
		while (session.Stops().Length > 0)
		{
			stops++;
			session.ContinueAll();
		}
		return stops;
	}

	// The breakpoint row of State for the node.
	private static JsonElement BreakpointOf(DebugSession session, uint node) =>
		session.State().GetProperty("Breakpoints").EnumerateArray().Single(row => row.GetProperty("Node").GetUInt32() == node);

	// Create runs a loop from 0 to last whose body writes Index; returns the body's node.
	private static (TestRuntime Runtime, DebugSession Session, uint Body, uint Loop) Looping(int last = 5)
	{
		DocumentBuilder document = new();
		document.Variable("Counter", "int", "10");
		GraphBuilder graph = document.Graph;
		uint loop = graph.Node("Flow.ForLoop", inputs: FormattableString.Invariant($$"""{ "First": 0, "Last": {{last}} }"""));
		uint body = graph.Write(loop, "Index");
		graph.Flow(graph.Node("Event.Create"), loop);
		graph.Flow(loop, body, "Body");
		TestRuntime runtime = TestRuntime.Start(document);
		return (runtime, new DebugSession(runtime), body, loop);
	}

	// A loop's Index in its body, the innermost loop's when loops nest, and a ForEach's Element.
	[Fact]
	public void ConditionsReadTheInnermostLoopsOutputs()
	{
		(TestRuntime runtime, DebugSession session, uint body, _) = Looping();
		session.Configure([session.Breakpoint(body, condition: "Index == 3")]);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["0", "1", "2"], runtime.Records);
		Assert.Single(session.Stops());
		session.Continue();
		Assert.Equal(6, runtime.Records.Count);
		JsonElement row = BreakpointOf(session, body);
		Assert.Equal(1, row.GetProperty("Hits").GetInt32());
		Assert.Equal(6, row.GetProperty("Reached").GetInt32());
		Assert.Equal(JsonValueKind.Null, row.GetProperty("Error").ValueKind);

		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint outer = graph.Node("Flow.ForLoop", inputs: """{ "First": 0, "Last": 1 }""");
		uint inner = graph.Node("Flow.ForLoop", inputs: """{ "First": 0, "Last": 2 }""");
		uint write = graph.Write(inner, "Index");
		uint each = graph.Node("Flow.ForEach");
		graph.Link(graph.Node("Call:Strada.ScriptCore.Tests.Probe.Numbers(int)", inputs: """{ "Count": 4 }"""), "Result", each, "Array");
		uint element = graph.Write(each, "Element");
		graph.Flow(graph.Node("Event.Create"), outer);
		graph.Flow(outer, inner, "Body");
		graph.Flow(inner, write, "Body");
		graph.Flow(outer, each, "Completed");
		graph.Flow(each, element, "Body");
		TestRuntime nested = TestRuntime.Start(document);
		DebugSession nestedSession = new(nested);
		nestedSession.Configure([nestedSession.Breakpoint(write, condition: "Index == 2"), nestedSession.Breakpoint(element, condition: "Element == 2")]);
		Assert.Equal(3, StopsOf(nestedSession, () => nested.Invoke(ScriptEvent.Create)));
	}

	// The outputs of the one start: a custom event's parameter, an event's Other and DeltaTime, a function's parameter, and a
	// function's locals.
	[Fact]
	public void ConditionsReadTheirStartsOutputsAndTheirFunctionsLocals()
	{
		DocumentBuilder document = new();
		FunctionBuilder tally = document.Function("Tally", inputs: ["Amount:int"]);
		tally.Local("Total", "int", "7");
		uint counted = tally.Graph.Write(tally.Entry, "Amount");
		tally.Graph.Flow(tally.Entry, counted);
		GraphBuilder graph = document.Graph;
		uint hit = graph.Node("Event.Custom", """{ "Name": "Hit", "Parameters": [{ "Name": "Row", "Type": "int" }] }""");
		uint row = graph.Write(hit, "Row");
		graph.Flow(hit, row);
		uint update = graph.Node("Event.Update");
		uint delta = graph.Write(update, "DeltaTime");
		graph.Flow(update, delta);
		uint contact = graph.Node("Event.CollisionEnter");
		uint other = graph.Write("contact");
		graph.Flow(contact, other);
		uint first = graph.Node("Function.Call", tally.Settings, """{ "Amount": 1 }""");
		uint second = graph.Node("Function.Call", tally.Settings, """{ "Amount": 2 }""");
		graph.Flow(graph.Node("Event.Create"), first);
		graph.Flow(first, second);
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Environment.Add(8, "Villain");
		DebugSession session = new(runtime);
		session.Configure(
		[
			session.Breakpoint(row, condition: "Row == 0"), session.Breakpoint(delta, condition: "DeltaTime > 0.5"),
			session.Breakpoint(other, condition: "Other != None"), session.Breakpoint(counted, condition: "Amount > 1 && Total == 7"),
		]);

		Assert.Equal(0, StopsOf(session, () => runtime.Runtime.SendEvent(TestRuntime.Hero, "Hit", [1])));
		Assert.Equal(1, StopsOf(session, () => runtime.Runtime.SendEvent(TestRuntime.Hero, "Hit", [0])));
		Assert.Equal(0, StopsOf(session, () => runtime.Invoke(ScriptEvent.Update, 0.25f)));
		Assert.Equal(1, StopsOf(session, () => runtime.Invoke(ScriptEvent.Update, 0.75f)));
		Assert.Equal(0, StopsOf(session, () => runtime.Invoke(ScriptEvent.CollisionEnter, other: 0)));
		Assert.Equal(1, StopsOf(session, () => runtime.Invoke(ScriptEvent.CollisionEnter, other: 8)));
		Assert.Equal(1, StopsOf(session, () => runtime.Invoke(ScriptEvent.Create)));
		Assert.All(session.State().GetProperty("Breakpoints").EnumerateArray(), static row => Assert.Equal(JsonValueKind.Null, row.GetProperty("Error").ValueKind));
	}

	// Names a condition cannot read: an event output of a node two starts lead to, an Index two loops that do not nest give,
	// two variables of one identifier form, an unknown name (with the nearest readable one). Each is the breakpoint's error:
	// it never stops.
	[Fact]
	public void NamesThatReadNothingAreTheBreakpointsError()
	{
		DocumentBuilder document = new();
		document.Variable("Max Health", "float", "10.0");
		document.Variable("Angle", "float", "0.0");
		document.Variable("Speed Limit", "float", "1.0");
		document.Variable("Speed_Limit", "float", "2.0");
		GraphBuilder graph = document.Graph;
		uint update = graph.Node("Event.Update");
		uint fixedUpdate = graph.Node("Event.FixedUpdate");
		uint shared = graph.Write("shared");
		graph.Flow(update, shared);
		graph.Flow(fixedUpdate, shared);
		uint left = graph.Node("Flow.ForLoop", inputs: """{ "First": 0, "Last": 1 }""");
		uint right = graph.Node("Flow.ForLoop", inputs: """{ "First": 0, "Last": 1 }""");
		uint both = graph.Write("both");
		graph.Flow(graph.Node("Event.Create"), left);
		graph.Flow(left, both, "Body");
		graph.Flow(left, right, "Completed");
		graph.Flow(right, both, "Body");
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		CompiledGraph events = runtime.Instance().Program.EventGraph;
		string Title(uint node) => events.Nodes[events.FindNode(node)].Title;

		foreach ((uint node, string condition, string error) in new[]
		{
			(shared, "DeltaTime > 0", $"line 1, column 1: DeltaTime is an output of {Title(update)} (node {update}), but {Title(fixedUpdate)} (node {fixedUpdate}) also leads here: a condition reads event outputs only when one event leads to the node"),
			(both, "Index == 0", $"line 1, column 1: Index is ambiguous here: {Title(left)} (node {left}) or {Title(right)} (node {right})"),
			(both, "Speed_Limit > 0.0", "line 1, column 1: Speed_Limit is ambiguous here: the variable Speed Limit or the variable Speed_Limit"),
			(both, "angle > 1.0", "line 1, column 1: unknown name angle: did you mean Angle?"),
			(both, "Max_Healt > 1.0", "line 1, column 1: unknown name Max_Healt: did you mean Max_Health?"),
			(both, "Index +", "line 1, column 7: expected a value after '+'"),
			(both, "Max_Health + 1.0", "line 1, column 1: the condition must be true or false, not a float"),
		})
		{
			session.Configure([session.Breakpoint(node, condition: condition)]);
			Assert.Equal(0, StopsOf(session, () =>
			{
				runtime.Invoke(ScriptEvent.Create);
				runtime.Invoke(ScriptEvent.Update, 0.5f);
			}));
			JsonElement row = BreakpointOf(session, node);
			Assert.Equal(error, DebugSession.Text(row, "Error"));
			Assert.Equal(0, row.GetProperty("Hits").GetInt32());
			Assert.True(row.GetProperty("Reached").GetInt32() > 0);
		}
		// Max_Health reads "Max Health".
		session.Configure([session.Breakpoint(both, condition: "Max_Health > 5.0")]);
		Assert.Equal(4, StopsOf(session, () => runtime.Invoke(ScriptEvent.Create)));
	}

	// The comparisons only conditions have: strings and enums with string literals (an enum by its value's name), entities
	// with None; and the types conditions refuse, by name.
	[Fact]
	public void ConditionsCompareStringsEnumsAndEntitiesAndRefuseOtherTypes()
	{
		DocumentBuilder document = new();
		document.Variable("State", "string", "\"Serve\"", isPublic: true);
		document.Variable("Mode", "Strada.ForceMode", "\"Impulse\"");
		document.Variable("Target", "Strada.Entity", isPublic: true);
		GraphBuilder graph = document.Graph;
		uint hit = graph.Node("Event.Custom", """{ "Name": "Hit", "Parameters": [{ "Name": "Rotation", "Type": "Strada.Quaternion" }] }""");
		uint write = graph.Write("hit");
		graph.Flow(hit, write);
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);

		foreach ((string condition, int stops) in new[]
		{
			("State == \"Serve\"", 1), ("State != \"Serve\"", 0), ("\"Rally\" == State", 0), ("State == \"Ser\\u0076e\" && !(Mode != \"Impulse\")", 1),
			("Mode == \"Force\"", 0), ("Target == None", 1), ("None != Target", 0),
		})
		{
			session.Configure([session.Breakpoint(write, condition: condition)]);
			Assert.Equal((condition, stops), (condition, StopsOf(session, () => runtime.Runtime.SendEvent(TestRuntime.Hero, "Hit", [Quaternion.Identity]))));
			Assert.Equal((condition, JsonValueKind.Null), (condition, BreakpointOf(session, write).GetProperty("Error").ValueKind));
		}
		runtime.Runtime.SetVariable(TestRuntime.Hero, "Target", typeof(Entity), new Entity(TestRuntime.Hero));
		session.Configure([session.Breakpoint(write, condition: "Target != None")]);
		Assert.Equal(1, StopsOf(session, () => runtime.Runtime.SendEvent(TestRuntime.Hero, "Hit", [Quaternion.Identity])));

		foreach ((string condition, string error) in new[]
		{
			("Mode == \"Impuls\"", "line 1, column 1: Force Mode has no value Impuls: Force, Impulse, Acceleration, VelocityChange"),
			("State == None", "line 1, column 1: State is a string: None compares only with entities"),
			("Target == \"Hero\"", "line 1, column 1: Target is an Entity: an entity compares only with None"),
			("State + 1 > 2", "line 1, column 1: State is a string: compare it with == or != to a string literal, as in State == \"Serve\""),
			("Rotation == None", "line 1, column 1: Rotation is a Quaternion: string literals compare only with strings and enums, and None only with entities"),
			("Rotation.X > 0.0", "line 1, column 1: Rotation is a Quaternion: conditions read numbers, booleans and vectors, and compare strings, enums and entities"),
			("\"Serve\" == \"Serve\"", "line 1, column 1: a string literal is compared with a name, as in State == \"Serve\""),
			("-\"Serve\" > 1", "line 1, column 2: a string literal is only compared with == or != to a name, as in State == \"Serve\""),
			("State == \"open", "line 1, column 10: the string literal is not closed: end it with \""),
		})
		{
			session.Configure([session.Breakpoint(write, condition: condition)]);
			Assert.Equal(0, StopsOf(session, () => runtime.Runtime.SendEvent(TestRuntime.Hero, "Hit", [Quaternion.Identity])));
			Assert.Equal(error, DebugSession.Text(BreakpointOf(session, write), "Error"));
		}
		// Expression nodes read no string literals.
		Assert.NotNull(ExpressionParser.Parse("State == \"Serve\"", static _ => []).Error);
	}

	// A condition that fails while it is evaluated stops, saying why: a broken condition must not skip silently. A hit count
	// lets the first hits where the condition holds run on.
	[Fact]
	public void AFailingConditionStopsAndHitCountsCountTheHitsWhereItHolds()
	{
		(TestRuntime runtime, DebugSession session, uint body, _) = Looping();
		session.Configure([session.Breakpoint(body, condition: "10 / Index == 5")]);
		runtime.Invoke(ScriptEvent.Create);
		JsonElement stop = session.SingleStop();
		Assert.Equal("Breakpoint", DebugSession.Text(stop, "Reason"));
		Assert.Equal("the condition failed: integer division by zero in 10 / Index", DebugSession.Text(stop, "Message"));
		Assert.Empty(runtime.Records);
		session.Continue();

		(runtime, session, body, _) = Looping();
		session.Configure([session.Breakpoint(body, hitCount: 2, condition: "Index % 2 == 0")]);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["0", "1"], runtime.Records);
		session.Continue();
		Assert.Equal(["0", "1", "2", "3"], runtime.Records);
		session.Continue();
		JsonElement row = BreakpointOf(session, body);
		Assert.Equal(3, row.GetProperty("Hits").GetInt32());
		Assert.Equal(6, row.GetProperty("Reached").GetInt32());
		// A condition that never holds: reached, never hit.
		session.Configure([session.Breakpoint(body, condition: "Index > 100")]);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Empty(session.Stops());
		row = BreakpointOf(session, body);
		Assert.Equal(0, row.GetProperty("Hits").GetInt32());
		Assert.Equal(6, row.GetProperty("Reached").GetInt32());
	}

	// A breakpoint on a node no flow enters takes no condition: it is its error, and it never stops.
	[Fact]
	public void APureNodesConditionIsRefused()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint add = graph.Node("Operator.Add", inputs: """{ "B": 2 }""");
		graph.Link(graph.Literal("int", "1"), "Value", add, "A");
		uint write = graph.Write(add, "Result");
		graph.Flow(graph.Node("Event.Create"), write);
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Configure([session.Breakpoint(add, condition: "true")]);
		Assert.Equal(0, StopsOf(session, () => runtime.Invoke(ScriptEvent.Create)));
		Assert.Equal(BreakpointConditions.PureCondition, DebugSession.Text(BreakpointOf(session, add), "Error"));
		Assert.Equal(BreakpointConditions.PureCondition, DebugSession.Text(session.CheckCondition(add, "true"), "Error"));
	}

	// CheckCondition compiles a condition against the program a node runs in, with or without a debugger: what it reads and the
	// node's scope, in lookup order; a document no loaded program compiles is not checked.
	[Fact]
	public void CheckConditionAnswersWhatAConditionReadsAndTheNodesScope()
	{
		DocumentBuilder document = new();
		document.Variable("Max Health", "float", "10.0");
		document.Variable("Index", "int", "0");
		GraphBuilder graph = document.Graph;
		uint hit = graph.Node("Event.Custom", """{ "Name": "Hit", "Parameters": [{ "Name": "Row", "Type": "int" }] }""");
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 0, "Last": 1 }""");
		uint body = graph.Write("body");
		graph.Flow(hit, loop);
		graph.Flow(loop, body, "Body");
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		// No debugger is needed.
		Assert.Equal("""{"Released":0}""", session.Send("""{ "Command": "Detach" }"""));

		JsonElement checkedReply = session.CheckCondition(body, "Index == 1 && Max_Health > Row");
		Assert.True(checkedReply.GetProperty("Checked").GetBoolean());
		Assert.Equal(JsonValueKind.Null, checkedReply.GetProperty("Error").ValueKind);
		Assert.Equal(["Index", "Max Health", "Row"], checkedReply.GetProperty("Reads").EnumerateArray().Select(static read => read.GetString()));
		List<(string, string, string, string, uint)> scope = [.. checkedReply.GetProperty("Scope").EnumerateArray().Select(static name =>
			(DebugSession.Text(name, "Name"), DebugSession.Text(name, "Form"), DebugSession.Text(name, "Type"), DebugSession.Text(name, "Source"),
			name.GetProperty("Node").GetUInt32()))];
		// The variable Index is shadowed by the loop's.
		Assert.Equal([("Index", "Index", "int", "Loop", loop), ("Row", "Row", "int", "Event", hit), ("Max Health", "Max_Health", "float", "Variable", 0u)], scope);
		JsonElement failed = session.CheckCondition(body, "Rows == 1");
		Assert.Equal("line 1, column 1: unknown name Rows: did you mean Row?", DebugSession.Text(failed, "Error"));
		JsonElement only = session.CheckCondition(body, "");
		Assert.Equal(JsonValueKind.Null, only.GetProperty("Error").ValueKind);
		Assert.Equal(3, only.GetProperty("Scope").GetArrayLength());
		JsonElement unknown = session.CheckCondition(body, "Index == 1", document: 999);
		Assert.False(unknown.GetProperty("Checked").GetBoolean());
		Assert.Empty(unknown.GetProperty("Scope").EnumerateArray());
		Assert.Equal(("InvalidRequest", "Condition: a condition has at most 1024 bytes"),
			session.Refused($$"""{ "Command": "CheckCondition", "Document": "100", "Node": {{body}}, "Condition": "{{new string('1', 1025)}}" }"""));
	}

	// A library has no variables: a condition on a library's node reads its loops, locals and parameters, never the variables
	// of the graph that calls it, so it compiles alike for every caller (spec-slice4 section 2.3, step 4).
	[Fact]
	public void AConditionOnALibraryNodeReadsNoCallersVariables()
	{
		MotionLibrary library = new();
		DocumentBuilder document = new();
		document.Variable("Health", "float", "10.0");
		GraphBuilder graph = document.Graph;
		uint call = graph.Node("Function.Call", CrossGraphTesting.Call(library.Report, CrossGraphTesting.Motion), """{ "Text": "hello" }""");
		graph.Flow(graph.Node("Event.Create"), call);
		TestRuntime runtime = new();
		runtime.LoadWorking(document.Build(), references: library.Bundle());
		Assert.Equal(VisualScriptStatus.Success, runtime.Create());
		DebugSession session = new(runtime);
		uint write = library.Report.Entry + 1;

		JsonElement reply = session.CheckCondition(write, "Health > 0.0", CrossGraphTesting.Motion);
		Assert.True(reply.GetProperty("Checked").GetBoolean());
		Assert.StartsWith("line 1, column 1: unknown name Health", DebugSession.Text(reply, "Error"), StringComparison.Ordinal);
		Assert.Equal(["Text"], reply.GetProperty("Scope").EnumerateArray().Select(static name => DebugSession.Text(name, "Name")));
		session.Configure([session.Breakpoint(write, CrossGraphTesting.Motion, condition: "Text == \"hello\"")]);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Single(session.Stops());
		Assert.Equal(JsonValueKind.Null, BreakpointOf(session, write).GetProperty("Error").ValueKind);
	}

	// Evaluate: formulas read-only against a stop's frame, in the scope of its node; any result type, or the formula's error.
	[Fact]
	public void FormulasAreEvaluatedAgainstASnapshot()
	{
		(TestRuntime runtime, DebugSession session, uint body, _) = Looping();
		session.Configure([session.Breakpoint(body, condition: "Index == 3")]);
		runtime.Invoke(ScriptEvent.Create);
		DebugSnapshotMember member = session.Debugger.Stops[0].Leaf;
		// The live values change; the snapshot does not.
		runtime.Runtime.SetVariable(TestRuntime.Hero, "Counter", typeof(int), 100);

		(Slot value, VisualType? type, string? error) = BreakpointConditions.Evaluate("Index * 2 + Counter", member, 0);
		Assert.Null(error);
		Assert.Equal("int", type!.Id);
		Assert.Equal(16, value.Int);
		(value, type, _) = BreakpointConditions.Evaluate("Index > 2", member, 0);
		Assert.Equal("bool", type!.Id);
		Assert.True(value.Bool);
		(_, type, error) = BreakpointConditions.Evaluate("Counter / 0", member, 0);
		Assert.Null(type);
		Assert.Equal("integer division by zero in Counter / 0", error);
		(_, _, error) = BreakpointConditions.Evaluate("Missing + 1", member, 0);
		Assert.Equal("line 1, column 1: unknown name Missing", error);
		session.Continue();
		Assert.Equal(6, runtime.Records.Count);
	}
}
