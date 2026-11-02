using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// Formulas against the graphs they abbreviate (spec-slice3 section 3.9): formulas drawn from the grammar over inputs of
// random types give the bits the same computation built from Operator, Select, Convert, Make, Break and the API's math
// members gives, aborting where it aborts; damaged formulas never throw; reading a formula is linear in its length.
public sealed class ExpressionFuzzTests
{
	private const ulong Seed = 0x464F524D554C4153UL;
	private const ulong CorpusSeed = 0x434F52505553UL;

	// The generator's formulas of Tests/Data/VisualScripting/Formulas.json: 400 drawn from the grammar and 100 damaged ones,
	// from a fixed seed.
	internal static IEnumerable<string> CorpusFormulas()
	{
		FuzzRandom random = new(CorpusSeed);
		for (int index = 0; index < 500; index++)
		{
			FormulaGenerator generator = new(random, null);
			string formula = generator.Generate(generator.PickKind(), 1 + random.Pick(4)).Text;
			yield return index < 400 ? formula : FormulaGenerator.Damage(random, formula);
		}
	}

	[Fact]
	public void FormulasComputeWhatTheirGraphsCompute()
	{
		FuzzRandom random = new(Seed);
		int rounds = Fuzzing.GetRounds(300);
		int aborted = 0;
		for (int round = 0; round < rounds; round++)
		{
			DocumentBuilder document = new();
			GraphBuilder graph = document.Graph;
			FormulaGenerator generator = new(random, graph);
			SlotKind kind = generator.PickKind();
			Term term = generator.Generate(kind, 1 + random.Pick(4));
			string type = ExpressionChecker.TypeOf(RuntimeTesting.Library, kind)!.Id;
			uint oracle = document.Variable("Oracle", type);
			uint result = document.Variable("Formula", type);
			uint expression = graph.Node("Expression", $$"""{ "Formula": {{RuntimeTesting.Quote(term.Text)}} }""");
			foreach ((string name, uint literal) in generator.Inputs)
			{
				graph.Link(literal, "Value", expression, name);
			}
			uint setOracle = graph.Set(oracle);
			graph.Link(term.Node, term.Pin, setOracle, "Value");
			graph.Flow(graph.Node("Event.Create"), setOracle);
			graph.Writes(setOracle, "Then", "oracle");
			uint setFormula = graph.Set(result);
			graph.Link(expression, "Result", setFormula, "Value");
			graph.Flow(graph.Node("Event.Update"), setFormula);
			graph.Writes(setFormula, "Then", "formula");
			string context = $"round {round}: {term.Text}";
			AnalyzedDocument analyzed = GraphAnalyzer.Analyze(RuntimeTesting.Library, Encoding.UTF8.GetBytes(document.Build()));
			Assert.True(!analyzed.HasErrors, $"{context}\n{VisualScriptTesting.Describe(analyzed.Diagnostics)}");
			Assert.True(VisualScriptTesting.Pin(analyzed, expression, "Result").Type.Id == type, $"{context}: the formula is not {type}");
			TestRuntime runtime = TestRuntime.Start(document);

			runtime.Invoke(ScriptEvent.Create);
			List<string> oracleErrors = [.. runtime.Environment.Errors];
			runtime.Invoke(ScriptEvent.Update);
			List<string> formulaErrors = [.. runtime.Environment.Errors.Skip(oracleErrors.Count)];
			if (oracleErrors.Count > 0)
			{
				aborted++;
				Assert.True(formulaErrors.Count == 1, $"{context}: the graph aborted ({oracleErrors[0]}) and the formula did not");
				string expected = Message(oracleErrors[0]) + " in ";
				Assert.True(Message(formulaErrors[0]).StartsWith(expected, StringComparison.Ordinal), $"{context}: {formulaErrors[0]} is not {expected}...");
				continue;
			}
			Assert.True(formulaErrors.Count == 0, $"{context}: the formula aborted ({string.Join("; ", formulaErrors)}) and the graph did not");
			Assert.Equal(["oracle", "formula"], runtime.Records);
			object? computed = runtime.Variable("Formula");
			object? expectedValue = runtime.Variable("Oracle");
			Assert.True(SameBits(expectedValue, computed), $"{context}: the graph gave {Text(expectedValue)} and the formula {Text(computed)}");
		}
		Assert.True(aborted > 0, "no formula aborted");
	}

