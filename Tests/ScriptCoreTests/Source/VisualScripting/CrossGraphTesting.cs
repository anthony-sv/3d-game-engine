using System;
using System.Text;
using Strada.VisualScripting;

namespace Strada.ScriptCore.Tests;

// Graphs that talk to graphs (spec-slice3 section 4): the handles and names of the other graphs the tests refer to, a function
// library of every kind of function, and analyses with a references bundle.
internal static class CrossGraphTesting
{
	internal const ulong Motion = 5550000000000000001;
	internal const string MotionName = "Scripts/Motion.sgraph";
	internal const ulong Physics = 5550000000000000002;
	internal const string PhysicsName = "Scripts/Physics.sgraph";
	internal const ulong Door = 8146390251172604461;
	internal const string DoorName = "Scripts/Door.sgraph";

	// The settings of a call of a library's function.
	internal static string Call(FunctionBuilder function, ulong library) => Call(function.Id, library);

	internal static string Call(uint function, ulong library) => FormattableString.Invariant($$"""{ "Function": {{function}}, "Graph": "{{library}}" }""");

	// The settings of a Send of an event without parameters to the graph.
	internal static string Send(string name, ulong graph, string parameters = "") =>
		FormattableString.Invariant($$"""{ "Name": {{RuntimeTesting.Quote(name)}}, "Parameters": [{{parameters}}], "Graph": "{{graph}}" }""");

	internal static AnalyzedDocument Analyze(string document, string references, LibraryAnalyses? libraries = null, string? name = null) =>
		GraphAnalyzer.Analyze(RuntimeTesting.Library, Encoding.UTF8.GetBytes(document), GraphReferences.Parse(Encoding.UTF8.GetBytes(references)),
			libraries ?? new LibraryAnalyses(), name);

	// A script whose custom event Hit takes (float Damage) and records "hit <Damage>".
	internal static DocumentBuilder DoorScript(string? enabled = null)
	{
		DocumentBuilder door = new();
		uint hit = door.Graph.Node("Event.Custom", """{ "Name": "Hit", "Parameters": [{ "Name": "Damage", "Type": "float" }] }""", enabled: enabled);
		uint text = door.Graph.Node("FormatString", """{ "Format": "hit {Damage}" }""");
		door.Graph.Link(hit, "Damage", text, "Damage");
		door.Graph.Flow(hit, door.Graph.Write(text, "Result"));
		return door;
	}
}

// The function library Scripts/Motion.sgraph of the tests: the pure Twice(int Value) -> int Result, whose input was X before
// (a former name); the impure Report(string Text), which records Text; the latent Pause(float Seconds), which waits, then
// records "resumed"; and the impure Ping(), which sends Ping to its Target left unlinked: the entity whose execution called it.
// delayEnabled is the Enabled of Pause's Delay (absent when null).
internal sealed class MotionLibrary
{
	internal MotionLibrary(string? delayEnabled = null)
	{
		Document.Kind = "Library";
		Twice = Document.Function("Twice", pure: true, ["Value:int:X"], ["Result:int"]);
		uint times = Twice.Graph.Node("Operator.Multiply", inputs: """{ "B": 2 }""");
		uint result = Twice.Graph.Node("Function.Return");
		Twice.Graph.Link(Twice.Entry, "Value", times, "A");
		Twice.Graph.Link(times, "Result", result, "Result");

		Report = Document.Function("Report", inputs: ["Text:string"]);
		Report.Graph.Flow(Report.Entry, Report.Graph.Write(Report.Entry, "Text"));

		Pause = Document.Function("Pause", inputs: ["Seconds:float"]);
		Delay = Pause.Graph.Node("Flow.Delay", enabled: delayEnabled);
		Pause.Graph.Link(Pause.Entry, "Seconds", Delay, "Duration");
		Pause.Graph.Flow(Pause.Entry, Delay);
		Pause.Graph.Writes(Delay, "Completed", "resumed");

		Ping = Document.Function("Ping");
		Ping.Graph.Flow(Ping.Entry, Ping.Graph.Node("Event.Send", """{ "Name": "Ping" }"""));
	}

	internal DocumentBuilder Document { get; } = new();

	internal FunctionBuilder Twice { get; }

	internal FunctionBuilder Report { get; }

	internal FunctionBuilder Pause { get; }

	// Pause's Flow.Delay.
	internal uint Delay { get; }

	internal FunctionBuilder Ping { get; }

	internal string Build() => Document.Build();

	// A bundle for a document (whose handle is TestRuntime.Graph) that calls this library.
	internal string Bundle() => new BundleBuilder().Full(CrossGraphTesting.Motion, CrossGraphTesting.MotionName, Document).Build();
}
