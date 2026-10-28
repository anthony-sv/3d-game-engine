using System;
using System.Collections.Generic;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The Array nodes (spec-slice3 section 3.4.5): pure, one step each; none changes an array, the ones that "change" one make a
// new one of at most 65,536 elements.
public sealed class ArrayNodeTests
{
	// A node fed by Literal nodes (pin, type, literal) and stored inputs, whose outputs are recorded in order on Create.
	private static TestRuntime Run(string type, string? settings, string? inputs, (string Pin, string Type, string Literal)[] literals,
		params string[] outputs)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint node = graph.Node(type, settings, inputs);
		foreach ((string pin, string literalType, string literal) in literals)
		{
			graph.Link(graph.Literal(literalType, literal), "Value", node, pin);
		}
		uint previous = graph.Node("Event.Create");
		foreach (string output in outputs)
		{
			uint write = graph.Write(node, output);
			graph.Flow(previous, write);
			previous = write;
		}
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		return runtime;
	}

	private static string Result(string type, string? settings, string? inputs, (string Pin, string Type, string Literal)[] literals, string output)
	{
		TestRuntime runtime = Run(type, settings, inputs, literals, output);
		Assert.Empty(runtime.Environment.Logs);
		return Assert.Single(runtime.Records);
	}

	// The aborting error of a node fed by a probe's array (source: Numbers(count) or RepeatText(text, count)).
	private static string Abort(string type, string arrayPin, string source, string sourceInputs, string? inputs = null, string? second = null)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint node = graph.Node(type, inputs: inputs);
		graph.Link(graph.Node(source, inputs: sourceInputs), "Result", node, arrayPin);
		if (second != null)
		{
			graph.Link(graph.Node(source, inputs: second), "Result", node, "B");
		}
		graph.Flow(graph.Node("Event.Create"), graph.Write(node, type == "String.Split" ? "Parts" : "Result"));
		TestRuntime runtime = TestRuntime.Start(document);
		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Create));
		string error = runtime.SingleError();
		return error[error.IndexOf("): ", StringComparison.Ordinal)..][3..];
	}

	[Theory]
	[InlineData("int[]", "[1, 2, 3]", "2", "True")]
	[InlineData("int[]", "[1, 2, 3]", "4", "False")]
	[InlineData("int[]", "[]", "1", "False")]
	[InlineData("string[]", """["a", "b"]""", "\"B\"", "False")]
	[InlineData("string[]", """["a", "b"]""", "\"b\"", "True")]
	[InlineData("Strada.Entity[]", """["0"]""", "\"0\"", "True")]
	[InlineData("Strada.KeyCode[]", """["Space", "W"]""", "\"W\"", "True")]
	[InlineData("Strada.Vector3[]", "[[1, 2, 3]]", "[1, 2, 3]", "True")]
	[InlineData("double[]", "[0.5]", "0.25", "False")]
	public void ContainsComparesByTheElementsEquality(string arrayType, string array, string value, string expected)
	{
		Assert.Equal(expected, Result("Array.Contains", null, $$"""{ "Value": {{value}} }""", [("Array", arrayType, array)], "Result"));
	}

	[Fact]
	public void ContainsNeverFindsNaN()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint nan = graph.Node("Call:Strada.Mathf.Sqrt(float)", inputs: """{ "Value": -1.0 }""");
		uint make = graph.Node("Array.Make", """{ "Type": "float", "Count": 1 }""");
		graph.Link(nan, "Result", make, "Element0");
		uint contains = graph.Node("Array.Contains");
		graph.Link(make, "Array", contains, "Array");
		graph.Link(nan, "Result", contains, "Value");
		uint index = graph.Node("Array.IndexOf");
		graph.Link(make, "Array", index, "Array");
		graph.Link(nan, "Result", index, "Value");
		uint write = graph.Write(contains, "Result");
		graph.Flow(graph.Node("Event.Create"), write);
		graph.Flow(write, graph.Write(index, "Index"));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["False", "-1"], runtime.Records);
	}

	[Theory]
	[InlineData("[1, 2, 1]", 1, 0, "0")]
	[InlineData("[1, 2, 1]", 1, 1, "2")]
	[InlineData("[1, 2, 1]", 1, -5, "0")]
	[InlineData("[1, 2, 1]", 1, 3, "-1")]
	[InlineData("[1, 2, 1]", 1, 8, "-1")]
	[InlineData("[1, 2, 1]", 7, 0, "-1")]
	[InlineData("[]", 1, 0, "-1")]
	public void IndexOfStartsAtTheClampedStartIndex(string array, int value, int start, string expected)
	{
		Assert.Equal(expected, Result("Array.IndexOf", null, FormattableString.Invariant($$"""{ "Value": {{value}}, "StartIndex": {{start}} }"""),
			[("Array", "int[]", array)], "Index"));
	}

	[Fact]
	public void MakeBuildsArraysOfItsTypeOrOfItsLinks()
	{
		Assert.Equal("[1, 0, 3]", Result("Array.Make", """{ "Type": "float", "Count": 3 }""", """{ "Element0": 1, "Element2": 3 }""", [], "Array"));
		Assert.Equal("[]", Result("Array.Make", """{ "Type": "Strada.Entity", "Count": 0 }""", null, [], "Array"));
		Assert.Equal("[Space, W]", Result("Array.Make", """{ "Type": "Strada.KeyCode" }""", """{ "Element0": "Space", "Element1": "W" }""", [], "Array"));
		// Without Type, the links decide the type: int and float join to float (unlinked elements take their literals at it).
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint make = graph.Node("Array.Make", """{ "Count": 3 }""", """{ "Element2": 4.5 }""");
		graph.Link(graph.Literal("int", "1"), "Value", make, "Element0");
		graph.Link(graph.Literal("float", "2.5"), "Value", make, "Element1");
		graph.Flow(graph.Node("Event.Create"), graph.Write(make, "Array"));
		AnalyzedDocument analyzed = VisualScriptTesting.Analyze(RuntimeTesting.Library, document.Build());
		VisualScriptTesting.AssertNoErrors(analyzed);
		Assert.Equal("float[]", VisualScriptTesting.Pin(analyzed, make, "Array").Type.Id);
		Assert.Equal("float", VisualScriptTesting.Pin(analyzed, make, "Element0", PinDirection.In).Type.Id);
		Assert.Equal("Make Array", VisualScriptTesting.Node(analyzed, make).Title);
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["[1, 2.5, 4.5]"], runtime.Records);

		AnalyzedDocument typed = VisualScriptTesting.Analyze(VisualScriptTesting.Document("""{ "ID": 1, "Type": "Array.Make", "Settings": { "Type": "float" } }"""));
		Assert.Equal("Make Float Array", VisualScriptTesting.Node(typed, 1).Title);
		Assert.Equal(["Element0: float", "Element1: float", "Array: float[]"], DescribePins(VisualScriptTesting.Node(typed, 1)));
	}

	[Fact]
	public void MakeWithoutElementsOrTypeIsRefused()
	{
		AnalyzedDocument empty = VisualScriptTesting.Analyze(VisualScriptTesting.Document("""{ "ID": 1, "Type": "Array.Make", "Settings": { "Count": 0 } }"""));
		Assert.Equal("Make Array with no elements needs the setting Type: the element type",
			VisualScriptTesting.Single(empty, DiagnosticCode.MissingSetting).Message);
		// Unconnected elements without Type have nothing to take a type from.
		AnalyzedDocument literals = VisualScriptTesting.Analyze(VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Array.Make", "Inputs": { "Element0": 1, "Element1": 2 } }"""));
		Assert.Equal(2, VisualScriptTesting.Errors(literals).Count);
		Assert.All(VisualScriptTesting.Errors(literals), static error =>
		{
			Assert.Equal(DiagnosticCode.UnresolvedWildcard, error.Code);
			Assert.EndsWith("until it is connected (or set Type: the element type)", error.Message, StringComparison.Ordinal);
		});
		foreach (string count in new[] { "-1", "65", "2.5", "\"3\"" })
		{
			AnalyzedDocument invalid = VisualScriptTesting.Analyze(VisualScriptTesting.Document(
				$$"""{ "ID": 1, "Type": "Array.Make", "Settings": { "Type": "int", "Count": {{count}} } }"""));
			Assert.Equal($"Count: expected an integer from 0 to 64, not {count}", VisualScriptTesting.Single(invalid, DiagnosticCode.InvalidSetting).Message);
		}
	}

	[Theory]
	[InlineData("Array.Set", """{ "Index": 1, "Value": 9 }""", "[1, 9, 3]")]
	[InlineData("Array.Add", """{ "Value": 9 }""", "[1, 2, 3, 9]")]
	[InlineData("Array.RemoveAt", """{ "Index": 0 }""", "[2, 3]")]
	[InlineData("Array.RemoveAt", """{ "Index": 2 }""", "[1, 2]")]
	[InlineData("Array.Insert", """{ "Index": 0, "Value": 9 }""", "[9, 1, 2, 3]")]
	[InlineData("Array.Insert", """{ "Index": 1, "Value": 9 }""", "[1, 9, 2, 3]")]
	[InlineData("Array.Insert", """{ "Index": 3, "Value": 9 }""", "[1, 2, 3, 9]")]
	public void ChangesMakeNewArraysAndLeaveTheirInputs(string type, string inputs, string expected)
	{
		DocumentBuilder document = new();
		uint numbers = document.Variable("Numbers", "int[]", "[1, 2, 3]");
		GraphBuilder graph = document.Graph;
		uint node = graph.Node(type, inputs: inputs);
		graph.Link(graph.Get(numbers), "Value", node, "Array");
		uint changed = graph.Write(node, "Result");
		graph.Flow(graph.Node("Event.Create"), changed);
		graph.Flow(changed, graph.Write(graph.Get(numbers), "Value"));
		TestRuntime runtime = TestRuntime.Start(document);
		int[] before = Assert.IsType<int[]>(runtime.Variable("Numbers"));

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal([expected, "[1, 2, 3]"], runtime.Records);
		Assert.Same(before, runtime.Variable("Numbers"));
		Assert.Equal([1, 2, 3], before);
	}

	[Fact]
	public void AppendJoinsTwoArraysOfOneType()
	{
		Assert.Equal("[1, 2, 3]", Result("Array.Append", null, null, [("A", "int[]", "[1, 2]"), ("B", "int[]", "[3]")], "Result"));
		Assert.Equal("[]", Result("Array.Append", null, null, [("A", "int[]", "[]"), ("B", "int[]", "[]")], "Result"));
		Assert.Equal("[a]", Result("Array.Append", null, null, [("A", "string[]", "[]"), ("B", "string[]", """["a"]""")], "Result"));
		AnalyzedDocument mixed = VisualScriptTesting.Analyze(VisualScriptTesting.Document("""
			{ "ID": 1, "Type": "Literal", "Settings": { "Type": "int[]", "Value": [1] } },
			{ "ID": 2, "Type": "Literal", "Settings": { "Type": "float[]", "Value": [1] } },
			{ "ID": 3, "Type": "Array.Append" }
			""", """{ "From": [1, "Value"], "To": [3, "A"] }, { "From": [2, "Value"], "To": [3, "B"] }"""));
		Assert.Equal("A is int[] and B is float[]: A, B, Result take one type, and these have no common type",
			VisualScriptTesting.Single(mixed, DiagnosticCode.TypeMismatch).Message);
	}

	[Theory]
	[InlineData("Array.Set", """{ "Index": 3, "Value": 1 }""", "index 3 is outside the array (length 3)")]
	[InlineData("Array.Set", """{ "Index": -1, "Value": 1 }""", "index -1 is outside the array (length 3)")]
	[InlineData("Array.RemoveAt", """{ "Index": 3 }""", "index 3 is outside the array (length 3)")]
	[InlineData("Array.Insert", """{ "Index": 4, "Value": 1 }""", "index 4 is outside [0, 3] where elements can be inserted")]
	[InlineData("Array.Insert", """{ "Index": -1, "Value": 1 }""", "index -1 is outside [0, 3] where elements can be inserted")]
	public void IndicesOutsideTheArrayAbort(string type, string inputs, string message)
	{
		Assert.Equal(message, Abort(type, "Array", "Call:Strada.ScriptCore.Tests.Probe.Numbers(int)", """{ "Count": 3 }""", inputs));
	}

	[Fact]
	public void MadeArraysHoldAtMost65536Elements()
	{
		const string numbers = "Call:Strada.ScriptCore.Tests.Probe.Numbers(int)";
		Assert.Equal(Intrinsics.MadeArrayMessage, Abort("Array.Add", "Array", numbers, """{ "Count": 65536 }""", """{ "Value": 1 }"""));
		Assert.Equal(Intrinsics.MadeArrayMessage, Abort("Array.Insert", "Array", numbers, """{ "Count": 65536 }""", """{ "Index": 0, "Value": 1 }"""));
		Assert.Equal(Intrinsics.MadeArrayMessage, Abort("Array.Append", "A", numbers, """{ "Count": 40000 }""", second: """{ "Count": 30000 }"""));
		Assert.Equal(Intrinsics.MadeArrayMessage, Abort("Array.Set", "Array", numbers, """{ "Count": 65537 }""", """{ "Index": 0, "Value": 1 }"""));
		Assert.Equal(Intrinsics.MadeArrayMessage,
			Abort("String.Split", "Text", "Call:Strada.ScriptCore.Tests.Probe.RepeatText(string,int)", """{ "Text": ",", "Count": 65536 }"""));
		Assert.Equal("arrays made in graphs hold at most 65,536 elements", Intrinsics.MadeArrayMessage);

		// At the cap is fine.
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint add = graph.Node("Array.Add", inputs: """{ "Value": 1 }""");
		graph.Link(graph.Node(numbers, inputs: """{ "Count": 65535 }"""), "Result", add, "Array");
		uint length = graph.Node("Array.Length");
		graph.Link(add, "Result", length, "Array");
		uint split = graph.Node("String.Split");
		graph.Link(graph.Node("Call:Strada.ScriptCore.Tests.Probe.RepeatText(string,int)", inputs: """{ "Text": ",", "Count": 65535 }"""), "Result",
			split, "Text");
		uint parts = graph.Node("Array.Length");
		graph.Link(split, "Parts", parts, "Array");
		uint write = graph.Write(length, "Length");
		graph.Flow(graph.Node("Event.Create"), write);
		graph.Flow(write, graph.Write(parts, "Length"));
		TestRuntime runtime = TestRuntime.Start(document);
		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(["65536", "65536"], runtime.Records);
	}

	// Elements of every type a slot holds in place are read and written without boxing (spec-slice3 section 3.4.2 rule 5).
	[Fact]
	public void TypedReadsDoNotAllocate()
	{
		NodeLibrary library = VisualScriptTesting.Library;
		(string Type, Array Array)[] arrays =
		[
			("bool", new[] { true }), ("int", new[] { 1 }), ("uint", new[] { 1u }), ("long", new[] { 1L }), ("ulong", new[] { 1UL }),
			("float", new[] { 1.0f }), ("double", new[] { 1.0 }), ("Strada.Vector2", new[] { new Vector2(1.0f, 2.0f) }),
			("Strada.Vector3", new[] { new Vector3(1.0f, 2.0f, 3.0f) }), ("Strada.Vector4", new[] { new Vector4(1.0f, 2.0f, 3.0f, 4.0f) }),
			("Strada.Quaternion", new[] { Quaternion.Identity }), ("Strada.Color", new[] { new Color(1.0f, 0.5f, 0.25f, 1.0f) }),
			("Strada.KeyCode", new[] { KeyCode.Space }), ("Strada.ForceMode", new[] { ForceMode.Impulse }), ("string", new[] { "a" }),
			("Strada.Entity", new Entity?[] { null }),
		];
		foreach ((string type, Array array) in arrays)
		{
			VisualType element = library.FindType(type)!;
			Slot read = ArrayElements.Read(array, 0, element);
			ArrayElements.Write(array, 0, element, read);
			Assert.Equal(array.GetValue(0), SlotValues.ToValue(ArrayElements.Read(array, 0, element), element));
			// The batch that allocated least counts, as RuntimeTesting.FewestBytesPerUpdate does it: what the reads allocate shows
			// in every batch, what tiered compilation or another thread's collection adds does not.
			long fewest = long.MaxValue;
			for (int batch = 0; batch < 20; batch++)
			{
				long before = GC.GetAllocatedBytesForCurrentThread();
				for (int round = 0; round < 50; round++)
				{
					read = ArrayElements.Read(array, 0, element);
					ArrayElements.Write(array, 0, element, read);
				}
				fewest = Math.Min(fewest, GC.GetAllocatedBytesForCurrentThread() - before);
			}
			Assert.True(fewest == 0, $"reading and writing {type} elements allocated {fewest} bytes");
		}
		// Enums of every underlying width keep their bits.
		Assert.Equal(KeyCode.Space, SlotValues.ToValue(ArrayElements.Read(new[] { KeyCode.Space }, 0, library.FindType("Strada.KeyCode")!),
			library.FindType("Strada.KeyCode")!));
	}

	[Fact]
	public void ForEachOverAnArrayVariable()
	{
		DocumentBuilder document = new();
		uint names = document.Variable("Names", "string[]", """["a", "b", "c"]""");
		GraphBuilder graph = document.Graph;
		uint each = graph.Node("Flow.ForEach");
		graph.Link(graph.Get(names), "Value", each, "Array");
		graph.Flow(graph.Node("Event.Create"), each);
		graph.Flow(each, graph.Write(each, "Element"), "Body");
		graph.Writes(each, "Completed", "done");
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["a", "b", "c", "done"], runtime.Records);
	}

	[Fact]
	public void ArraysCrossEventSendAndFunctions()
	{
		DocumentBuilder document = new();
		FunctionBuilder grow = document.Function("Grow", inputs: ["Items:int[]"], outputs: ["Grown:int[]"]);
		uint add = grow.Graph.Node("Array.Add", inputs: """{ "Value": 4 }""");
		uint done = grow.Graph.Node("Function.Return");
		grow.Graph.Link(grow.Entry, "Items", add, "Array");
		grow.Graph.Link(add, "Result", done, "Grown");
		grow.Graph.Flow(grow.Entry, done);
		GraphBuilder graph = document.Graph;
		const string parameters = """[{ "Name": "Items", "Type": "int[]" }]""";
		uint got = graph.Node("Event.Custom", $$"""{ "Name": "Got", "Parameters": {{parameters}} }""");
		graph.Flow(got, graph.Write(got, "Items"));
		uint call = graph.Node("Function.Call", grow.Settings);
		graph.Link(graph.Literal("int[]", "[1, 2, 3]"), "Value", call, "Items");
		uint send = graph.Node("Event.Send", $$"""{ "Name": "Got", "Parameters": {{parameters}} }""");
		graph.Link(call, "Grown", send, "Items");
		graph.Flow(graph.Node("Event.Create"), call);
		graph.Flow(call, send);
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.True(runtime.Runtime.SendEvent(TestRuntime.Hero, "Got", [new[] { 7 }]));
		Assert.Equal(["[1, 2, 3, 4]", "[7]"], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	private static List<string> DescribePins(AnalyzedNode node)
	{
		List<string> pins = [];
		foreach (AnalyzedPin pin in node.Pins)
		{
			pins.Add($"{pin.Name}: {pin.Type.Id}");
		}
		return pins;
	}
}
