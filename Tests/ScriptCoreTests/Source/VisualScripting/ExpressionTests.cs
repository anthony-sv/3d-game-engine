using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.ExceptionServices;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Threading;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The Expression node (spec-slice3 section 3.3): formulas read, typed and run as one pure node, their inputs named by the
// names they use, their problems located in their text.
public sealed class ExpressionTests
{
	// Formula, inputs ("name:type=value" separated by ';', each linked from a Literal of its type), the result's type and its
	// value as text.
	public static TheoryData<string, string, string, string> TypedRows => new()
	{
		{ "a * b + sin(t)", "a:float=2.0;b:float=3.0;t:float=0.0", "float", "6" },
		{ "Index + 1", "Index:int=41", "int", "42" },
		{ "count / 2", "count:int=7", "int", "3" },
		{ "count / 2.0", "count:int=7", "float", "3.5" },
		{ "pos + dir * speed * dt", "pos:Strada.Vector3=[1.0, 1.0, 1.0];dir:Strada.Vector3=[0.0, 1.0, 1.0];speed:float=4.0;dt:float=0.5", "Strada.Vector3",
			"(1, 3, 3)" },
		{ "x > 0 && 1 / x > 2", "x:int=0", "bool", "False" },
		{ "flag ? a : b", "flag:bool=true;a:int=2;b:float=0.5", "float", "2" },
		{ "clamp(v, 0, 1)", "v:float=1.5", "float", "1" },
		{ "lerp(a, b, 0.25)", "a:float=0.0;b:float=8.0", "float", "2" },
		{ "smoothstep(0, 1, 0.5)", "", "float", "0.5" },
		{ "v.Y", "v:Strada.Vector3=[1.0, 2.0, 3.0]", "float", "2" },
		{ "int(-2.7)", "", "int", "-2" },
		{ "int(1e10)", "", "int", "2147483647" },
		{ "int(sqrt(-1))", "", "int", "0" },
		{ "7 % -3", "", "int", "1" },
		{ "-7 % 3", "", "int", "-1" },
		{ "2147483647 + 1", "", "int", "-2147483648" },
		{ "pi", "", "float", "3.1415927" },
		{ "u + i", "u:uint=4000000000;i:int=-1", "long", "3999999999" },
		{ "distance / speed", "distance:float=10.0;speed:float=4.0", "float", "2.5" },
		{ "exp + 1", "exp:float=2.0", "float", "3" },
		{ "v.y", "v:Strada.Vector3=[1.0, 2.0, 3.0]", "float", "2" },
		{ "tau / 2 == pi", "", "bool", "True" },
		{ "a < 1 || b / 0 > 1", "a:int=0;b:int=5", "bool", "True" },
		{ "-u", "u:uint=3", "long", "-3" },
		{ "!(a == b)", "a:Strada.Vector2=[1.0, 2.0];b:Strada.Vector2=[1.0, 2.0]", "bool", "False" },
		{ "length(vec3(3, 4, 0)) + dot(v, v)", "v:Strada.Vector2=[1.0, 2.0]", "float", "10" },
		{ "cross(vec3(1, 0, 0), vec3(0, 1, 0))", "", "Strada.Vector3", "(0, 0, 1)" },
		{ "normalize(vec2(0, 0))", "", "Strada.Vector2", "(0, 0)" },
		{ "2 * v / 4", "v:Strada.Vector4=[2.0, 4.0, 6.0, 8.0]", "Strada.Vector4", "(1, 2, 3, 4)" },
		{ "sign(-0.0) == 0 && sign(-3) == -1 && abs(-2147483647 - 1) == -2147483647 - 1 && abs(-2147483648) == 2147483648", "", "bool", "True" },
		{ "min(2, 3000000000) + max(-1, 1) + clamp(5, 7, 6)", "", "long", "9" },
		{ "floor(-1.5) + ceil(1.2) + round(2.5) + trunc(-1.7) + frac(1.25)", "", "float", "1.25" },
		{ "pow(2, 10) + atan2(0, 1) + step(1, 2) + degrees(radians(90))", "", "float", "1115" },
		{ "1.5d + 1", "", "double", "2.5" },
		{ "a\n\t* b", "a:float=2.0;b:float=4.0", "float", "8" },
		{ "lerp(p, q, 0.5).X", "p:Strada.Vector3=[0.0, 0.0, 0.0];q:Strada.Vector3=[4.0, 2.0, 0.0]", "float", "2" },
		{ "clamp(v, 0, 1)", "v:Strada.Vector3=[-1.0, 0.5, 2.0]", "Strada.Vector3", "(0, 0.5, 1)" },
		{ "ulong(-1) / ulong(2)", "", "ulong", "9223372036854775807" },
		{ "3000000000 + 1", "", "long", "3000000001" },
		// The largest float, as every float reader of the engine bounds it (R6.3).
		{ "3.4028235e38", "", "float", "3.4028235E+38" },
		{ "-3.4028235e38", "", "float", "-3.4028235E+38" },
		// Precedence and associativity.
		{ "2 + 3 * 4", "", "int", "14" },
		{ "10 - 4 - 3", "", "int", "3" },
		{ "100 / 10 / 5", "", "int", "2" },
		{ "17 % 5 * 2", "", "int", "4" },
		{ "true ? 1 : false ? 2 : 3", "", "int", "1" },
		{ "false ? 1 : false ? 2 : 3", "", "int", "3" },
		{ "-v.X", "v:Strada.Vector2=[2.0, 5.0]", "float", "-2" },
		{ "1 < 2 == true", "", "bool", "True" },
		{ "!true == false", "", "bool", "True" },
		{ "true || false && false", "", "bool", "True" },
		{ "1 + 2 < 4 && 3 != 3 || 2 * 2 >= 4", "", "bool", "True" },
		{ "- -2", "", "int", "2" },
	};

