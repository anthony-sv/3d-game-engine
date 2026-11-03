using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The node library of the runtime tests (the scripting API, the probes and two script classes), and runtimes over fake
// environments with one instance on the entity Hero.
internal static class RuntimeTesting
{
	internal const string Write = "Call:Strada.ScriptCore.Tests.Probe.Write(string)";
	internal const string Next = "Call:Strada.ScriptCore.Tests.Probe.Next()";

	private static readonly Lazy<NodeLibrary> s_Library = new(() => NodeLibraryBuilder.Build(new NodeLibrarySources
	{
		ApiTypes = [.. NodeLibrarySources.GetApiTypes(), typeof(Probe), typeof(Pair), typeof(Holder), typeof(ProbeAccess)],
		ScriptClasses = [typeof(Mover), typeof(Jumper)],
	}));

	internal static NodeLibrary Library => s_Library.Value;

	internal static string Quote(string text) => JsonSerializer.Serialize(text);
}

// A runtime with a fake environment. Hero (entity 7) runs the graph Graph (handle 100) once Start created it.
internal sealed class TestRuntime
{
	internal const ulong Hero = 7;
	internal const ulong Graph = 100;
	internal const string Name = "Assets/Test.sgraph";

	internal TestRuntime()
		: this(static () => RuntimeTesting.Library)
	{
	}

	// A runtime over another node library (tests of members the runtime tests' library does not have).
	internal TestRuntime(Func<NodeLibrary> library)
	{
		Probe.Reset();
		Runtime = new VisualScriptRuntime(Environment, library);
		Environment.Add(Hero, "Hero");
	}

	internal FakeEnvironment Environment { get; } = new();

	internal VisualScriptRuntime Runtime { get; }

	internal List<string> Records => Probe.Records;

	// A runtime whose Hero runs the document (which must analyze without errors).
	internal static TestRuntime Start(string document, string variables = "")
	{
		TestRuntime runtime = new();
		runtime.LoadWorking(document);
		Assert.Equal(VisualScriptStatus.Success, runtime.Create(variables: variables));
		return runtime;
	}

	internal static TestRuntime Start(DocumentBuilder document, string variables = "") => Start(document.Build(), variables);

	// references: the bundle of the other graphs the document refers to (spec-slice3 section 2.4); none when empty.
	internal IReadOnlyList<Diagnostic> Load(string document, ulong graph = Graph, string name = Name, string references = "") =>
		Runtime.Load(graph, name, Encoding.UTF8.GetBytes(document), Encoding.UTF8.GetBytes(references));

	internal void LoadWorking(string document, ulong graph = Graph, string references = "", string name = Name)
	{
		IReadOnlyList<Diagnostic> errors =
			[.. Load(document, graph, name, references).Where(static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error)];
		Assert.True(errors.Count == 0, $"{errors.Count} errors:\n{VisualScriptTesting.Describe(errors)}");
	}

	internal VisualScriptStatus Create(ulong entity = Hero, ulong graph = Graph, string variables = "") =>
		Runtime.CreateInstance(entity, graph, Encoding.UTF8.GetBytes(variables));

	internal VisualScriptStatus Invoke(ScriptEvent scriptEvent, float timeStep = 0.0f, ulong other = 0, ulong entity = Hero) =>
		Runtime.Invoke(entity, scriptEvent, timeStep, other);

	internal GraphInstance Instance(ulong entity = Hero) => Runtime.FindInstance(entity) ?? throw new InvalidOperationException($"entity {entity} has no instance");

	// The fewest bytes one of updates Updates of Hero's instance allocated. A collection caused by another thread can count
	// the unused rest of this thread's allocation buffer as allocated, so the Update that allocated least counts: what the
	// graph allocates shows in every one.
	internal long FewestBytesPerUpdate(int updates = 50, float timeStep = 0.016f)
	{
		long fewest = long.MaxValue;
		for (int update = 0; update < updates; update++)
		{
			long before = GC.GetAllocatedBytesForCurrentThread();
			Invoke(ScriptEvent.Update, timeStep);
			fewest = Math.Min(fewest, GC.GetAllocatedBytesForCurrentThread() - before);
		}
		return fewest;
	}

	internal object? Variable(string name, ulong entity = Hero)
	{
		GraphInstance instance = Instance(entity);
		int index = instance.FindVariable(name);
		Assert.True(index >= 0, $"the graph has no variable {name}");
		return instance.ReadVariable(index);
	}

	// The one error logged; fails when there is none or more than one.
	internal string SingleError()
	{
		List<string> errors = [.. Environment.Errors];
		Assert.True(errors.Count == 1, $"expected one error, got {errors.Count}:\n{string.Join("\n", Environment.Logs.Select(static log => $"{log.Level}: {log.Message}"))}");
		return errors[0];
	}