	[Fact]
	public void DamagedFormulasNeverThrowOutOfAnalysisOrEvaluation()
	{
		FuzzRandom random = new(Seed ^ 0xDA);
		int rounds = Fuzzing.GetRounds(400);
		int ran = 0;
		for (int round = 0; round < rounds; round++)
		{
			FormulaGenerator generator = new(random, null);
			string formula = FormulaGenerator.Damage(random, generator.Generate(generator.PickKind(), 1 + random.Pick(4)).Text);
			DocumentBuilder document = new();
			GraphBuilder graph = document.Graph;
			uint expression = graph.Node("Expression", $$"""{ "Formula": {{RuntimeTesting.Quote(formula)}} }""");
			graph.Flow(graph.Node("Event.Update"), graph.Write(expression, "Result"));
			try
			{
				TestRuntime runtime = new();
				IReadOnlyList<Diagnostic> diagnostics = runtime.Load(document.Build());
				if (diagnostics.Any(static diagnostic => diagnostic.Severity == DiagnosticSeverity.Error))
				{
					continue;
				}
				Assert.Equal(VisualScriptStatus.Success, runtime.Create());
				runtime.Invoke(ScriptEvent.Update);
				ran++;
			}
			catch (Exception exception) when (exception is not Xunit.Sdk.IAssertionException)
			{
				Assert.Fail($"round {round}: {formula}\nthrew {exception}");
			}
		}
		Assert.True(ran > 0, "no damaged formula ran");
	}

	// The lexer and the parser count their steps: at most 64 per byte of a formula, whatever it holds.
	[Fact]
	public void ReadingAFormulaIsLinearInItsLength()
	{
		List<string> formulas =
		[
			new string('(', 4000),
			string.Concat(Enumerable.Repeat("-", 4000)) + "a",
			"a" + string.Concat(Enumerable.Repeat("+a", 2000)),
			new string('9', 4000),
			new string('a', 4000),
			string.Concat(Enumerable.Repeat("sin(", 1000)),
			string.Concat(Enumerable.Repeat("a?b:", 1000)) + "c",
			string.Concat(Enumerable.Repeat("v.", 2000)),
			new string('#', 4000),
			string.Concat(Enumerable.Repeat("é", 2000)),
			string.Concat(Enumerable.Repeat("1e+", 1300)),
			string.Concat(Enumerable.Repeat("(a,", 1300)),
			string.Concat(Enumerable.Repeat(" \n\t", 1300)),
		];
		FuzzRandom random = new(Seed ^ 0x11);
		for (int round = 0; round < Fuzzing.GetRounds(400); round++)
		{
			FormulaGenerator generator = new(random, null);
			string formula = generator.Generate(generator.PickKind(), 1 + random.Pick(5)).Text;
			formulas.Add(formula);
			formulas.Add(FormulaGenerator.Damage(random, formula));
		}
		foreach (string formula in formulas)
		{
			int bytes = Encoding.UTF8.GetByteCount(formula);
			ParsedFormula parsed = ExpressionParser.Parse(formula, static _ => []);
			Assert.True(parsed.Steps <= 64 * Math.Max(1, bytes), $"{parsed.Steps} steps for {bytes} bytes: {formula[..Math.Min(formula.Length, 80)]}");
		}
	}

	// The message of an aborting error, after the node and instance it names.
	private static string Message(string error)
	{
		string marker = FormattableString.Invariant($"on Hero ({TestRuntime.Hero}): ");
		int at = error.IndexOf(marker, StringComparison.Ordinal);
		return at >= 0 ? error[(at + marker.Length)..] : error;
	}

