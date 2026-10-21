using System.Globalization;
using System.Threading;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The data nodes of spec section 4.4 and pure evaluation (spec section 5.3, Data evaluation).
public sealed class DataNodeTests
{
	// A graph whose Create event writes the value of the output (from, output) as text.
	private static TestRuntime WriteOnCreate(DocumentBuilder document, uint from, string output)
	{
		GraphBuilder graph = document.Graph;
		graph.Flow(graph.Node("Event.Create"), graph.Write(from, output));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Invoke(ScriptEvent.Create);
		return runtime;
	}

	[Theory]
	[InlineData("bool", "true", "True")]
	[InlineData("int", "-12", "-12")]
	[InlineData("uint", "4000000000", "4000000000")]
	[InlineData("long", "\"-9000000000\"", "-9000000000")]
	[InlineData("ulong", "\"18446744073709551615\"", "18446744073709551615")]
	[InlineData("float", "0.1", "0.1")]
	[InlineData("double", "0.1", "0.1")]
	[InlineData("string", "\"text\"", "text")]
	[InlineData("Strada.Vector2", "[1.0, 2.5]", "(1, 2.5)")]
	[InlineData("Strada.Vector3", "[1.0, 2.0, 3.0]", "(1, 2, 3)")]
	[InlineData("Strada.Vector4", "[1.0, 2.0, 3.0, 4.0]", "(1, 2, 3, 4)")]
	[InlineData("Strada.Quaternion", "[0.0, 0.0, 0.0, 2.0]", "(0, 0, 0, 1)")]
	[InlineData("Strada.Color", "[0.5, 0.25, 1.0, 2.0]", "(0.5, 0.25, 1, 2)")]
	[InlineData("Strada.ForceMode", "\"Impulse\"", "Impulse")]
	[InlineData("Strada.KeyCode", "\"Space\"", "Space")]
	[InlineData("Strada.Entity", "\"0\"", "None")]
	[InlineData("Strada.Material", "\"12\"", "Material(12)")]
	[InlineData("Strada.AssetHandle", "\"12\"", "12")]
	public void LiteralsOutputTheirValues(string type, string value, string expected)
	{
		DocumentBuilder document = new();
		TestRuntime runtime = WriteOnCreate(document, document.Graph.Literal(type, value), "Value");
		Assert.Equal([expected], runtime.Records);
	}

	[Fact]
	public void TextOfValuesUsesTheInvariantCulture()
	{
		CultureInfo culture = Thread.CurrentThread.CurrentCulture;
		try
		{
			Thread.CurrentThread.CurrentCulture = new CultureInfo("de-DE");
			DocumentBuilder document = new();
			uint text = document.Graph.Node("FormatString", """{ "Format": "{A} {B}" }""");
			document.Graph.Link(document.Graph.Literal("float", "1.5"), "Value", text, "A");
			document.Graph.Link(document.Graph.Literal("double", "-2.25"), "Value", text, "B");
			TestRuntime runtime = WriteOnCreate(document, text, "Result");
			Assert.Equal(["1.5 -2.25"], runtime.Records);
		}
		finally
		{
			Thread.CurrentThread.CurrentCulture = culture;
		}
	}