	// The Formula setting (its JSON value, absent when null), the inputs as TypedRows give them, and the diagnostic: its code,
	// message and span (UTF-8 bytes; no span when the start is -1). Every diagnostic names the setting Formula.
	public static TheoryData<string?, string, string, string, int, int> ErrorRows
	{
		get
		{
			string tooLong = new('1', ExpressionParser.MaxBytes + 1);
			// A formula over the cap names no inputs, even when it has names (the corpus pins it for the editor's lister).
			string tooLongWithAName = "q + " + new string('1', ExpressionParser.MaxBytes);
			string longName = new('x', ExpressionParser.MaxIdentifierBytes + 1);
			string manyParts = "a" + string.Concat(Enumerable.Repeat("+a", ExpressionParser.MaxParts));
			string deep = new string('(', ExpressionParser.MaxNesting + 1) + "a" + new string(')', ExpressionParser.MaxNesting + 1);
			string manyInputs = string.Join(" + ", Enumerable.Range(0, ExpressionParser.MaxInputs + 1).Select(static index => $"a{index}"));
			int lastInput = manyInputs.LastIndexOf('a');
			return new()
			{
				{ null, "", "MissingSetting", "Expression needs the setting Formula: a formula such as a * b + sin(t)", -1, -1 },
				{ "42", "", "InvalidSetting", "Formula: expected a string, not 42", -1, -1 },
				{ "\"\"", "", "InvalidExpression", "the formula is empty", 0, 0 },
				{ "\" \\n \"", "", "InvalidExpression", "the formula is empty", 0, 0 },
				{ "\"a # b\"", "", "InvalidExpression", "unexpected '#'", 2, 3 },
				{ "\"é + €\"", "", "InvalidExpression", "unexpected 'é'", 0, 2 },
				{ "\"1e40\"", "", "InvalidExpression", "1e40 is too large for a float: write 1e40d for a double", 0, 4 },
				{ "\"99999999999999999999\"", "", "InvalidExpression", "99999999999999999999 is too large for an integer", 0, 20 },
				{ "\"1e400d\"", "", "InvalidExpression", "1e400d is too large for a double", 0, 6 },
				{ "\"3.4028236e38\"", "", "InvalidExpression", "3.4028236e38 is too large for a float: write 3.4028236e38d for a double", 0, 12 },
				{ "\"1e400\"", "", "InvalidExpression", "1e400 is too large for a float or a double", 0, 5 },
				// A name over the cap is no input: the corpus pins that a is the only one.
				{ $"\"a + {longName}\"", "", "InvalidExpression",
					$"{JsonText.Shorten(longName)} is too long: a name has at most {ExpressionParser.MaxIdentifierBytes} bytes", 4, 4 + longName.Length },
				{ "\".5\"", "", "InvalidExpression", "write 0.5, not .5", 0, 2 },
				{ "\"a +\"", "", "InvalidExpression", "expected a value after '+'", 2, 3 },
				{ "\"* a\"", "", "InvalidExpression", "expected a value, not '*'", 0, 1 },
				{ "\"a * (b + c\"", "", "InvalidExpression", "expected ')' to close the '(' at line 1, column 5", 4, 5 },
				{ "\"sin(a b)\"", "", "InvalidExpression", "expected ')' to close the '(' at line 1, column 4", 3, 4 },
				{ "\"a\\n  ? b\"", "", "InvalidExpression", "expected ':' for the '?' at line 2, column 3", 4, 5 },
				{ "\"a b\"", "", "InvalidExpression", "unexpected 'b' after the formula", 2, 3 },
				{ "\"v.\"", "", "InvalidExpression", "expected a component (X, Y, Z or W) after '.'", 1, 2 },
				{ "\"a * b + sni(t)\"", "", "InvalidExpression", "unknown function sni: did you mean sin?", 8, 11 },
				{ "\"pi(2)\"", "", "InvalidExpression", "pi is a constant, not a function", 0, 2 },
				{ "\"lerp(a, b)\"", "", "InvalidExpression", "lerp takes 3 arguments, not 2", 0, 10 },
				{ "\"sin()\"", "", "InvalidExpression", "sin takes 1 argument, not 0", 0, 5 },
				{ "\"Self + 1\"", "", "ReservedName", "Self is a reserved pin name (Execute, Then, Target, Self, Default): name the input otherwise", 0, 4 },
				{ "\"2 * Result\"", "", "ReservedName", "Result names the node's output: name the input otherwise", 4, 10 },
				{ "\"v.W\"", "v:Strada.Vector3=[1.0, 2.0, 3.0]", "InvalidExpression", "Vector3 has no component W (X, Y, Z)", 2, 3 },
				{ "\"speed.x\"", "", "InvalidExpression", "speed is float: it has no components", 0, 7 },
				{ "\"a + flag\"", "flag:bool=true", "InvalidExpression", "+ takes numbers or vectors, not bool", 0, 8 },
				{ "\"i + u\"", "i:int=1;u:ulong=\"2\"", "InvalidExpression", "int and ulong have no common type: convert one with long(...) or double(...)", 0, 5 },
				{ "\"p + q\"", "p:Strada.Vector2=[1.0, 2.0];q:Strada.Vector3=[1.0, 2.0, 3.0]", "InvalidExpression", "Vector2 and Vector3 do not combine", 0, 5 },
				{ "\"v + s\"", "v:Strada.Vector3=[1.0, 2.0, 3.0]", "InvalidExpression",
					"a Vector3 adds only to a Vector3: write v + vec3(s, s, s) to add a number to each component", 0, 5 },
				{ "\"v * d\"", "v:Strada.Vector3=[1.0, 2.0, 3.0];d:double=2.0", "InvalidExpression",
					"a double does not combine with a Vector3: convert it with float(...)", 0, 5 },
				{ "\"a && b\"", "", "InvalidExpression", "&& takes bool, not float", 0, 6 },
				{ "\"i ? a : b\"", "i:int=1", "InvalidExpression", "the condition of ?: is bool, not int", 0, 1 },
				{ "\"flag ? v : a\"", "flag:bool=true;v:Strada.Vector3=[1.0, 2.0, 3.0]", "InvalidExpression",
					"the branches of ?: have no common type: Vector3 and float", 0, 12 },
				{ "\"sin(v)\"", "v:Strada.Vector3=[1.0, 2.0, 3.0]", "InvalidExpression", "sin takes a number, not Vector3", 0, 6 },
				{ "\"cross(v, a)\"", "v:Strada.Vector3=[1.0, 2.0, 3.0]", "InvalidExpression", "cross takes two Vector3", 0, 11 },
				{ "\"flag == i\"", "flag:bool=true;i:int=1", "InvalidExpression", "== compares values of one type: bool and int", 0, 9 },
				{ "\"-u\"", "u:ulong=\"1\"", "InvalidExpression", "ulong values cannot be negated: convert with long(...) or double(...)", 0, 2 },
				{ $"\"{tooLong}\"", "", "InvalidExpression", $"a formula has at most {ExpressionParser.MaxBytes} bytes", 0, tooLong.Length },
				{ $"\"{tooLongWithAName}\"", "", "InvalidExpression", $"a formula has at most {ExpressionParser.MaxBytes} bytes", 0, tooLongWithAName.Length },
				{ $"\"{manyParts}\"", "", "InvalidExpression", $"a formula has at most {ExpressionParser.MaxParts} parts", ExpressionParser.MaxParts - 1,
					ExpressionParser.MaxParts },
				{ $"\"{deep}\"", "", "InvalidExpression", $"a formula nests at most {ExpressionParser.MaxNesting} levels deep", ExpressionParser.MaxNesting,
					ExpressionParser.MaxNesting + 1 },
				{ $"\"{manyInputs}\"", "", "InvalidExpression", $"a formula uses at most {ExpressionParser.MaxInputs} inputs", lastInput, manyInputs.Length },
			};
		}
	}