	// Bit for bit for integers and vectors; floating-point numbers equal or both NaN.
	private static bool SameBits(object? expected, object? actual) => (expected, actual) switch
	{
		(float a, float b) => a == b || (float.IsNaN(a) && float.IsNaN(b)),
		(double a, double b) => a == b || (double.IsNaN(a) && double.IsNaN(b)),
		(Vector2 a, Vector2 b) => Bits(a.X) == Bits(b.X) && Bits(a.Y) == Bits(b.Y),
		(Vector3 a, Vector3 b) => Bits(a.X) == Bits(b.X) && Bits(a.Y) == Bits(b.Y) && Bits(a.Z) == Bits(b.Z),
		(Vector4 a, Vector4 b) => Bits(a.X) == Bits(b.X) && Bits(a.Y) == Bits(b.Y) && Bits(a.Z) == Bits(b.Z) && Bits(a.W) == Bits(b.W),
		_ => Equals(expected, actual),
	};

	private static int Bits(float value) => BitConverter.SingleToInt32Bits(value);

	private static string Text(object? value) => value switch
	{
		float number => number.ToString("R", CultureInfo.InvariantCulture),
		double number => number.ToString("R", CultureInfo.InvariantCulture),
		null => "null",
		_ => Convert.ToString(value, CultureInfo.InvariantCulture) ?? string.Empty,
	};
}

// A part of a generated formula: its text and type, and the node output computing it in the oracle graph.
internal readonly record struct Term(string Text, SlotKind Kind, uint Node, string Pin);

// Draws formulas from the grammar of spec-slice3 section 3.3, typed as it types them, and builds the same computation as a
// graph of nodes (the oracle) when it has one. Compound parts are parenthesized, so the oracle's structure is the formula's.
// No integer division or modulo is placed where &&, || or ?: may skip it: the graph evaluates both sides, and both must
// meet the same aborting operations.
internal sealed class FormulaGenerator
{
	private const string Mathf = "Call:Strada.Mathf.";

	private static readonly SlotKind[] s_Kinds =
	[
		SlotKind.Bool, SlotKind.Int, SlotKind.UInt, SlotKind.Long, SlotKind.ULong, SlotKind.Float, SlotKind.Double, SlotKind.Vector2, SlotKind.Vector3,
		SlotKind.Vector4,
	];

	private static readonly string[] s_Names = ["a", "b", "speed", "dt", "exp", "length", "x", "int", "v", "_t0", "Pos", "w2"];

	private static readonly string[] s_Ints = ["0", "1", "-1", "7", "-13", "2147483647", "-2147483648", "100000"];
	private static readonly string[] s_UInts = ["0", "1", "3", "4000000000", "4294967295"];
	private static readonly string[] s_Longs = ["\"0\"", "\"-1\"", "\"5\"", "\"9223372036854775807\"", "\"-9223372036854775808\"", "\"3000000000\""];
	private static readonly string[] s_ULongs = ["\"0\"", "\"1\"", "\"18446744073709551615\"", "\"5000000000\""];
	private static readonly string[] s_Floats = ["0.0", "-0.0", "1.5", "-2.25", "0.1", "1e30", "-0.001", "3.0", "1e38", "0.5"];

	private static readonly string[] s_IntConstants = ["0", "1", "2", "3", "7", "10", "100", "2147483647"];
	private static readonly string[] s_FloatConstants = ["0.5", "1.5", "2.0", "0.1", "1e-3", "1e30", "3.25", "4.0"];
	private static readonly string[] s_DoubleConstants = ["0.5", "2.0", "1e300", "0.1"];

	private static readonly string[] s_Damage =
	[
		"a", "b", "x", "X", "y", "Z", "0", "1", "9", ".", "_", "(", ")", "+", "-", "*", "/", "%", "<", ">", "=", "!", "&", "|", "?", ":", ",", "#", " ",
		"\n", "\t", "é", "😀", "sin(", "pi", "true", "1e", "d", "Self", "Result", "vec3(", "int(", "&&", "==", "..",
	];

	private readonly FuzzRandom m_Random;
	private readonly GraphBuilder? m_Graph;
	private readonly List<(string Name, SlotKind Kind, uint Literal)> m_Inputs = [];

	// graph is where the oracle is built; without one only the formula is drawn.
	internal FormulaGenerator(FuzzRandom random, GraphBuilder? graph)
	{
		m_Random = random;
		m_Graph = graph;
	}

	// The inputs the formula uses and the Literal node of each.
	internal IEnumerable<(string Name, uint Literal)> Inputs => m_Inputs.Select(static input => (input.Name, input.Literal));

