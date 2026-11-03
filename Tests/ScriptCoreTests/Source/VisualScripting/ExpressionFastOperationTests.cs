using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The typed operations of formulas (spec-slice4 section 9.5): the compiler emits them for the hot operations at int and float,
// and each computes exactly what Operators.Arithmetic and Operators.Compare compute, over the extreme operands (wrapping
// integers such as int.MaxValue + 1 and int.MinValue * -1, NaN, signed zeros, infinities, subnormals) and random ones.
public sealed class ExpressionFastOperationTests
{
	private const ulong Seed = 11150;

	private static readonly int[] s_Ints =
	[
		0, 1, -1, 2, -2, 3, 7, 46341, -46341, 65536, int.MaxValue, int.MinValue, int.MaxValue - 1, int.MinValue + 1,
	];

	private static readonly float[] s_Floats =
	[
		0.0f, -0.0f, 1.0f, -1.0f, 0.1f, 3.0f, 1.0e30f, -1.0e30f, float.NaN, -float.NaN, BitConverter.Int32BitsToSingle(0x7FC12345),
		float.PositiveInfinity, float.NegativeInfinity, float.Epsilon, -float.Epsilon, BitConverter.Int32BitsToSingle(0x00400000),
		BitConverter.Int32BitsToSingle(0x00800000), float.MaxValue, float.MinValue,
	];

	private static readonly (ExpressionOp Op, OperatorKind Operator)[] s_IntOperations =
	[
		(ExpressionOp.AddInt32, OperatorKind.Add), (ExpressionOp.SubtractInt32, OperatorKind.Subtract), (ExpressionOp.MultiplyInt32, OperatorKind.Multiply),
	];

	private static readonly (ExpressionOp Op, OperatorKind Operator)[] s_FloatOperations =
	[
		(ExpressionOp.AddFloat, OperatorKind.Add), (ExpressionOp.SubtractFloat, OperatorKind.Subtract), (ExpressionOp.MultiplyFloat, OperatorKind.Multiply),
		(ExpressionOp.DivideFloat, OperatorKind.Divide),
	];

	private static readonly OperatorKind[] s_Comparisons = [OperatorKind.Less, OperatorKind.LessOrEqual, OperatorKind.Greater, OperatorKind.GreaterOrEqual];

	[Fact]
	public void TheTypedOperationsComputeWhatTheOperatorsCompute()
	{
		FuzzRandom random = new(Seed);
		List<(int A, int B)> ints = [.. s_Ints.SelectMany(static a => s_Ints.Select(b => (a, b)))];
		List<(float A, float B)> floats = [.. s_Floats.SelectMany(static a => s_Floats.Select(b => (a, b)))];
		int rounds = Fuzzing.GetRounds(2000);
		for (int round = 0; round < rounds; round++)
		{
			ints.Add((unchecked((int)random.Next()), unchecked((int)random.Next())));
			// Any bit pattern: NaNs with payloads and subnormals among them.
			floats.Add((BitConverter.Int32BitsToSingle(unchecked((int)random.Next())), BitConverter.Int32BitsToSingle(unchecked((int)random.Next()))));
		}
		foreach ((int a, int b) in ints)
		{
			Slot left = Slot.FromInt(a);
			Slot right = Slot.FromInt(b);
			foreach ((ExpressionOp op, OperatorKind kind) in s_IntOperations)
			{
				Assert.Equal(Operators.Arithmetic(kind, SlotKind.Int, left, right).Int, Run(op, SlotKind.Int, 0, left, right).Int);
			}
			foreach (OperatorKind comparison in s_Comparisons)
			{
				Assert.Equal(Operators.Compare(comparison, SlotKind.Int, left, right), Run(ExpressionOp.CompareInt32, SlotKind.Int, (byte)comparison, left, right).Bool);
			}
		}
		foreach ((float a, float b) in floats)
		{
			Slot left = Slot.FromFloat(a);
			Slot right = Slot.FromFloat(b);
			foreach ((ExpressionOp op, OperatorKind kind) in s_FloatOperations)
			{
				float expected = Operators.Arithmetic(kind, SlotKind.Float, left, right).Float;
				float actual = Run(op, SlotKind.Float, 0, left, right).Float;
				// Of two NaN operands, which payload the result keeps is the JIT's choice (it may commute the operands), in the
				// operators as in the typed operations: NaN results are equal as NaN, every other result bit for bit.
				bool equal = (float.IsNaN(expected) && float.IsNaN(actual)) || BitConverter.SingleToInt32Bits(expected) == BitConverter.SingleToInt32Bits(actual);
				Assert.True(equal, FormattableString.Invariant(
					$"{op} of {BitConverter.SingleToInt32Bits(a):X8} and {BitConverter.SingleToInt32Bits(b):X8}: {BitConverter.SingleToInt32Bits(expected):X8} expected, {BitConverter.SingleToInt32Bits(actual):X8} computed"));
			}
			foreach (OperatorKind comparison in s_Comparisons)
			{
				Assert.Equal(Operators.Compare(comparison, SlotKind.Float, left, right), Run(ExpressionOp.CompareFloat, SlotKind.Float, (byte)comparison, left, right).Bool);
			}
		}
	}

