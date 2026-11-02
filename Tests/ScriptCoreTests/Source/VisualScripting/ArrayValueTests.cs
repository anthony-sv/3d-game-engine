using System;
using System.Buffers;
using System.Linq;
using System.Text;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Arrays as values (spec-slice3 sections 3.4.1 to 3.4.4, 3.4.6 and 3.4.7): a type for every type with values, literals,
// defaults that are never null, element-wise equality and text, live edit of array variables, and the arrays graphs refuse.
public sealed class ArrayValueTests
{
	private static NodeLibrary Library => VisualScriptTesting.Library;

	private static VisualType Type(string id) => Library.FindType(id) ?? throw new InvalidOperationException($"the library has no type {id}");

	private static DecodedLiteral Decode(string type, string json)
	{
		using JsonDocument document = JsonDocument.Parse(json);
		return LiteralCodec.Decode(Type(type), document.RootElement.Clone());
	}

	private static string Write(string type, object? value)
	{
		ArrayBufferWriter<byte> buffer = new();
		using (Utf8JsonWriter writer = new(buffer, LibraryWriter.CreateOptions()))
		{
			LiteralCodec.Write(writer, Type(type), value);
		}
		return Encoding.UTF8.GetString(buffer.WrittenSpan);
	}

	[Fact]
	public void ArrayTypesExistForEveryTypeWithValues()
	{
		int arrays = 0;
		foreach (VisualType element in Library.Types)
		{
			if (!element.HasValues || element.Kind == TypeKind.Array)
			{
				continue;
			}
			Assert.False(element.ClrType!.IsByRefLike, $"{element.Id} is a ref struct, which has no array type");
			VisualType array = element.ArrayType ?? throw new InvalidOperationException($"{element.Id} has no array type");
			Assert.Same(array, Library.FindType(element.Id + "[]"));
			Assert.Equal(TypeKind.Array, array.Kind);
			Assert.Same(element, array.Element);
			Assert.Equal(element.ClrType.MakeArrayType(), array.ClrType);
			Assert.Equal(element.Title + " Array", array.Title);
			Assert.Equal(["System.Object"], array.ImplicitTo.Select(static type => type.Id));
			Assert.Equal(ScriptFieldType.None, array.FieldType);
			Assert.Equal(element.HasLiteral, array.HasLiteral);
			Assert.Null(array.ArrayType);
			Array empty = Assert.IsAssignableFrom<Array>(array.EmptyArray);
			Assert.Empty(empty);
			Assert.Equal(array.ClrType, empty.GetType());
			arrays++;
		}
		Assert.True(arrays > 50, $"{arrays} array types");
		Assert.Equal(arrays, Library.Types.Count(static type => type.Kind == TypeKind.Array));
		// Exec and the wildcard have none.
		Assert.Null(VisualType.Exec.ArrayType);
		Assert.Null(Library.FindType("exec[]"));
	}

	// The canonical texts are those EL's C++ codec writes (VisualScriptLiteralsTests.cpp): element by element in the element
	// type's canonical form.
	[Theory]
	[InlineData("int[]", "[1, -2, 3]", "[1,-2,3]")]
	[InlineData("float[]", "[1.5, 2, 4, -0.0]", "[1.5,2,4,-0.0]")]
	[InlineData("string[]", """["a", "", "b c"]""", """["a","","b c"]""")]
	[InlineData("Strada.Vector3[]", "[[0, 0, 0], [1, 0, 0.5]]", "[[0,0,0],[1,0,0.5]]")]
	[InlineData("Strada.KeyCode[]", """["Space", "W"]""", """["Space","W"]""")]
	[InlineData("Strada.Entity[]", """["0"]""", """["0"]""")]
	[InlineData("Strada.Material[]", """["0", "123"]""", """["0","123"]""")]
	[InlineData("int[]", "[]", "[]")]
	public void LiteralsDecodeAndWrite(string type, string literal, string canonical)
	{
		DecodedLiteral decoded = Decode(type, literal);
		Assert.True(decoded.IsValid, decoded.Message);
		Array value = Assert.IsAssignableFrom<Array>(decoded.Value);
		Assert.Equal(Type(type).ClrType, value.GetType());
		Assert.Equal(canonical, Write(type, value));
		// The canonical text decodes to the same text again.
		Assert.Equal(canonical, Write(type, Decode(type, canonical).Value));
	}

