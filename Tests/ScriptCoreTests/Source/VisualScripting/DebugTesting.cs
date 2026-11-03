using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Text.Json;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Debugger configurations made in the tests as Configure reads them.
internal static class DebugTesting
{
	// A configuration of the breakpoints, with no targets and no watches.
	internal static DebugConfiguration Configuration(IEnumerable<DebugBreakpointSetting> breakpoints)
	{
		string rows = string.Join(", ", breakpoints.Select(static breakpoint => FormattableString.Invariant(
			$$"""{ "Document": "{{breakpoint.Document}}", "Node": {{breakpoint.Node}}, "Type": {{RuntimeTesting.Quote(breakpoint.Type)}}, "Enabled": {{(breakpoint.Enabled ? "true" : "false")}}, "Condition": {{RuntimeTesting.Quote(breakpoint.Condition)}}, "HitCount": {{breakpoint.HitCount}} }""")));
		using JsonDocument request = JsonDocument.Parse(
			$$"""{ "Command": "Configure", "Revision": 1, "Breakpoints": [{{rows}}], "Targets": [], "Watches": [], "BreakOnErrors": false }""");
		return DebugConfiguration.Read(request.RootElement);
	}
}

// The editor's side of the debugger protocol (spec-slice4 section 7.8) for the debugger tests: a VisualScriptDebugger attached
// to a test runtime through Attach, configured, read and resumed with the protocol's own requests.
internal sealed class DebugSession
{
	private readonly TestRuntime m_Runtime;
	private uint m_Revision;

	internal DebugSession(TestRuntime runtime)
	{
		m_Runtime = runtime;
		Assert.Equal("{}", Send("""{ "Command": "Attach" }"""));
	}

	internal VisualScriptDebugger Debugger => Assert.IsType<VisualScriptDebugger>(m_Runtime.Runtime.Debugger);

	// The reply text of a request.
	internal string Send(string request)
	{
		HostReply reply = m_Runtime.Runtime.Debug(Encoding.UTF8.GetBytes(request));
		Assert.Equal(VisualScriptStatus.Success, reply.Status);
		return Encoding.UTF8.GetString(reply.Text!);
	}

	internal JsonElement Request(string request)
	{
		using JsonDocument document = JsonDocument.Parse(Send(request));
		return document.RootElement.Clone();
	}

	// A request whose reply must not be an error: error replies have a Code (CheckCondition's reply has an Error of its own).
	internal JsonElement Expect(string request)
	{
		JsonElement reply = Request(request);
		Assert.False(reply.TryGetProperty("Code", out _), $"{request} answered {reply}");
		return reply;
	}

	// The code and message of an error reply.
	internal (string Code, string Error) Refused(string request)
	{
		JsonElement reply = Request(request);
		Assert.True(reply.TryGetProperty("Code", out _), $"{request} answered {reply}");
		return (Text(reply, "Code"), Text(reply, "Error"));
	}

	// A breakpoint of the Breakpoints array: the node of the document, with the Type its compiled node has in the programs the
	// instances run.
	internal string Breakpoint(uint node, ulong document = TestRuntime.Graph, bool enabled = true, uint hitCount = 0, string? type = null,
		string condition = "") =>
		FormattableString.Invariant(
			$$"""{ "Document": "{{document}}", "Node": {{node}}, "Type": {{RuntimeTesting.Quote(type ?? TypeOf(node, document))}}, "Enabled": {{(enabled ? "true" : "false")}}, "Condition": {{RuntimeTesting.Quote(condition)}}, "HitCount": {{hitCount}} }""");

	// Configures the breakpoints (Breakpoint's texts), the targets ({ "Graph", "Entity" } texts), the watches and BreakOnErrors.
	internal JsonElement Configure(IEnumerable<string>? breakpoints = null, IEnumerable<string>? targets = null, bool breakOnErrors = false,
		IEnumerable<string>? watches = null)
	{
		string request = FormattableString.Invariant(
			$$"""{ "Command": "Configure", "Revision": {{++m_Revision}}, "Breakpoints": [{{string.Join(", ", breakpoints ?? [])}}], "Targets": [{{string.Join(", ", targets ?? [])}}], "Watches": [{{string.Join(", ", watches ?? [])}}], "BreakOnErrors": {{(breakOnErrors ? "true" : "false")}} }""");
		JsonElement reply = Expect(request);
		Assert.Equal(m_Revision, reply.GetProperty("Revision").GetUInt32());
		return reply;
	}

	internal JsonElement Break(params uint[] nodes) => Configure(nodes.Select(node => Breakpoint(node)));

	internal static string Target(ulong graph, ulong entity) => FormattableString.Invariant($$"""{ "Graph": "{{graph}}", "Entity": "{{entity}}" }""");

	internal JsonElement State() => Expect("""{ "Command": "State" }""");

	internal JsonElement[] Stops() => [.. State().GetProperty("Stops").EnumerateArray()];

	internal JsonElement SingleStop()
	{
		JsonElement[] stops = Stops();
		Assert.Single(stops);
		return stops[0];
	}

	internal ulong StopRevision => State().GetProperty("StopRevision").GetUInt64();

	// Continue: the current stop, a stop by ID, or every stop.
	internal JsonElement Continue(int? stop = null) => Resume("Continue", stop);