	// Formulas and the inputs they name, in order.
	public static TheoryData<string, string> InputOrderRows => new()
	{
		{ "b * a + b", "b,a" },
		{ "distance / speed", "distance,speed" },
		{ "exp + exp(1)", "exp" },
		{ "v.X + w.y", "v,w" },
		{ "sin(t) + sin", "t,sin" },
		{ "a.b.c", "a" },
		{ "true || pi > tau", "" },
		{ "x + 1e3 + e", "x,e" },
		{ "1e + 2", "e" },
		{ "3dx", "x" },
		{ "a ( b", "b" },
		{ "a # b", "a,b" },
		{ "Self + a + Result", "a" },
		{ "int + float(int)", "int" },
		{ "_x1 +\n_x1 * Z_", "_x1,Z_" },
	};

	// Every formula of the rows above, for the corpus.
	internal static IEnumerable<string> RowFormulas()
	{
		foreach (TheoryDataRow<string, string, string, string> row in TypedRows)
		{
			yield return row.Data.Item1;
		}
		foreach (TheoryDataRow<string?, string, string, string, int, int> row in ErrorRows)
		{
			if (row.Data.Item1 is { } setting && JsonDocument.Parse(setting).RootElement is { ValueKind: JsonValueKind.String } text)
			{
				yield return text.GetString()!;
			}
		}
		foreach (TheoryDataRow<string, string> row in InputOrderRows)
		{
			yield return row.Data.Item1;
		}
	}