	// The prefix of messages about a node of Hero's graph.
	internal static string On(string title, uint node) => FormattableString.Invariant($"{Name}: {title} (node {node}) on Hero ({Hero}): ");
}

// Builds content-form documents. Variables, functions, locals and nodes take IDs from one counter, in the order they are
// added. Kind is written when set ("Library" for a function library).
internal sealed class DocumentBuilder
{
	private readonly List<string> m_Variables = [];
	private readonly List<FunctionBuilder> m_Functions = [];
	private uint m_NextId = 1;

	internal DocumentBuilder()
	{
		Graph = new GraphBuilder(this);
	}

	// The event graph.
	internal GraphBuilder Graph { get; }

	internal string? Kind { get; set; }

	internal uint NextId() => m_NextId++;

	internal uint Variable(string name, string type, string? value = null, bool isPublic = false, params string[] formerNames)
	{
		uint id = NextId();
		StringBuilder text = new(FormattableString.Invariant($"{{ \"ID\": {id}, \"Name\": {RuntimeTesting.Quote(name)}, \"Type\": {RuntimeTesting.Quote(type)}"));
		if (value != null)
		{
			text.Append(", \"Value\": ").Append(value);
		}
		if (isPublic)
		{
			text.Append(", \"Public\": true");
		}
		if (formerNames.Length > 0)
		{
			text.Append(", \"FormerNames\": [").Append(string.Join(", ", formerNames.Select(RuntimeTesting.Quote))).Append(']');
		}
		m_Variables.Add(text.Append(" }").ToString());
		return id;
	}

	// parameters: "Name:type" pairs, or "Name:type:Former,Former" with the parameter's former names.
	internal FunctionBuilder Function(string name, bool pure = false, string[]? inputs = null, string[]? outputs = null)
	{
		FunctionBuilder function = new(this, NextId(), name, pure, inputs ?? [], outputs ?? []);
		m_Functions.Add(function);
		return function;
	}

	internal string Build() =>
		$$"""{ {{(Kind != null ? $"\"Kind\": {RuntimeTesting.Quote(Kind)}, " : "")}}"Variables": [{{string.Join(", ", m_Variables)}}], "Functions": [{{string.Join(", ", m_Functions.Select(static function => function.Json()))}}], "Graph": {{Graph.Json()}} }""";
}

internal sealed class FunctionBuilder
{
	private readonly List<string> m_Locals = [];
	private readonly string m_Name;
	private readonly bool m_Pure;
	private readonly string[] m_Inputs;
	private readonly string[] m_Outputs;
	private readonly DocumentBuilder m_Document;

	internal FunctionBuilder(DocumentBuilder document, uint id, string name, bool pure, string[] inputs, string[] outputs)
	{
		m_Document = document;
		Id = id;
		m_Name = name;
		m_Pure = pure;
		m_Inputs = inputs;
		m_Outputs = outputs;
		Graph = new GraphBuilder(document);
		Entry = Graph.Node("Function.Entry");
	}

	internal uint Id { get; }

	internal GraphBuilder Graph { get; }

	internal uint Entry { get; }

	internal string Settings => FormattableString.Invariant($"{{ \"Function\": {Id} }}");

	internal uint Local(string name, string type, string? value = null)
	{
		uint id = m_Document.NextId();
		string text = FormattableString.Invariant($"{{ \"ID\": {id}, \"Name\": {RuntimeTesting.Quote(name)}, \"Type\": {RuntimeTesting.Quote(type)}");
		m_Locals.Add(value != null ? $"{text}, \"Value\": {value} }}" : $"{text} }}");
		return id;
	}

	internal string Json() => FormattableString.Invariant(
		$$"""{ "ID": {{Id}}, "Name": {{RuntimeTesting.Quote(m_Name)}}, "Pure": {{(m_Pure ? "true" : "false")}}, "Inputs": [{{Parameters(m_Inputs)}}], "Outputs": [{{Parameters(m_Outputs)}}], "Locals": [{{string.Join(", ", m_Locals)}}], "Graph": {{Graph.Json()}} }""");

	private static string Parameters(string[] parameters) => string.Join(", ", parameters.Select(static parameter =>
	{
		string[] parts = parameter.Split(':');
		string formerNames = parts.Length > 2 ? $", \"FormerNames\": [{string.Join(", ", parts[2].Split(',').Select(RuntimeTesting.Quote))}]" : string.Empty;
		return $$"""{ "Name": {{RuntimeTesting.Quote(parts[0])}}, "Type": {{RuntimeTesting.Quote(parts[1])}}{{formerNames}} }""";
	}));
}

