using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Text.Json;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

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

	// A request whose reply must not be an error.
	internal JsonElement Expect(string request)
	{
		JsonElement reply = Request(request);
		Assert.False(reply.TryGetProperty("Error", out JsonElement error), $"{request} answered {error}");
		return reply;
	}

	// A breakpoint of the Breakpoints array: the node of the document, with the Type its compiled node has in Hero's program.
	internal string Breakpoint(uint node, ulong document = TestRuntime.Graph, bool enabled = true, uint hitCount = 0, string? type = null) =>
		FormattableString.Invariant(
			$$"""{ "Document": "{{document}}", "Node": {{node}}, "Type": {{RuntimeTesting.Quote(type ?? TypeOf(node, document))}}, "Enabled": {{(enabled ? "true" : "false")}}, "Condition": "", "HitCount": {{hitCount}} }""");

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
	internal JsonElement Continue(int? stop = null) => Expect(stop is { } id
		? FormattableString.Invariant($$"""{ "Command": "Resume", "Action": "Continue", "Stop": {{id}} }""")
		: """{ "Command": "Resume", "Action": "Continue" }""");

	internal JsonElement ContinueAll() => Expect("""{ "Command": "Resume", "Action": "Continue", "All": true }""");

	// The node of a stop's innermost frame.
	internal static uint NodeOf(JsonElement stop) => stop.GetProperty("Frames")[0].GetProperty("Node").GetUInt32();

	internal static string Text(JsonElement element, string member) => element.GetProperty(member).GetString()!;

	// The Type of a node of the document in Hero's program: the instance's own document (the event graph and its functions) or
	// a library compiled into the program.
	internal string TypeOf(uint node, ulong document = TestRuntime.Graph)
	{
		GraphInstance instance = m_Runtime.Instance();
		GraphProgram program = instance.Program;
		IEnumerable<CompiledGraph> graphs = [program.EventGraph, .. program.Functions.Select(static function => function.Body)];
		foreach (CompiledGraph graph in graphs)
		{
			if (ProgramTrace.DocumentOf(graph, instance.Graph) != document)
			{
				continue;
			}
			int index = graph.FindNode(node);
			if (index >= 0)
			{
				return graph.Nodes[index].Type;
			}
		}
		throw new ArgumentException(string.Create(CultureInfo.InvariantCulture, $"Hero's program has no node {node} of {document}"), nameof(node));
	}
}
