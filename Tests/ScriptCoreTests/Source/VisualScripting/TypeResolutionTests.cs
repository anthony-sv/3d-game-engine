using System.Linq;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Wildcard resolution (spec section 3.5), the operators (section 4.4), Convert and the conversions of links (section 3.4).
public sealed class TypeResolutionTests
{
	private static NodeLibrary Library => VisualScriptTesting.Library;

	private static VisualType Type(string id) => Library.FindType(id)!;

	// Variables of the given types (IDs 21, 22, ...) and a Variable.Get of each (node IDs 11, 12, ...), plus the given nodes
	// (IDs below 11) and links.
	private static AnalyzedDocument WithVariables(string[] types, string nodes, string links)
	{
		string variables = string.Join(", ", types.Select(static (type, index) => $$"""{ "ID": {{index + 21}}, "Name": "V{{index + 1}}", "Type": "{{type}}" }"""));
		string gets = string.Join(", ", types.Select(static (_, index) => $$"""{ "ID": {{index + 11}}, "Type": "Variable.Get", "Settings": { "Variable": {{index + 21}} } }"""));
		return VisualScriptTesting.Analyze(VisualScriptTesting.Document(gets + (nodes.Length > 0 ? ", " + nodes : ""), links, variables));
	}

	private static AnalyzedDocument Binary(string op, string a, string b) =>
		WithVariables([a, b], $$"""{ "ID": 1, "Type": "{{op}}" }""", """{ "From": [11, "Value"], "To": [1, "A"] }, { "From": [12, "Value"], "To": [1, "B"] }""");

	[Fact]
	public void NumericOperandsJoin()
	{
		AnalyzedDocument mixed = Binary("Operator.Add", "int", "uint");
		VisualScriptTesting.AssertNoErrors(mixed);
		Assert.Equal("long", VisualScriptTesting.Pin(mixed, 1, "Result").Type.Id);
		Assert.Equal(new OperatorBinding(OperatorMode.Numeric, Type("long"), null), VisualScriptTesting.Node(mixed, 1).Binding);
		Assert.Equal(ConversionKind.Identity, VisualScriptTesting.Pin(mixed, 1, "A", PinDirection.In).Source!.Conversion);

		Assert.Equal("float", VisualScriptTesting.Pin(Binary("Operator.Multiply", "int", "float"), 1, "Result").Type.Id);
		Assert.Equal("double", VisualScriptTesting.Pin(Binary("Operator.Subtract", "float", "double"), 1, "Result").Type.Id);
		Assert.Equal("ulong", VisualScriptTesting.Pin(Binary("Operator.Add", "uint", "ulong"), 1, "Result").Type.Id);
		// Two integer types join only to an integer type, as in C#.
		VisualScriptTesting.Single(Binary("Operator.Add", "long", "ulong"), DiagnosticCode.TypeMismatch);

		AnalyzedDocument unsigned = Binary("Operator.Add", "int", "ulong");
		Diagnostic mismatch = VisualScriptTesting.Single(unsigned, DiagnosticCode.TypeMismatch);
		Assert.Equal(1u, mismatch.Node);
		Assert.True(VisualScriptTesting.Pin(unsigned, 1, "Result").Type.IsWildcard);

		Assert.Equal(Type("int"), TypeRules.Join(Type("int"), Type("int")));
		Assert.Equal(Type("long"), TypeRules.Join(Type("uint"), Type("int")));
		Assert.Null(TypeRules.Join(Type("int"), Type("ulong")));
		Assert.Null(TypeRules.Join(Type("long"), Type("ulong")));
		Assert.Equal(Type("double"), TypeRules.Join(Type("long"), Type("double")));
	}

	[Fact]
	public void ReferencesJoinAtTheirNearestCommonBaseClass()
	{
		Assert.Equal(Type("Strada.Asset"), TypeRules.Join(Type("Strada.Mesh"), Type("Strada.Material")));
		Assert.Equal(Type("Strada.Component"), TypeRules.Join(Type("Strada.RigidBodyComponent"), Type("Strada.BoxColliderComponent")));
		Assert.Equal(Type("Strada.Entity"), TypeRules.Join(Type("Strada.Script"), Type("Strada.Entity")));
		Assert.Equal(Type("System.Object"), TypeRules.Join(Type("Strada.Entity"), Type("System.Object")));
		// Unrelated classes do not join (rule 7, to string, never applies to a join).
		Assert.Null(TypeRules.Join(Type("Strada.Entity"), Type("Strada.Material")));
		Assert.Null(TypeRules.Join(Type("string"), Type("Strada.Entity")));
		Assert.Null(TypeRules.Join(Type("int"), Type("string")));
		Assert.Null(TypeRules.Join(Type("Strada.Vector3"), Type("Strada.Vector2")));
		Assert.Null(TypeRules.Join(VisualType.Wildcard, VisualType.Wildcard));
	}

