using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.Json;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Every diagnostic code of spec section 6.2 (and the four the VM adds), where it is reported, and its suggestions.
public sealed class AnalysisDiagnosticsTests
{
	private const string Update = """{ "ID": 1, "Type": "Event.Update" }""";

	private static string Function(string name, bool pure, string nodes, string links = "", string inputs = "", string outputs = "") =>
		$$"""{ "ID": 100, "Name": "{{name}}", "Pure": {{(pure ? "true" : "false")}}, "Inputs": [{{inputs}}], "Outputs": [{{outputs}}], "Locals": [], "Graph": { "Nodes": [{{nodes}}], "Links": [{{links}}], "Comments": [] } }""";

	private static string Entry(uint id) => $$"""{ "ID": {{id}}, "Type": "Function.Entry" }""";

	private static string Return(uint id) => $$"""{ "ID": {{id}}, "Type": "Function.Return" }""";

	// A document that raises each code.
	private static readonly Dictionary<DiagnosticCode, string> s_Cases = new()
	{
		[DiagnosticCode.InvalidDocument] = "[1, 2]",
		[DiagnosticCode.UnknownNodeType] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Flow.Nothing" }"""),
		[DiagnosticCode.AmbiguousNode] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Call:Strada.Random.Range" }"""),
		[DiagnosticCode.NodeUpdated] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Call:Strada.Input.IsKeyDown", "Inputs": { "Key": "Space" } }"""),
		[DiagnosticCode.UnknownPin] = VisualScriptTesting.Document(Update + """, { "ID": 2, "Type": "Flow.Branch" }""",
			"""{ "From": [1, "Than"], "To": [2, "Execute"] }"""),
		[DiagnosticCode.UnusedInput] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Flow.Branch", "Inputs": { "Conditon": true } }"""),
		[DiagnosticCode.MultipleExecLinks] = VisualScriptTesting.Document(Update + """, { "ID": 2, "Type": "Flow.Branch" }, { "ID": 3, "Type": "Flow.Branch" }""",
			"""{ "From": [1, "Then"], "To": [2, "Execute"] }, { "From": [1, "Then"], "To": [3, "Execute"] }"""),
		[DiagnosticCode.MultipleDataLinks] = VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Flow.Branch" }, { "ID": 2, "Type": "Literal", "Settings": { "Type": "bool" } }, { "ID": 3, "Type": "Literal", "Settings": { "Type": "bool" } }""",
			"""{ "From": [2, "Value"], "To": [1, "Condition"] }, { "From": [3, "Value"], "To": [1, "Condition"] }"""),
		[DiagnosticCode.TypeMismatch] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Flow.Branch" }, { "ID": 2, "Type": "Self" }""",
			"""{ "From": [2, "Self"], "To": [1, "Condition"] }"""),
		[DiagnosticCode.UnresolvedWildcard] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Operator.Add" }"""),
		[DiagnosticCode.NoOperator] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Operator.Add" }, { "ID": 2, "Type": "Self" }""",
			"""{ "From": [2, "Self"], "To": [1, "A"] }, { "From": [2, "Self"], "To": [1, "B"] }"""),
		[DiagnosticCode.NoConversion] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Convert", "Settings": { "To": "bool" } }, { "ID": 2, "Type": "Self" }""",
			"""{ "From": [2, "Self"], "To": [1, "Value"] }"""),
		[DiagnosticCode.InvalidValue] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Flow.Branch", "Inputs": { "Condition": 1 } }"""),
		[DiagnosticCode.EntityValue] = VisualScriptTesting.Document("", variables: """{ "ID": 1, "Name": "Partner", "Type": "Strada.Entity", "Value": "5" }"""),
		[DiagnosticCode.ZeroQuaternion] = VisualScriptTesting.Document("",
			variables: """{ "ID": 1, "Name": "Turn", "Type": "Strada.Quaternion", "Value": [0.0, 0.0, 0.0, 0.0] }"""),
		[DiagnosticCode.InvalidSetting] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Flow.Sequence", "Settings": { "Count": 1 } }"""),
		[DiagnosticCode.MissingSetting] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Event.KeyPressed" }"""),
		[DiagnosticCode.UnknownSetting] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Flow.Branch", "Settings": { "Cout": 2 } }"""),
		[DiagnosticCode.MissingTypeArgument] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Call:Strada.Entity.GetComponent<T>()" }"""),
		[DiagnosticCode.InvalidCase] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Flow.Switch", "Settings": { "Type": "int", "Cases": [1, 1] } }"""),
		[DiagnosticCode.DuplicateEvent] = VisualScriptTesting.Document(Update + """, { "ID": 2, "Type": "Event.Update" }"""),
		[DiagnosticCode.EventInFunction] = VisualScriptTesting.Document("", functions: Function("Run", false, Entry(101) + ", " + """{ "ID": 102, "Type": "Event.Update" }""")),
		[DiagnosticCode.EventInputLinked] = VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Event.GamepadButtonPressed", "Settings": { "Button": "A" } }, { "ID": 2, "Type": "Literal", "Settings": { "Type": "int" } }""",
			"""{ "From": [2, "Value"], "To": [1, "Gamepad"] }"""),
		[DiagnosticCode.InvalidName] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Event.Custom", "Settings": { "Name": "Hit.Hard" } }"""),
		[DiagnosticCode.ReservedName] = VisualScriptTesting.Document("", functions: Function("Run", false, Entry(101), inputs: """{ "Name": "Then", "Type": "int" }""")),
		[DiagnosticCode.UnknownType] = VisualScriptTesting.Document("", variables: """{ "ID": 1, "Name": "Offset", "Type": "Strada.Vectr3" }"""),
		[DiagnosticCode.InvalidType] = VisualScriptTesting.Document("", variables: """{ "ID": 1, "Name": "Flow", "Type": "exec" }"""),
		[DiagnosticCode.NotExposable] = VisualScriptTesting.Document("", variables: """{ "ID": 1, "Name": "World", "Type": "Strada.Matrix4", "Public": true }"""),
		[DiagnosticCode.UnknownVariable] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Variable.Get", "Settings": { "Variable": 99 } }"""),
		[DiagnosticCode.UnknownFunction] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Function.Call", "Settings": { "Function": 99 } }"""),
		[DiagnosticCode.MissingEntry] = VisualScriptTesting.Document("", functions: Function("Run", false, "")),
		[DiagnosticCode.DuplicateEntry] = VisualScriptTesting.Document("", functions: Function("Run", false, Entry(101) + ", " + Entry(102))),
		[DiagnosticCode.MissingReturn] = VisualScriptTesting.Document("", functions: Function("Half", true, Entry(101))),
		[DiagnosticCode.DuplicateReturn] = VisualScriptTesting.Document("", functions: Function("Half", true, Entry(101) + ", " + Return(102) + ", " + Return(103))),
		[DiagnosticCode.LatentInPureContext] = VisualScriptTesting.Document("",
			functions: Function("Half", true, Entry(101) + ", " + Return(102) + ", " + """{ "ID": 103, "Type": "Flow.Delay" }""")),
		[DiagnosticCode.TickInFunction] = VisualScriptTesting.Document("", functions: Function("Run", false, Entry(101) + ", " + """{ "ID": 102, "Type": "Flow.Tween" }""")),
		[DiagnosticCode.PureCycle] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Operator.Not" }, { "ID": 2, "Type": "Operator.Not" }""",
			"""{ "From": [1, "Result"], "To": [2, "A"] }, { "From": [2, "Result"], "To": [1, "A"] }"""),
		[DiagnosticCode.PureRecursion] = VisualScriptTesting.Document("", functions: Function("Again", true,
			Entry(101) + ", " + Return(102) + ", " + """{ "ID": 103, "Type": "Function.Call", "Settings": { "Function": 100 } }""")),
		[DiagnosticCode.ImpureInPureFunction] = VisualScriptTesting.Document("", functions: Function("Half", true,
			Entry(101) + ", " + Return(102) + ", " + """{ "ID": 103, "Type": "Call:Strada.Log.Info(string)" }""")),
		[DiagnosticCode.NotInFunction] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Function.Entry" }"""),
		[DiagnosticCode.DisabledIgnored] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Self", "Enabled": "Disabled" }"""),
		[DiagnosticCode.UnusedSplit] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Flow.Branch", "Split": { "Inputs": ["Condition"] } }"""),
		[DiagnosticCode.InvalidExpression] = VisualScriptTesting.Document("""{ "ID": 1, "Type": "Expression", "Settings": { "Formula": "a +" } }"""),
	};

	public static TheoryData<string> Codes => [.. s_Cases.Keys.Select(static code => code.ToString())];

	[Theory]
	[MemberData(nameof(Codes))]
	public void EachCodeIsReported(string name)
	{
		DiagnosticCode code = Enum.Parse<DiagnosticCode>(name);
		AnalyzedDocument document = VisualScriptTesting.Analyze(s_Cases[code]);
		Diagnostic diagnostic = document.Diagnostics.FirstOrDefault(diagnostic => diagnostic.Code == code)
			?? throw new Xunit.Sdk.XunitException($"no {code} in:\n{VisualScriptTesting.Describe(document.Diagnostics)}");
		bool warning = code is DiagnosticCode.NodeUpdated or DiagnosticCode.UnusedInput or DiagnosticCode.ZeroQuaternion or DiagnosticCode.UnknownSetting or
			DiagnosticCode.DisabledIgnored or DiagnosticCode.UnusedSplit;
		Assert.Equal(warning ? DiagnosticSeverity.Warning : DiagnosticSeverity.Error, diagnostic.Severity);
		Assert.NotEmpty(diagnostic.Message);
		// The analysis JSON writes it with its code name.
		Assert.Contains($"\"Code\":\"{name}\"", Encoding.UTF8.GetString(AnalysisWriter.Write(document)), StringComparison.Ordinal);
	}

	[Fact]
	public void EveryCodeHasACase()
	{
		Assert.Equal(Enum.GetValues<DiagnosticCode>().Order(), s_Cases.Keys.Order());
	}

	[Fact]
	public void DocumentsThatAreNotObjectsAreOneInvalidDocument()
	{
		foreach (string text in new[] { "", "[]", "42", "\"text\"", "null", "{", "{ \"Graph\": }", new string('[', 100) })
		{
			AnalyzedDocument document = VisualScriptTesting.Analyze(text);
			Diagnostic diagnostic = Assert.Single(document.Diagnostics);
			Assert.Equal(DiagnosticCode.InvalidDocument, diagnostic.Code);
			Assert.Empty(document.EventGraph.Nodes);
			JsonElement root = VisualScriptTesting.ParseObject(AnalysisWriter.Write(document));
			Assert.Equal(0, root.GetProperty("Nodes").GetArrayLength());
			Assert.Equal(1, root.GetProperty("Diagnostics").GetArrayLength());
		}
		// An empty object is an empty, valid document.
		Assert.Empty(VisualScriptTesting.Analyze("{}").Diagnostics);
	}

	// JSON text is UTF-8 and its strings are text: bytes that are not UTF-8 (a stray continuation byte, an overlong form, a
	// sequence cut short), and an escaped surrogate without its pair, in a value or a member name, refuse the document.
	[Fact]
	public void DocumentsThatAreNotTextAreOneInvalidDocument()
	{
		byte[][] documents =
		[
			[.. """{ "Graph": { "Nodes": [ { "ID": 1, "Type": "Self"""u8, 0xC3, 0x28, .. "\" } ] } }"u8],
			[.. """{ "Description": """u8, (byte)'"', 0xC0, 0xAF, .. "\" }"u8],
			[.. "{ \""u8, 0xE2, 0x82, .. "\": 1 }"u8],
			[.. """{ "Description": "x"""u8, 0xF0, 0x9F, 0x98],
			"""{ "Graph": { "Nodes": [ { "ID": 1, "Type": "Self\uD800" } ] } }"""u8.ToArray(),
			"""{ "Description": "\uDC00\uD800" }"""u8.ToArray(),
			"""{ "\uD83D": 1 }"""u8.ToArray(),
		];
		foreach (byte[] text in documents)
		{
			AnalyzedDocument document = GraphAnalyzer.Analyze(VisualScriptTesting.Library, text);
			Diagnostic diagnostic = Assert.Single(document.Diagnostics);
			Assert.Equal(DiagnosticCode.InvalidDocument, diagnostic.Code);
			Assert.Equal(1, VisualScriptTesting.ParseObject(AnalysisWriter.Write(document)).GetProperty("Diagnostics").GetArrayLength());
		}
		// A surrogate pair escaped as such is text.
		VisualScriptTesting.AssertNoErrors(VisualScriptTesting.Analyze("""{ "Description": "\uD83D\uDE00" }"""));
	}

	[Fact]
	public void StructuralDefectsAreInvalidDocumentWithTheirPath()
	{
		AnalyzedDocument document = VisualScriptTesting.Analyze("""
			{ "Variables": [ { "ID": 1, "Name": "A", "Type": "int" }, { "ID": 1, "Name": "B", "Type": "int" } ],
			  "Graph": { "Nodes": [ { "ID": 2, "Type": "Self" }, { "Type": "Self" } ], "Links": [ { "From": ["2", "Self"], "To": [7, "In"] } ] } }
			""");
		string[] messages = [.. document.Diagnostics.Where(static diagnostic => diagnostic.Code == DiagnosticCode.InvalidDocument)
			.Select(static diagnostic => diagnostic.Message)];
		Assert.Contains(messages, static message => message.StartsWith("Variables[1].ID:", StringComparison.Ordinal));
		Assert.Contains(messages, static message => message.StartsWith("Graph.Nodes[1].ID:", StringComparison.Ordinal));
		Assert.Contains(messages, static message => message.StartsWith("Graph.Links[0].From:", StringComparison.Ordinal));
		Assert.Single(document.Variables);
		Assert.Single(document.EventGraph.Nodes);
	}

	[Fact]
	public void UnknownPinsNameTheNodePinAndLinkWithSuggestions()
	{
		AnalyzedDocument document = VisualScriptTesting.Analyze(s_Cases[DiagnosticCode.UnknownPin]);
		Diagnostic diagnostic = VisualScriptTesting.Single(document, DiagnosticCode.UnknownPin);
		Assert.Equal(1u, diagnostic.Node);
		Assert.Equal("Than", diagnostic.Pin);
		Assert.Equal(PinDirection.Out, diagnostic.Direction);
		Assert.Equal(new PinReference(1, "Than"), diagnostic.Link!.From);
		Assert.Equal(["Then"], diagnostic.Suggestions);
		JsonElement written = VisualScriptTesting.ParseObject(AnalysisWriter.Write(document)).GetProperty("Diagnostics")[0];
		Assert.Equal(
			"""{"Severity":"Error","Code":"UnknownPin","Function":0,"Node":1,"Pin":"Than","Direction":"Out","Link":{"From":[1,"Than"],"To":[2,"Execute"]},"Message":"Update has no output pin \"Than\"","Suggestions":["Then"]}""",
			written.GetRawText());

		// A pin of the other direction is named in the message.
		AnalyzedDocument reversed = VisualScriptTesting.Analyze(VisualScriptTesting.Document(Update + """, { "ID": 2, "Type": "Flow.Branch" }""",
			"""{ "From": [2, "Condition"], "To": [1, "Then"] }"""));
		Assert.Contains(VisualScriptTesting.Errors(reversed), static diagnostic => diagnostic.Message.Contains("(Condition is an input)", StringComparison.Ordinal));
	}

	[Fact]
	public void UnknownNamesSuggestTheirNearestCandidates()
	{
		Diagnostic type = VisualScriptTesting.Single(VisualScriptTesting.Analyze(s_Cases[DiagnosticCode.UnknownType]), DiagnosticCode.UnknownType);
		Assert.Equal(1u, type.Variable);
		Assert.Equal(0u, type.Function);
		Assert.Equal("Strada.Vector3", type.Suggestions[0]);

		AnalyzedDocument variables = VisualScriptTesting.Analyze(VisualScriptTesting.Document("""{ "ID": 3, "Type": "Variable.Get", "Settings": { "Variable": 21 } }""",
			variables: """{ "ID": 1, "Name": "A", "Type": "int" }, { "ID": 2, "Name": "B", "Type": "int" }, { "ID": 20, "Name": "C", "Type": "int" }"""));
		Diagnostic variable = VisualScriptTesting.Single(variables, DiagnosticCode.UnknownVariable);
		Assert.Equal(3u, variable.Node);
		Assert.Equal(["1", "2", "20"], variable.Suggestions);

		AnalyzedDocument functions = VisualScriptTesting.Analyze(VisualScriptTesting.Document("""{ "ID": 1, "Type": "Function.Call", "Settings": { "Function": 101 } }""",
			functions: Function("Run", false, Entry(110))));
		Assert.Equal(["100"], VisualScriptTesting.Single(functions, DiagnosticCode.UnknownFunction).Suggestions);

		Diagnostic input = VisualScriptTesting.Single(VisualScriptTesting.Analyze(s_Cases[DiagnosticCode.UnusedInput]), DiagnosticCode.UnusedInput);
		Assert.Equal("Conditon", input.Pin);
		Assert.Equal(["Condition"], input.Suggestions);

		Diagnostic setting = VisualScriptTesting.Single(VisualScriptTesting.Analyze(
			VisualScriptTesting.Document("""{ "ID": 1, "Type": "Flow.Sequence", "Settings": { "Cout": 3 } }""")), DiagnosticCode.UnknownSetting);
		Assert.Equal(["Count"], setting.Suggestions);
	}

	[Fact]
	public void LocalsAreKnownOnlyInTheirFunction()
	{
		string function = """
			{ "ID": 100, "Name": "Sum", "Pure": false, "Inputs": [], "Outputs": [], "Locals": [ { "ID": 101, "Name": "Total", "Type": "int", "Value": 5 } ],
			  "Graph": { "Nodes": [ { "ID": 102, "Type": "Function.Entry" }, { "ID": 103, "Type": "Variable.Get", "Settings": { "Variable": 101 } } ], "Links": [], "Comments": [] } }
			""";
		AnalyzedDocument document = VisualScriptTesting.Analyze(VisualScriptTesting.Document("""{ "ID": 1, "Type": "Variable.Get", "Settings": { "Variable": 101 } }""",
			functions: function));
		Diagnostic diagnostic = VisualScriptTesting.Single(document, DiagnosticCode.UnknownVariable);
		Assert.Equal(1u, diagnostic.Node);
		Assert.Contains("is a local of Sum", diagnostic.Message, StringComparison.Ordinal);
		AnalyzedNode local = VisualScriptTesting.Node(document, 103);
		Assert.Equal("int", local.FindPin("Value", PinDirection.Out)!.Type.Id);
		AnalyzedVariable total = Assert.IsType<VariableBinding>(local.Binding).Variable;
		Assert.True(total.IsLocal);
		Assert.Equal(5, total.Value);
	}

	[Fact]
	public void FunctionDiagnosticsNameTheFunction()
	{
		Diagnostic missing = VisualScriptTesting.Single(VisualScriptTesting.Analyze(s_Cases[DiagnosticCode.MissingEntry]), DiagnosticCode.MissingEntry);
		Assert.Equal(100u, missing.Function);
		Assert.Null(missing.Node);
		Diagnostic reserved = VisualScriptTesting.Single(VisualScriptTesting.Analyze(s_Cases[DiagnosticCode.ReservedName]), DiagnosticCode.ReservedName);
		Assert.Equal(100u, reserved.Function);
		Diagnostic duplicate = VisualScriptTesting.Single(VisualScriptTesting.Analyze(s_Cases[DiagnosticCode.DuplicateEntry]), DiagnosticCode.DuplicateEntry);
		Assert.Equal(102u, duplicate.Node);
		Assert.Equal(100u, duplicate.Function);
		// A pure function without exec pins: its Entry has no Then and its Return no Execute.
		AnalyzedDocument pure = VisualScriptTesting.Analyze(VisualScriptTesting.Document("", functions: Function("Half", true, Entry(101) + ", " + Return(102),
			"""{ "From": [101, "Value"], "To": [102, "Half"] }""", """{ "Name": "Value", "Type": "float" }""", """{ "Name": "Half", "Type": "float" }""")));
		VisualScriptTesting.AssertNoErrors(pure);
		Assert.Equal(["Out:Value:float"], VisualScriptTesting.Node(pure, 101).Pins.Select(static pin => $"{pin.Direction}:{pin.Name}:{pin.Type.Id}"));
		Assert.Equal(["In:Half:float"], VisualScriptTesting.Node(pure, 102).Pins.Select(static pin => $"{pin.Direction}:{pin.Name}:{pin.Type.Id}"));
	}

	[Fact]
	public void InputEventsAreUniquePerKeyNotPerSpelling()
	{
		AnalyzedDocument document = VisualScriptTesting.Analyze(VisualScriptTesting.Document("""
			{ "ID": 1, "Type": "Event.KeyPressed", "Settings": { "Key": "Space" } }, { "ID": 2, "Type": "Event.KeyPressed", "Settings": { "Key": 32 } },
			{ "ID": 3, "Type": "Event.KeyReleased", "Settings": { "Key": "Space" } }, { "ID": 4, "Type": "Event.KeyPressed", "Settings": { "Key": "G" } },
			{ "ID": 5, "Type": "Event.Custom", "Settings": { "Name": "Hit" } }, { "ID": 6, "Type": "Event.Custom", "Settings": { "Name": "Hit" } },
			{ "ID": 7, "Type": "Event.Custom", "Settings": { "Name": "Miss" } }
			"""));
		uint?[] duplicates = [.. document.Diagnostics.Where(static diagnostic => diagnostic.Code == DiagnosticCode.DuplicateEvent).Select(static diagnostic => diagnostic.Node)];
		Assert.Equal([2u, 6u], duplicates);
	}

	[Fact]
	public void DefaultsToSelfPinsTakeNoStoredValue()
	{
		AnalyzedDocument document = VisualScriptTesting.Analyze(VisualScriptTesting.Document(
			"""{ "ID": 1, "Type": "Get:Strada.Entity.Name", "Inputs": { "Target": "0" } }"""));
		Diagnostic diagnostic = VisualScriptTesting.Single(document, DiagnosticCode.InvalidValue);
		Assert.Equal("Target", diagnostic.Pin);
		Assert.Contains("leave Target unconnected to use Self", diagnostic.Message, StringComparison.Ordinal);
	}

	[Fact]
	public void SuggestionSearchesAreBounded()
	{
		StringBuilder nodes = new();
		for (int index = 1; index <= AnalysisContext.MaxSuggestionSearches + 10; index++)
		{
			nodes.Append(index > 1 ? ", " : "").Append($$"""{ "ID": {{index}}, "Type": "Flow.Brnach{{new string('x', index % 7)}}" }""");
		}
		AnalyzedDocument document = VisualScriptTesting.Analyze(VisualScriptTesting.Document(nodes.ToString()));
		List<Diagnostic> unknown = [.. document.Diagnostics.Where(static diagnostic => diagnostic.Code == DiagnosticCode.UnknownNodeType)];
		Assert.Equal(AnalysisContext.MaxSuggestionSearches + 10, unknown.Count);
		Assert.NotEmpty(unknown[0].Suggestions);
		Assert.Empty(unknown[^1].Suggestions);
		// Texts far from every candidate get none.
		Assert.Empty(Suggestions.Nearest(new string('q', 500), VisualScriptTesting.Library.NodeTypes));
	}
}
