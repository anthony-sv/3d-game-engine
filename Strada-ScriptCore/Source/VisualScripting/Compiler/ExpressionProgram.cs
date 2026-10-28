using System;
using System.Collections.Generic;
using System.Text;

namespace Strada.VisualScripting;

// The instructions of a formula's program (spec-slice3 section 3.3.8): typed three-address code over the slots of the
// frame. Kind is the kind the instruction computes at; A to D are slots it reads and Target the slot it writes.
internal enum ExpressionOp : byte
{
	// Target = A.
	Copy,
	// Target = A <OperatorKind in Detail> B at Kind (Operators.Arithmetic).
	Arithmetic,
	// Target = -A at Kind (Operators.Negate; vectors per component).
	Negate,
	// Target = A <OperatorKind in Detail> B at Kind (Operators.Compare).
	Compare,
	// Target = A == B at Kind; Detail 1: !=. Kind is numeric, Bool or a vector.
	Equality,
	// Target = !A.
	Not,
	// Target = A converted by the ConversionOp in Detail (a numeric widening).
	Widen,
	// Target = A converted from Kind2 to Kind (Conversions.NumericCast).
	Cast,
	// Target = A <operator> B per component of the vector Kind; Detail packs the operator and the operands' shapes.
	Vector,
	// Target = component Detail (0 to 3) of the vector A of Kind.
	Component,
	// Target = the vector Kind from the floats A, B, C and D.
	Construct,
	// Target = the ExpressionFunction in Detail of A, B and C, whose operands are Kind2 and whose result is Kind.
	Call,
	// When A is false, continue at Jump.
	JumpIfFalse,
	// When A is true, continue at Jump.
	JumpIfTrue,
	// Continue at Jump.
	Jump,
}

// The fixed functions of formulas (spec-slice3 section 3.3.6).
internal enum ExpressionFunction : byte
{
	Abs,
	Sign,
	Min,
	Max,
	Clamp,
	Floor,
	Ceil,
	Round,
	Trunc,
	Frac,
	Sqrt,
	Exp,
	Log,
	Log2,
	Log10,
	Pow,
	Atan2,
	Sin,
	Cos,
	Tan,
	Asin,
	Acos,
	Atan,
	Radians,
	Degrees,
	Lerp,
	Smoothstep,
	Step,
	Length,
	Normalize,
	Dot,
	Distance,
	Cross,
}

// The shapes of a Vector instruction's operands.
internal enum VectorOperands : byte
{
	VectorVector,
	VectorScalar,
	ScalarVector,
}

internal readonly record struct ExpressionInstruction(ExpressionOp Op, SlotKind Kind, SlotKind Kind2, byte Detail, int Target, int A, int B, int C, int D,
	int Jump, int Span);

// A formula compiled for one Expression node: its code, and for each instruction the part of the formula it computes.
internal sealed class ExpressionProgram
{
	internal required ExpressionInstruction[] Code { get; init; }

	// UTF-8 byte ranges of the formula, by Span index.
	internal required (int Start, int End)[] Spans { get; init; }

	internal required string Formula { get; init; }

	// The text of a span, for the messages of aborting errors (made only when one happens).
	internal string TextOf(int span)
	{
		(int start, int end) = Spans[span];
		byte[] bytes = Encoding.UTF8.GetBytes(Formula);
		return Encoding.UTF8.GetString(bytes, start, end - start);
	}
}

// The packing of a Vector instruction's Detail: the operator in the low four bits, the operands' shapes above them.
internal static class ExpressionCode
{
	internal static byte Pack(OperatorKind op, VectorOperands operands) => (byte)((int)op | ((int)operands << 4));

	internal static OperatorKind OperatorOf(byte detail) => (OperatorKind)(detail & 0x0F);

	internal static VectorOperands OperandsOf(byte detail) => (VectorOperands)(detail >> 4);
}