	[Fact]
	public void SelectJoinsItsInputs()
	{
		string nodes = """{ "ID": 1, "Type": "Select" }""";
		string links = """{ "From": [11, "Value"], "To": [1, "True"] }, { "From": [12, "Value"], "To": [1, "False"] }""";
		AnalyzedDocument numbers = WithVariables(["int", "float"], nodes, links);
		VisualScriptTesting.AssertNoErrors(numbers);
		Assert.Equal("float", VisualScriptTesting.Pin(numbers, 1, "Result").Type.Id);
		Assert.Equal(ConversionKind.Widen, VisualScriptTesting.Pin(numbers, 1, "True", PinDirection.In).Source!.Conversion);

		AnalyzedDocument assets = WithVariables(["Strada.Mesh", "Strada.Material"], nodes, links);
		VisualScriptTesting.AssertNoErrors(assets);
		Assert.Equal("Strada.Asset", VisualScriptTesting.Pin(assets, 1, "Result").Type.Id);
		Assert.Equal(ConversionKind.Upcast, VisualScriptTesting.Pin(assets, 1, "False", PinDirection.In).Source!.Conversion);

		AnalyzedDocument unrelated = WithVariables(["string", "Strada.Entity"], nodes, links);
		Assert.Equal(1u, VisualScriptTesting.Single(unrelated, DiagnosticCode.TypeMismatch).Node);

		// One linked input resolves the group; the other's stored value decodes with that type.
		AnalyzedDocument literal = WithVariables(["float"], """{ "ID": 1, "Type": "Select", "Inputs": { "False": 2.5 } }""",
			"""{ "From": [11, "Value"], "To": [1, "True"] }""");
		VisualScriptTesting.AssertNoErrors(literal);
		Assert.Equal(2.5f, VisualScriptTesting.Pin(literal, 1, "False", PinDirection.In).Value);
		// With nothing linked, the group has no type.
		AnalyzedDocument unlinked = VisualScriptTesting.Analyze(VisualScriptTesting.Document("""{ "ID": 1, "Type": "Select" }"""));
		Assert.Equal(["True", "False"], unlinked.Diagnostics.Where(static diagnostic => diagnostic.Code == DiagnosticCode.UnresolvedWildcard)
			.Select(static diagnostic => diagnostic.Pin));
	}

	[Fact]
	public void AStoredOperandTakesTheLinkedOperandsType()
	{
		AnalyzedDocument integer = WithVariables(["int"], """{ "ID": 1, "Type": "Operator.Add", "Inputs": { "B": 1 } }""",
			"""{ "From": [11, "Value"], "To": [1, "A"] }""");
		VisualScriptTesting.AssertNoErrors(integer);
		Assert.Equal("int", VisualScriptTesting.Pin(integer, 1, "B", PinDirection.In).Type.Id);
		Assert.Equal(1, VisualScriptTesting.Pin(integer, 1, "B", PinDirection.In).Value);
		Assert.Equal("int", VisualScriptTesting.Pin(integer, 1, "Result").Type.Id);

		AnalyzedDocument real = WithVariables(["float"], """{ "ID": 1, "Type": "Operator.Less", "Inputs": { "A": 1 } }""",
			"""{ "From": [11, "Value"], "To": [1, "B"] }""");
		VisualScriptTesting.AssertNoErrors(real);
		Assert.Equal(1.0f, VisualScriptTesting.Pin(real, 1, "A", PinDirection.In).Value);

		AnalyzedDocument fractional = WithVariables(["int"], """{ "ID": 1, "Type": "Operator.Add", "Inputs": { "B": 1.5 } }""",
			"""{ "From": [11, "Value"], "To": [1, "A"] }""");
		Assert.Equal("B", VisualScriptTesting.Single(fractional, DiagnosticCode.InvalidValue).Pin);

		// Without a stored value the other operand stays unresolved.
		AnalyzedDocument missing = WithVariables(["int"], """{ "ID": 1, "Type": "Operator.Add" }""", """{ "From": [11, "Value"], "To": [1, "A"] }""");
		Assert.Equal("B", VisualScriptTesting.Single(missing, DiagnosticCode.UnresolvedWildcard).Pin);
	}