	[Theory]
	[MemberData(nameof(TypedRows))]
	public void TypedFormulasEvaluate(string formula, string inputs, string type, string expected)
	{
		(DocumentBuilder document, uint expression) = Build(RuntimeTesting.Quote(formula), inputs);
		GraphBuilder graph = document.Graph;
		graph.Flow(graph.Node("Event.Create"), graph.Write(expression, "Result"));
		AnalyzedDocument analyzed = GraphAnalyzer.Analyze(RuntimeTesting.Library, Encoding.UTF8.GetBytes(document.Build()));
		VisualScriptTesting.AssertNoErrors(analyzed);
		Assert.Equal(type, VisualScriptTesting.Pin(analyzed, expression, "Result").Type.Id);
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
		Assert.Equal([expected], runtime.Records);
		Assert.Empty(runtime.Environment.Logs);
	}

	[Theory]
	[MemberData(nameof(ErrorRows))]
	public void ErrorsNameTheirSpan(string? setting, string inputs, string code, string message, int start, int end)
	{
		(DocumentBuilder document, uint expression) = Build(setting, inputs);
		AnalyzedDocument analyzed = GraphAnalyzer.Analyze(RuntimeTesting.Library, Encoding.UTF8.GetBytes(document.Build()));
		List<Diagnostic> errors = [.. VisualScriptTesting.Errors(analyzed)];
		Diagnostic diagnostic = Assert.Single(errors);
		Assert.Equal(Enum.Parse<DiagnosticCode>(code), diagnostic.Code);
		Assert.Equal(message, diagnostic.Message);
		Assert.Equal(expression, diagnostic.Node);
		// Every finding names the setting Formula (spec-slice3 section 3.3.9); one about the formula's text, its span too.
		Assert.Equal("Formula", diagnostic.Setting);
		(int, int)? span = start < 0 ? null : (start, end);
		Assert.Equal(span, diagnostic.Span);
		// A formula that does not read leaves Result untyped; one that does not type-check as well.
		Assert.False(VisualScriptTesting.Pin(analyzed, expression, "Result").Type.HasValues);
	}

	[Fact]
	public void AnUnknownFunctionSuggestsTheNearestNamesSwapsIncluded()
	{
		AnalyzedDocument analyzed = Analyze("\"a * b + sni(t)\"");
		Diagnostic diagnostic = VisualScriptTesting.Single(analyzed, DiagnosticCode.InvalidExpression);
		Assert.Equal(["sin", "asin", "int"], diagnostic.Suggestions);
		Assert.Equal([], VisualScriptTesting.Single(Analyze("\"zzzzzz(1)\""), DiagnosticCode.InvalidExpression).Suggestions);
		Assert.Equal("unknown function zzzzzz", VisualScriptTesting.Single(Analyze("\"zzzzzz(1)\""), DiagnosticCode.InvalidExpression).Message);
	}

	// F's pinned text (reports/s3-f.md): the analysis JSON writes Setting and Span after Variable, before Message.
	[Fact]
	public void TheDiagnosticOfAFormulaWritesItsSettingAndSpan()
	{
		Diagnostic diagnostic = new(DiagnosticCode.InvalidExpression, 0, "unknown function sni: did you mean sin?")
		{
			Node = 12,
			Setting = "Formula",
			Span = (8, 11),
			Suggestions = ["sin", "sign", "min"],
		};
		Assert.Equal(
			"""[{"Severity":"Error","Code":"InvalidExpression","Function":0,"Node":12,"Setting":"Formula","Span":[8,11],"Message":"unknown function sni: did you mean sin?","Suggestions":["sin","sign","min"]}]""",
			Encoding.UTF8.GetString(AnalysisWriter.WriteDiagnostics([diagnostic])));
	}

