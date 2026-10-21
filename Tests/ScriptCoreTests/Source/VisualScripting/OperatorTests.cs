using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The operators (spec section 4.4): numeric joins with C# unchecked arithmetic, text, operator methods, comparisons, and
// the equality table.
public sealed class OperatorTests
{
	// The result of operator(a, b) on literals of the types, as text; b null for unary operators.
	private static TestRuntime Evaluate(string op, string typeA, string a, string? typeB = null, string? b = null)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint node = graph.Node(op);
		graph.Link(graph.Literal(typeA, a), "Value", node, "A");
		if (typeB != null)
		{
			graph.Link(graph.Literal(typeB, b!), "Value", node, "B");
		}
		graph.Flow(graph.Node("Event.Create"), graph.Write(node, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		return runtime;
	}

	[Theory]
	[InlineData("Operator.Add", "int", "2", "int", "3", "5")]
	[InlineData("Operator.Add", "int", "-1", "uint", "4000000000", "3999999999")]
	[InlineData("Operator.Add", "int", "2147483647", "int", "1", "-2147483648")]
	[InlineData("Operator.Subtract", "uint", "0", "uint", "1", "4294967295")]
	[InlineData("Operator.Multiply", "long", "\"4611686018427387904\"", "int", "4", "0")]
	[InlineData("Operator.Multiply", "int", "3", "float", "0.5", "1.5")]
	[InlineData("Operator.Divide", "int", "7", "int", "-2", "-3")]
	[InlineData("Operator.Modulo", "int", "-7", "int", "3", "-1")]
	[InlineData("Operator.Divide", "ulong", "\"18446744073709551615\"", "uint", "5", "3689348814741910323")]
	[InlineData("Operator.Divide", "float", "1.0", "float", "0.0", "Infinity")]
	[InlineData("Operator.Modulo", "double", "7.5", "double", "2.0", "1.5")]
	[InlineData("Operator.Add", "float", "0.5", "double", "0.25", "0.75")]
	[InlineData("Operator.Add", "string", "\"a\"", "int", "5", "a5")]
	[InlineData("Operator.Add", "Strada.Vector3", "[1.0, 2.0, 3.0]", "string", "\"!\"", "(1, 2, 3)!")]
	[InlineData("Operator.Add", "Strada.Vector3", "[1.0, 2.0, 3.0]", "Strada.Vector3", "[4.0, 5.0, 6.0]", "(5, 7, 9)")]
	[InlineData("Operator.Multiply", "Strada.Vector3", "[1.0, 2.0, 3.0]", "float", "2.0", "(2, 4, 6)")]
	[InlineData("Operator.Multiply", "Strada.Vector3", "[1.0, 2.0, 3.0]", "int", "2", "(2, 4, 6)")]
	[InlineData("Operator.Multiply", "float", "3.0", "Strada.Vector3", "[1.0, 2.0, 3.0]", "(3, 6, 9)")]
	[InlineData("Operator.Subtract", "Strada.Vector2", "[1.0, 2.0]", "Strada.Vector2", "[0.5, 0.5]", "(0.5, 1.5)")]
	public void ArithmeticWorksOnTheJoinTypeOrTheOperatorMethod(string op, string typeA, string a, string typeB, string b, string expected)
	{
		TestRuntime runtime = Evaluate(op, typeA, a, typeB, b);
		Assert.Equal([expected], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Theory]
	[InlineData("int", "-5", "5")]
	[InlineData("int", "-2147483648", "-2147483648")]
	[InlineData("uint", "4000000000", "-4000000000")]
	[InlineData("double", "0.5", "-0.5")]
	[InlineData("Strada.Vector3", "[1.0, -2.0, 0.0]", "(-1, 2, -0)")]
	public void NegateFollowsCSharp(string type, string value, string expected)
	{
		TestRuntime runtime = Evaluate("Operator.Negate", type, value);
		Assert.Equal([expected], runtime.Records);
	}

	[Theory]
	[InlineData("Operator.Divide", "int", "1", "int", "0", "integer division by zero")]
	[InlineData("Operator.Modulo", "long", "\"1\"", "int", "0", "integer modulo by zero")]
	[InlineData("Operator.Divide", "uint", "1", "uint", "0", "integer division by zero")]
	[InlineData("Operator.Divide", "int", "-2147483648", "int", "-1", "integer overflow: the smallest int divided by -1")]
	[InlineData("Operator.Modulo", "long", "\"-9223372036854775808\"", "int", "-1", "integer overflow: the remainder of the smallest long divided by -1")]
	public void IntegerDivisionByZeroOrOverflowAborts(string op, string typeA, string a, string typeB, string b, string message)
	{
		TestRuntime runtime = Evaluate(op, typeA, a, typeB, b);
		Assert.Empty(runtime.Records);
		string title = op == "Operator.Divide" ? "Divide" : "Modulo";
		Assert.Equal(TestRuntime.On(title, 1) + message, runtime.SingleError());
	}

	[Theory]
	[InlineData("Operator.Less", "int", "2", "long", "\"3\"", "True")]
	[InlineData("Operator.LessOrEqual", "uint", "3", "int", "3", "True")]
	[InlineData("Operator.Greater", "float", "0.5", "int", "1", "False")]
	[InlineData("Operator.GreaterOrEqual", "double", "1.0", "float", "1.0", "True")]
	[InlineData("Operator.Less", "ulong", "\"18446744073709551615\"", "uint", "1", "False")]
	public void ComparisonsJoinNumbers(string op, string typeA, string a, string typeB, string b, string expected)
	{
		TestRuntime runtime = Evaluate(op, typeA, a, typeB, b);
		Assert.Equal([expected], runtime.Records);
	}

	[Theory]
	[InlineData("Operator.And", "true", "false", "False")]
	[InlineData("Operator.Or", "true", "false", "True")]
	[InlineData("Operator.Xor", "true", "true", "False")]
	public void BooleanOperatorsCombineBothOperands(string op, string a, string b, string expected)
	{
		TestRuntime runtime = Evaluate(op, "bool", a, "bool", b);
		Assert.Equal([expected], runtime.Records);
	}

	[Fact]
	public void NotInvertsItsOperand()
	{
		TestRuntime runtime = Evaluate("Operator.Not", "bool", "false");
		Assert.Equal(["True"], runtime.Records);
	}

	[Fact]
	public void AStoredOperandTakesTheLinkedOperandsType()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint add = graph.Node("Operator.Add", inputs: """{ "B": "1" }""");
		graph.Link(graph.Literal("long", "\"9000000000\""), "Value", add, "A");
		uint less = graph.Node("Operator.Less", inputs: """{ "A": 0.5 }""");
		graph.Link(graph.Literal("double", "1.0"), "Value", less, "B");
		uint text = graph.Node("FormatString", """{ "Format": "{Sum} {Less}" }""");
		graph.Link(add, "Result", text, "Sum");
		graph.Link(less, "Result", text, "Less");
		graph.Flow(graph.Node("Event.Create"), graph.Write(text, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["9000000001 True"], runtime.Records);
	}

	[Theory]
	[InlineData("int", "1", "float", "1.0", "True")]
	[InlineData("uint", "4000000000", "long", "\"4000000000\"", "True")]
	[InlineData("bool", "true", "bool", "false", "False")]
	[InlineData("string", "\"a\"", "string", "\"A\"", "False")]
	[InlineData("string", "\"é\"", "string", "\"é\"", "True")]
	[InlineData("Strada.ForceMode", "\"Impulse\"", "Strada.ForceMode", "1", "True")]
	[InlineData("Strada.Vector3", "[1.0, 2.0, 3.0]", "Strada.Vector3", "[1.0, 2.0, 3.0]", "True")]
	[InlineData("Strada.Material", "\"5\"", "Strada.Material", "\"5\"", "True")]
	[InlineData("Strada.Material", "\"5\"", "Strada.Material", "\"0\"", "False")]
	[InlineData("Strada.Material", "\"0\"", "Strada.Material", "\"0\"", "True")]
	[InlineData("Strada.Entity", "\"0\"", "Strada.Entity", "\"0\"", "True")]
	[InlineData("Strada.AssetHandle", "\"5\"", "Strada.AssetHandle", "\"5\"", "True")]
	public void EqualityFollowsTheTable(string typeA, string a, string typeB, string b, string expected)
	{
		TestRuntime equal = Evaluate("Operator.Equal", typeA, a, typeB, b);
		Assert.Equal([expected], equal.Records);
		TestRuntime notEqual = Evaluate("Operator.NotEqual", typeA, a, typeB, b);
		Assert.Equal([expected == "True" ? "False" : "True"], notEqual.Records);
	}

	[Fact]
	public void NaNEqualsNothing()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint nan = graph.Node("Operator.Divide", inputs: """{ "B": 0.0 }""");
		graph.Link(graph.Literal("double", "0.0"), "Value", nan, "A");
		uint equal = graph.Node("Operator.Equal");
		graph.Link(nan, "Result", equal, "A");
		graph.Link(nan, "Result", equal, "B");
		graph.Flow(graph.Node("Event.Create"), graph.Write(equal, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["False"], runtime.Records);
	}

	// Two operands made by probes, compared: entity, asset and component equality (spec section 4.4).
	private static TestRuntime Compare(string sourceA, string? inputsA, string sourceB, string? inputsB, bool selfIsMover = false)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint equal = graph.Node("Operator.Equal");
		uint a = graph.Node(sourceA, inputs: inputsA);
		uint b = graph.Node(sourceB, inputs: inputsB);
		graph.Link(a, sourceA == "Self" ? "Self" : "Result", equal, "A");
		graph.Link(b, sourceB == "Self" ? "Self" : "Result", equal, "B");
		foreach (uint node in new[] { a, b })
		{
			string source = node == a ? sourceA : sourceB;
			if (source.Contains("(Strada.Entity)", System.StringComparison.Ordinal))
			{
				graph.Link(graph.Node("Self"), "Self", node, "Entity");
			}
		}
		graph.Flow(graph.Node("Event.Create"), graph.Write(equal, "Result"));
		TestRuntime runtime = new();
		if (selfIsMover)
		{
			runtime.Environment.Add(TestRuntime.Hero, "Hero", new Mover());
		}
		runtime.LoadWorking(document.Build());
		runtime.Create();
		runtime.Invoke(ScriptEvent.Create);
		return runtime;
	}

	private const string EntityOf = "Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)";
	private const string ScriptComponentOf = "Call:Strada.ScriptCore.Tests.Probe.ScriptComponentOf(Strada.Entity)";
	private const string TransformOf = "Call:Strada.ScriptCore.Tests.Probe.TransformOf(Strada.Entity)";

	[Fact]
	public void EntitiesAndScriptInstancesCompareByID()
	{
		Assert.Equal(["True"], Compare("Self", null, EntityOf, """{ "Id": "7" }""", selfIsMover: true).Records);
		Assert.Equal(["False"], Compare("Self", null, EntityOf, """{ "Id": "8" }""").Records);
		Assert.Equal(["False"], Compare("Self", null, EntityOf, """{ "Id": "0" }""").Records);
	}

	[Fact]
	public void AssetsCompareByTypeAndHandle()
	{
		Assert.Equal(["True"], Compare("Call:Strada.ScriptCore.Tests.Probe.MaterialOf(ulong)", """{ "Handle": "4" }""",
			"Call:Strada.ScriptCore.Tests.Probe.MaterialOf(ulong)", """{ "Handle": "4" }""").Records);
		Assert.Equal(["False"], Compare("Call:Strada.ScriptCore.Tests.Probe.MaterialOf(ulong)", """{ "Handle": "4" }""",
			"Call:Strada.ScriptCore.Tests.Probe.MeshOf(ulong)", """{ "Handle": "4" }""").Records);
	}

	[Fact]
	public void TwoViewsOfOneComponentAreEqual()
	{
		Assert.Equal(["True"], Compare(ScriptComponentOf, null, ScriptComponentOf, null).Records);
		Assert.Equal(["False"], Compare(ScriptComponentOf, null, TransformOf, null).Records);
	}

	[Fact]
	public void StructsWithoutAnEqualityOperatorCompareTheirValues()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint equal = graph.Node("Operator.Equal");
		uint different = graph.Node("Operator.Equal");
		uint first = graph.Node("Make", """{ "Type": "Strada.ScriptCore.Tests.Pair" }""", """{ "First": 1, "Second": 2 }""");
		uint second = graph.Node("Make", """{ "Type": "Strada.ScriptCore.Tests.Pair" }""", """{ "First": 1, "Second": 2 }""");
		uint third = graph.Node("Make", """{ "Type": "Strada.ScriptCore.Tests.Pair" }""", """{ "First": 2, "Second": 1 }""");
		graph.Link(first, "Value", equal, "A");
		graph.Link(second, "Value", equal, "B");
		graph.Link(first, "Value", different, "A");
		graph.Link(third, "Value", different, "B");
		uint text = graph.Node("FormatString", """{ "Format": "{Same} {Different}" }""");
		graph.Link(equal, "Result", text, "Same");
		graph.Link(different, "Result", text, "Different");
		graph.Flow(graph.Node("Event.Create"), graph.Write(text, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["True False"], runtime.Records);
	}

	[Fact]
	public void ClassesWithoutAnEqualityOperatorCompareReferences()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint create = graph.Node("Event.Create");
		uint first = graph.Node("New:Strada.ScriptCore.Tests.Holder()");
		uint second = graph.Node("New:Strada.ScriptCore.Tests.Holder()");
		uint same = graph.Node("Operator.Equal");
		uint different = graph.Node("Operator.Equal");
		graph.Link(first, "Value", same, "A");
		graph.Link(first, "Value", same, "B");
		graph.Link(first, "Value", different, "A");
		graph.Link(second, "Value", different, "B");
		uint text = graph.Node("FormatString", """{ "Format": "{Same} {Different}" }""");
		graph.Link(same, "Result", text, "Same");
		graph.Link(different, "Result", text, "Different");
		graph.Flow(create, first);
		graph.Flow(first, second);
		graph.Flow(second, graph.Write(text, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["True False"], runtime.Records);
	}
}
