using System;
using System.Linq;
using System.Reflection;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Unconstrained generic methods (spec-slice3 section 3.6): their type parameters are inferred from links (an array input's
// element, or the join of the inputs of type T), or named by TypeArguments, and the method is closed once per type.
public sealed class GenericMethodTests
{
	private const string AreEqual = "Call:Strada.Testing.Assert.AreEqual<T>(T,T,string)";
	private const string First = "Call:Strada.ScriptCore.Tests.Probe.First<T>(T[])";
	private const string Repeat = "Call:Strada.ScriptCore.Tests.Probe.Repeat<T>(T,int)";
	private const string Default = "Call:Strada.ScriptCore.Tests.Probe.Default<T>()";
	private const string Echo = "Call:Strada.ScriptCore.Tests.Probe.Echo<T>(T)";

	private static AnalyzedDocument Analyze(DocumentBuilder document) => VisualScriptTesting.Analyze(RuntimeTesting.Library, document.Build());

	[Fact]
	public void TheLibraryDescribesInferredMethods()
	{
		MemberNodeDefinition areEqual = Assert.IsType<MemberNodeDefinition>(VisualScriptTesting.LibraryNode(AreEqual));
		Assert.True(areEqual.GenericInference);
		Assert.Equal(TypeConstraint.None, areEqual.GenericDomain);
		Assert.Equal("Are Equal", areEqual.Title);
		Assert.Equal("Testing", areEqual.Category);
		Assert.Equal("Are Not Equal", VisualScriptTesting.LibraryNode("Call:Strada.Testing.Assert.AreNotEqual<T>(T,T,string)").Title);
		// Assert.Throws takes a delegate and constrains its type parameter: it is no node.
		Assert.DoesNotContain(VisualScriptTesting.Library.NodeTypes, static type => type.StartsWith("Call:Strada.Testing.Assert.Throws", StringComparison.Ordinal));

		using JsonDocument library = JsonDocument.Parse(LibraryWriter.Write(RuntimeTesting.Library));
		JsonElement Node(string type) => library.RootElement.GetProperty("Nodes").EnumerateArray().Single(node => node.GetProperty("Type").GetString() == type);
		string pins = Node(AreEqual).GetProperty("Pins").GetRawText();
		Assert.Contains("""{"Name":"Expected","Direction":"In","Type":"*","Constraint":"Any","Group":"T"}""", pins, StringComparison.Ordinal);
		Assert.Contains("""{"Name":"Actual","Direction":"In","Type":"*","Constraint":"Any","Group":"T"}""", pins, StringComparison.Ordinal);
		Assert.Contains("""{"Name":"Message","Direction":"In","Type":"string","Default":""}""", pins, StringComparison.Ordinal);
		Assert.Equal("""[{"Name":"TypeArguments","Kind":"TypeList","Constraint":"Any"}]""", Node(AreEqual).GetProperty("Settings").GetRawText());
		Assert.Contains("""{"Name":"Items","Direction":"In","Type":"*","Constraint":"Array","Group":"T[]"}""", Node(First).GetProperty("Pins").GetRawText(),
			StringComparison.Ordinal);
		Assert.Contains("""{"Name":"Result","Direction":"Out","Type":"*","Constraint":"Any","ElementOf":"Items"}""",
			Node(First).GetProperty("Pins").GetRawText(), StringComparison.Ordinal);
		Assert.Contains("""{"Name":"Result","Direction":"Out","Type":"*","Constraint":"Array","ArrayOf":"Value"}""",
			Node(Repeat).GetProperty("Pins").GetRawText(), StringComparison.Ordinal);
		// T appears only in the result: TypeArguments is required.
		Assert.Equal("""[{"Name":"TypeArguments","Kind":"TypeList","Required":true,"Constraint":"Any"}]""", Node(Default).GetProperty("Settings").GetRawText());
	}

	[Fact]
	public void AreEqualInfersItsTypeFromLinks()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint passes = graph.Node(AreEqual);
		graph.Link(graph.Literal("int", "3"), "Value", passes, "Expected");
		graph.Link(graph.Literal("float", "3.0"), "Value", passes, "Actual");
		uint fails = graph.Node(AreEqual, inputs: """{ "Actual": "b" }""");
		graph.Link(graph.Literal("string", "\"a\""), "Value", fails, "Expected");
		graph.Flow(graph.Node("Event.Create"), passes);
		graph.Flow(passes, fails);
		AnalyzedDocument analyzed = Analyze(document);
		VisualScriptTesting.AssertNoErrors(analyzed);
		// An int and a float join to float, as C# infers T.
		Assert.Equal("float", VisualScriptTesting.Pin(analyzed, passes, "Expected", PinDirection.In).Type.Id);
		Assert.Equal("float", VisualScriptTesting.Pin(analyzed, passes, "Actual", PinDirection.In).Type.Id);
		MemberBinding binding = Assert.IsType<MemberBinding>(VisualScriptTesting.Node(analyzed, passes).Binding);
		Assert.Equal(typeof(float), Assert.Single(((MethodInfo)binding.Member).GetGenericArguments()));
		Assert.Equal("float", Assert.Single(binding.TypeArguments).Id);
		// An unconnected input of the group takes the group's type: "b" is a string.
		Assert.Equal("string", VisualScriptTesting.Pin(analyzed, fails, "Actual", PinDirection.In).Type.Id);

		TestRuntime runtime = TestRuntime.Start(document);
		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal(TestRuntime.On("Are Equal", fails) + "AssertionException: expected a, got b", runtime.SingleError());
	}

	[Fact]
	public void TypeArgumentsFixTheType()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint check = graph.Node(AreEqual, """{ "TypeArguments": ["double"] }""", """{ "Actual": 2.0 }""");
		graph.Link(graph.Literal("int", "2"), "Value", check, "Expected");
		uint arrays = graph.Node(AreEqual, """{ "TypeArguments": ["int[]"] }""", """{ "Expected": [1, 2], "Actual": [1, 2] }""");
		graph.Flow(graph.Node("Event.Create"), check);
		graph.Flow(check, arrays);
		AnalyzedDocument analyzed = Analyze(document);
		VisualScriptTesting.AssertNoErrors(analyzed);
		Assert.Equal("double", VisualScriptTesting.Pin(analyzed, check, "Expected", PinDirection.In).Type.Id);
		Assert.Equal("int[]", VisualScriptTesting.Pin(analyzed, arrays, "Actual", PinDirection.In).Type.Id);
		TestRuntime runtime = TestRuntime.Start(document);
		// EqualityComparer<int[]>.Default compares arrays by reference: two literals are two arrays.
		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Create));
		Assert.StartsWith(TestRuntime.On("Are Equal", arrays) + "AssertionException", runtime.SingleError(), StringComparison.Ordinal);

		foreach ((string arguments, DiagnosticCode code, string message) in new[]
		{
			("[\"exec\"]", DiagnosticCode.InvalidSetting, "TypeArguments: exec is not a type with values"),
			("[\"nope\"]", DiagnosticCode.UnknownType, "TypeArguments: unknown type \"nope\""),
			("[\"int[][]\"]", DiagnosticCode.UnknownType, "TypeArguments: \"int[][]\": arrays of arrays are not types of graphs"),
			("[\"int\", \"int\"]", DiagnosticCode.InvalidSetting, "TypeArguments: expected an array of 1 type ID, not [\"int\", \"int\"]"),
		})
		{
			AnalyzedDocument invalid = VisualScriptTesting.Analyze(VisualScriptTesting.Document(
				$$"""{ "ID": 1, "Type": "{{AreEqual}}", "Settings": { "TypeArguments": {{arguments}} } }"""));
			Assert.Equal(message, VisualScriptTesting.Single(invalid, code).Message);
			// The pins of T are left to the setting: no link types them.
			Assert.Equal(WildcardSource.None, VisualScriptTesting.Pin(invalid, 1, "Expected", PinDirection.In).Description.Source);
		}
	}

	[Fact]
	public void UnlinkedInferredPinsAreUnresolved()
	{
		AnalyzedDocument analyzed = VisualScriptTesting.Analyze(VisualScriptTesting.Document($$"""{ "ID": 1, "Type": "{{AreEqual}}" }"""));
		Assert.Equal(["Expected", "Actual"], VisualScriptTesting.Errors(analyzed).Select(static error =>
		{
			Assert.Equal(DiagnosticCode.UnresolvedWildcard, error.Code);
			return error.Pin;
		}));
		Assert.Null(VisualScriptTesting.Node(analyzed, 1).Binding);
	}

	[Fact]
	public void IncompatibleLinksAreATypeMismatch()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint check = graph.Node(AreEqual);
		graph.Link(graph.Literal("int", "1"), "Value", check, "Expected");
		graph.Link(graph.Node("Self"), "Self", check, "Actual");
		Assert.Equal("Expected is int and Actual is Strada.Entity: Expected, Actual take one type, and these have no common type",
			VisualScriptTesting.Single(Analyze(document), DiagnosticCode.TypeMismatch).Message);
	}

	[Fact]
	public void ArrayParametersInferTheirElement()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint first = graph.Node(First);
		graph.Link(graph.Literal("Strada.Vector3[]", "[[1, 2, 3], [4, 5, 6]]"), "Value", first, "Items");
		uint text = graph.Node("FormatString", """{ "Format": "{Value}" }""");
		graph.Link(first, "Result", text, "Value");
		graph.Flow(graph.Node("Event.Create"), graph.Write(text, "Result"));
		AnalyzedDocument analyzed = Analyze(document);
		Assert.Equal("Strada.Vector3", VisualScriptTesting.Pin(analyzed, first, "Result").Type.Id);
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["(1, 2, 3)"], runtime.Records);
	}

	[Fact]
	public void ArrayResultsFollowTheirElement()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint repeat = graph.Node(Repeat, inputs: """{ "Count": 3 }""");
		graph.Link(graph.Literal("string", "\"a\""), "Value", repeat, "Value");
		uint numbers = graph.Node(Repeat, inputs: """{ "Count": 2 }""");
		graph.Link(graph.Literal("int", "7"), "Value", numbers, "Value");
		uint length = graph.Node("Array.Length");
		graph.Link(numbers, "Result", length, "Array");
		uint write = graph.Write(repeat, "Result");
		graph.Flow(graph.Node("Event.Create"), write);
		graph.Flow(write, graph.Write(length, "Length"));
		AnalyzedDocument analyzed = Analyze(document);
		Assert.Equal("string[]", VisualScriptTesting.Pin(analyzed, repeat, "Result").Type.Id);
		Assert.Equal("int[]", VisualScriptTesting.Pin(analyzed, numbers, "Result").Type.Id);
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["[a, a, a]", "2"], runtime.Records);
	}

	[Fact]
	public void TypeOnlyInResultsNeedsTypeArguments()
	{
		AnalyzedDocument missing = VisualScriptTesting.Analyze(RuntimeTesting.Library, VisualScriptTesting.Document($$"""{ "ID": 1, "Type": "{{Default}}" }"""));
		Assert.Equal("Default needs TypeArguments: 1 type ID, as no input decides T",
			VisualScriptTesting.Single(missing, DiagnosticCode.MissingTypeArgument).Message);

		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint zero = graph.Node(Default, """{ "TypeArguments": ["Strada.Vector2"] }""");
		graph.Flow(graph.Node("Event.Create"), graph.Write(zero, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["(0, 0)"], runtime.Records);
	}

	// The method is closed when the graph is analyzed, once per node, and its invoker is built the first time a node of each
	// closed method runs, never per call.
	[Fact]
	public void ClosingHappensOncePerType()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint[] echoes = new uint[3];
		uint previous = graph.Node("Event.Update");
		for (int index = 0; index < echoes.Length; index++)
		{
			echoes[index] = graph.Node(Echo);
			graph.Link(graph.Literal(index < 2 ? "int" : "float", "1"), "Value", echoes[index], "Value");
			uint write = graph.Write(echoes[index], "Result");
			graph.Flow(previous, write);
			previous = write;
		}
		AnalyzedDocument analyzed = Analyze(document);
		MethodInfo Closed(uint node) => (MethodInfo)Assert.IsType<MemberBinding>(VisualScriptTesting.Node(analyzed, node).Binding).Member;
		Assert.Same(Closed(echoes[0]), Closed(echoes[1]));
		Assert.NotSame(Closed(echoes[0]), Closed(echoes[2]));

		TestRuntime runtime = TestRuntime.Start(document);
		int before = runtime.Runtime.InvokerCount;
		for (int update = 0; update < 5; update++)
		{
			runtime.Invoke(ScriptEvent.Update);
		}
		// Echo<int>, Echo<float> and Probe.Write.
		Assert.Equal(before + 3, runtime.Runtime.InvokerCount);
		Assert.Equal(15, runtime.Records.Count);
		Assert.All(runtime.Records, static record => Assert.Equal("1", record));
	}
}