	internal SlotKind PickKind() => s_Kinds[m_Random.Pick(s_Kinds.Length)];

	// A formula of the kind, at most depth levels of operations deep.
	internal Term Generate(SlotKind kind, int depth) => Generate(kind, depth, false);

	// A formula with one to three characters inserted, deleted, repeated or swapped.
	internal static string Damage(FuzzRandom random, string formula)
	{
		StringBuilder text = new(formula);
		int edits = 1 + random.Pick(3);
		for (int edit = 0; edit < edits; edit++)
		{
			int at = random.Pick(text.Length + 1);
			// Never between the two halves of a surrogate pair.
			if (at > 0 && at < text.Length && char.IsLowSurrogate(text[at]))
			{
				at--;
			}
			switch (random.Pick(4))
			{
				case 0:
					text.Insert(at, s_Damage[random.Pick(s_Damage.Length)]);
					break;
				case 1 when text.Length > 0:
					RemoveCodePoint(text, Math.Min(at, text.Length - 1));
					break;
				case 2 when at < text.Length:
					int length = 1 + random.Pick(Math.Min(8, text.Length - at));
					string copy = text.ToString(at, length);
					if (char.IsHighSurrogate(copy[^1]) || char.IsLowSurrogate(copy[0]))
					{
						break;
					}
					text.Insert(at, copy);
					break;
				default:
					text.Insert(at, s_Damage[random.Pick(s_Damage.Length)]);
					break;
			}
		}
		return text.ToString();
	}

	private static void RemoveCodePoint(StringBuilder text, int at)
	{
		if (char.IsLowSurrogate(text[at]) && at > 0)
		{
			at--;
		}
		text.Remove(at, at + 1 < text.Length && char.IsHighSurrogate(text[at]) && char.IsLowSurrogate(text[at + 1]) ? 2 : 1);
	}

	// skippable: the part may not run (an operand that &&, || or ?: skips), so it holds no integer division.
	private Term Generate(SlotKind kind, int depth, bool skippable)
	{
		if (depth <= 0 || m_Random.Pick(5) == 0)
		{
			return Leaf(kind);
		}
		depth--;
		return kind switch
		{
			SlotKind.Bool => Bool(depth, skippable),
			SlotKind.Vector2 or SlotKind.Vector3 or SlotKind.Vector4 => VectorTerm(kind, depth, skippable),
			_ => Number(kind, depth, skippable),
		};
	}

	private Term Leaf(SlotKind kind)
	{
		if (m_Random.Pick(3) != 0 && Input(kind) is { } input)
		{
			return input;
		}
		switch (kind)
		{
			case SlotKind.Bool:
				string flag = m_Random.Pick(2) == 0 ? "true" : "false";
				return Constant(flag, kind, flag);
			case SlotKind.Int:
				string integer = s_IntConstants[m_Random.Pick(s_IntConstants.Length)];
				return Constant(integer, kind, integer);
			case SlotKind.Long:
				return Constant("3000000000", kind, "\"3000000000\"");
			case SlotKind.ULong:
				return Constant("18446744073709551615", kind, "\"18446744073709551615\"");
			case SlotKind.Float:
				string real = s_FloatConstants[m_Random.Pick(s_FloatConstants.Length)];
				return Constant(real, kind, real);
			case SlotKind.Double:
				string wide = s_DoubleConstants[m_Random.Pick(s_DoubleConstants.Length)];
				return Constant(wide + "d", kind, wide);
			case SlotKind.UInt:
				return Cast(SlotKind.UInt, Leaf(SlotKind.Int));
			default:
				return Construct(kind, 0, true);
		}
	}