	[Fact]
	public void SpansAreUtf8BytesAndPositionsCountCodePoints()
	{
		// "é" is two bytes, "😀" four bytes and one code point (two UTF-16 units).
		Diagnostic diagnostic = VisualScriptTesting.Single(Analyze(RuntimeTesting.Quote("é😀 + (a")), DiagnosticCode.InvalidExpression);
		Assert.Equal("unexpected 'é'", diagnostic.Message);
		Assert.Equal((0, 2), diagnostic.Span);
		diagnostic = VisualScriptTesting.Single(Analyze(RuntimeTesting.Quote("a\t+ b + (😀")), DiagnosticCode.InvalidExpression);
		Assert.Equal("unexpected '😀'", diagnostic.Message);
		Assert.Equal((9, 13), diagnostic.Span);
		diagnostic = VisualScriptTesting.Single(Analyze(RuntimeTesting.Quote("x\r\n\t + (\t(a)")), DiagnosticCode.InvalidExpression);
		Assert.Equal("expected ')' to close the '(' at line 2, column 5", diagnostic.Message);
		Assert.Equal((7, 8), diagnostic.Span);
	}

	[Theory]
	[MemberData(nameof(InputOrderRows))]
	public void InputsAreNamedInOrderOfFirstUse(string formula, string expected)
	{
		AnalyzedDocument analyzed = Analyze(RuntimeTesting.Quote(formula));
		Assert.Equal(expected.Length > 0 ? expected.Split(',') : [], InputNames(analyzed));
	}