// Compiles a formula's typed tree into its program (spec-slice3 section 3.3.8). Inputs are read from the node's staging
// slots, the root writes the node's output, constants get a template slot each (one per distinct value) and every other
// value a frame slot, reused once the instruction that reads it is emitted: the template grows by at most one slot per part
// of the formula, so by at most 1024. Widenings and casts of constants are folded into constants of their own.
internal sealed class ExpressionCompiler
{
	private readonly ExpressionBinding m_Binding;
	private readonly int m_InputBase;
	private readonly Func<Slot, int> m_Allocate;
	private readonly int[] m_ByteOffsets;
	private readonly List<ExpressionInstruction> m_Code = [];
	private readonly List<(int Start, int End)> m_Spans = [];
	private readonly Dictionary<(SlotKind Kind, ulong Bits), int> m_Constants = [];
	private readonly HashSet<int> m_Temporaries = [];
	private readonly Stack<int> m_Free = new();

	private ExpressionCompiler(ExpressionBinding binding, int inputBase, Func<Slot, int> allocate)
	{
		m_Binding = binding;
		m_InputBase = inputBase;
		m_Allocate = allocate;
		m_ByteOffsets = ByteOffsets(binding.Formula);
	}

	// allocate adds a slot with an initial value to the frame's template and gives its index.
	internal static ExpressionProgram Compile(ExpressionBinding binding, int inputBase, int output, Func<Slot, int> allocate)
	{
		ExpressionCompiler compiler = new(binding, inputBase, allocate);
		compiler.EmitInto(binding.Syntax, output);
		return new ExpressionProgram { Code = [.. compiler.m_Code], Spans = [.. compiler.m_Spans], Formula = binding.Formula };
	}

	// Computes the expression into target.
	private void EmitInto(TypedExpression expression, int target)
	{
		int slot = Emit(expression, target);
		if (slot != target)
		{
			Add(ExpressionOp.Copy, expression, target, a: slot);
			Release(slot);
		}
	}

	// Computes the expression; its value is in the slot returned, which is target when target is not -1 and the expression is
	// computed (inputs and constants are read where they are).
	private int Emit(TypedExpression expression, int target = -1)
	{
		switch (expression.Kind)
		{
			case TypedExpressionKind.Input:
				return m_InputBase + expression.Input;
			case TypedExpressionKind.Constant:
				return Constant(expression.Value, expression.Type);
			case TypedExpressionKind.Widen when expression.Operands[0].Kind == TypedExpressionKind.Constant:
				Slot widened = expression.Operands[0].Value;
				Conversions.Widen(ref widened, (ConversionOp)expression.Detail);
				return Constant(widened, expression.Type);
			case TypedExpressionKind.Cast when expression.Operands[0].Kind == TypedExpressionKind.Constant:
				return Constant(Conversions.NumericCast(expression.Operands[0].Value, expression.Kind2, expression.Type), expression.Type);
			case TypedExpressionKind.And:
			case TypedExpressionKind.Or:
				return ShortCircuit(expression, target);
			case TypedExpressionKind.Conditional:
				return Conditional(expression, target);
		}
		TypedExpression[] operands = expression.Operands;
		Span<int> slots = stackalloc int[4];
		for (int index = 0; index < operands.Length; index++)
		{
			slots[index] = Emit(operands[index]);
		}
		for (int index = 0; index < operands.Length; index++)
		{
			Release(slots[index]);
		}
		int result = target >= 0 ? target : Temporary();
		ExpressionOp op = expression.Kind switch
		{
			TypedExpressionKind.Widen => ExpressionOp.Widen,
			TypedExpressionKind.Cast => ExpressionOp.Cast,
			TypedExpressionKind.Arithmetic => ExpressionOp.Arithmetic,
			TypedExpressionKind.Negate => ExpressionOp.Negate,
			TypedExpressionKind.Compare => ExpressionOp.Compare,
			TypedExpressionKind.Equality => ExpressionOp.Equality,
			TypedExpressionKind.Not => ExpressionOp.Not,
			TypedExpressionKind.Vector => ExpressionOp.Vector,
			TypedExpressionKind.Component => ExpressionOp.Component,
			TypedExpressionKind.Construct => ExpressionOp.Construct,
			_ => ExpressionOp.Call,
		};
		// Compare and Equality compute at their operands' kind; Component reads a vector of Kind2.
		SlotKind kind = expression.Kind is TypedExpressionKind.Compare or TypedExpressionKind.Equality or TypedExpressionKind.Component
			? expression.Kind2
			: expression.Type;
		SlotKind kind2 = expression.Kind is TypedExpressionKind.Cast or TypedExpressionKind.Call ? expression.Kind2 : expression.Type;
		Add(op, expression, result, kind, kind2, expression.Detail, Read(slots, operands.Length, 0), Read(slots, operands.Length, 1),
			Read(slots, operands.Length, 2), Read(slots, operands.Length, 3));
		return result;
	}