	// An input of the kind: one the formula uses already, or a new one; null when every name is taken by inputs of other kinds.
	private Term? Input(SlotKind kind)
	{
		foreach ((string name, SlotKind inputKind, uint literal) in m_Inputs)
		{
			if (inputKind == kind && m_Random.Pick(2) == 0)
			{
				return new Term(name, kind, literal, "Value");
			}
		}
		List<string> free = [.. s_Names.Where(name => m_Inputs.All(input => input.Name != name))];
		if (free.Count == 0)
		{
			return m_Inputs.FindIndex(input => input.Kind == kind) is var index and >= 0
				? new Term(m_Inputs[index].Name, kind, m_Inputs[index].Literal, "Value")
				: null;
		}
		string chosen = free[m_Random.Pick(free.Count)];
		string value = kind switch
		{
			SlotKind.Bool => m_Random.Pick(2) == 0 ? "true" : "false",
			SlotKind.Int => s_Ints[m_Random.Pick(s_Ints.Length)],
			SlotKind.UInt => s_UInts[m_Random.Pick(s_UInts.Length)],
			SlotKind.Long => s_Longs[m_Random.Pick(s_Longs.Length)],
			SlotKind.ULong => s_ULongs[m_Random.Pick(s_ULongs.Length)],
			SlotKind.Float or SlotKind.Double => s_Floats[m_Random.Pick(s_Floats.Length)],
			_ => "[" + string.Join(", ", Enumerable.Range(0, ExpressionChecker.ComponentCount(kind)).Select(_ => s_Floats[m_Random.Pick(s_Floats.Length)])) + "]",
		};
		uint node = m_Graph?.Literal(TypeId(kind), value) ?? 0;
		m_Inputs.Add((chosen, kind, node));
		return new Term(chosen, kind, node, "Value");
	}

	private Term Constant(string text, SlotKind kind, string value) => new(text, kind, m_Graph?.Literal(TypeId(kind), value) ?? 0, "Value");

	private Term Bool(int depth, bool skippable)
	{
		switch (m_Random.Pick(6))
		{
			case 0:
				SlotKind numeric = PickNumeric();
				(Term left, Term right) = Operands(numeric, depth, skippable);
				string[] comparisons = ["<", "<=", ">", ">="];
				string[] nodes = ["Operator.Less", "Operator.LessOrEqual", "Operator.Greater", "Operator.GreaterOrEqual"];
				int which = m_Random.Pick(comparisons.Length);
				return Binary(left, comparisons[which], right, nodes[which], SlotKind.Bool);
			case 1:
				SlotKind compared = s_Kinds[m_Random.Pick(s_Kinds.Length)];
				(Term a, Term b) = compared == SlotKind.Bool || ExpressionChecker.IsVector(compared)
					? (Generate(compared, depth, skippable), Generate(compared, depth, skippable))
					: Operands(compared, depth, skippable);
				bool equal = m_Random.Pick(2) == 0;
				return Binary(a, equal ? "==" : "!=", b, equal ? "Operator.Equal" : "Operator.NotEqual", SlotKind.Bool);
			case 2:
				Term operand = Generate(SlotKind.Bool, depth, skippable);
				return Unary("!", operand, "Operator.Not", SlotKind.Bool);
			case 3:
			case 4:
				bool and = m_Random.Pick(2) == 0;
				Term first = Generate(SlotKind.Bool, depth, skippable);
				Term second = Generate(SlotKind.Bool, depth, true);
				return Binary(first, and ? "&&" : "||", second, and ? "Operator.And" : "Operator.Or", SlotKind.Bool);
			default:
				return Conditional(SlotKind.Bool, depth, skippable);
		}
	}

	private Term Number(SlotKind kind, int depth, bool skippable)
	{
		int choice = m_Random.Pick(kind == SlotKind.Float ? 7 : 5);
		switch (choice)
		{
			case 0:
			case 1:
				string[] operators = ["+", "-", "*", "/", "%"];
				string[] nodes = ["Operator.Add", "Operator.Subtract", "Operator.Multiply", "Operator.Divide", "Operator.Modulo"];
				int count = IsInteger(kind) && skippable ? 3 : 5;
				int which = m_Random.Pick(count);
				(Term left, Term right) = Operands(kind, depth, skippable);
				return Binary(left, operators[which], right, nodes[which], kind);
			case 2:
				if (kind is SlotKind.Int or SlotKind.Float or SlotKind.Double)
				{
					return Unary("-", Generate(kind, depth, skippable), "Operator.Negate", kind);
				}
				if (kind == SlotKind.Long && m_Random.Pick(2) == 0)
				{
					return Unary("-", Generate(SlotKind.UInt, depth, skippable), "Operator.Negate", kind);
				}
				return Cast(kind, Generate(PickNumeric(), depth, skippable));
			case 3:
				return Cast(kind, Generate(PickNumeric(), depth, skippable));
			case 4:
				return Conditional(kind, depth, skippable);
			case 5:
				SlotKind vector = (SlotKind)((int)SlotKind.Vector2 + m_Random.Pick(3));
				return VectorToFloat(vector, depth, skippable);
			default:
				return Function(depth, skippable);
		}
	}