	[Fact]
	public void LiteralValuesAreTheElements()
	{
		Assert.Equal(new[] { 1.5f, 2.0f, 4.0f }, Decode("float[]", "[1.5, 2, 4]").Value);
		Assert.Equal(new[] { new Vector3(0.0f, 0.0f, 0.0f), new Vector3(1.0f, 0.0f, 0.0f) }, Decode("Strada.Vector3[]", "[[0, 0, 0], [1, 0, 0]]").Value);
		Assert.Equal(new[] { KeyCode.Space, KeyCode.W }, Decode("Strada.KeyCode[]", """["Space", "W"]""").Value);
		Assert.Equal(new Entity?[] { null }, Decode("Strada.Entity[]", """["0"]""").Value);
		Material?[] materials = Assert.IsType<Material?[]>(Decode("Strada.Material[]", """["0", "123"]""").Value);
		Assert.Null(materials[0]);
		Assert.Equal(new AssetHandle(123), materials[1]!.Handle);
		// [] is the type's one empty array; null is no array.
		Assert.Same(Type("int[]").EmptyArray, Decode("int[]", "[]").Value);
		Assert.Equal("[]", Write("int[]", null));
	}

	[Fact]
	public void InvalidLiteralsNameTheElement()
	{
		DecodedLiteral element = Decode("float[]", """[1, 2, "x"]""");
		Assert.Equal(LiteralProblem.InvalidValue, element.Problem);
		Assert.Equal(2, element.Index);
		Assert.Equal("""Value[2]: expected a finite number of at most 3.4028235e38, not "x" """.TrimEnd(), element.Describe("Value"));
		DecodedLiteral entity = Decode("Strada.Entity[]", """["0", "5"]""");
		Assert.Equal(LiteralProblem.EntityValue, entity.Problem);
		Assert.Equal(1, entity.Index);
		Assert.Equal("expected an array of int values, not 5", Decode("int[]", "5").Message);
		Assert.Equal("null is not a value: leave the value out to use the default", Decode("int[]", "null").Message);
		Assert.Equal("Strada.RaycastHit[] has no literal form: connect the pin instead", Decode("Strada.RaycastHit[]", "[]").Message);

		// The cap of spec-slice3 section 3.2.
		string Numbers(int count) => "[" + string.Join(", ", Enumerable.Repeat("1", count)) + "]";
		Assert.Equal(4096, Assert.IsType<int[]>(Decode("int[]", Numbers(4096)).Value).Length);
		DecodedLiteral tooMany = Decode("int[]", Numbers(4097));
		Assert.Equal(LiteralProblem.InvalidValue, tooMany.Problem);
		Assert.Equal(-1, tooMany.Index);
		Assert.Equal("an array literal has at most 4096 elements", tooMany.Message);

		// Where documents hold them: a variable's Value, a Literal's Value, an input's stored value.
		AnalyzedDocument variable = VisualScriptTesting.Analyze(VisualScriptTesting.Document("",
			variables: """{ "ID": 1, "Name": "Speeds", "Type": "float[]", "Value": [1, 2, "x"] }"""));
		Assert.Equal("""Speeds: Value[2]: expected a finite number of at most 3.4028235e38, not "x" """.TrimEnd(),
			VisualScriptTesting.Single(variable, DiagnosticCode.InvalidValue).Message);
		AnalyzedDocument literal = VisualScriptTesting.Analyze(VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Literal", "Settings": { "Type": "Strada.KeyCode[]", "Value": ["Space", "Nope"] } }"""));
		Assert.Equal("Value[1]: \"Nope\" is not an enumerator of Strada.KeyCode", VisualScriptTesting.Single(literal, DiagnosticCode.InvalidValue).Message);
		DocumentBuilder document = new();
		FunctionBuilder count = document.Function("Count", pure: true, ["Items:int[]"], ["N:int"]);
		count.Graph.Node("Function.Return");
		document.Graph.Node("Function.Call", count.Settings, """{ "Items": [1, 2.5] }""");
		Assert.Equal("Items[1]: expected an integer from -2147483648 to 2147483647, not 2.5",
			VisualScriptTesting.Single(VisualScriptTesting.Analyze(RuntimeTesting.Library, document.Build()), DiagnosticCode.InvalidValue).Message);
	}

	[Fact]
	public void DefaultsAreEmptyNotNull()
	{
		DocumentBuilder document = new();
		uint points = document.Variable("Points", "Strada.Vector3[]");
		FunctionBuilder count = document.Function("Count", pure: true, ["Items:int[]"], ["N:int"]);
		uint length = count.Graph.Node("Array.Length");
		uint result = count.Graph.Node("Function.Return");
		count.Graph.Link(count.Entry, "Items", length, "Array");
		count.Graph.Link(length, "Length", result, "N");
		GraphBuilder graph = document.Graph;
		// An unconnected input, a member that returns null, a variable without a Value.
		uint call = graph.Node("Function.Call", count.Settings);
		uint none = graph.Node("Array.Length");
		graph.Link(graph.Node("Call:Strada.ScriptCore.Tests.Probe.NoEntities()"), "Result", none, "Array");
		uint stored = graph.Node("Array.Length");
		graph.Link(graph.Get(points), "Value", stored, "Array");
		uint first = graph.Write(call, "N");
		graph.Flow(graph.Node("Event.Create"), first);
		uint second = graph.Write(none, "Length");
		graph.Flow(first, second);
		graph.Flow(second, graph.Write(stored, "Length"));
		// A custom event sent a null array.
		uint hit = graph.Node("Event.Custom", """{ "Name": "Hit", "Parameters": [{ "Name": "Targets", "Type": "Strada.Entity[]" }] }""");
		uint targets = graph.Node("Array.Length");
		graph.Link(hit, "Targets", targets, "Array");
		graph.Flow(hit, graph.Write(targets, "Length"));
		TestRuntime runtime = TestRuntime.Start(document);

		Vector3[] initial = Assert.IsType<Vector3[]>(runtime.Variable("Points"));
		Assert.Empty(initial);
		Assert.Same(RuntimeTesting.Library.FindType("Strada.Vector3[]")!.EmptyArray, initial);
		runtime.Invoke(ScriptEvent.Create);
		Assert.True(runtime.Runtime.SendEvent(TestRuntime.Hero, "Hit", [null]));
		Assert.Equal(["0", "0", "0", "0"], runtime.Records);

		runtime.Runtime.SetVariable(TestRuntime.Hero, "Points", typeof(Vector3[]), new[] { new Vector3(1.0f, 2.0f, 3.0f) });
		Assert.Single(Assert.IsType<Vector3[]>(runtime.Variable("Points")));
		runtime.Runtime.SetVariable(TestRuntime.Hero, "Points", typeof(Vector3[]), null);
		Assert.Empty(Assert.IsType<Vector3[]>(runtime.Variable("Points")));
		Assert.Empty(runtime.Environment.Logs);
	}

	// IsValid of an array is always true: no array of a graph is null (spec-slice3 section 3.4.2 rule 1).
	[Fact]
	public void AnArrayIsAlwaysValid()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint valid = graph.Node("IsValid");
		graph.Link(graph.Node("Call:Strada.ScriptCore.Tests.Probe.NoEntities()"), "Result", valid, "Value");
		graph.Flow(graph.Node("Event.Create"), graph.Write(valid, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["True"], runtime.Records);
	}

	[Fact]
	public void ArraysCompareElementWise()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint a = graph.Literal("int[]", "[1, 2, 3]");
		uint nan = graph.Node("Array.Make", """{ "Type": "float", "Count": 1 }""");
		graph.Link(graph.Node("Call:Strada.Mathf.Sqrt(float)", inputs: """{ "Value": -1.0 }"""), "Result", nan, "Element0");
		(string Operator, uint A, string APin, uint B, string BPin)[] comparisons =
		[
			("Operator.Equal", a, "Value", graph.Literal("int[]", "[1, 2, 3]"), "Value"),
			("Operator.Equal", a, "Value", graph.Literal("int[]", "[1, 2]"), "Value"),
			("Operator.NotEqual", a, "Value", graph.Literal("int[]", "[1, 3, 2]"), "Value"),
			// IEEE for floats: NaN equals nothing, itself included.
			("Operator.Equal", nan, "Array", nan, "Array"),
			("Operator.Equal", graph.Literal("Strada.Entity[]", """["0", "0"]"""), "Value", graph.Literal("Strada.Entity[]", """["0", "0"]"""), "Value"),
			("Operator.Equal", graph.Literal("Strada.Vector3[]", "[[1, 2, 3]]"), "Value", graph.Literal("Strada.Vector3[]", "[[1, 2, 3]]"), "Value"),
			("Operator.Equal", graph.Literal("string[]", """["a"]"""), "Value", graph.Literal("string[]", """["A"]"""), "Value"),
			("Operator.Equal", graph.Literal("int[]", "[]"), "Value", graph.Literal("int[]", "[]"), "Value"),
		];
		uint previous = graph.Node("Event.Create");
		foreach ((string op, uint left, string leftPin, uint right, string rightPin) in comparisons)
		{
			uint compare = graph.Node(op);
			graph.Link(left, leftPin, compare, "A");
			graph.Link(right, rightPin, compare, "B");
			uint write = graph.Write(compare, "Result");
			graph.Flow(previous, write);
			previous = write;
		}
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["True", "False", "True", "False", "True", "True", "False", "True"], runtime.Records);

		// Arrays of different types have no common type.
		AnalyzedDocument mixed = VisualScriptTesting.Analyze(VisualScriptTesting.Document("""
			{ "ID": 1, "Type": "Literal", "Settings": { "Type": "int[]", "Value": [1] } },
			{ "ID": 2, "Type": "Literal", "Settings": { "Type": "float[]", "Value": [1] } },
			{ "ID": 3, "Type": "Operator.Equal" }
			""", """{ "From": [1, "Value"], "To": [3, "A"] }, { "From": [2, "Value"], "To": [3, "B"] }"""));
		Assert.Equal("int[] and float[] cannot be compared: they have no common type", VisualScriptTesting.Single(mixed, DiagnosticCode.TypeMismatch).Message);
	}

	[Fact]
	public void ArraysAsTextListTheirElements()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint text = graph.Node("FormatString", """{ "Format": "{A}|{B}|{C}|{D}|{E}|{F}" }""");
		graph.Link(graph.Literal("int[]", "[1, 2, 3]"), "Value", text, "A");
		graph.Link(graph.Literal("int[]", "[]"), "Value", text, "B");
		graph.Link(graph.Literal("Strada.Entity[]", """["0", "0"]"""), "Value", text, "C");
		graph.Link(graph.Literal("float[]", "[1.5, 0.1, 1e-7]"), "Value", text, "D");
		graph.Link(graph.Literal("Strada.Vector3[]", "[[1, 2, 3]]"), "Value", text, "E");
		graph.Link(graph.Literal("Strada.KeyCode[]", """["Space", "W"]"""), "Value", text, "F");
		graph.Flow(graph.Node("Event.Create"), graph.Write(text, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["[1, 2, 3]|[]|[None, None]|[1.5, 0.1, 1E-07]|[(1, 2, 3)]|[Space, W]"], runtime.Records);
		Assert.Equal("[1, [a, None]]", Conversions.ToText(new object?[] { 1, new object?[] { "a", null } }));
		object?[] cycle = new object?[1];
		cycle[0] = cycle;
		// An array held as a System.Object that holds itself: its text is short, however many times it holds itself.
		Assert.Equal(new string('[', 16) + "[...]" + new string(']', 16), Conversions.ToText(cycle));
		object?[] many = new object?[1000];
		Array.Fill(many, many);
		Assert.InRange(Conversions.ToText(many).Length, 4096, 4096 + 32 * 16);
	}

	[Fact]
	public void ArrayVariablesLiveEditByType()
	{
		DocumentBuilder document = new();
		uint values = document.Variable("Values", "float[]", "[1, 2]");
		GraphBuilder graph = document.Graph;
		graph.Flow(graph.Node("Event.Update"), graph.Write(graph.Get(values), "Value"));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Update);
		runtime.Runtime.SetVariable(TestRuntime.Hero, "Values", typeof(float[]), new[] { 5.0f });

		// The same type keeps the value; a new Value only starts new instances.
		runtime.LoadWorking(document.Build().Replace("[1, 2]", "[3]", StringComparison.Ordinal));
		runtime.Invoke(ScriptEvent.Update);
		// float[] and int[] are other types: the variable starts at its new Value.
		runtime.LoadWorking(document.Build().Replace("\"float[]\", \"Value\": [1, 2]", "\"int[]\", \"Value\": [7, 8]", StringComparison.Ordinal));
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal(["[1, 2]", "[5]", "[7, 8]"], runtime.Records);
	}

	[Fact]
	public void PublicArraysAreNotExposable()
	{
		AnalyzedDocument document = VisualScriptTesting.Analyze(VisualScriptTesting.Document("",
			variables: """{ "ID": 1, "Name": "Points", "Type": "Strada.Vector3[]", "Public": true }"""));
		Diagnostic diagnostic = VisualScriptTesting.Single(document, DiagnosticCode.NotExposable);
		Assert.Equal(1u, diagnostic.Variable);
		Assert.Equal("Points: arrays cannot be set on components yet: keep the variable private and fill it in the graph " +
			"(Make Array from public variables, or the Children of a child entity that holds the points)", diagnostic.Message);
		// Private arrays, locals and parameters are fine.
		DocumentBuilder fine = new();
		fine.Variable("Points", "Strada.Vector3[]");
		FunctionBuilder function = fine.Function("Pick", inputs: ["From:Strada.Entity[]"], outputs: ["Picked:Strada.Entity[]"]);
		function.Local("Seen", "string[]", """["a"]""");
		fine.Graph.Node("Event.Custom", """{ "Name": "Hit", "Parameters": [{ "Name": "Damage", "Type": "float[]" }] }""");
		VisualScriptTesting.AssertNoErrors(VisualScriptTesting.Analyze(fine.Build()));
	}

	[Fact]
	public void NestedArraysAreUnknownTypes()
	{
		AnalyzedDocument variable = VisualScriptTesting.Analyze(VisualScriptTesting.Document("",
			variables: """{ "ID": 1, "Name": "Grid", "Type": "int[][]" }"""));
		Assert.Equal("Grid: \"int[][]\": arrays of arrays are not types of graphs", VisualScriptTesting.Single(variable, DiagnosticCode.UnknownType).Message);
		AnalyzedDocument literal = VisualScriptTesting.Analyze(VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Literal", "Settings": { "Type": "float[][]" } }"""));
		Assert.Equal("Type: \"float[][]\": arrays of arrays are not types of graphs", VisualScriptTesting.Single(literal, DiagnosticCode.UnknownType).Message);
		AnalyzedDocument typed = VisualScriptTesting.Analyze(VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Array.Make", "Settings": { "Type": "int[]" } }"""));
		Assert.Equal("Type: Make Array makes arrays of a type with values that is not an array, not int[]",
			VisualScriptTesting.Single(typed, DiagnosticCode.InvalidSetting).Message);
		AnalyzedDocument linked = VisualScriptTesting.Analyze(VisualScriptTesting.Document("""
			{ "ID": 1, "Type": "Literal", "Settings": { "Type": "int[]", "Value": [1] } },
			{ "ID": 2, "Type": "Array.Make", "Settings": { "Count": 1 } }
			""", """{ "From": [1, "Value"], "To": [2, "Element0"] }"""));
		Assert.Equal("Element0 is int[], so Array would be an array of arrays: arrays of arrays are not types of graphs",
			VisualScriptTesting.Single(linked, DiagnosticCode.TypeMismatch).Message);
		Assert.True(VisualScriptTesting.Pin(linked, 2, "Array").Type.IsWildcard);
	}

	// No covariance (spec-slice3 section 3.4.1): an array of a script class is no array of entities.
	[Fact]
	public void ArraysHaveNoCovariance()
	{
		DocumentBuilder document = new();
		uint movers = document.Variable("Movers", "Strada.ScriptCore.Tests.Mover[]");
		uint entities = document.Variable("Entities", "Strada.Entity[]");
		uint set = document.Graph.Set(entities);
		document.Graph.Link(document.Graph.Get(movers), "Value", set, "Value");
		AnalyzedDocument analyzed = VisualScriptTesting.Analyze(RuntimeTesting.Library, document.Build());
		Assert.Contains("cannot connect", VisualScriptTesting.Single(analyzed, DiagnosticCode.TypeMismatch).Message, StringComparison.Ordinal);
		Assert.Null(TypeRules.GetJoinConversion(RuntimeTesting.Library.FindType("Strada.ScriptCore.Tests.Mover[]")!,
			RuntimeTesting.Library.FindType("Strada.Entity[]")!));
		Assert.Null(TypeRules.Join(RuntimeTesting.Library.FindType("Strada.ScriptCore.Tests.Mover[]")!, RuntimeTesting.Library.FindType("Strada.Entity[]")!));
	}
}