	// a && b: a → r; JumpIfFalse r → end; b → r; end. a || b likewise with JumpIfTrue.
	private int ShortCircuit(TypedExpression expression, int target)
	{
		int result = target >= 0 ? target : Temporary();
		EmitInto(expression.Operands[0], result);
		int jump = Add(expression.Kind == TypedExpressionKind.And ? ExpressionOp.JumpIfFalse : ExpressionOp.JumpIfTrue, expression, -1, a: result);
		EmitInto(expression.Operands[1], result);
		Patch(jump);
		return result;
	}

	// c ? x : y: c → q; JumpIfFalse q → else; x → r; Jump → end; else: y → r; end.
	private int Conditional(TypedExpression expression, int target)
	{
		int condition = Emit(expression.Operands[0]);
		int otherwise = Add(ExpressionOp.JumpIfFalse, expression, -1, a: condition);
		Release(condition);
		int result = target >= 0 ? target : Temporary();
		EmitInto(expression.Operands[1], result);
		int end = Add(ExpressionOp.Jump, expression, -1);
		Patch(otherwise);
		EmitInto(expression.Operands[2], result);
		Patch(end);
		return result;
	}

	private int Add(ExpressionOp op, TypedExpression expression, int target, SlotKind kind = SlotKind.Bool, SlotKind kind2 = SlotKind.Bool,
		byte detail = 0, int a = -1, int b = -1, int c = -1, int d = -1)
	{
		m_Spans.Add((m_ByteOffsets[expression.Start], m_ByteOffsets[expression.End]));
		m_Code.Add(new ExpressionInstruction(op, kind, kind2, detail, target, a, b, c, d, -1, m_Spans.Count - 1));
		return m_Code.Count - 1;
	}

	// A jump continues after the last instruction emitted so far.
	private void Patch(int jump)
	{
		m_Code[jump] = m_Code[jump] with { Jump = m_Code.Count };
	}

	private static int Read(Span<int> slots, int count, int index) => index < count ? slots[index] : -1;

	private int Constant(Slot value, SlotKind kind)
	{
		(SlotKind, ulong) key = (kind, value.ULong);
		if (!m_Constants.TryGetValue(key, out int slot))
		{
			slot = m_Allocate(value);
			m_Constants[key] = slot;
		}
		return slot;
	}

	private int Temporary()
	{
		if (m_Free.TryPop(out int slot))
		{
			return slot;
		}
		slot = m_Allocate(default);
		m_Temporaries.Add(slot);
		return slot;
	}

	// A temporary whose value was read is free again; inputs, constants and targets are not temporaries.
	private void Release(int slot)
	{
		if (m_Temporaries.Contains(slot) && !m_Free.Contains(slot))
		{
			m_Free.Push(slot);
		}
	}

	// The UTF-8 byte offset of each UTF-16 index of a text (and of its end).
	private static int[] ByteOffsets(string text)
	{
		int[] offsets = new int[text.Length + 1];
		int bytes = 0;
		for (int index = 0; index < text.Length; index++)
		{
			offsets[index] = bytes;
			char character = text[index];
			if (char.IsHighSurrogate(character) && index + 1 < text.Length && char.IsLowSurrogate(text[index + 1]))
			{
				offsets[index + 1] = bytes;
				bytes += 4;
				index++;
				continue;
			}
			bytes += character < 0x80 ? 1 : character < 0x800 ? 2 : 3;
		}
		offsets[text.Length] = bytes;
		return offsets;
	}
}