	[Fact]
	public void UnconnectedFormatStringPlaceholdersAreText()
	{
		AnalyzedDocument document = WithVariables(["float"],
			"""{ "ID": 1, "Type": "FormatString", "Settings": { "Format": "{Who} is {{at}} {Where} and {Who}" }, "Inputs": { "Who": "Probe" } }""",
			"""{ "From": [11, "Value"], "To": [1, "Where"] }""");
		VisualScriptTesting.AssertNoErrors(document);
		AnalyzedNode node = VisualScriptTesting.Node(document, 1);
		Assert.Equal(["In:Who:string", "In:Where:float", "Out:Result:string"], node.Pins.Select(static pin => $"{pin.Direction}:{pin.Name}:{pin.Type.Id}"));
		FormatBinding binding = Assert.IsType<FormatBinding>(node.Binding);
		Assert.Equal(["Who", "Where"], binding.Placeholders);
		Assert.Equal([new FormatSegment(null, 0), new FormatSegment(" is {at} ", -1), new FormatSegment(null, 1), new FormatSegment(" and ", -1),
			new FormatSegment(null, 0)], binding.Segments);
		Assert.Equal("Probe", node.FindPin("Who", PinDirection.In)!.Value);

		foreach (string format in new[] { "{Who", "Who}", "{}", "{a.b}", "{ x}" })
		{
			AnalyzedDocument broken = VisualScriptTesting.Analyze(VisualScriptTesting.Document(
				$$"""{ "ID": 1, "Type": "FormatString", "Settings": { "Format": "{{format}}" } }"""));
			Assert.True(VisualScriptTesting.Has(broken, DiagnosticCode.InvalidSetting) || VisualScriptTesting.Has(broken, DiagnosticCode.InvalidName), format);
		}
		AnalyzedDocument reserved = VisualScriptTesting.Analyze(VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "FormatString", "Settings": { "Format": "{Then}" } }"""));
		VisualScriptTesting.Single(reserved, DiagnosticCode.ReservedName);
	}

	[Fact]
	public void ReroutesPassTheirTypeOn()
	{
		AnalyzedDocument exec = VisualScriptTesting.Analyze(VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Event.Update" }, { "ID": 2, "Type": "Reroute" }, { "ID": 3, "Type": "Flow.Branch", "Inputs": { "Condition": true } }""",
			"""{ "From": [1, "Then"], "To": [2, "In"] }, { "From": [2, "Out"], "To": [3, "Execute"] }"""));
		VisualScriptTesting.AssertNoErrors(exec);
		Assert.True(VisualScriptTesting.Pin(exec, 2, "Out").Type.IsExec);
		Assert.False(VisualScriptTesting.Node(exec, 2).Pure);

		AnalyzedDocument data = WithVariables(["float"], """{ "ID": 1, "Type": "Reroute" }, { "ID": 2, "Type": "Reroute" }, { "ID": 3, "Type": "Operator.Negate" }""",
			"""{ "From": [11, "Value"], "To": [1, "In"] }, { "From": [1, "Out"], "To": [2, "In"] }, { "From": [2, "Out"], "To": [3, "A"] }""");
		VisualScriptTesting.AssertNoErrors(data);
		Assert.Equal("float", VisualScriptTesting.Pin(data, 3, "Result").Type.Id);
		Assert.True(VisualScriptTesting.Node(data, 1).Pure);

		// A Reroute whose In is unconnected takes the type of where its Out leads, through other Reroutes.
		AnalyzedDocument downstream = VisualScriptTesting.Analyze(VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Reroute", "Inputs": { "In": true } }, { "ID": 2, "Type": "Reroute" }, { "ID": 3, "Type": "Flow.Branch" }""",
			"""{ "From": [1, "Out"], "To": [2, "In"] }, { "From": [2, "Out"], "To": [3, "Condition"] }"""));
		VisualScriptTesting.AssertNoErrors(downstream);
		Assert.Equal("bool", VisualScriptTesting.Pin(downstream, 1, "In", PinDirection.In).Type.Id);
		Assert.Equal(true, VisualScriptTesting.Pin(downstream, 1, "In", PinDirection.In).Value);
		Assert.Equal("bool", VisualScriptTesting.Pin(downstream, 2, "Out").Type.Id);

		// A reroute cycle resolves to nothing, and is a pure cycle.
		AnalyzedDocument cycle = VisualScriptTesting.Analyze(VisualScriptTesting.Document("""{ "ID": 1, "Type": "Reroute" }, { "ID": 2, "Type": "Reroute" }""",
			"""{ "From": [1, "Out"], "To": [2, "In"] }, { "From": [2, "Out"], "To": [1, "In"] }"""));
		VisualScriptTesting.Single(cycle, DiagnosticCode.PureCycle);
	}

	[Fact]
	public void ArrayElementsHaveTheElementType()
	{
		string children = """{ "ID": 1, "Type": "Get:Strada.Entity.Children" }""";
		AnalyzedDocument document = VisualScriptTesting.Analyze(VisualScriptTesting.Document(
			children + """, { "ID": 2, "Type": "Array.Get", "Inputs": { "Index": 0 } }, { "ID": 3, "Type": "Array.Length" }, { "ID": 4, "Type": "Event.Update" }, { "ID": 5, "Type": "Flow.ForEach" }""",
			"""{ "From": [1, "Value"], "To": [2, "Array"] }, { "From": [1, "Value"], "To": [3, "Array"] }, { "From": [4, "Then"], "To": [5, "Execute"] }, { "From": [1, "Value"], "To": [5, "Array"] }"""));
		VisualScriptTesting.AssertNoErrors(document);
		Assert.Equal("Strada.Entity", VisualScriptTesting.Pin(document, 2, "Element").Type.Id);
		Assert.Equal("Strada.Entity[]", VisualScriptTesting.Pin(document, 3, "Array", PinDirection.In).Type.Id);
		Assert.Equal("Strada.Entity", VisualScriptTesting.Pin(document, 5, "Element").Type.Id);

		AnalyzedDocument notArray = WithVariables(["int"], """{ "ID": 1, "Type": "Array.Length" }""", """{ "From": [11, "Value"], "To": [1, "Array"] }""");
		Diagnostic mismatch = VisualScriptTesting.Single(notArray, DiagnosticCode.TypeMismatch);
		Assert.NotNull(mismatch.Link);
		Assert.Contains("an array", mismatch.Message, System.StringComparison.Ordinal);
	}

	[Fact]
	public void OperatorsUseTheOperatorsOfTheirTypes()
	{
		AnalyzedDocument scale = Binary("Operator.Multiply", "Strada.Vector3", "float");
		VisualScriptTesting.AssertNoErrors(scale);
		Assert.Equal("Strada.Vector3", VisualScriptTesting.Pin(scale, 1, "Result").Type.Id);
		OperatorBinding binding = Assert.IsType<OperatorBinding>(VisualScriptTesting.Node(scale, 1).Binding);
		Assert.Equal(OperatorMode.Method, binding.Mode);
		Assert.Equal(typeof(Vector3).GetMethod("op_Multiply", [typeof(Vector3), typeof(float)]), binding.Method);
		// An int operand widens to the float parameter.
		Assert.Equal(typeof(Vector3).GetMethod("op_Multiply", [typeof(Vector3), typeof(float)]),
			Assert.IsType<OperatorBinding>(VisualScriptTesting.Node(Binary("Operator.Multiply", "Strada.Vector3", "int"), 1).Binding).Method);
		Assert.Equal("Strada.Vector3", VisualScriptTesting.Pin(Binary("Operator.Multiply", "Strada.Quaternion", "Strada.Vector3"), 1, "Result").Type.Id);

		VisualScriptTesting.Single(Binary("Operator.Add", "Strada.Entity", "Strada.Entity"), DiagnosticCode.NoOperator);
		VisualScriptTesting.Single(Binary("Operator.Less", "string", "string"), DiagnosticCode.NoOperator);

		AnalyzedDocument text = Binary("Operator.Add", "int", "string");
		VisualScriptTesting.AssertNoErrors(text);
		Assert.Equal("string", VisualScriptTesting.Pin(text, 1, "Result").Type.Id);
		Assert.Equal(OperatorMode.Concatenate, Assert.IsType<OperatorBinding>(VisualScriptTesting.Node(text, 1).Binding).Mode);

		AnalyzedDocument negate = WithVariables(["uint"], """{ "ID": 1, "Type": "Operator.Negate" }""", """{ "From": [11, "Value"], "To": [1, "A"] }""");
		Assert.Equal("long", VisualScriptTesting.Pin(negate, 1, "Result").Type.Id);
		AnalyzedDocument negateVector = WithVariables(["Strada.Vector3"], """{ "ID": 1, "Type": "Operator.Negate" }""", """{ "From": [11, "Value"], "To": [1, "A"] }""");
		Assert.Equal("Strada.Vector3", VisualScriptTesting.Pin(negateVector, 1, "Result").Type.Id);
		VisualScriptTesting.Single(WithVariables(["ulong"], """{ "ID": 1, "Type": "Operator.Negate" }""", """{ "From": [11, "Value"], "To": [1, "A"] }"""),
			DiagnosticCode.NoOperator);
	}

	[Fact]
	public void EqualityFollowsTheTable()
	{
		static OperatorMode Mode(string a, string b) => Assert.IsType<OperatorBinding>(VisualScriptTesting.Node(Binary("Operator.Equal", a, b), 1).Binding).Mode;

		Assert.Equal(OperatorMode.Numeric, Mode("int", "double"));
		Assert.Equal(OperatorMode.ValueEquality, Mode("bool", "bool"));
		Assert.Equal(OperatorMode.ValueEquality, Mode("string", "string"));
		Assert.Equal(OperatorMode.ValueEquality, Mode("Strada.ForceMode", "Strada.ForceMode"));
		Assert.Equal(OperatorMode.EntityEquality, Mode("Strada.Entity", "Strada.Script"));
		Assert.Equal(OperatorMode.AssetEquality, Mode("Strada.Material", "Strada.Material"));
		Assert.Equal(OperatorMode.AssetEquality, Mode("Strada.Material", "Strada.Mesh"));
		Assert.Equal(OperatorMode.ComponentEquality, Mode("Strada.RigidBodyComponent", "Strada.RigidBodyComponent"));
		Assert.Equal(OperatorMode.Method, Mode("Strada.Vector3", "Strada.Vector3"));
		Assert.Equal(OperatorMode.Method, Mode("Strada.AssetHandle", "Strada.AssetHandle"));
		Assert.Equal(OperatorMode.ObjectEquality, Mode("System.Object", "System.Object"));
		foreach ((string a, string b) in new[] { ("Strada.ForceMode", "Strada.KeyCode"), ("Strada.ForceMode", "int"), ("bool", "int"),
			("Strada.Entity", "Strada.Material"), ("int", "ulong"), ("Strada.Vector3", "Strada.Vector2") })
		{
			VisualScriptTesting.Single(Binary("Operator.NotEqual", a, b), DiagnosticCode.TypeMismatch);
		}
	}

	[Fact]
	public void ConvertPlansEachPairOfTypes()
	{
		static ConvertBinding? Plan(string from, string to)
		{
			AnalyzedDocument document = WithVariables([from], $$"""{ "ID": 1, "Type": "Convert", "Settings": { "To": "{{to}}" } }""",
				"""{ "From": [11, "Value"], "To": [1, "Value"] }""");
			return VisualScriptTesting.Node(document, 1).Binding as ConvertBinding;
		}

		Assert.Equal(ConvertPlan.NumericCast, Plan("int", "float")!.Plan);
		Assert.Equal(ConvertPlan.NumericCast, Plan("double", "uint")!.Plan);
		Assert.Equal(ConvertPlan.EnumToInteger, Plan("Strada.ForceMode", "int")!.Plan);
		Assert.Equal(ConvertPlan.IntegerToEnum, Plan("long", "Strada.ForceMode")!.Plan);
		Assert.Equal(ConvertPlan.ParseBool, Plan("string", "bool")!.Plan);
		Assert.Equal(ConvertPlan.ParseNumber, Plan("string", "double")!.Plan);
		Assert.Equal(ConvertPlan.ParseEnum, Plan("string", "Strada.KeyCode")!.Plan);
		Assert.Equal(ConvertPlan.ToText, Plan("Strada.Vector3", "string")!.Plan);
		Assert.Equal(ConvertPlan.Identity, Plan("float", "float")!.Plan);
		Assert.Equal(ConvertPlan.ScriptInstance, Plan("Strada.Entity", "Strada.Script")!.Plan);
		ConvertBinding handle = Plan("Strada.Material", "Strada.AssetHandle")!;
		Assert.Equal((ConvertPlan.Link, ConversionKind.AssetToHandle), (handle.Plan, handle.Link));
		Assert.Equal(ConversionKind.Upcast, Plan("Strada.Script", "Strada.Entity")!.Link);
		ConvertBinding color = Plan("Strada.Color", "Strada.Vector4")!;
		Assert.Equal(ConvertPlan.Method, color.Plan);
		Assert.Equal("op_Explicit", color.Method!.Name);
		Assert.Equal(ConvertPlan.Method, Plan("Strada.Vector4", "Strada.Color")!.Plan);
		Assert.Null(Plan("Strada.Entity", "bool"));
		Assert.Null(Plan("Strada.Vector3", "Strada.Vector2"));
		Assert.Null(Plan("System.Object", "Strada.Entity"));
	}

	[Fact]
	public void LinksConvertByTheRules()
	{
		static ConversionKind? Link(string from, string to)
		{
			AnalyzedDocument document = WithVariables([from, to],
				"""{ "ID": 1, "Type": "Event.Update" }, { "ID": 2, "Type": "Variable.Set", "Settings": { "Variable": 22 } }""",
				"""{ "From": [1, "Then"], "To": [2, "Execute"] }, { "From": [11, "Value"], "To": [2, "Value"] }""");
			AnalyzedLink link = VisualScriptTesting.Pin(document, 2, "Value", PinDirection.In).Links.Single();
			return link.IsValid ? link.Conversion : null;
		}

		Assert.Equal(ConversionKind.Identity, Link("int", "int"));
		Assert.Equal(ConversionKind.Widen, Link("int", "double"));
		Assert.Equal(ConversionKind.Widen, Link("uint", "ulong"));
		Assert.Equal(ConversionKind.Upcast, Link("Strada.Mesh", "Strada.Asset"));
		Assert.Equal(ConversionKind.Upcast, Link("Strada.Script", "Strada.Entity"));
		Assert.Equal(ConversionKind.Upcast, Link("Strada.Entity", "System.Object"));
		Assert.Equal(ConversionKind.Box, Link("Strada.Vector3", "System.Object"));
		Assert.Equal(ConversionKind.ComponentToEntity, Link("Strada.RigidBodyComponent", "Strada.Entity"));
		Assert.Equal(ConversionKind.AssetToHandle, Link("Strada.Material", "Strada.AssetHandle"));
		Assert.Equal(ConversionKind.ToText, Link("Strada.Vector3", "string"));
		Assert.Equal(ConversionKind.ToText, Link("Strada.Entity", "string"));
		Assert.Null(Link("long", "int"));
		Assert.Null(Link("int", "uint"));
		Assert.Null(Link("float", "int"));
		Assert.Null(Link("Strada.Entity", "Strada.Script"));
		Assert.Null(Link("Strada.RigidBodyComponent", "Strada.Component"));
		Assert.Null(Link("string", "int"));
		Assert.Null(Link("Strada.Asset", "Strada.AssetHandle"));
	}

	[Fact]
	public void IsValidTakesReferences()
	{
		AnalyzedDocument entity = WithVariables(["Strada.Entity"], """{ "ID": 1, "Type": "IsValid" }""", """{ "From": [11, "Value"], "To": [1, "Value"] }""");
		VisualScriptTesting.AssertNoErrors(entity);
		AnalyzedDocument number = WithVariables(["int"], """{ "ID": 1, "Type": "IsValid" }""", """{ "From": [11, "Value"], "To": [1, "Value"] }""");
		Assert.Contains("a reference", VisualScriptTesting.Single(number, DiagnosticCode.TypeMismatch).Message, System.StringComparison.Ordinal);
	}

	[Fact]
	public void ExecAndDataPinsDoNotMix()
	{
		AnalyzedDocument document = VisualScriptTesting.Analyze(VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Event.Update" }, { "ID": 2, "Type": "Flow.Branch" }""",
			"""{ "From": [1, "Then"], "To": [2, "Condition"] }, { "From": [1, "DeltaTime"], "To": [2, "Execute"] }"""));
		Assert.Equal(2, document.Diagnostics.Count(static diagnostic => diagnostic.Code == DiagnosticCode.TypeMismatch && diagnostic.Link != null));
	}
}