	// Tests/Data/VisualScripting/Formulas.json pins the input rule with the formulas of these rows and of the fuzz test's
	// generator: the analysis gives each formula exactly the inputs it lists (the editor's formula lister is tested against
	// the same file). When the rule or the formulas change, the expected file is written next to the test executable.
	[Fact]
	public void InputsFollowTheCorpus()
	{
		List<(string Formula, string[] Inputs)> expected = [];
		foreach (string formula in RowFormulas().Concat(ExpressionFuzzTests.CorpusFormulas()))
		{
			expected.Add((formula, [.. InputNames(Analyze(RuntimeTesting.Quote(formula)))]));
		}
		List<(string Formula, string[] Inputs)> stored = [];
		using (JsonDocument corpus = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(VisualScriptTesting.FixtureDirectory, "Formulas.json"))))
		{
			foreach (JsonElement entry in corpus.RootElement.EnumerateArray())
			{
				stored.Add((entry.GetProperty("Formula").GetString()!, [.. entry.GetProperty("Inputs").EnumerateArray().Select(static name => name.GetString()!)]));
			}
		}
		bool same = expected.Count == stored.Count &&
			expected.Zip(stored).All(static pair => pair.First.Formula == pair.Second.Formula && pair.First.Inputs.SequenceEqual(pair.Second.Inputs));
		if (!same)
		{
			string written = Path.Combine(AppContext.BaseDirectory, "ExpectedFixtures", "Formulas.json");
			Directory.CreateDirectory(Path.GetDirectoryName(written)!);
			File.WriteAllText(written, WriteCorpus(expected));
			Assert.Fail($"Tests/Data/VisualScripting/Formulas.json is not the corpus the analysis gives: the expected file is {written}, copy it over the fixture.");
		}
	}

	// A chain (a + a + ..., a && a || ..., v.X.X ...) is as deep as it is long, and analysis runs on the engine's main
	// thread, whose stack the editor shares: the longest chains the caps allow analyze, compile and run on a small stack.
	[Fact]
	public void TheLongestChainsNeedLittleStack()
	{
		// Each formula has 1023 parts: 512 operands and 511 operators.
		const int terms = ExpressionParser.MaxParts / 2;
		static string Chain(string operand, string op, int count) => string.Join(op, Enumerable.Repeat(operand, count));
		(string Formula, string Inputs, string Expected)[] rows =
		[
			(Chain("a", " + ", terms), "a:int=1", "512"),
			(Chain("a", " * ", terms - 1) + " + f", "a:int=1;f:float=0.5", "1.5"),
			(Chain("v", " + ", terms), "v:Strada.Vector2=[1.0, 2.0]", "(512, 1024)"),
			(Chain("a", " && ", terms - 1) + " || a", "a:bool=true", "True"),
			(Chain("t", " == ", terms), "t:bool=true", "True"),
		];
		string components = "v" + string.Concat(Enumerable.Repeat(".X", ExpressionParser.MaxParts - 1));
		ExceptionDispatchInfo? failure = null;
		Thread thread = new(() =>
		{
			try
			{
				foreach ((string formula, string inputs, string expected) in rows)
				{
					(DocumentBuilder document, uint expression) = Build(RuntimeTesting.Quote(formula), inputs);
					GraphBuilder graph = document.Graph;
					graph.Flow(graph.Node("Event.Create"), graph.Write(expression, "Result"));
					TestRuntime runtime = TestRuntime.Start(document);
					Assert.Equal(VisualScriptStatus.Success, runtime.Invoke(ScriptEvent.Create));
					Assert.Equal([expected], runtime.Records);
				}
				Diagnostic diagnostic = VisualScriptTesting.Single(Analyze(RuntimeTesting.Quote(components)), DiagnosticCode.InvalidExpression);
				Assert.Equal("v is float: it has no components", diagnostic.Message);
			}
			catch (Exception exception)
			{
				failure = ExceptionDispatchInfo.Capture(exception);
			}
		}, 256 * 1024);
		thread.Start();
		thread.Join();
		failure?.Throw();
	}

	[Fact]
	public void UnlinkedInputsAreFloats()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint expression = graph.Node("Expression", """{ "Formula": "a / b + c" }""", """{ "a": 7, "b": 2 }""");
		graph.Flow(graph.Node("Event.Create"), graph.Write(expression, "Result"));
		AnalyzedDocument analyzed = GraphAnalyzer.Analyze(RuntimeTesting.Library, Encoding.UTF8.GetBytes(document.Build()));
		VisualScriptTesting.AssertNoErrors(analyzed);
		Assert.All(["a", "b", "c"], name => Assert.Equal("float", VisualScriptTesting.Pin(analyzed, expression, name, PinDirection.In).Type.Id));
		Assert.Equal(7.0f, VisualScriptTesting.Pin(analyzed, expression, "a", PinDirection.In).Value);
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["3.5"], runtime.Records);
	}

	[Fact]
	public void ALinkOfAnotherTypeIsATypeMismatch()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint expression = graph.Node("Expression", """{ "Formula": "a + 1" }""");
		uint text = graph.Literal("string", "\"x\"");
		graph.Link(text, "Value", expression, "a");
		uint write = graph.Node(RuntimeTesting.Write);
		graph.Link(expression, "Result", write, "Text");
		AnalyzedDocument analyzed = GraphAnalyzer.Analyze(RuntimeTesting.Library, Encoding.UTF8.GetBytes(document.Build()));

		Diagnostic mismatch = VisualScriptTesting.Single(analyzed, DiagnosticCode.TypeMismatch);
		Assert.Equal("Expression's a takes a bool, a number or a vector (Vector2, Vector3, Vector4), not string", mismatch.Message);
		Assert.False(VisualScriptTesting.Has(analyzed, DiagnosticCode.InvalidExpression));
		Assert.False(VisualScriptTesting.Pin(analyzed, expression, "a", PinDirection.In).Type.HasValues);
		Assert.False(VisualScriptTesting.Pin(analyzed, expression, "Result").Type.HasValues);
		// The formula is not typed: Result's link reports that it gives no value; the input itself raises nothing more.
		Diagnostic unresolved = VisualScriptTesting.Single(analyzed, DiagnosticCode.UnresolvedWildcard);
		Assert.NotNull(unresolved.Link);
	}

	[Fact]
	public void IntegerDivisionByZeroAbortsNamingTheSubExpression()
	{
		(DocumentBuilder document, uint expression) = Build("\"n + (m / d)\"", "n:int=1;m:int=5;d:int=0");
		GraphBuilder graph = document.Graph;
		graph.Flow(graph.Node("Event.Create"), graph.Write(expression, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		Assert.Equal(VisualScriptStatus.Failure, runtime.Invoke(ScriptEvent.Create));
		Assert.Empty(runtime.Records);
		Assert.Equal(TestRuntime.On("Expression", expression) + "integer division by zero in m / d", runtime.SingleError());
	}

	[Fact]
	public void TheSmallestValueDividedByMinusOneAbortsAsTheOperatorDoes()
	{
		(DocumentBuilder document, uint expression) = Build("\"a % b\"", "a:long=\"-9223372036854775808\";b:int=-1");
		GraphBuilder graph = document.Graph;
		graph.Flow(graph.Node("Event.Create"), graph.Write(expression, "Result"));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(TestRuntime.On("Expression", expression) + "integer overflow: the remainder of the smallest long divided by -1 in a % b", runtime.SingleError());
	}

	[Fact]
	public void ABrokenFormulaKeepsTheLastWorkingProgram()
	{
		string Graph(string formula)
		{
			DocumentBuilder document = new();
			GraphBuilder graph = document.Graph;
			uint expression = graph.Node("Expression", $$"""{ "Formula": {{RuntimeTesting.Quote(formula)}} }""", """{ "a": 2 }""");
			graph.Flow(graph.Node("Event.Update"), graph.Write(expression, "Result"));
			return document.Build();
		}
		TestRuntime runtime = TestRuntime.Start(Graph("a + 1"));
		runtime.Invoke(ScriptEvent.Update);

		// While the formula is half typed, the program is broken and the instance keeps running the working one.
		IReadOnlyList<Diagnostic> diagnostics = runtime.Load(Graph("a +"));
		Assert.Contains(diagnostics, static diagnostic => diagnostic.Code == DiagnosticCode.InvalidExpression);
		runtime.Invoke(ScriptEvent.Update);
		runtime.LoadWorking(Graph("a * 10"));
		runtime.Invoke(ScriptEvent.Update);
		Assert.Equal(["3", "3", "20"], runtime.Records);
	}

	[Fact]
	public void ResultSplitsLate()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint expression = graph.Node("Expression", """{ "Formula": "vec3(a, a * 2, 7)" }""", """{ "a": 1.5 }""", split: """{ "Outputs": ["Result"] }""");
		graph.Flow(graph.Node("Event.Create"), graph.Write(expression, "Result.Y"));
		AnalyzedDocument analyzed = GraphAnalyzer.Analyze(RuntimeTesting.Library, Encoding.UTF8.GetBytes(document.Build()));
		VisualScriptTesting.AssertNoErrors(analyzed);
		Assert.Equal(["a", "Result.X", "Result.Y", "Result.Z"], VisualScriptTesting.Node(analyzed, expression).GetShownPins().Select(static pin => pin.Name));
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["3"], runtime.Records);
	}

	[Fact]
	public void AnExpressionIsPureAndCannotBeDisabled()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint expression = graph.Node("Expression", """{ "Formula": "a * 3" }""", """{ "a": 2 }""", enabled: "Disabled");
		graph.Flow(graph.Node("Event.Create"), graph.Write(expression, "Result"));
		AnalyzedDocument analyzed = GraphAnalyzer.Analyze(RuntimeTesting.Library, Encoding.UTF8.GetBytes(document.Build()));
		Assert.Equal(DiagnosticSeverity.Warning, VisualScriptTesting.Single(analyzed, DiagnosticCode.DisabledIgnored).Severity);
		TestRuntime runtime = TestRuntime.Start(document);

		runtime.Invoke(ScriptEvent.Create);
		Assert.Equal(["6"], runtime.Records);
	}

	[Fact]
	public void AnExpressionIsOneStep()
	{
		// As LimitTests' counting loop: 4 steps per iteration (the loop, the Set, its Get and the Expression), however many
		// operators the formula has.
		static TestRuntime Counting(int count)
		{
			DocumentBuilder document = new();
			uint counter = document.Variable("Counter", "int", "0");
			GraphBuilder graph = document.Graph;
			uint loop = graph.Node("Flow.ForLoop", inputs: $$"""{ "First": 1, "Last": {{count}} }""");
			uint add = graph.Node("Expression", """{ "Formula": "c + 1 + 0 * c - 0 + 0 * 2 - c * 0 + 0 + 0 - 0" }""");
			uint set = graph.Set(counter);
			graph.Link(graph.Get(counter), "Value", add, "c");
			graph.Link(add, "Result", set, "Value");
			graph.Flow(graph.Node("Event.Create"), loop);
			graph.Flow(loop, set, "Body");
			return TestRuntime.Start(document);
		}
		TestRuntime fits = Counting(249_999);
		Assert.Equal(VisualScriptStatus.Success, fits.Invoke(ScriptEvent.Create));
		Assert.Equal(249_999, fits.Variable("Counter"));
		TestRuntime exceeds = Counting(250_000);
		Assert.Equal(VisualScriptStatus.Failure, exceeds.Invoke(ScriptEvent.Create));
		Assert.Equal(249_999, exceeds.Variable("Counter"));
	}

	// A library over part of the API has no Vector4: a formula that gives one is refused, one that only uses one runs.
	[Fact]
	public void AFormulaGivingATypeTheLibraryLacksIsRefused()
	{
		NodeLibrary library = NodeLibraryBuilder.Build(new NodeLibrarySources { ApiTypes = [typeof(Log)] });
		Assert.Null(library.FindType(typeof(Vector4)));
		AnalyzedDocument refused = VisualScriptTesting.Analyze(library, Build(RuntimeTesting.Quote("vec4(1, 2, 3, 4)"), "").Document.Build());
		Diagnostic diagnostic = VisualScriptTesting.Single(refused, DiagnosticCode.InvalidExpression);
		Assert.Equal("the formula gives a Vector4, a type this node library does not have", diagnostic.Message);
		Assert.Equal((0, 16), diagnostic.Span);
		AnalyzedDocument used = VisualScriptTesting.Analyze(library, Build(RuntimeTesting.Quote("vec4(1, 2, 3, 4).W"), "").Document.Build());
		VisualScriptTesting.AssertNoErrors(used);
		Assert.Equal("float", VisualScriptTesting.Pin(used, 1, "Result").Type.Id);
	}

	[Fact]
	public void TheLibraryDescribesTheExpressionNode()
	{
		NodeDefinition node = VisualScriptTesting.LibraryNode("Expression");
		Assert.Equal("Expression", node.Title);
		Assert.Equal("Math", node.Category);
		Assert.Equal(NodeKind.Operator, node.Kind);
		Assert.True(node.Pure);
		Assert.False(node.Latent);
		Assert.Equal(["formula", "math", "calculate", "equation", "expr", "="], node.Keywords);
		PinDescription result = Assert.Single(node.Pins);
		Assert.Equal(("Result", PinDirection.Out, PinConstraint.Any), (result.Name, result.Direction, result.Constraint));
		Assert.True(result.Type.IsWildcard);
		SettingDescription formula = Assert.Single(node.Settings);
		Assert.Equal(("Formula", SettingKind.Expression, true, (object?)string.Empty), (formula.Name, formula.Kind, formula.Required, formula.Default));
		JsonElement written = VisualScriptTesting.ParseObject(LibraryWriter.Write(VisualScriptTesting.Library)).GetProperty("Nodes").EnumerateArray()
			.Single(static entry => entry.GetProperty("Type").GetString() == "Expression");
		Assert.Equal("""{"Name":"Formula","Kind":"Expression","Default":"","Required":true}""", written.GetProperty("Settings")[0].GetRawText());
		Assert.Equal("""{"Name":"Result","Direction":"Out","Type":"*","Constraint":"Any"}""", written.GetProperty("Pins")[0].GetRawText());
	}

	[Fact]
	public void TheAnalysisShowsTheFormulaAsTheSubtitleAndItsInputsWithTheirConstraint()
	{
		AnalyzedDocument analyzed = Analyze(RuntimeTesting.Quote("  a *\n\n   b +   sin(t)  "));
		AnalyzedNode node = VisualScriptTesting.Node(analyzed, 1);
		Assert.Equal("a * b + sin(t)", node.Subtitle);
		Assert.All(node.Pins.Where(static pin => pin.IsInput), static pin => Assert.Equal(PinConstraint.Expression, pin.Description.Constraint));
		string longFormula = string.Join(" + ", Enumerable.Range(0, 30).Select(static index => $"v{index}"));
		string subtitle = VisualScriptTesting.Node(Analyze(RuntimeTesting.Quote(longFormula)), 1).Subtitle;
		Assert.Equal(longFormula[..59] + "…", subtitle);
		JsonElement written = VisualScriptTesting.ParseObject(AnalysisWriter.Write(analyzed)).GetProperty("Nodes")[0];
		Assert.Equal("a * b + sin(t)", written.GetProperty("Subtitle").GetString());
		Assert.Equal("Expression", written.GetProperty("Pins")[0].GetProperty("Constraint").GetString());
	}

	[Fact]
	public void ABrokenFormulaKeepsItsInputsAndTakesTheLinksItIsGiven()
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint expression = graph.Node("Expression", """{ "Formula": "speed * (dt + " }""");
		uint other = graph.Literal("float", "1.0");
		graph.Link(other, "Value", expression, "speed");
		graph.Link(other, "Value", expression, "later");
		AnalyzedDocument analyzed = GraphAnalyzer.Analyze(RuntimeTesting.Library, Encoding.UTF8.GetBytes(document.Build()));
		Assert.Equal(["speed", "dt", "later"], InputNames(analyzed));
		Diagnostic diagnostic = Assert.Single(analyzed.Diagnostics);
		Assert.Equal(DiagnosticCode.InvalidExpression, diagnostic.Code);
	}

	// A document whose event graph starts with the Expression node of the Formula setting (raw JSON, absent when null), with a
	// Literal linked into each input named in inputs ("name:type=value;...").
	internal static (DocumentBuilder Document, uint Expression) Build(string? setting, string inputs)
	{
		DocumentBuilder document = new();
		GraphBuilder graph = document.Graph;
		uint expression = graph.Node("Expression", setting != null ? $$"""{ "Formula": {{setting}} }""" : null);
		foreach (string input in inputs.Split(';', StringSplitOptions.RemoveEmptyEntries))
		{
			int colon = input.IndexOf(':', StringComparison.Ordinal);
			int equals = input.IndexOf('=', StringComparison.Ordinal);
			uint literal = graph.Literal(input[(colon + 1)..equals], input[(equals + 1)..]);
			graph.Link(literal, "Value", expression, input[..colon]);
		}
		return (document, expression);
	}

	private static AnalyzedDocument Analyze(string setting) =>
		GraphAnalyzer.Analyze(RuntimeTesting.Library, Encoding.UTF8.GetBytes(Build(setting, "").Document.Build()));

	// The input pins of the document's Expression node (node 1), in order.
	private static IEnumerable<string> InputNames(AnalyzedDocument analyzed) =>
		VisualScriptTesting.Node(analyzed, 1).Pins.Where(static pin => pin.IsInput).Select(static pin => pin.Name);

	// The corpus file: one entry per line.
	private static string WriteCorpus(List<(string Formula, string[] Inputs)> entries)
	{
		JsonSerializerOptions options = new() { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };
		StringBuilder text = new("[\n");
		for (int index = 0; index < entries.Count; index++)
		{
			text.Append("\t{\"Formula\": ").Append(JsonSerializer.Serialize(entries[index].Formula, options)).Append(", \"Inputs\": [")
				.Append(string.Join(", ", entries[index].Inputs.Select(name => JsonSerializer.Serialize(name, options)))).Append("]}")
				.Append(index + 1 < entries.Count ? ",\n" : "\n");
		}
		return text.Append("]\n").ToString();
	}
}