	private Term VectorTerm(SlotKind kind, int depth, bool skippable)
	{
		switch (m_Random.Pick(7))
		{
			case 0:
				// Vector2 and Vector4 have no vector-by-vector division.
				int count = kind == SlotKind.Vector3 ? 4 : 3;
				string[] operators = ["+", "-", "*", "/"];
				string[] nodes = ["Operator.Add", "Operator.Subtract", "Operator.Multiply", "Operator.Divide"];
				int which = m_Random.Pick(count);
				return Binary(Generate(kind, depth, skippable), operators[which], Generate(kind, depth, skippable), nodes[which], kind);
			case 1:
				SlotKind scalar = PickScalar();
				Term vector = Generate(kind, depth, skippable);
				Term number = Generate(scalar, depth, skippable);
				return m_Random.Pick(3) switch
				{
					0 => Binary(vector, "*", number, "Operator.Multiply", kind),
					1 => Binary(number, "*", vector, "Operator.Multiply", kind),
					_ => Binary(vector, "/", number, "Operator.Divide", kind),
				};
			case 2:
				return Unary("-", Generate(kind, depth, skippable), "Operator.Negate", kind);
			case 3:
				return Construct(kind, depth, skippable);
			case 4:
				return Conditional(kind, depth, skippable);
			case 5:
				Term a = Generate(kind, depth, skippable);
				Term b = Generate(kind, depth, skippable);
				Term t = Generate(PickScalar(), depth, skippable);
				string type = TypeId(kind);
				return Call($"lerp({a.Text}, {b.Text}, {t.Text})", kind, $"Call:{type}.Lerp({type},{type},float)", a, b, t);
			default:
				if (kind == SlotKind.Vector3 && m_Random.Pick(2) == 0)
				{
					Term left = Generate(kind, depth, skippable);
					Term right = Generate(kind, depth, skippable);
					return Call($"cross({left.Text}, {right.Text})", kind, "Call:Strada.Vector3.Cross(Strada.Vector3,Strada.Vector3)", left, right);
				}
				if (kind != SlotKind.Vector4)
				{
					Term operand = Generate(kind, depth, skippable);
					return Call($"normalize({operand.Text})", kind, $"Get:{TypeId(kind)}.Normalized", operand);
				}
				return Construct(kind, depth, skippable);
		}
	}

	// length, dot or distance of vectors, or a component.
	private Term VectorToFloat(SlotKind vector, int depth, bool skippable)
	{
		string type = TypeId(vector);
		Term a = Generate(vector, depth, skippable);
		switch (m_Random.Pick(4))
		{
			case 0:
				return Call($"length({a.Text})", SlotKind.Float, $"Get:{type}.Length", a);
			case 1:
				Term b = Generate(vector, depth, skippable);
				return Call($"dot({a.Text}, {b.Text})", SlotKind.Float, $"Call:{type}.Dot({type},{type})", a, b);
			case 2 when vector != SlotKind.Vector4:
				Term other = Generate(vector, depth, skippable);
				return Call($"distance({a.Text}, {other.Text})", SlotKind.Float, $"Call:{type}.Distance({type},{type})", a, other);
			default:
				string[] components = vector switch
				{
					SlotKind.Vector2 => ["X", "Y"],
					SlotKind.Vector3 => ["X", "Y", "Z"],
					_ => ["X", "Y", "Z", "W"],
				};
				string component = components[m_Random.Pick(components.Length)];
				string written = m_Random.Pick(2) == 0 ? component : component.ToLowerInvariant();
				uint split = m_Graph?.Node("Break", $$"""{ "Type": "{{type}}" }""") ?? 0;
				Link(a, split, "Value");
				return new Term($"({a.Text}).{written}", SlotKind.Float, split, component);
		}
	}