	[Theory]
	[InlineData("int", "a + b", "AddInt32")]
	[InlineData("int", "a - b", "SubtractInt32")]
	[InlineData("int", "a * b", "MultiplyInt32")]
	[InlineData("int", "a < b", "CompareInt32")]
	[InlineData("int", "a >= b", "CompareInt32")]
	[InlineData("float", "a + b", "AddFloat")]
	[InlineData("float", "a - b", "SubtractFloat")]
	[InlineData("float", "a * b", "MultiplyFloat")]
	[InlineData("float", "a / b", "DivideFloat")]
	[InlineData("float", "a <= b", "CompareFloat")]
	[InlineData("float", "a > b", "CompareFloat")]
	// Integer division and modulo check their divisor, other kinds have no typed form: they stay general.
	[InlineData("int", "a / b", "Arithmetic")]
	[InlineData("int", "a % b", "Arithmetic")]
	[InlineData("float", "a % b", "Arithmetic")]
	[InlineData("long", "a + b", "Arithmetic")]
	[InlineData("double", "a * b", "Arithmetic")]
	[InlineData("double", "a < b", "Compare")]
	public void TheCompilerEmitsTheTypedOperations(string type, string formula, string expected)
	{
		DocumentBuilder document = new();
		uint a = document.Variable("A", type);
		uint b = document.Variable("B", type);
		GraphBuilder graph = document.Graph;
		uint expression = graph.Node("Expression", $$"""{ "Formula": {{RuntimeTesting.Quote(formula)}} }""");
		graph.Link(graph.Get(a), "Value", expression, "a");
		graph.Link(graph.Get(b), "Value", expression, "b");
		uint result = document.Variable("Result", formula.Contains('<', StringComparison.Ordinal) || formula.Contains('>', StringComparison.Ordinal) ? "bool" : type);
		uint set = graph.Set(result);
		graph.Link(expression, "Result", set, "Value");
		graph.Flow(graph.Node("Event.Update"), set);
		AnalyzedDocument analyzed = GraphAnalyzer.Analyze(RuntimeTesting.Library, Encoding.UTF8.GetBytes(document.Build()));
		VisualScriptTesting.AssertNoErrors(analyzed);
		GraphProgram program = ProgramCompiler.Compile(analyzed, "Typed");
		ExpressionInstruction instruction = Assert.Single(program.EventGraph.Nodes.Single(static node => node.Op == Opcode.Expression).Expression!.Code);
		Assert.Equal(Enum.Parse<ExpressionOp>(expected), instruction.Op);
	}

	// The one instruction op over the slots A (0) and B (1), into 2.
	private static Slot Run(ExpressionOp op, SlotKind kind, byte detail, Slot a, Slot b)
	{
		ExpressionProgram program = new()
		{
			Code = [new ExpressionInstruction(op, kind, kind, detail, 2, 0, 1, -1, -1, -1, 0)],
			Spans = [(0, 0)],
			Formula = string.Empty,
		};
		Slot[] slots = [a, b, default];
		ExpressionMachine.Run(program, slots);
		return slots[2];
	}
}
