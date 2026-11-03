using System;
using System.Linq;
using System.Text;
using System.Text.Json;
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
}