	[Fact]
	public void SelfIsResolvedThroughTheEnvironmentAtEveryEvaluation()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint self = graph.Node("Self");
		uint first = graph.Write(self, "Self");
		uint second = graph.Write(self, "Self");
		graph.Flow(graph.Node("Event.Create"), first);
		graph.Flow(first, second);
		TestRuntime runtime = TestRuntime.Start(document);
		int lookups = runtime.Environment.EntityLookups;

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["Entity(7)", "Entity(7)"], runtime.Records);
		Assert.Equal(lookups + 2, runtime.Environment.EntityLookups);
	}

	[Fact]
	public void SelfIsTheScriptInstanceOfAnEntityThatHasOne()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint self = graph.Node("Self");
		uint mover = graph.Node("Convert", """{ "To": "Strada.ScriptCore.Tests.Mover" }""");
		uint jumper = graph.Node("Convert", """{ "To": "Strada.ScriptCore.Tests.Jumper" }""");
		uint text = graph.Node("FormatString", """{ "Format": "{Mover} {Jumper}" }""");
		graph.Link(self, "Self", mover, "Value");
		graph.Link(self, "Self", jumper, "Value");
		graph.Link(mover, "Result", text, "Mover");
		graph.Link(jumper, "Result", text, "Jumper");
		graph.Flow(graph.Node("Event.Create"), graph.Write(text, "Result"));
		TestRuntime runtime = new();
		runtime.Environment.Add(TestRuntime.Hero, "Hero", new Mover());
		runtime.LoadWorking(document.Build());
		runtime.Create();

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["Entity(7) None"], runtime.Records);
	}

	[Fact]
	public void PureNodesAreEvaluatedOncePerEvaluationOfTheirConsumer()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint next = graph.Node(RuntimeTesting.Next);
		uint sum = graph.Node("Operator.Add");
		graph.Link(next, "Result", sum, "A");
		graph.Link(next, "Result", sum, "B");
		uint first = graph.Write(sum, "Result");
		uint second = graph.Write(next, "Result");
		graph.Flow(graph.Node("Event.Create"), first);
		graph.Flow(first, second);
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["2", "2"], runtime.Records);
		Assert.Equal(2, Probe.Evaluations);
	}

	[Fact]
	public void SelectEvaluatesBothInputs()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint select = graph.Node("Select", inputs: """{ "Condition": true }""");
		graph.Link(graph.Node(RuntimeTesting.Next), "Result", select, "True");
		graph.Link(graph.Node(RuntimeTesting.Next), "Result", select, "False");
		TestRuntime runtime = WriteOnCreate(document, select, "Result");
		Assert.Equal(["1"], runtime.Records);
		Assert.Equal(2, Probe.Evaluations);
	}

	[Fact]
	public void SelectJoinsItsInputsTypes()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint select = graph.Node("Select", inputs: """{ "Condition": false }""");
		graph.Link(graph.Literal("int", "3"), "Value", select, "True");
		graph.Link(graph.Literal("float", "0.5"), "Value", select, "False");
		uint twice = graph.Node("Operator.Multiply", inputs: """{ "B": 2 }""");
		graph.Link(select, "Result", twice, "A");
		TestRuntime runtime = WriteOnCreate(document, twice, "Result");
		Assert.Equal(["1"], runtime.Records);
	}

	[Theory]
	[InlineData("Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)", """{ "Id": "0" }""", "False")]
	[InlineData("Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)", """{ "Id": "7" }""", "True")]
	[InlineData("Call:Strada.ScriptCore.Tests.Probe.EntityOf(ulong)", """{ "Id": "99" }""", "False")]
	[InlineData("Call:Strada.ScriptCore.Tests.Probe.MaterialOf(ulong)", """{ "Handle": "0" }""", "False")]
	[InlineData("Call:Strada.ScriptCore.Tests.Probe.MaterialOf(ulong)", """{ "Handle": "5" }""", "True")]
	[InlineData("Call:Strada.ScriptCore.Tests.Probe.NoHolder()", null, "False")]
	[InlineData("Call:Strada.ScriptCore.Tests.Probe.Entities(int)", """{ "Count": 0 }""", "True")]
	public void IsValidJudgesReferencesByTheirType(string source, string? inputs, string expected)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint valid = graph.Node("IsValid");
		graph.Link(graph.Node(source, inputs: inputs), "Result", valid, "Value");
		TestRuntime runtime = WriteOnCreate(document, valid, "Result");
		Assert.Equal([expected], runtime.Records);
	}

	[Fact]
	public void AComponentIsValidWhileItsEntityHasIt()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint valid = graph.Node("IsValid");
		uint view = graph.Node("Call:Strada.ScriptCore.Tests.Probe.ScriptComponentOf(Strada.Entity)");
		graph.Link(graph.Node("Self"), "Self", view, "Entity");
		graph.Link(view, "Result", valid, "Value");
		graph.Flow(graph.Node("Event.Update"), graph.Write(valid, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Update);
		runtime.Environment[TestRuntime.Hero].Components.Add(typeof(ScriptComponent));
		runtime.Invoke(ScriptEvent.Update);
		runtime.Environment[TestRuntime.Hero].Valid = false;
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal(["False", "True", "False"], runtime.Records);
	}

	[Theory]
	[InlineData("float", "2.75", "int", "2")]
	[InlineData("float", "-2.75", "int", "-2")]
	[InlineData("double", "1e20", "int", "2147483647")]
	[InlineData("double", "-1e20", "long", "-9223372036854775808")]
	[InlineData("double", "-3.5", "uint", "0")]
	[InlineData("double", "1e30", "ulong", "18446744073709551615")]
	[InlineData("long", "\"5000000000\"", "int", "705032704")]
	[InlineData("int", "-1", "uint", "4294967295")]
	[InlineData("int", "-1", "ulong", "18446744073709551615")]
	[InlineData("int", "16777217", "float", "16777216")]
	[InlineData("double", "0.1", "float", "0.1")]
	[InlineData("Strada.ForceMode", "\"Acceleration\"", "int", "2")]
	[InlineData("int", "1", "Strada.ForceMode", "Impulse")]
	[InlineData("int", "77", "Strada.ForceMode", "77")]
	[InlineData("string", "\"42\"", "int", "42")]
	[InlineData("string", "\" -7 \"", "long", "-7")]
	[InlineData("string", "\"2.5\"", "float", "2.5")]
	[InlineData("string", "\"TRUE\"", "bool", "True")]
	[InlineData("string", "\"false\"", "bool", "False")]
	[InlineData("string", "\"VelocityChange\"", "Strada.ForceMode", "VelocityChange")]
	[InlineData("Strada.Vector3", "[1.0, 2.0, 3.0]", "string", "(1, 2, 3)")]
	[InlineData("bool", "true", "string", "True")]
	[InlineData("Strada.Material", "\"3\"", "Strada.AssetHandle", "3")]
	[InlineData("Strada.Material", "\"0\"", "Strada.AssetHandle", "0")]
	[InlineData("Strada.Color", "[1.0, 0.5, 0.25, 1.0]", "Strada.Vector4", "(1, 0.5, 0.25, 1)")]
	public void ConvertConvertsValues(string fromType, string value, string toType, string expected)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint convert = graph.Node("Convert", $$"""{ "To": {{RuntimeTesting.Quote(toType)}} }""");
		graph.Link(graph.Literal(fromType, value), "Value", convert, "Value");
		TestRuntime runtime = WriteOnCreate(document, convert, "Result");
		Assert.Equal([expected], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Fact]
	public void ConvertNaNToAnIntegerGivesZero()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint nan = graph.Node("Operator.Divide", inputs: """{ "B": 0.0 }""");
		graph.Link(graph.Literal("float", "0.0"), "Value", nan, "A");
		uint convert = graph.Node("Convert", """{ "To": "int" }""");
		graph.Link(nan, "Result", convert, "Value");
		TestRuntime runtime = WriteOnCreate(document, convert, "Result");
		Assert.Equal(["0"], runtime.Records);
	}

	[Fact]
	public void ConvertFromUnreadableTextGivesTheTypeDefaultAndWarnsOncePerNodeAndInstance()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint convert = graph.Node("Convert", """{ "To": "int" }""");
		graph.Link(graph.Literal("string", "\"4x\""), "Value", convert, "Value");
		graph.Flow(graph.Node("Event.Update"), graph.Write(convert, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);
		runtime.Environment.Add(8, "Villain");
		runtime.Create(8);

		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update);
		runtime.Invoke(ScriptEvent.Update, entity: 8);
		Assert.Equal(["0", "0", "0"], runtime.Records);
		Assert.Equal(
		[
			TestRuntime.On("To Int", convert) + "\"4x\" cannot be read as Int: the result is 0",
			System.FormattableString.Invariant($"{TestRuntime.Name}: To Int (node {convert}) on Villain (8): \"4x\" cannot be read as Int: the result is 0"),
		], runtime.Environment.Warnings);
		Assert.Empty(runtime.Environment.Errors);
	}

	[Fact]
	public void MakeAndBreakBuildAndSplitStructs()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint make = graph.Node("Make", """{ "Type": "Strada.Vector3" }""", """{ "X": 1.0, "Y": 2.0, "Z": 3.0 }""");
		uint split = graph.Node("Break", """{ "Type": "Strada.Vector3" }""");
		uint zero = graph.Node("Make", """{ "Type": "Strada.Quaternion" }""");
		uint pair = graph.Node("Make", """{ "Type": "Strada.ScriptCore.Tests.Pair" }""", """{ "First": 2, "Second": 5 }""");
		uint sum = graph.Node("Call:Strada.ScriptCore.Tests.Pair.Sum()");
		uint hit = graph.Node("Call:Strada.ScriptCore.Tests.Probe.HitOf(Strada.Entity,float)", inputs: """{ "Distance": 4.5 }""");
		uint hitParts = graph.Node("Break", """{ "Type": "Strada.RaycastHit" }""");
		uint handle = graph.Node("Break", """{ "Type": "Strada.AssetHandle" }""");
		uint text = graph.Node("FormatString", """{ "Format": "{Vector} {Y} {Zero} {Sum} {Entity} {Distance} {Point} {ID} {IsValid}" }""");
		graph.Link(make, "Value", split, "Value");
		graph.Link(make, "Value", text, "Vector");
		graph.Link(split, "Y", text, "Y");
		graph.Link(zero, "Value", text, "Zero");
		graph.Link(pair, "Value", sum, "Target");
		graph.Link(sum, "Result", text, "Sum");
		graph.Link(graph.Node("Self"), "Self", hit, "Entity");
		graph.Link(hit, "Result", hitParts, "Value");
		graph.Link(hitParts, "Entity", text, "Entity");
		graph.Link(hitParts, "Distance", text, "Distance");
		graph.Link(hitParts, "Point", text, "Point");
		graph.Link(graph.Literal("Strada.AssetHandle", "\"9\""), "Value", handle, "Value");
		graph.Link(handle, "ID", text, "ID");
		graph.Link(handle, "IsValid", text, "IsValid");
		TestRuntime runtime = WriteOnCreate(document, text, "Result");
		// Make does not normalize: a quaternion of zeros stays one.
		Assert.Equal(["(1, 2, 3) 2 (0, 0, 0, 0) 7 Entity(7) 4.5 (0, 1, 0) 9 True"], runtime.Records);
	}

	[Fact]
	public void FormatStringInsertsItsPlaceholders()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint text = graph.Node("FormatString", """{ "Format": "{{{Count}}} of {Total}, {Name}{Count}!" }""", """{ "Total": "ten" }""");
		graph.Link(graph.Literal("int", "3"), "Value", text, "Count");
		TestRuntime runtime = WriteOnCreate(document, text, "Result");
		// An unconnected placeholder without a value is text, "" by default.
		Assert.Equal(["{3} of ten, 3!"], runtime.Records);
	}

	[Fact]
	public void ArrayNodesReadLengthsAndElements()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint entities = graph.Node("Call:Strada.ScriptCore.Tests.Probe.Entities(int)", inputs: """{ "Count": 3 }""");
		uint points = graph.Node("Call:Strada.ScriptCore.Tests.Probe.Points(int)", inputs: """{ "Count": 3 }""");
		uint length = graph.Node("Array.Length");
		uint entity = graph.Node("Array.Get", inputs: """{ "Index": 1 }""");
		uint point = graph.Node("Array.Get", inputs: """{ "Index": 2 }""");
		uint text = graph.Node("FormatString", """{ "Format": "{Length} {Entity} {Point}" }""");
		graph.Link(entities, "Result", length, "Array");
		graph.Link(entities, "Result", entity, "Array");
		graph.Link(points, "Result", point, "Array");
		graph.Link(length, "Length", text, "Length");
		graph.Link(entity, "Element", text, "Entity");
		graph.Link(point, "Element", text, "Point");
		TestRuntime runtime = WriteOnCreate(document, text, "Result");
		Assert.Equal(["3 Entity(2) (2, 4, 6)"], runtime.Records);
	}
}