	// A float function whose Mathf member computes what the formula's function computes.
	private Term Function(int depth, bool skippable)
	{
		switch (m_Random.Pick(6))
		{
			case 0:
				string[] unary = ["abs", "floor", "ceil", "round", "sqrt", "exp", "log", "sin", "cos", "tan", "asin", "acos", "atan"];
				string[] members = ["Abs", "Floor", "Ceil", "Round", "Sqrt", "Exp", "Log", "Sin", "Cos", "Tan", "Asin", "Acos", "Atan"];
				int which = m_Random.Pick(unary.Length);
				// abs has integer forms: its argument is a float; the others take any number below double.
				Term argument = Generate(which == 0 ? SlotKind.Float : PickScalar(), depth, skippable);
				return Call($"{unary[which]}({argument.Text})", SlotKind.Float, $"{Mathf}{members[which]}(float)", argument);
			case 1:
				bool min = m_Random.Pick(2) == 0;
				Term a = Generate(SlotKind.Float, depth, skippable);
				Term b = Generate(SlotKind.Float, depth, skippable);
				return Call($"{(min ? "min" : "max")}({a.Text}, {b.Text})", SlotKind.Float, $"{Mathf}{(min ? "Min" : "Max")}(float,float)", a, b);
			case 2:
				bool pow = m_Random.Pick(2) == 0;
				Term x = Generate(PickScalar(), depth, skippable);
				Term y = Generate(PickScalar(), depth, skippable);
				return Call($"{(pow ? "pow" : "atan2")}({x.Text}, {y.Text})", SlotKind.Float, $"{Mathf}{(pow ? "Pow" : "Atan2")}(float,float)", x, y);
			case 3:
				Term from = Generate(PickScalar(), depth, skippable);
				Term to = Generate(PickScalar(), depth, skippable);
				Term t = Generate(PickScalar(), depth, skippable);
				return Call($"lerp({from.Text}, {to.Text}, {t.Text})", SlotKind.Float, $"{Mathf}LerpUnclamped(float,float,float)", from, to, t);
			default:
				// clamp(x, lo, hi) is min(max(x, lo), hi).
				Term value = Generate(SlotKind.Float, depth, skippable);
				Term low = Generate(SlotKind.Float, depth, skippable);
				Term high = Generate(SlotKind.Float, depth, skippable);
				Term raised = Call(string.Empty, SlotKind.Float, $"{Mathf}Max(float,float)", value, low);
				Term clamped = Call(string.Empty, SlotKind.Float, $"{Mathf}Min(float,float)", raised, high);
				return clamped with { Text = $"clamp({value.Text}, {low.Text}, {high.Text})" };
		}
	}

	private Term Conditional(SlotKind kind, int depth, bool skippable)
	{
		Term condition = Generate(SlotKind.Bool, depth, skippable);
		(Term whenTrue, Term whenFalse) = kind == SlotKind.Bool || ExpressionChecker.IsVector(kind)
			? (Generate(kind, depth, true), Generate(kind, depth, true))
			: Operands(kind, depth, true);
		uint select = m_Graph?.Node("Select") ?? 0;
		Link(condition, select, "Condition");
		Link(whenTrue, select, "True");
		Link(whenFalse, select, "False");
		return new Term($"({condition.Text}{Space()}?{Space()}{whenTrue.Text}{Space()}:{Space()}{whenFalse.Text})", kind, select, "Result");
	}

	// vecN of numbers, each converted to float.
	private Term Construct(SlotKind kind, int depth, bool skippable)
	{
		string type = TypeId(kind);
		string[] fields = ["X", "Y", "Z", "W"];
		uint make = m_Graph?.Node("Make", $$"""{ "Type": "{{type}}" }""") ?? 0;
		List<string> texts = [];
		for (int index = 0; index < ExpressionChecker.ComponentCount(kind); index++)
		{
			Term component = depth > 0 ? Generate(PickNumeric(), depth - 1, skippable) : Leaf(PickNumeric());
			Link(component.Kind == SlotKind.Float ? component : Cast(SlotKind.Float, component) with { Text = component.Text }, make, fields[index]);
			texts.Add(component.Text);
		}
		return new Term($"vec{ExpressionChecker.ComponentCount(kind)}({string.Join(", ", texts)})", kind, make, "Value");
	}