	internal JsonElement ContinueAll() => Expect("""{ "Command": "Resume", "Action": "Continue", "All": true }""");

	// A resume of the current stop or of a stop by ID: Continue, StepInto, StepOver or StepOut.
	internal JsonElement Resume(string action, int? stop = null) => Expect(stop is { } id
		? FormattableString.Invariant($$"""{ "Command": "Resume", "Action": "{{action}}", "Stop": {{id}} }""")
		: $$"""{ "Command": "Resume", "Action": "{{action}}" }""");

	// Arms a Break, for one graph or any; cancels it and the step.
	internal JsonElement BreakNext(ulong? document = null) => Expect(document is { } handle
		? FormattableString.Invariant($$"""{ "Command": "Break", "Cancel": false, "Document": "{{handle}}" }""")
		: """{ "Command": "Break", "Cancel": false }""");

	internal JsonElement CancelBreak() => Expect("""{ "Command": "Break", "Cancel": true }""");

	internal JsonElement RunTo(uint node, ulong document = TestRuntime.Graph) =>
		Expect(FormattableString.Invariant($$"""{ "Command": "RunTo", "Document": "{{document}}", "Node": {{node}} }"""));

	internal JsonElement CheckCondition(uint node, string condition, ulong document = TestRuntime.Graph) => Expect(FormattableString.Invariant(
		$$"""{ "Command": "CheckCondition", "Document": "{{document}}", "Node": {{node}}, "Condition": {{RuntimeTesting.Quote(condition)}} }"""));

	// Describe of a stop's frame (0: innermost), with formulas to evaluate against it.
	internal JsonElement Describe(int stop, int frame = 0, params string[] formulas)
	{
		string evaluate = formulas.Length > 0 ? $", \"Evaluate\": [{string.Join(", ", formulas.Select(RuntimeTesting.Quote))}]" : string.Empty;
		return Expect(FormattableString.Invariant($$"""{ "Command": "Describe", "Stop": {{stop}}, "Frame": {{frame}}{{evaluate}} }"""));
	}

	// Record: starts a recording (the members of Start as JSON text), sets the frame index, stops it with its reply.
	internal JsonElement StartRecording(string? entity = null, string? documents = null, string? nodes = null, bool values = true, bool summary = false,
		int maxSteps = 100_000, int maxValues = 100_000) => Expect(FormattableString.Invariant(
		$$"""{ "Command": "Record", "Start": { "Entity": {{entity ?? "null"}}, "Documents": {{documents ?? "null"}}, "Nodes": {{nodes ?? "null"}}, "Values": {{(values ? "true" : "false")}}, "Summary": {{(summary ? "true" : "false")}}, "MaxSteps": {{maxSteps}}, "MaxValues": {{maxValues}} } }"""));

	internal JsonElement RecordFrame(int frame) => Expect(FormattableString.Invariant($$"""{ "Command": "Record", "Frame": {{frame}} }"""));

	internal JsonElement StopRecording() => Expect("""{ "Command": "Record", "Stop": true }""");

	// The node of a stop's innermost frame.
	internal static uint NodeOf(JsonElement stop) => stop.GetProperty("Frames")[0].GetProperty("Node").GetUInt32();

	internal static int IdOf(JsonElement stop) => stop.GetProperty("ID").GetInt32();

	internal static JsonElement Frame(JsonElement stop, int index = 0) => stop.GetProperty("Frames")[index];

	internal static string Text(JsonElement element, string member) => element.GetProperty(member).GetString()!;

	// "<node> <place> <note>" of the current stop's innermost frame ("12 Enter", "5 Continue next iteration: Index 2"), or the
	// resume's outcome when no stop is left.
	internal string Position()
	{
		JsonElement state = State();
		if (state.GetProperty("Current").ValueKind == JsonValueKind.Null)
		{
			return "none";
		}
		int current = state.GetProperty("Current").GetInt32();
		JsonElement stop = state.GetProperty("Stops").EnumerateArray().Single(stop => IdOf(stop) == current);
		string note = Text(Frame(stop), "Note");
		return FormattableString.Invariant($"{NodeOf(stop)} {Text(stop, "Place")}{(note.Length > 0 ? " " + note : "")}");
	}

	// The positions a step action reaches from the current stop until it reaches no stop, and the outcome it ended with.
	internal (List<string> Positions, string Outcome) Walk(string action, int limit = 100)
	{
		List<string> positions = [Position()];
		for (int step = 0; step < limit; step++)
		{
			JsonElement resumed = Resume(action);
			string outcome = Text(resumed, "Outcome");
			if (outcome != "Stopped")
			{
				return (positions, outcome);
			}
			positions.Add(Position());
		}
		throw new InvalidOperationException($"{action} reached more than {limit} stops");
	}

	// The Type of a node of the document in the programs the instances run: an instance's own document (the event graph and its
	// functions) or a library compiled into the program.
	internal string TypeOf(uint node, ulong document = TestRuntime.Graph)
	{
		foreach (GraphInstance instance in m_Runtime.Runtime.Instances)
		{
			if (BreakpointConditions.Find(instance.Program, instance.Graph, document, node) is (_, { } found))
			{
				return found.Type;
			}
		}
		throw new ArgumentException(string.Create(CultureInfo.InvariantCulture, $"no running program has the node {node} of {document}"), nameof(node));
	}
}