internal sealed class GraphBuilder
{
	private readonly DocumentBuilder m_Document;
	private readonly List<string> m_Nodes = [];
	private readonly List<string> m_Links = [];

	internal GraphBuilder(DocumentBuilder document)
	{
		m_Document = document;
	}

	// settings, inputs and split are JSON objects; enabled is the node's Enabled ("Disabled", "DevelopmentOnly"), absent when
	// null.
	internal uint Node(string type, string? settings = null, string? inputs = null, string? enabled = null, string? split = null)
	{
		uint id = m_Document.NextId();
		StringBuilder text = new(FormattableString.Invariant($"{{ \"ID\": {id}, \"Type\": {RuntimeTesting.Quote(type)}"));
		if (settings != null)
		{
			text.Append(", \"Settings\": ").Append(settings);
		}
		if (inputs != null)
		{
			text.Append(", \"Inputs\": ").Append(inputs);
		}
		if (split != null)
		{
			text.Append(", \"Split\": ").Append(split);
		}
		if (enabled != null)
		{
			text.Append(", \"Enabled\": ").Append(RuntimeTesting.Quote(enabled));
		}
		m_Nodes.Add(text.Append(" }").ToString());
		return id;
	}

	internal void Link(uint from, string output, uint to, string input)
	{
		m_Links.Add(FormattableString.Invariant($"{{ \"From\": [{from}, {RuntimeTesting.Quote(output)}], \"To\": [{to}, {RuntimeTesting.Quote(input)}] }}"));
	}

	internal void Flow(uint from, uint to, string output = "Then", string input = "Execute")
	{
		Link(from, output, to, input);
	}

	// A Probe.Write node that records text.
	internal uint Write(string text) => Node(RuntimeTesting.Write, inputs: $$"""{ "Text": {{RuntimeTesting.Quote(text)}} }""");

	// A Probe.Write node that records the value linked into it (as text).
	internal uint Write(uint from, string output)
	{
		uint write = Node(RuntimeTesting.Write);
		Link(from, output, write, "Text");
		return write;
	}

	// A chain of Probe.Write nodes after from (through output), one per text; returns the last.
	internal uint Writes(uint from, string output, params string[] texts)
	{
		uint previous = from;
		string exit = output;
		foreach (string text in texts)
		{
			uint write = Write(text);
			Flow(previous, write, exit);
			previous = write;
			exit = "Then";
		}
		return previous;
	}

	internal uint Literal(string type, string value) => Node("Literal", $$"""{ "Type": {{RuntimeTesting.Quote(type)}}, "Value": {{value}} }""");

	internal uint Get(uint variable) => Node("Variable.Get", FormattableString.Invariant($"{{ \"Variable\": {variable} }}"));

	internal uint Set(uint variable, string? value = null) =>
		Node("Variable.Set", FormattableString.Invariant($"{{ \"Variable\": {variable} }}"), value != null ? $$"""{ "Value": {{value}} }""" : null);

	internal string Json() => $$"""{ "Nodes": [{{string.Join(", ", m_Nodes)}}], "Links": [{{string.Join(", ", m_Links)}}], "Comments": [] }""";
}

// Builds references bundles (spec-slice3 section 2.4): the handle of the document they accompany, and the other graphs, each
// sent in full (a library), as its interface, or absent with its error.
internal sealed class BundleBuilder
{
	private readonly List<string> m_Graphs = [];
	private readonly ulong m_Self;

	internal BundleBuilder(ulong self = TestRuntime.Graph)
	{
		m_Self = self;
	}

	internal BundleBuilder Full(ulong graph, string name, string document) => Add(graph, name, "Full", document);

	internal BundleBuilder Full(ulong graph, string name, DocumentBuilder document) => Full(graph, name, document.Build());

	internal BundleBuilder Interface(ulong graph, string name, string document) => Add(graph, name, "Interface", document);

	internal BundleBuilder Interface(ulong graph, string name, DocumentBuilder document) => Interface(graph, name, document.Build());

	internal BundleBuilder Absent(ulong graph, string error)
	{
		m_Graphs.Add(FormattableString.Invariant($$"""{ "Graph": "{{graph}}", "Name": "", "Error": {{RuntimeTesting.Quote(error)}} }"""));
		return this;
	}

	internal string Build() => FormattableString.Invariant($$"""{ "Self": "{{m_Self}}", "Graphs": [{{string.Join(", ", m_Graphs)}}] }""");

	private BundleBuilder Add(ulong graph, string name, string form, string document)
	{
		m_Graphs.Add(FormattableString.Invariant(
			$$"""{ "Graph": "{{graph}}", "Name": {{RuntimeTesting.Quote(name)}}, "Form": "{{form}}", "Document": {{document}} }"""));
		return this;
	}
}