	// Two operands whose join is the kind: one of the kind, the other of it or of a kind that widens to it.
	private (Term Left, Term Right) Operands(SlotKind kind, int depth, bool skippable)
	{
		SlotKind[] narrower = kind switch
		{
			SlotKind.Long => [SlotKind.Int, SlotKind.UInt],
			SlotKind.ULong => [SlotKind.UInt],
			SlotKind.Float => [SlotKind.Int, SlotKind.UInt, SlotKind.Long, SlotKind.ULong],
			SlotKind.Double => [SlotKind.Int, SlotKind.UInt, SlotKind.Long, SlotKind.ULong, SlotKind.Float],
			_ => [],
		};
		SlotKind other = narrower.Length > 0 && m_Random.Pick(2) == 0 ? narrower[m_Random.Pick(narrower.Length)] : kind;
		Term same = Generate(kind, depth, skippable);
		Term mixed = Generate(other, depth, skippable);
		return m_Random.Pick(2) == 0 ? (same, mixed) : (mixed, same);
	}

	private Term Cast(SlotKind kind, Term operand)
	{
		uint convert = m_Graph?.Node("Convert", $$"""{ "To": "{{TypeId(kind)}}" }""") ?? 0;
		Link(operand, convert, "Value");
		string name = ExpressionChecker.Name(kind);
		return new Term($"{name}({operand.Text})", kind, convert, "Result");
	}

	private Term Binary(Term left, string op, Term right, string type, SlotKind kind)
	{
		uint node = m_Graph?.Node(type) ?? 0;
		Link(left, node, "A");
		Link(right, node, "B");
		return new Term($"({left.Text}{Space()}{op}{Space()}{right.Text})", kind, node, "Result");
	}

	private Term Unary(string op, Term operand, string type, SlotKind kind)
	{
		uint node = m_Graph?.Node(type) ?? 0;
		Link(operand, node, "A");
		return new Term($"{op}({operand.Text})", kind, node, "Result");
	}

	// A member node whose data inputs take the arguments in order; its first data output is the term.
	private Term Call(string text, SlotKind kind, string type, params Term[] arguments)
	{
		if (m_Graph == null)
		{
			return new Term(text, kind, 0, string.Empty);
		}
		NodeDefinition definition = RuntimeTesting.Library.FindNode(type) ?? throw new InvalidOperationException($"the library has no node {type}");
		uint node = m_Graph.Node(type);
		List<PinDescription> inputs = [.. definition.Pins.Where(static pin => pin.IsInput && !pin.IsExec)];
		for (int index = 0; index < arguments.Length; index++)
		{
			Link(arguments[index], node, inputs[index].Name);
		}
		return new Term(text, kind, node, definition.Pins.First(static pin => !pin.IsInput && !pin.IsExec).Name);
	}

	private void Link(Term from, uint to, string input)
	{
		m_Graph?.Link(from.Node, from.Pin, to, input);
	}

	// White space between tokens: mostly a space, now and then none, a line break or a tab.
	private string Space() => m_Random.Pick(8) switch
	{
		0 => string.Empty,
		1 => "\n",
		2 => "\t",
		_ => " ",
	};

	private SlotKind PickNumeric() => (SlotKind)((int)SlotKind.Int + m_Random.Pick(6));

	// A number below double: what scalar arguments of float functions and vectors may be.
	private SlotKind PickScalar() => (SlotKind)((int)SlotKind.Int + m_Random.Pick(5));

	private static bool IsInteger(SlotKind kind) => kind is SlotKind.Int or SlotKind.UInt or SlotKind.Long or SlotKind.ULong;

	private static string TypeId(SlotKind kind) => kind switch
	{
		SlotKind.Bool => "bool",
		SlotKind.Int => "int",
		SlotKind.UInt => "uint",
		SlotKind.Long => "long",
		SlotKind.ULong => "ulong",
		SlotKind.Float => "float",
		SlotKind.Double => "double",
		_ => "Strada." + kind,
	};
}
