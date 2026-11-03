using System;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The VisualScriptDebug protocol (spec-slice4 section 7.8): Attach, Detach, Configure, State, Resume, Break, RunTo and
// CheckCondition with their requests and replies, and the error replies of requests it refuses, which change nothing.
public sealed class DebugProtocolTests
{
	private static TestRuntime StoppingRuntime(out uint write)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		write = graph.Write("update");
		graph.Flow(graph.Node("Event.Update"), write);
		return TestRuntime.Start(document);
	}

	private static string Reply(TestRuntime runtime, string request) => Encoding.UTF8.GetString(runtime.Runtime.Debug(Encoding.UTF8.GetBytes(request)).Text!);

	private static (string Code, string Error) ErrorOf(string reply)
	{
		using JsonDocument document = JsonDocument.Parse(reply);
		JsonElement root = document.RootElement;
		Assert.Equal(["Error", "Code"], root.EnumerateObject().Select(static member => member.Name));
		return (root.GetProperty("Code").GetString()!, root.GetProperty("Error").GetString()!);
	}

	[Fact]
	public void AttachIsIdempotentAndDetachReleasesTheStoppedExecutions()
	{
		TestRuntime runtime = StoppingRuntime(out uint write);
		DebugSession session = new(runtime);
		VisualScriptDebugger debugger = session.Debugger;
		Assert.Equal("{}", session.Send("""{ "Command": "Attach" }"""));
		Assert.Same(debugger, session.Debugger);
		session.Break(write);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Single(runtime.Instance().DebugFibers);

		Assert.Equal("""{"Released":1}""", session.Send("""{ "Command": "Detach" }"""));
		Assert.Null(runtime.Runtime.Debugger);
		Assert.Empty(runtime.Instance().DebugFibers);
		Assert.Empty(runtime.Records);
		Assert.Equal("""{"Released":0}""", session.Send("""{ "Command": "Detach" }"""));
		// Executions run untraced again; a new Attach starts with no configuration.
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal(["update"], runtime.Records);
		Assert.Equal("{}", session.Send("""{ "Command": "Attach" }"""));
		Assert.NotSame(debugger, session.Debugger);
		Assert.Empty(session.State().GetProperty("Breakpoints").EnumerateArray());
	}

	[Theory]
	[InlineData("""{ "Command": "State" }""")]
	[InlineData("""{ "Command": "Resume", "Action": "Continue" }""")]
	[InlineData("""{ "Command": "Configure", "Revision": 1, "Breakpoints": [], "Targets": [], "Watches": [], "BreakOnErrors": false }""")]
	[InlineData("""{ "Command": "Resume", "Action": "StepInto" }""")]
	[InlineData("""{ "Command": "Break", "Cancel": false }""")]
	[InlineData("""{ "Command": "RunTo", "Document": "100", "Node": 1 }""")]
	[InlineData("""{ "Command": "Describe", "Stop": 1, "Frame": 0 }""")]
	[InlineData("""{ "Command": "Record", "Frame": 1 }""")]
	[InlineData("""{ "Command": "Record", "Stop": true }""")]
	[InlineData("""{ "Command": "Record", "Start": { "Entity": null, "Documents": null, "Nodes": null, "Values": true, "Summary": true, "MaxSteps": 500, "MaxValues": 500 } }""")]
	public void CommandsOfTheDebuggerNeedOneAttached(string request)
	{
		TestRuntime runtime = StoppingRuntime(out _);
		Assert.Equal(("NotAttached", "no debugger is attached"), ErrorOf(Reply(runtime, request)));
	}

	[Fact]
	public void TheStateReplyHasEveryMemberOfItsForm()
	{
		TestRuntime runtime = StoppingRuntime(out uint write);
		DebugSession session = new(runtime);
		session.Configure([session.Breakpoint(write)]);
		runtime.Invoke(ScriptEvent.Update);

		JsonElement state = session.State();
		Assert.Equal(["Revision", "StopRevision", "Current", "Passed", "Stepping", "Stops", "Breakpoints"], state.EnumerateObject().Select(static member => member.Name));
		Assert.Equal(1u, state.GetProperty("Revision").GetUInt32());
		Assert.Equal(1UL, state.GetProperty("StopRevision").GetUInt64());
		Assert.Equal(JsonValueKind.Null, state.GetProperty("Stepping").ValueKind);
		JsonElement stop = state.GetProperty("Stops")[0];
		Assert.Equal(
			["ID", "Reason", "Resumable", "Entity", "Graph", "Place", "Message", "Migrated", "Changed", "ChangedMore", "Breakpoint", "Frames", "Start"],
			stop.EnumerateObject().Select(static member => member.Name));
		Assert.Equal(1, stop.GetProperty("ID").GetInt32());
		Assert.Equal(["Document", "Node"], stop.GetProperty("Breakpoint").EnumerateObject().Select(static member => member.Name));
		Assert.Equal(["Kind", "Entity", "Graph", "Document", "Function", "FunctionName", "Node", "Title", "Pin", "Note"],
			stop.GetProperty("Frames")[0].EnumerateObject().Select(static member => member.Name));
		Assert.Equal("Execute", stop.GetProperty("Frames")[0].GetProperty("Pin").GetString());
		Assert.Equal(["Kind", "Graph", "Document", "Node", "Title", "Pin"], stop.GetProperty("Start").EnumerateObject().Select(static member => member.Name));
		Assert.Equal(["Document", "Node", "Hits", "Reached", "Active", "Reason", "Error"],
			state.GetProperty("Breakpoints")[0].EnumerateObject().Select(static member => member.Name));

		JsonElement resumed = session.Continue(1);
		Assert.Equal(["Outcome", "Failed", "Stop", "StopRevision"], resumed.EnumerateObject().Select(static member => member.Name));
		Assert.Equal(2UL, resumed.GetProperty("StopRevision").GetUInt64());
	}

	// A step's reply has Continue's form; while it waits, State's Stepping names its stop, its action and where it waits. Break
	// and RunTo answer {}; CheckCondition answers whether it checked, the error, what the condition reads and the scope.
	[Fact]
	public void StepsBreakRunToAndCheckConditionAnswerTheirForms()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 1.0 }""");
		graph.Flow(graph.Node("Event.Update"), delay);
		graph.Writes(delay, "Completed", "after");
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Break(delay);
		runtime.Invoke(ScriptEvent.Update);

		JsonElement waiting = session.Resume("StepOver");
		Assert.Equal(["Outcome", "Failed", "Stop", "StopRevision"], waiting.EnumerateObject().Select(static member => member.Name));
		Assert.Equal("Waiting", waiting.GetProperty("Outcome").GetString());
		JsonElement stepping = session.State().GetProperty("Stepping");
		Assert.Equal(["Stop", "Action", "WaitsIn"], stepping.EnumerateObject().Select(static member => member.Name));
		Assert.Equal(["Document", "Function", "Node", "Title", "Entity"], stepping.GetProperty("WaitsIn").EnumerateObject().Select(static member => member.Name));
		Assert.Equal("{}", session.Send("""{ "Command": "Break", "Cancel": true }"""));
		Assert.Equal("{}", session.Send("""{ "Command": "Break", "Cancel": false, "Document": "100" }"""));
		Assert.Equal("{}", session.Send(FormattableString.Invariant($$"""{ "Command": "RunTo", "Document": "100", "Node": {{delay}} }""")));
		JsonElement checkedReply = session.CheckCondition(delay, "Duration > 0.5");
		Assert.Equal(["Checked", "Error", "Reads", "Scope"], checkedReply.EnumerateObject().Select(static member => member.Name));
		Assert.Equal("unknown name Duration", checkedReply.GetProperty("Error").GetString());
		JsonElement scoped = session.CheckCondition(delay, "DeltaTime > 0.5");
		Assert.Equal(JsonValueKind.Null, scoped.GetProperty("Error").ValueKind);
		Assert.Equal(["Name", "Form", "Type", "Source", "Node"], scoped.GetProperty("Scope")[0].EnumerateObject().Select(static member => member.Name));
	}

	// A read-only stop does not step: Continue dismisses it.
	[Fact]
	public void AStepOfAReadOnlyStopIsRefused()
	{
		DocumentBuilder document = new();
		uint divisor = document.Variable("Divisor", "int", "0");
		GraphBuilder graph = document.Graph;
		uint divide = graph.Node("Operator.Divide", inputs: """{ "A": 10 }""");
		graph.Link(graph.Get(divisor), "Value", divide, "B");
		graph.Flow(graph.Node("Event.Update"), graph.Write(divide, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Configure(breakOnErrors: true);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal("Error", session.SingleStop().GetProperty("Reason").GetString());

		Assert.Equal(("InvalidState", "Stop: stop 1 is read-only (Error): it does not step, and Continue dismisses it"),
			ErrorOf(session.Send("""{ "Command": "Resume", "Action": "StepInto" }""")));
		Assert.Equal("Dismissed", session.Continue().GetProperty("Outcome").GetString());
	}

	// Every refusal names the member at fault and changes nothing.
	[Theory]
	[InlineData("""[]""", "the request is not a JSON object")]
	[InlineData("""{ }""", "Command: missing")]
	[InlineData("""{ "Command": 3 }""", "Command: not a string")]
	[InlineData("""{ "Command": "Wait" }""", "Command: unknown command \"Wait\"")]
	[InlineData("""{ "Command": "State", "Extra": 1 }""", "Extra: unknown member")]
	[InlineData("""{ "Command": "State", "Command": "State" }""", "Command: a second member of the name")]
	[InlineData("""{ "Command": "Configure", "Breakpoints": [], "Targets": [], "Watches": [], "BreakOnErrors": false }""", "Revision: missing")]
	[InlineData("""{ "Command": "Configure", "Revision": -1, "Breakpoints": [], "Targets": [], "Watches": [], "BreakOnErrors": false }""", "Revision: not an unsigned 32-bit integer")]
	[InlineData("""{ "Command": "Configure", "Revision": 1, "Breakpoints": {}, "Targets": [], "Watches": [], "BreakOnErrors": false }""", "Breakpoints: not an array")]
	[InlineData("""{ "Command": "Configure", "Revision": 1, "Breakpoints": [3], "Targets": [], "Watches": [], "BreakOnErrors": false }""", "Breakpoints[0]: not an object")]
	[InlineData("""{ "Command": "Configure", "Revision": 1, "Breakpoints": [{ "Document": 100, "Node": 1, "Type": "T", "Enabled": true, "Condition": "", "HitCount": 0 }], "Targets": [], "Watches": [], "BreakOnErrors": false }""", "Breakpoints[0].Document: not a decimal string of an unsigned 64-bit integer")]
	[InlineData("""{ "Command": "Configure", "Revision": 1, "Breakpoints": [{ "Document": "1x", "Node": 1, "Type": "T", "Enabled": true, "Condition": "", "HitCount": 0 }], "Targets": [], "Watches": [], "BreakOnErrors": false }""", "Breakpoints[0].Document: not a decimal string of an unsigned 64-bit integer")]
	[InlineData("""{ "Command": "Configure", "Revision": 1, "Breakpoints": [{ "Document": "100", "Node": 1, "Type": "T", "Enabled": true, "HitCount": 0 }], "Targets": [], "Watches": [], "BreakOnErrors": false }""", "Breakpoints[0].Condition: missing")]
	[InlineData("""{ "Command": "Configure", "Revision": 1, "Breakpoints": [{ "Document": "100", "Node": 1, "Type": "T", "Enabled": true, "Condition": "", "HitCount": 0 }, { "Document": "100", "Node": 1, "Type": "U", "Enabled": true, "Condition": "", "HitCount": 0 }], "Targets": [], "Watches": [], "BreakOnErrors": false }""", "Breakpoints[1]: a second breakpoint of the node")]
	[InlineData("""{ "Command": "Configure", "Revision": 1, "Breakpoints": [], "Targets": [{ "Graph": "100", "Entity": "7" }, { "Graph": "100", "Entity": "8" }], "Watches": [], "BreakOnErrors": false }""", "Targets[1]: a second target of the graph")]
	[InlineData("""{ "Command": "Configure", "Revision": 1, "Breakpoints": [], "Targets": [], "Watches": [{ "Document": "100" }], "BreakOnErrors": false }""", "Watches[0].Function: missing")]
	[InlineData("""{ "Command": "Configure", "Revision": 1, "Breakpoints": [], "Targets": [], "Watches": [], "BreakOnErrors": 1 }""", "BreakOnErrors: not a boolean")]
	[InlineData("""{ "Command": "Resume" }""", "Action: missing")]
	[InlineData("""{ "Command": "Resume", "Action": "Jump" }""", "Action: unknown action \"Jump\"")]
	[InlineData("""{ "Command": "Resume", "Action": "StepOver", "All": true }""", "All: only Continue resumes every stop; a step resumes one")]
	[InlineData("""{ "Command": "Resume", "Action": "1" }""", "Action: unknown action \"1\"")]
	[InlineData("""{ "Command": "Break" }""", "Cancel: missing")]
	[InlineData("""{ "Command": "Break", "Cancel": true, "Document": "100" }""", "Document: a cancel names no graph")]
	[InlineData("""{ "Command": "Break", "Cancel": false, "Document": "0" }""", "Document: 0 is no graph: leave Document out to break in any graph")]
	[InlineData("""{ "Command": "Break", "Cancel": false, "Graph": "100" }""", "Graph: unknown member")]
	[InlineData("""{ "Command": "RunTo", "Document": "100" }""", "Node: missing")]
	[InlineData("""{ "Command": "RunTo", "Document": 100, "Node": 1 }""", "Document: not a decimal string of an unsigned 64-bit integer")]
	[InlineData("""{ "Command": "CheckCondition", "Document": "100", "Node": 1 }""", "Condition: missing")]
	[InlineData("""{ "Command": "Resume", "Action": "Continue", "All": true, "Stop": 1 }""", "Stop: All continues every stop")]
	[InlineData("""{ "Command": "Resume", "Action": "Continue", "Stop": 0 }""", "Stop: not a stop ID")]
	[InlineData("""{ "Command": "\uD800" }""", "Command: not valid Unicode text")]
	[InlineData("""{ "Command": "State", "\uDC00": 1 }""", "a member's name: not valid Unicode text")]
	[InlineData("""{ "\uDC00": 1, "Command": "State" }""", "a member's name: not valid Unicode text")]
	[InlineData("""{ "Command": "Record", "Start": { "\uDC00": 1 } }""", "Start: a member's name: not valid Unicode text")]
	public void MalformedRequestsAnswerInvalidRequest(string request, string error)
	{
		TestRuntime runtime = StoppingRuntime(out uint write);
		DebugSession session = new(runtime);
		session.Configure([session.Breakpoint(write)]);
		runtime.Invoke(ScriptEvent.Update);
		string before = session.Send("""{ "Command": "State" }""");

		Assert.Equal(("InvalidRequest", error), ErrorOf(session.Send(request)));
		Assert.Equal(before, session.Send("""{ "Command": "State" }"""));
	}

	// Bytes that are no JSON or no UTF-8 text are refused before anything is read.
	[Fact]
	public void RequestsThatAreNotJsonTextAnswerInvalidRequest()
	{
		TestRuntime runtime = StoppingRuntime(out _);
		foreach (byte[] request in new[]
		{
			Encoding.UTF8.GetBytes("{ \"Command\": "),
			[],
			Encoding.UTF8.GetBytes(new string('[', 65) + new string(']', 65)),
		})
		{
			HostReply reply = runtime.Runtime.Debug(request);
			Assert.Equal(VisualScriptStatus.Success, reply.Status);
			(string code, string error) = ErrorOf(Encoding.UTF8.GetString(reply.Text!));
			Assert.Equal("InvalidRequest", code);
			Assert.StartsWith("the request is not JSON: ", error, System.StringComparison.Ordinal);
		}
		byte[] bytes = [.. "{ \"Command\": \""u8, 0xFF, 0xFE, .. "\" }"u8];
		Assert.Equal(("InvalidRequest", "the request is not UTF-8 text"), ErrorOf(Encoding.UTF8.GetString(runtime.Runtime.Debug(bytes).Text!)));
	}

	[Fact]
	public void ResumeAnswersNotFoundWithoutTheStop()
	{
		TestRuntime runtime = StoppingRuntime(out uint write);
		DebugSession session = new(runtime);
		Assert.Equal(("NotFound", "no execution is stopped"), ErrorOf(session.Send("""{ "Command": "Resume", "Action": "Continue" }""")));
		session.Configure([session.Breakpoint(write)]);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal(("NotFound", "Stop: no stop of the ID"), ErrorOf(session.Send("""{ "Command": "Resume", "Action": "Continue", "Stop": 2 }""")));
		Assert.Single(session.Stops());
		// All without stops continues nothing.
		session.ContinueAll();
		JsonElement nothing = session.ContinueAll();
		Assert.Equal("Dismissed", nothing.GetProperty("Outcome").GetString());
		Assert.Equal(JsonValueKind.Null, nothing.GetProperty("Stop").ValueKind);
	}

	// A resume that reaches another stop answers it and makes it current.
	[Fact]
	public void ResumeAnswersTheStopItReached()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint first = graph.Write("first");
		uint second = graph.Write("second");
		graph.Flow(graph.Node("Event.Update"), first);
		graph.Flow(first, second);
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Environment.Add(8, "Villain");
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(8));
		DebugSession session = new(runtime);
		session.Break(first, second);
		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update, entity: 8);
		Assert.Equal(1, session.State().GetProperty("Current").GetInt32());

		JsonElement resumed = session.Continue(2);
		Assert.Equal("Stopped", resumed.GetProperty("Outcome").GetString());
		Assert.Equal(3, resumed.GetProperty("Stop").GetInt32());
		Assert.Equal(3, session.State().GetProperty("Current").GetInt32());
		Assert.Equal([1, 3], session.Stops().Select(static stop => stop.GetProperty("ID").GetInt32()));
		JsonElement all = session.ContinueAll();
		Assert.Equal("Stopped", all.GetProperty("Outcome").GetString());
		Assert.Equal(4, all.GetProperty("Stop").GetInt32());
		Assert.Equal("Ended", session.ContinueAll().GetProperty("Outcome").GetString());
		Assert.Equal(["first", "first", "second", "second"], runtime.Records);
	}

	// Hurt(Amount) keeps Amount in its local Left and writes "hurt"; Update calls it for each Index of 0 to 2; Health is 10.
	private static (TestRuntime Runtime, DebugSession Session, uint Loop, uint Call, uint Hurt, FunctionBuilder Function) StoppedInAFunction()
	{
		DocumentBuilder document = new();
		document.Variable("Health", "int", "10", isPublic: true);
		FunctionBuilder function = document.Function("Hurt", inputs: ["Amount:int"]);
		uint left = function.Local("Left", "int");
		uint keep = function.Graph.Node("Variable.Set", FormattableString.Invariant($"{{ \"Variable\": {left} }}"));
		function.Graph.Link(function.Entry, "Amount", keep, "Value");
		uint hurt = function.Graph.Write("hurt");
		function.Graph.Flow(function.Entry, keep);
		function.Graph.Flow(keep, hurt);
		GraphBuilder graph = document.Graph;
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 0, "Last": 2 }""");
		uint call = graph.Node("Function.Call", function.Settings);
		graph.Link(loop, "Index", call, "Amount");
		graph.Flow(graph.Node("Event.Update"), loop);
		graph.Flow(loop, call, "Body");
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Configure([session.Breakpoint(hurt, hitCount: 2)]);
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		return (runtime, session, loop, call, hurt, function);
	}

	private static string[] Members(JsonElement element) => [.. element.EnumerateObject().Select(static member => member.Name)];

	// Describe answers a frame's sections; frame 0 is the function's frame, frame 1 the event graph's below it, where the loop
	// is; formulas evaluate in each frame's scope.
	[Fact]
	public void DescribeAnswersEveryMemberOfItsForm()
	{
		(TestRuntime runtime, DebugSession session, uint loop, uint call, uint hurt, FunctionBuilder function) = StoppedInAFunction();
		Assert.Equal(["hurt"], runtime.Records);

		JsonElement inner = session.Describe(1, 0, "Amount + Left", "Amount > 0", "Missing");
		Assert.Equal(["Stop", "Frame", "Entity", "Graph", "Document", "Function", "Node", "Event", "Parameters", "Locals", "Variables", "Loops", "Computed",
			"Evaluated", "Truncated"], Members(inner));
		Assert.Equal(1, inner.GetProperty("Stop").GetInt32());
		Assert.Equal("7", DebugSession.Text(inner, "Entity"));
		Assert.Equal("100", DebugSession.Text(inner, "Document"));
		Assert.Equal(function.Id, inner.GetProperty("Function").GetUInt32());
		Assert.Equal(["ID", "Inputs", "Outputs"], Members(inner.GetProperty("Node")));
		Assert.Equal(hurt, inner.GetProperty("Node").GetProperty("ID").GetUInt32());
		Assert.Equal("""[{"Name":"Text","Type":"string","Value":"hurt","Currency":"Current"}]""", inner.GetProperty("Node").GetProperty("Inputs").GetRawText());
		Assert.Equal("""[{"Name":"Amount","Type":"int","Value":1,"Currency":"Current"}]""", inner.GetProperty("Parameters").GetRawText());
		Assert.Equal("""[{"Name":"Left","Type":"int","Value":1,"Currency":"Current"}]""", inner.GetProperty("Locals").GetRawText());
		Assert.Equal("""[{"Name":"Health","Type":"int","Value":10,"Currency":"Current","ID":1,"Public":true}]""", inner.GetProperty("Variables").GetRawText());
		Assert.Empty(inner.GetProperty("Loops").EnumerateArray());
		Assert.Equal(
			"""[{"Formula":"Amount + Left","Value":{"Type":"int","Value":2}},{"Formula":"Amount > 0","Value":{"Type":"bool","Value":true}},{"Formula":"Missing","Error":"unknown name Missing"}]""",
			inner.GetProperty("Evaluated").GetRawText());
		Assert.Equal(0, inner.GetProperty("Truncated").GetInt32());

		JsonElement outer = session.Describe(1, 1, "Index * 10 + Health");
		Assert.Equal(0u, outer.GetProperty("Function").GetUInt32());
		Assert.Equal(call, outer.GetProperty("Node").GetProperty("ID").GetUInt32());
		Assert.Equal("""[{"Name":"Amount","Type":"int","Value":1,"Currency":"Current"}]""", outer.GetProperty("Node").GetProperty("Inputs").GetRawText());
		Assert.Equal(FormattableString.Invariant($$"""[{"Node":{{loop}},"Kind":"ForLoop","Index":1,"First":0,"Last":2}]"""), outer.GetProperty("Loops").GetRawText());
		Assert.Equal("DeltaTime", DebugSession.Text(outer.GetProperty("Event")[0], "Name"));
		Assert.Empty(outer.GetProperty("Parameters").EnumerateArray());
		Assert.Equal("""{"Type":"int","Value":20}""", outer.GetProperty("Evaluated")[0].GetProperty("Value").GetRawText());
		// The hit count stops every hit from the second on.
		Assert.Equal("Stopped", DebugSession.Text(session.Continue(), "Outcome"));
		Assert.Equal("Ended", DebugSession.Text(session.Continue(), "Outcome"));
		Assert.Equal(["hurt", "hurt", "hurt"], runtime.Records);
	}

	// A Sequence's entry says its next output; a ForEach's its index, length and element.
	[Fact]
	public void DescribeShowsEveryActiveLoopAndSequence()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint sequence = graph.Node("Flow.Sequence", """{ "Count": 2 }""");
		uint each = graph.Node("Flow.ForEach");
		graph.Link(graph.Node("Call:Strada.ScriptCore.Tests.Probe.Numbers(int)", inputs: """{ "Count": 3 }"""), "Result", each, "Array");
		uint element = graph.Write(each, "Element");
		graph.Flow(graph.Node("Event.Update"), sequence);
		graph.Flow(sequence, each, "Then0");
		graph.Flow(each, element, "Body");
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		session.Configure([session.Breakpoint(element, hitCount: 2)]);
		runtime.Invoke(ScriptEvent.Update);

		JsonElement described = session.Describe(1);
		Assert.Equal(FormattableString.Invariant(
			$$$"""[{"Node":{{{sequence}}},"Kind":"Sequence","Next":"Then1"},{"Node":{{{each}}},"Kind":"ForEach","Index":1,"Length":3,"Element":{"Name":"Element","Type":"int","Value":1}}]"""),
			described.GetProperty("Loops").GetRawText());
		// The write's input comes from the loop's element at the Enter position, converted as the node will load it.
		Assert.Equal("""[{"Name":"Text","Type":"string","Value":"1","Currency":"Current"}]""", described.GetProperty("Node").GetProperty("Inputs").GetRawText());
	}

	// Describe refuses a stop that does not exist, a frame beyond the stop's, a native boundary, and Evaluate's excess.
	[Fact]
	public void DescribeRefusesWhatItCannotDescribe()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		graph.Flow(graph.Node("Event.Update"), graph.Node("Call:Strada.ScriptCore.Tests.Probe.Reenter(ulong,int)", inputs: """{ "Entity": "8", "ScriptEvent": 0 }"""));
		uint created = graph.Write("created");
		graph.Flow(graph.Node("Event.Create"), created);
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Environment.Add(8, "Villain");
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(8));
		Probe.Runtime = runtime.Runtime;
		DebugSession session = new(runtime);
		session.Break(created);
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal("Passed", DebugSession.Text(session.SingleStop(), "Reason"));

		Assert.Equal("8", DebugSession.Text(session.Describe(1), "Entity"));
		Assert.Equal("7", DebugSession.Text(session.Describe(1, 2), "Entity"));
		(string code, string error) = session.Refused("""{ "Command": "Describe", "Stop": 1, "Frame": 1 }""");
		Assert.Equal("InvalidState", code);
		Assert.StartsWith("Frame: frame 1 is engine code (", error, StringComparison.Ordinal);
		Assert.EndsWith("): it holds no values", error, StringComparison.Ordinal);
		Assert.Equal(("NotFound", "Frame: stop 1 has 3 frames"), session.Refused("""{ "Command": "Describe", "Stop": 1, "Frame": 3 }"""));
		Assert.Equal(("NotFound", "Stop: no stop of the ID"), session.Refused("""{ "Command": "Describe", "Stop": 2, "Frame": 0 }"""));
		string seventeen = string.Join(", ", Enumerable.Repeat("\"1\"", 17));
		Assert.Equal(("InvalidRequest", "Evaluate: at most 16 formulas"),
			session.Refused($$"""{ "Command": "Describe", "Stop": 1, "Frame": 0, "Evaluate": [{{seventeen}}] }"""));
		Assert.Equal(("InvalidRequest", "Evaluate[0]: a formula has at most 1024 bytes"),
			session.Refused($$"""{ "Command": "Describe", "Stop": 1, "Frame": 0, "Evaluate": ["{{new string('1', 1025)}}"] }"""));
		Assert.Equal(("InvalidRequest", "Evaluate[0]: not a string"), session.Refused("""{ "Command": "Describe", "Stop": 1, "Frame": 0, "Evaluate": [1] }"""));
		Assert.Equal(("InvalidRequest", "Frame: missing"), session.Refused("""{ "Command": "Describe", "Stop": 1 }"""));
	}

	// Instances lists who runs which graph, in the order they were created, a page at a time: each one's state, what its
	// fibers wait for and its stops. It needs no debugger.
	[Fact]
	public void InstancesListWhoRunsWhatAndWhatItWaitsFor()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 2.0 }""");
		graph.Flow(graph.Node("Event.Create"), delay);
		uint update = graph.Write("update");
		graph.Flow(graph.Node("Event.Update"), update);
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Environment.Add(8, "Villain");
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(8));
		DocumentBuilder waiter = new();
		uint frames = waiter.Graph.Node("Flow.WaitFrames", inputs: """{ "Frames": 3 }""");
		uint create = waiter.Graph.Node("Event.Create");
		waiter.Graph.Flow(create, frames);
		uint interval = waiter.Graph.Node("Flow.Interval", inputs: """{ "Interval": 0.5, "Count": 4 }""");
		uint sequence = waiter.Graph.Node("Flow.Sequence", """{ "Count": 2 }""");
		waiter.Graph.Flow(waiter.Graph.Node("Event.FixedUpdate"), sequence);
		waiter.Graph.Flow(sequence, interval, "Then0", "Start");
		runtime.LoadWorking(waiter.Build(), 200);
		runtime.Environment.Add(9, "Waiter");
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(9, 200));
		runtime.Invoke(ScriptEvent.Create);
		runtime.Invoke(ScriptEvent.Update, 0.5f);
		runtime.Invoke(ScriptEvent.Create, entity: 9);
		runtime.Invoke(ScriptEvent.FixedUpdate, entity: 9);
		runtime.Invoke(ScriptEvent.Update, 0.75f, entity: 9);
		DebugSession session = new(runtime);
		session.Break(update);
		runtime.Invoke(ScriptEvent.Update, entity: 8);

		JsonElement all = session.Expect("""{ "Command": "Instances" }""");
		Assert.Equal(["Total", "Instances"], Members(all));
		Assert.Equal(3, all.GetProperty("Total").GetInt32());
		JsonElement[] instances = [.. all.GetProperty("Instances").EnumerateArray()];
		Assert.Equal(["7", "8", "9"], instances.Select(static instance => DebugSession.Text(instance, "Entity")));
		Assert.Equal(["Entity", "Graph", "State", "Waits", "Stops"], Members(instances[0]));
		Assert.Equal(["Waiting", "Stopped", "Waiting"], instances.Select(static instance => DebugSession.Text(instance, "State")));
		Assert.Equal(FormattableString.Invariant(
			$$"""[{"Document":"100","Function":0,"Node":{{delay}},"Title":"Delay","Kind":"Delay","Remaining":1.5,"Frames":null,"Index":null}]"""),
			instances[0].GetProperty("Waits").GetRawText());
		Assert.Equal("[1]", instances[1].GetProperty("Stops").GetRawText());
		Assert.Equal(FormattableString.Invariant(
			$$"""[{"Document":"200","Function":0,"Node":{{frames}},"Title":"Wait Frames","Kind":"WaitFrames","Remaining":null,"Frames":2,"Index":null},{"Document":"200","Function":0,"Node":{{interval}},"Title":"Interval","Kind":"Interval","Remaining":0.25,"Frames":null,"Index":1}]"""),
			instances[2].GetProperty("Waits").GetRawText());

		JsonElement page = session.Expect("""{ "Command": "Instances", "Offset": 1, "Limit": 1 }""");
		Assert.Equal(3, page.GetProperty("Total").GetInt32());
		Assert.Equal("8", DebugSession.Text(Assert.Single(page.GetProperty("Instances").EnumerateArray()), "Entity"));
		Assert.Equal(1, session.Expect("""{ "Command": "Instances", "Graph": "200" }""").GetProperty("Total").GetInt32());
		Assert.Equal(0, session.Expect("""{ "Command": "Instances", "Entity": "99" }""").GetProperty("Total").GetInt32());
		Assert.Equal(("InvalidRequest", "Limit: 1 to 500 instances"), session.Refused("""{ "Command": "Instances", "Limit": 501 }"""));
		Assert.Equal(("InvalidRequest", "Limit: 1 to 500 instances"), session.Refused("""{ "Command": "Instances", "Limit": 0 }"""));
		session.Send("""{ "Command": "Detach" }""");
		Assert.Equal("[]", session.Expect("""{ "Command": "Instances", "Entity": "8" }""").GetProperty("Instances")[0].GetProperty("Stops").GetRawText());
	}

	// Record takes one of Start, Frame and Stop: Start's members are all required, the caps within their range, Nodes only with
	// Documents; Frame and Stop need a recording. Values and Instances refuse their malformed members too.
	[Theory]
	[InlineData("""{ "Command": "Record" }""", "InvalidRequest", "Record: exactly one of Start, Frame and Stop")]
	[InlineData("""{ "Command": "Record", "Frame": 1, "Stop": true }""", "InvalidRequest", "Record: exactly one of Start, Frame and Stop")]
	[InlineData("""{ "Command": "Record", "Stop": false }""", "InvalidRequest", "Stop: only true stops the recording")]
	[InlineData("""{ "Command": "Record", "Frame": -1 }""", "InvalidRequest", "Frame: not an unsigned 32-bit integer")]
	[InlineData("""{ "Command": "Record", "Frame": 2 }""", "InvalidState", "no recording runs: Record Start begins one")]
	[InlineData("""{ "Command": "Record", "Start": [] }""", "InvalidRequest", "Start: not an object")]
	[InlineData("""{ "Command": "Record", "Start": { "Entity": null, "Documents": null, "Nodes": null, "Values": true, "Summary": true, "MaxSteps": 500 } }""", "InvalidRequest", "Start.MaxValues: missing")]
	[InlineData("""{ "Command": "Record", "Start": { "Entity": null, "Documents": null, "Nodes": [3], "Values": true, "Summary": true, "MaxSteps": 500, "MaxValues": 500 } }""", "InvalidRequest", "Start.Nodes: node IDs repeat across documents: name the Documents")]
	[InlineData("""{ "Command": "Record", "Start": { "Entity": null, "Documents": ["x"], "Nodes": null, "Values": true, "Summary": true, "MaxSteps": 500, "MaxValues": 500 } }""", "InvalidRequest", "Start.Documents[0]: not a decimal string of an unsigned 64-bit integer")]
	[InlineData("""{ "Command": "Record", "Start": { "Entity": 7, "Documents": null, "Nodes": null, "Values": true, "Summary": true, "MaxSteps": 500, "MaxValues": 500 } }""", "InvalidRequest", "Start.Entity: not a decimal string of an unsigned 64-bit integer")]
	[InlineData("""{ "Command": "Record", "Start": { "Entity": null, "Documents": null, "Nodes": null, "Values": true, "Summary": true, "MaxSteps": 0, "MaxValues": 500 } }""", "InvalidRequest", "Start.MaxSteps: 1 to 100000")]
	[InlineData("""{ "Command": "Record", "Start": { "Entity": null, "Documents": null, "Nodes": null, "Values": true, "Summary": true, "MaxSteps": 500, "MaxValues": 100001 } }""", "InvalidRequest", "Start.MaxValues: 1 to 100000")]
	[InlineData("""{ "Command": "Record", "Start": { "Entity": null, "Documents": null, "Nodes": null, "Values": 1, "Summary": true, "MaxSteps": 500, "MaxValues": 500 } }""", "InvalidRequest", "Start.Values: not a boolean")]
	[InlineData("""{ "Command": "Values" }""", "InvalidRequest", "Entity: missing")]
	[InlineData("""{ "Command": "Values", "Entity": "7", "Nodes": [-1] }""", "InvalidRequest", "Nodes[0]: not an unsigned 32-bit integer")]
	[InlineData("""{ "Command": "Instances", "Offset": "1" }""", "InvalidRequest", "Offset: not an unsigned 32-bit integer")]
	public void RecordValuesAndInstancesRefuseMalformedRequests(string request, string code, string error)
	{
		TestRuntime runtime = StoppingRuntime(out _);
		DebugSession session = new(runtime);
		Assert.Equal((code, error), ErrorOf(session.Send(request)));
		Assert.Null(session.Debugger.Recording);
	}

	// A recording runs from Start to Stop; a second Start replaces the first; Detach drops it.
	[Fact]
	public void ARecordingRunsFromStartToStop()
	{
		TestRuntime runtime = StoppingRuntime(out uint write);
		DebugSession session = new(runtime);
		Assert.Equal("{}", session.StartRecording(summary: true).GetRawText());
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal("{}", session.StartRecording().GetRawText());
		Assert.Equal("{}", session.RecordFrame(4).GetRawText());
		runtime.Invoke(ScriptEvent.Update);
		JsonElement recorded = session.StopRecording();
		JsonElement[] steps = [.. recorded.GetProperty("Steps").EnumerateArray()];
		Assert.Equal(2, steps.Length);
		Assert.All(steps, static step => Assert.Equal(4, step.GetProperty("Frame").GetInt32()));
		Assert.Equal(write, steps[1].GetProperty("Node").GetUInt32());
		Assert.Equal(["Step", "Node", "Pin", "Direction", "Type", "Value"], Members(recorded.GetProperty("Values")[0]));
		Assert.Null(session.Debugger.Recording);
		session.StartRecording();
		session.Send("""{ "Command": "Detach" }""");
		session.Send("""{ "Command": "Attach" }""");
		Assert.Null(session.Debugger.Recording);
	}

	// A RobustnessTests-style fuzz (spec-slice4 section 20.6): every command's request, damaged, against a debugger that holds
	// stops, a recording and fibers, never throws and always answers a JSON object; the debugger stays usable.
	[Fact]
	public void MutatedRequestsNeverThrow()
	{
		DocumentBuilder document = new();
		uint health = document.Variable("Health", "int", "3", isPublic: true);
		GraphBuilder graph = document.Graph;
		uint delay = graph.Node("Flow.Delay", inputs: """{ "Duration": 1.0 }""");
		uint loop = graph.Node("Flow.ForLoop", inputs: """{ "First": 0, "Last": 2 }""");
		uint write = graph.Write(loop, "Index");
		graph.Flow(graph.Node("Event.Update"), loop);
		graph.Flow(loop, write, "Body");
		graph.Flow(loop, delay, "Completed");
		TestRuntime runtime = TestRuntime.Start(document);
		DebugSession session = new(runtime);
		string[] seeds =
		[
			"""{ "Command": "Attach" }""",
			"""{ "Command": "State" }""",
			FormattableString.Invariant(
				$$"""{ "Command": "Configure", "Revision": 3, "Breakpoints": [{ "Document": "100", "Node": {{write}}, "Type": {{RuntimeTesting.Quote(session.TypeOf(write))}}, "Enabled": true, "Condition": "Index == 1", "HitCount": 0 }], "Targets": [{ "Graph": "100", "Entity": "7" }], "Watches": [{ "Document": "100", "Function": 0 }], "BreakOnErrors": true }"""),
			"""{ "Command": "Resume", "Action": "StepOver" }""",
			"""{ "Command": "Resume", "Action": "Continue", "All": true }""",
			"""{ "Command": "Break", "Cancel": false, "Document": "100" }""",
			FormattableString.Invariant($$"""{ "Command": "RunTo", "Document": "100", "Node": {{delay}} }"""),
			FormattableString.Invariant($$"""{ "Command": "CheckCondition", "Document": "100", "Node": {{write}}, "Condition": "Health > 2" }"""),
			"""{ "Command": "Describe", "Stop": 1, "Frame": 0, "Evaluate": ["Index + Health", "Health / 0"] }""",
			FormattableString.Invariant($$"""{ "Command": "Values", "Entity": "7", "Function": 0, "Nodes": [{{write}}, {{health}}], "Variables": true, "Timelines": true }"""),
			"""{ "Command": "Instances", "Graph": "100", "Entity": "7", "Offset": 0, "Limit": 10 }""",
			"""{ "Command": "Record", "Start": { "Entity": "7", "Documents": ["100"], "Nodes": [3, 4], "Values": true, "Summary": false, "MaxSteps": 50, "MaxValues": 50 } }""",
			"""{ "Command": "Record", "Frame": 2 }""",
			"""{ "Command": "Record", "Stop": true }""",
			"""{ "Command": "Detach" }""",
		];
		JsonNode[] originals = [.. seeds.Select(static seed => JsonNode.Parse(seed)!)];
		DocumentMutator mutator = new(11600);
		int rounds = Fuzzing.GetRounds(300);
		int answered = 0;
		for (int round = 0; round < rounds; round++)
		{
			if (round % 10 == 0)
			{
				// Keep stops, fibers and counts to read and resume.
				session.Send("""{ "Command": "Attach" }""");
				session.Send(seeds[2]);
				runtime.Invoke(ScriptEvent.Update, 0.25f);
			}
			// Every third request is sent whole, so the debugger's state moves on (stops, steps, recordings) between damaged ones.
			JsonNode original = originals[round % originals.Length];
			byte[] request = round % 3 == 0 ? Encoding.UTF8.GetBytes(original.ToJsonString()) : mutator.Write(mutator.Damage(original, round));
			HostReply reply = runtime.Runtime.Debug(request);
			Assert.Equal(VisualScriptStatus.Success, reply.Status);
			using JsonDocument parsed = JsonDocument.Parse(reply.Text!);
			Assert.Equal(JsonValueKind.Object, parsed.RootElement.ValueKind);
			answered += parsed.RootElement.TryGetProperty("Code", out _) ? 0 : 1;
			Assert.Contains(runtime.Runtime.TakeTrace().Status, new[] { VisualScriptStatus.Success, VisualScriptStatus.NotFound });
		}
		TestContext.Current.TestOutputHelper?.WriteLine(FormattableString.Invariant($"{rounds} rounds, {answered} answered without an error"));
		Assert.True(answered > rounds / 6, "too few requests were answered without an error");
		// The debugger still works: attach, continue everything, then run untraced.
		session.Send("""{ "Command": "Attach" }""");
		session.ContinueAll();
		session.Send("""{ "Command": "Detach" }""");
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Update));
	}
}
