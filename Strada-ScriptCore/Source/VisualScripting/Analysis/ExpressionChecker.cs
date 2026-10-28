using System;
using System.Collections.Generic;

namespace Strada.VisualScripting;

internal enum TypedExpressionKind : byte
{
	// The node's input at Input.
	Input,
	// Value, of Type.
	Constant,
	// The operand converted to Type by the ConversionOp in Detail.
	Widen,
	// The operand converted from Kind2 to Type by Conversions.NumericCast.
	Cast,
	// Two operands at Type with the OperatorKind in Detail.
	Arithmetic,
	Negate,
	// Two operands at Kind2 with the OperatorKind in Detail; Type is Bool.
	Compare,
	// Two operands at Kind2; Detail 1 is !=; Type is Bool.
	Equality,
	Not,
	// Per component of the vector Type: the OperatorKind and the VectorOperands in Detail (ExpressionCode.Pack).
	Vector,
	// Component Detail (0 to 3) of a vector of Kind2; Type is Float.
	Component,
	// A vector of Type from its components, floats.
	Construct,
	// The ExpressionFunction in Detail over operands of Kind2 (Clamp of a vector by numbers: Kind2 is the vector and the
	// bounds are floats), giving Type.
	Call,
	And,
	Or,
	Conditional,
}

// A node of a formula's typed tree (spec-slice3 section 3.3.5): what the compiler turns into instructions. Every implicit
// widening is a Widen node of its own; Start and End are the UTF-16 range of the text it computes.
internal sealed class TypedExpression
{
	internal TypedExpression(TypedExpressionKind kind, SlotKind type, int start, int end)
	{
		Kind = kind;
		Type = type;
		Start = start;
		End = end;
	}

	internal TypedExpressionKind Kind { get; }

	internal SlotKind Type { get; }

	internal int Start { get; }

	internal int End { get; }

	internal SlotKind Kind2 { get; init; }

	internal byte Detail { get; init; }

	internal TypedExpression[] Operands { get; init; } = [];

	internal Slot Value { get; init; }

	internal int Input { get; init; }
}

// Types a parsed formula bottom-up for the types of its inputs (spec-slice3 sections 3.3.4 to 3.3.6): the typed tree and
// its type, or the first problem.
internal sealed class ExpressionChecker
{
	private readonly NodeLibrary m_Library;
	private readonly string m_Text;
	private readonly IReadOnlyList<SlotKind> m_Inputs;

	private ExpressionChecker(NodeLibrary library, string text, IReadOnlyList<SlotKind> inputs)
	{
		m_Library = library;
		m_Text = text;
		m_Inputs = inputs;
	}

	internal ExpressionError? Error { get; private set; }

	// The kinds a formula's values may have; the others are refused by the Expression constraint before a formula is typed.
	internal static SlotKind? KindOf(VisualType type) => SlotValues.KindOf(type) switch
	{
		SlotKind.Bool or SlotKind.Int or SlotKind.UInt or SlotKind.Long or SlotKind.ULong or SlotKind.Float or SlotKind.Double or SlotKind.Vector2 or
			SlotKind.Vector3 or SlotKind.Vector4 when type.HasValues && type.Kind != TypeKind.Enum => SlotValues.KindOf(type),
		_ => null,
	};

	// The library type of a formula's kind. Every library has the numeric primitives and bool; it has a vector type only when
	// its API uses that vector, so null is possible for those.
	internal static VisualType? TypeOf(NodeLibrary library, SlotKind kind) => library.FindType(ClrTypeOf(kind));

	internal static (TypedExpression? Root, ExpressionError? Error) Check(NodeLibrary library, ParsedFormula formula, IReadOnlyList<SlotKind> inputs)
	{
		ExpressionChecker checker = new(library, formula.Text, inputs);
		TypedExpression? root = checker.Check(formula.Root!);
		return (root, root == null ? checker.Error : null);
	}

	// The name of a kind in messages.
	internal static string Name(SlotKind kind) => kind switch
	{
		SlotKind.Bool => "bool",
		SlotKind.Int => "int",
		SlotKind.UInt => "uint",
		SlotKind.Long => "long",
		SlotKind.ULong => "ulong",
		SlotKind.Float => "float",
		SlotKind.Double => "double",
		_ => kind.ToString(),
	};

	internal static bool IsNumeric(SlotKind kind) => kind is SlotKind.Int or SlotKind.UInt or SlotKind.Long or SlotKind.ULong or SlotKind.Float or SlotKind.Double;

	internal static bool IsVector(SlotKind kind) => kind is SlotKind.Vector2 or SlotKind.Vector3 or SlotKind.Vector4;

	internal static int ComponentCount(SlotKind vector) => vector switch
	{
		SlotKind.Vector2 => 2,
		SlotKind.Vector3 => 3,
		_ => 4,
	};

	private static Type ClrTypeOf(SlotKind kind) => kind switch
	{
		SlotKind.Bool => typeof(bool),
		SlotKind.Int => typeof(int),
		SlotKind.UInt => typeof(uint),
		SlotKind.Long => typeof(long),
		SlotKind.ULong => typeof(ulong),
		SlotKind.Float => typeof(float),
		SlotKind.Double => typeof(double),
		SlotKind.Vector2 => typeof(Vector2),
		SlotKind.Vector3 => typeof(Vector3),
		_ => typeof(Vector4),
	};

	private TypedExpression? Check(ExpressionSyntax syntax)
	{
		switch (syntax.Kind)
		{
			case ExpressionSyntaxKind.Literal:
				return new TypedExpression(TypedExpressionKind.Constant, syntax.LiteralKind, syntax.Start, syntax.End) { Value = syntax.Literal };
			case ExpressionSyntaxKind.Input:
				return new TypedExpression(TypedExpressionKind.Input, m_Inputs[syntax.Input], syntax.Start, syntax.End) { Input = syntax.Input };
			case ExpressionSyntaxKind.Unary:
				return Unary(syntax);
			case ExpressionSyntaxKind.Binary:
				return Binary(syntax);
			case ExpressionSyntaxKind.Conditional:
				return Conditional(syntax);
			case ExpressionSyntaxKind.Component:
				return Component(syntax);
			default:
				return Call(syntax);
		}
	}

	private TypedExpression? Unary(ExpressionSyntax syntax)
	{
		TypedExpression? operand = Check(syntax.Operands[0]);
		if (operand == null)
		{
			return null;
		}
		SlotKind type = operand.Type;
		switch (syntax.Operator)
		{
			case ExpressionPunctuation.Bang:
				return type == SlotKind.Bool
					? new TypedExpression(TypedExpressionKind.Not, SlotKind.Bool, syntax.Start, syntax.End) { Operands = [operand] }
					: Fail($"! takes bool, not {Name(type)}", syntax);
			case ExpressionPunctuation.Plus:
				return IsNumeric(type) || IsVector(type) ? operand : Fail("+ takes numbers or vectors, not bool", syntax);
		}
		if (type == SlotKind.Bool)
		{
			return Fail("- takes numbers or vectors, not bool", syntax);
		}
		if (type == SlotKind.ULong)
		{
			return Fail("ulong values cannot be negated: convert with long(...) or double(...)", syntax);
		}
		// C# unary minus: a uint becomes a long.
		if (type == SlotKind.UInt)
		{
			operand = Widen(operand, SlotKind.Long);
			type = SlotKind.Long;
		}
		return new TypedExpression(TypedExpressionKind.Negate, type, syntax.Start, syntax.End) { Operands = [operand] };
	}

	// A left-associative chain (a + b - c ...) is as deep as it is long, which only MaxParts bounds: it is typed from its
	// first operand up in a loop. Right operands are of a higher level or nested, so the stack grows only with the nesting
	// MaxNesting bounds, whatever the formula's length (analysis runs on the engine's main thread).
	private TypedExpression? Binary(ExpressionSyntax syntax)
	{
		List<ExpressionSyntax> chain = [];
		ExpressionSyntax first = syntax;
		while (first.Kind == ExpressionSyntaxKind.Binary)
		{
			chain.Add(first);
			first = first.Operands[0];
		}
		TypedExpression? left = Check(first);
		for (int index = chain.Count - 1; index >= 0 && left != null; index--)
		{
			left = Binary(chain[index], left);
		}
		return left;
	}

	// A binary operator whose left operand is typed.
	private TypedExpression? Binary(ExpressionSyntax syntax, TypedExpression left)
	{
		TypedExpression? right = Check(syntax.Operands[1]);
		if (right == null)
		{
			return null;
		}
		switch (syntax.Operator)
		{
			case ExpressionPunctuation.Plus:
				return Arithmetic(syntax, OperatorKind.Add, "+", left, right);
			case ExpressionPunctuation.Minus:
				return Arithmetic(syntax, OperatorKind.Subtract, "-", left, right);
			case ExpressionPunctuation.Star:
				return Arithmetic(syntax, OperatorKind.Multiply, "*", left, right);
			case ExpressionPunctuation.Slash:
				return Arithmetic(syntax, OperatorKind.Divide, "/", left, right);
			case ExpressionPunctuation.Percent:
				return Arithmetic(syntax, OperatorKind.Modulo, "%", left, right);
			case ExpressionPunctuation.Less:
				return Compare(syntax, OperatorKind.Less, "<", left, right);
			case ExpressionPunctuation.LessOrEqual:
				return Compare(syntax, OperatorKind.LessOrEqual, "<=", left, right);
			case ExpressionPunctuation.Greater:
				return Compare(syntax, OperatorKind.Greater, ">", left, right);
			case ExpressionPunctuation.GreaterOrEqual:
				return Compare(syntax, OperatorKind.GreaterOrEqual, ">=", left, right);
			case ExpressionPunctuation.Equal:
				return Equality(syntax, false, left, right);
			case ExpressionPunctuation.NotEqual:
				return Equality(syntax, true, left, right);
			default:
				return Logical(syntax, left, right);
		}
	}

	private TypedExpression? Arithmetic(ExpressionSyntax syntax, OperatorKind op, string text, TypedExpression left, TypedExpression right)
	{
		SlotKind a = left.Type;
		SlotKind b = right.Type;
		if (a == SlotKind.Bool || b == SlotKind.Bool)
		{
			return Fail($"{text} takes numbers or vectors, not bool", syntax);
		}
		if (IsNumeric(a) && IsNumeric(b))
		{
			if (Join(a, b) is not { } join)
			{
				return Fail($"{Name(a)} and {Name(b)} have no common type: convert one with long(...) or double(...)", syntax);
			}
			return new TypedExpression(TypedExpressionKind.Arithmetic, join, syntax.Start, syntax.End)
			{
				Detail = (byte)op,
				Operands = [Widen(left, join), Widen(right, join)],
			};
		}
		if (op == OperatorKind.Modulo)
		{
			return Fail($"% takes numbers, not {Name(IsVector(a) ? a : b)}", syntax);
		}
		if (IsVector(a) && IsVector(b))
		{
			if (a != b)
			{
				return Fail($"{Name(a)} and {Name(b)} do not combine", syntax);
			}
			return Vector(syntax, op, VectorOperands.VectorVector, a, left, right);
		}
		// A vector and a number.
		bool vectorFirst = IsVector(a);
		SlotKind vector = vectorFirst ? a : b;
		SlotKind scalar = vectorFirst ? b : a;
		string example = Example(vector);
		switch (op)
		{
			case OperatorKind.Add:
				return Fail($"a {Name(vector)} adds only to a {Name(vector)}: write v + {example} to add a number to each component", syntax);
			case OperatorKind.Subtract:
				return Fail($"a {Name(vector)} subtracts only a {Name(vector)}: write v - {example} to subtract a number from each component", syntax);
			case OperatorKind.Divide when !vectorFirst:
				return Fail($"a number cannot be divided by a {Name(vector)}: write {example} / v to divide it by each component", syntax);
		}
		if (scalar == SlotKind.Double)
		{
			return Fail($"a double does not combine with a {Name(vector)}: convert it with float(...)", syntax);
		}
		return vectorFirst
			? Vector(syntax, op, VectorOperands.VectorScalar, vector, left, Widen(right, SlotKind.Float))
			: Vector(syntax, op, VectorOperands.ScalarVector, vector, Widen(left, SlotKind.Float), right);
	}

	private static TypedExpression Vector(ExpressionSyntax syntax, OperatorKind op, VectorOperands operands, SlotKind vector, TypedExpression left,
		TypedExpression right) =>
		new(TypedExpressionKind.Vector, vector, syntax.Start, syntax.End) { Detail = ExpressionCode.Pack(op, operands), Operands = [left, right] };

	private TypedExpression? Compare(ExpressionSyntax syntax, OperatorKind op, string text, TypedExpression left, TypedExpression right)
	{
		SlotKind a = left.Type;
		SlotKind b = right.Type;
		if (!IsNumeric(a) || !IsNumeric(b))
		{
			return Fail($"{text} takes numbers, not {Name(IsNumeric(a) ? b : a)}", syntax);
		}
		if (Join(a, b) is not { } join)
		{
			return Fail($"{Name(a)} and {Name(b)} have no common type: convert one with long(...) or double(...)", syntax);
		}
		return new TypedExpression(TypedExpressionKind.Compare, SlotKind.Bool, syntax.Start, syntax.End)
		{
			Kind2 = join,
			Detail = (byte)op,
			Operands = [Widen(left, join), Widen(right, join)],
		};
	}

	private TypedExpression? Equality(ExpressionSyntax syntax, bool invert, TypedExpression left, TypedExpression right)
	{
		SlotKind a = left.Type;
		SlotKind b = right.Type;
		SlotKind kind;
		if (IsNumeric(a) && IsNumeric(b))
		{
			if (Join(a, b) is not { } join)
			{
				return Fail($"{Name(a)} and {Name(b)} have no common type: convert one with long(...) or double(...)", syntax);
			}
			kind = join;
		}
		else if (a == b)
		{
			kind = a;
		}
		else
		{
			return Fail($"{(invert ? "!=" : "==")} compares values of one type: {Name(a)} and {Name(b)}", syntax);
		}
		return new TypedExpression(TypedExpressionKind.Equality, SlotKind.Bool, syntax.Start, syntax.End)
		{
			Kind2 = kind,
			Detail = (byte)(invert ? 1 : 0),
			Operands = [Widen(left, kind), Widen(right, kind)],
		};
	}

	private TypedExpression? Logical(ExpressionSyntax syntax, TypedExpression left, TypedExpression right)
	{
		bool and = syntax.Operator == ExpressionPunctuation.And;
		if (left.Type != SlotKind.Bool || right.Type != SlotKind.Bool)
		{
			return Fail($"{(and ? "&&" : "||")} takes bool, not {Name(left.Type != SlotKind.Bool ? left.Type : right.Type)}", syntax);
		}
		return new TypedExpression(and ? TypedExpressionKind.And : TypedExpressionKind.Or, SlotKind.Bool, syntax.Start, syntax.End) { Operands = [left, right] };
	}

	private TypedExpression? Conditional(ExpressionSyntax syntax)
	{
		TypedExpression? condition = Check(syntax.Operands[0]);
		if (condition == null)
		{
			return null;
		}
		if (condition.Type != SlotKind.Bool)
		{
			return Fail($"the condition of ?: is bool, not {Name(condition.Type)}", syntax.Operands[0]);
		}
		TypedExpression? whenTrue = Check(syntax.Operands[1]);
		if (whenTrue == null)
		{
			return null;
		}
		TypedExpression? whenFalse = Check(syntax.Operands[2]);
		if (whenFalse == null)
		{
			return null;
		}
		SlotKind a = whenTrue.Type;
		SlotKind b = whenFalse.Type;
		SlotKind? type = a == b ? a : IsNumeric(a) && IsNumeric(b) ? Join(a, b) : null;
		if (type is not { } result)
		{
			return Fail($"the branches of ?: have no common type: {Name(a)} and {Name(b)}", syntax);
		}
		return new TypedExpression(TypedExpressionKind.Conditional, result, syntax.Start, syntax.End)
		{
			Operands = [condition, Widen(whenTrue, result), Widen(whenFalse, result)],
		};
	}

	// .X .Y .Z .W, or the same in lowercase. A chain of them (v.X.X ...) is typed from its vector up in a loop, as binary
	// chains are.
	private TypedExpression? Component(ExpressionSyntax syntax)
	{
		List<ExpressionSyntax> chain = [];
		ExpressionSyntax vector = syntax;
		while (vector.Kind == ExpressionSyntaxKind.Component)
		{
			chain.Add(vector);
			vector = vector.Operands[0];
		}
		TypedExpression? value = Check(vector);
		for (int index = chain.Count - 1; index >= 0 && value != null; index--)
		{
			value = Component(chain[index], value);
		}
		return value;
	}

	// A component of a typed value.
	private TypedExpression? Component(ExpressionSyntax syntax, TypedExpression vector)
	{
		if (!IsVector(vector.Type))
		{
			ExpressionSyntax operand = syntax.Operands[0];
			return Fail($"{m_Text[operand.Start..operand.End]} is {Name(vector.Type)}: it has no components", syntax);
		}
		int count = ComponentCount(vector.Type);
		int index = syntax.Name switch
		{
			"X" or "x" => 0,
			"Y" or "y" => 1,
			"Z" or "z" => 2,
			"W" or "w" => 3,
			_ => -1,
		};
		if (index < 0 || index >= count)
		{
			string components = count switch
			{
				2 => "X, Y",
				3 => "X, Y, Z",
				_ => "X, Y, Z, W",
			};
			return FailAt($"{Name(vector.Type)} has no component {syntax.Name} ({components})", syntax.NameStart, syntax.NameEnd);
		}
		return new TypedExpression(TypedExpressionKind.Component, SlotKind.Float, syntax.Start, syntax.End)
		{
			Kind2 = vector.Type,
			Detail = (byte)index,
			Operands = [vector],
		};
	}

	private TypedExpression? Call(ExpressionSyntax syntax)
	{
		ExpressionCallee callee = syntax.Callee!;
		TypedExpression[] arguments = new TypedExpression[syntax.Operands.Length];
		for (int index = 0; index < arguments.Length; index++)
		{
			TypedExpression? argument = Check(syntax.Operands[index]);
			if (argument == null)
			{
				return null;
			}
			arguments[index] = argument;
		}
		switch (callee.Kind)
		{
			case ExpressionCalleeKind.Cast:
				return CastCall(syntax, callee, arguments[0]);
			case ExpressionCalleeKind.Construct:
				return Construct(syntax, callee, arguments);
		}
		ExpressionFunction function = callee.Function;
		switch (function)
		{
			case ExpressionFunction.Abs:
			case ExpressionFunction.Sign:
				return IsNumeric(arguments[0].Type) || IsVector(arguments[0].Type)
					? Function(syntax, function, arguments[0].Type, arguments[0].Type, arguments)
					: Fail($"{callee.Name} takes a number or a vector, not {Name(arguments[0].Type)}", syntax);
			case ExpressionFunction.Min:
			case ExpressionFunction.Max:
				return MinMax(syntax, callee, arguments);
			case ExpressionFunction.Clamp:
				return Clamp(syntax, arguments);
			case ExpressionFunction.Floor:
			case ExpressionFunction.Ceil:
			case ExpressionFunction.Round:
			case ExpressionFunction.Trunc:
			case ExpressionFunction.Frac:
				if (IsVector(arguments[0].Type))
				{
					return Function(syntax, function, arguments[0].Type, arguments[0].Type, arguments);
				}
				return IsNumeric(arguments[0].Type)
					? RealFunction(syntax, function, arguments)
					: Fail($"{callee.Name} takes a number or a vector, not {Name(arguments[0].Type)}", syntax);
			case ExpressionFunction.Length:
			case ExpressionFunction.Normalize:
				if (!IsVector(arguments[0].Type))
				{
					return Fail($"{callee.Name} takes a vector, not {Name(arguments[0].Type)}", syntax);
				}
				return Function(syntax, function, arguments[0].Type, function == ExpressionFunction.Length ? SlotKind.Float : arguments[0].Type, arguments);
			case ExpressionFunction.Dot:
			case ExpressionFunction.Distance:
				if (!IsVector(arguments[0].Type) || arguments[0].Type != arguments[1].Type)
				{
					return Fail($"{callee.Name} takes two vectors of one type, not {Name(arguments[0].Type)} and {Name(arguments[1].Type)}", syntax);
				}
				return Function(syntax, function, arguments[0].Type, SlotKind.Float, arguments);
			case ExpressionFunction.Cross:
				return arguments[0].Type == SlotKind.Vector3 && arguments[1].Type == SlotKind.Vector3
					? Function(syntax, function, SlotKind.Vector3, SlotKind.Vector3, arguments)
					: Fail("cross takes two Vector3", syntax);
			case ExpressionFunction.Lerp:
				return Lerp(syntax, arguments);
			default:
				// sqrt, exp, log, log2, log10, pow, atan2, sin, cos, tan, asin, acos, atan, radians, degrees, smoothstep, step.
				foreach (TypedExpression argument in arguments)
				{
					if (!IsNumeric(argument.Type))
					{
						return Fail($"{callee.Name} takes {(arguments.Length == 1 ? "a number" : "numbers")}, not {Name(argument.Type)}", syntax);
					}
				}
				return RealFunction(syntax, function, arguments);
		}
	}

	private TypedExpression? CastCall(ExpressionSyntax syntax, ExpressionCallee callee, TypedExpression argument)
	{
		if (!IsNumeric(argument.Type))
		{
			return Fail($"{callee.Name} takes a number, not {Name(argument.Type)}", syntax);
		}
		return Cast(argument, callee.Type, syntax.Start, syntax.End);
	}

	private TypedExpression? Construct(ExpressionSyntax syntax, ExpressionCallee callee, TypedExpression[] arguments)
	{
		TypedExpression[] components = new TypedExpression[arguments.Length];
		for (int index = 0; index < arguments.Length; index++)
		{
			if (!IsNumeric(arguments[index].Type))
			{
				return Fail($"{callee.Name} takes numbers, not {Name(arguments[index].Type)}", syntax);
			}
			// Each number converts as Convert converts numbers.
			components[index] = Cast(arguments[index], SlotKind.Float, arguments[index].Start, arguments[index].End);
		}
		return new TypedExpression(TypedExpressionKind.Construct, callee.Type, syntax.Start, syntax.End) { Operands = components };
	}

	private TypedExpression? MinMax(ExpressionSyntax syntax, ExpressionCallee callee, TypedExpression[] arguments)
	{
		SlotKind a = arguments[0].Type;
		SlotKind b = arguments[1].Type;
		if (IsNumeric(a) && IsNumeric(b))
		{
			if (Join(a, b) is not { } join)
			{
				return Fail($"{Name(a)} and {Name(b)} have no common type: convert one with long(...) or double(...)", syntax);
			}
			return Function(syntax, callee.Function, join, join, [Widen(arguments[0], join), Widen(arguments[1], join)]);
		}
		if (IsVector(a) && a == b)
		{
			return Function(syntax, callee.Function, a, a, arguments);
		}
		return Fail($"{callee.Name} takes numbers or two vectors of one type, not {Name(a)} and {Name(b)}", syntax);
	}

	// clamp(x, lo, hi) = min(max(x, lo), hi): numbers at their join; a vector and two vectors of its type; or a vector and two
	// numbers (floats for each component).
	private TypedExpression? Clamp(ExpressionSyntax syntax, TypedExpression[] arguments)
	{
		SlotKind x = arguments[0].Type;
		SlotKind low = arguments[1].Type;
		SlotKind high = arguments[2].Type;
		if (IsNumeric(x) && IsNumeric(low) && IsNumeric(high))
		{
			if (Join(x, low) is not { } first || Join(first, high) is not { } join)
			{
				return Fail($"{Name(x)}, {Name(low)} and {Name(high)} have no common type: convert one with long(...) or double(...)", syntax);
			}
			return Function(syntax, ExpressionFunction.Clamp, join, join, [Widen(arguments[0], join), Widen(arguments[1], join), Widen(arguments[2], join)]);
		}
		if (IsVector(x) && low == x && high == x)
		{
			return Function(syntax, ExpressionFunction.Clamp, x, x, arguments);
		}
		if (IsVector(x) && IsNumeric(low) && IsNumeric(high))
		{
			if (low == SlotKind.Double || high == SlotKind.Double)
			{
				return Fail($"a double does not combine with a {Name(x)}: convert it with float(...)", syntax);
			}
			return new TypedExpression(TypedExpressionKind.Call, x, syntax.Start, syntax.End)
			{
				Kind2 = SlotKind.Float,
				Detail = (byte)ExpressionFunction.Clamp,
				Operands = [arguments[0], Widen(arguments[1], SlotKind.Float), Widen(arguments[2], SlotKind.Float)],
			};
		}
		return Fail($"clamp takes numbers, or a vector and bounds of its type or numbers, not {Name(x)}, {Name(low)} and {Name(high)}", syntax);
	}

	// lerp(a, b, t) = a + (b - a) * t, not clamped: numbers at their real join, or two vectors of one type and a number.
	private TypedExpression? Lerp(ExpressionSyntax syntax, TypedExpression[] arguments)
	{
		SlotKind a = arguments[0].Type;
		SlotKind b = arguments[1].Type;
		SlotKind t = arguments[2].Type;
		if (IsNumeric(a) && IsNumeric(b) && IsNumeric(t))
		{
			return RealFunction(syntax, ExpressionFunction.Lerp, arguments);
		}
		if (IsVector(a) && a == b && IsNumeric(t))
		{
			if (t == SlotKind.Double)
			{
				return Fail($"a double does not combine with a {Name(a)}: convert it with float(...)", syntax);
			}
			return new TypedExpression(TypedExpressionKind.Call, a, syntax.Start, syntax.End)
			{
				Kind2 = a,
				Detail = (byte)ExpressionFunction.Lerp,
				Operands = [arguments[0], arguments[1], Widen(arguments[2], SlotKind.Float)],
			};
		}
		return Fail($"lerp takes numbers, or two vectors of one type and a number, not {Name(a)}, {Name(b)} and {Name(t)}", syntax);
	}

	// A function over the reals: integer arguments become floats, and the arguments join (a double makes every one a double).
	private TypedExpression RealFunction(ExpressionSyntax syntax, ExpressionFunction function, TypedExpression[] arguments)
	{
		SlotKind real = SlotKind.Float;
		foreach (TypedExpression argument in arguments)
		{
			if (argument.Type == SlotKind.Double)
			{
				real = SlotKind.Double;
			}
		}
		TypedExpression[] widened = new TypedExpression[arguments.Length];
		for (int index = 0; index < arguments.Length; index++)
		{
			widened[index] = Widen(arguments[index], real);
		}
		return Function(syntax, function, real, real, widened);
	}

	private static TypedExpression Function(ExpressionSyntax syntax, ExpressionFunction function, SlotKind operands, SlotKind result,
		TypedExpression[] arguments) =>
		new(TypedExpressionKind.Call, result, syntax.Start, syntax.End) { Kind2 = operands, Detail = (byte)function, Operands = arguments };

	// The numeric join of spec section 3.5 rule 3 (TypeRules.Join): null when the two have no common type.
	private SlotKind? Join(SlotKind a, SlotKind b)
	{
		VisualType? join = TypeRules.Join(TypeOf(m_Library, a)!, TypeOf(m_Library, b)!);
		return join != null ? SlotValues.KindOf(join) : null;
	}

	// The operand converted to a wider numeric kind; itself when it has that kind.
	private static TypedExpression Widen(TypedExpression operand, SlotKind type)
	{
		if (operand.Type == type)
		{
			return operand;
		}
		return new TypedExpression(TypedExpressionKind.Widen, type, operand.Start, operand.End)
		{
			Detail = (byte)Conversions.Widening(operand.Type, type),
			Operands = [operand],
		};
	}

	private static TypedExpression Cast(TypedExpression operand, SlotKind type, int start, int end)
	{
		if (operand.Type == type)
		{
			return operand;
		}
		return new TypedExpression(TypedExpressionKind.Cast, type, start, end) { Kind2 = operand.Type, Operands = [operand] };
	}

	// v + vec3(s, s, s), for messages.
	private static string Example(SlotKind vector) => vector switch
	{
		SlotKind.Vector2 => "vec2(s, s)",
		SlotKind.Vector3 => "vec3(s, s, s)",
		_ => "vec4(s, s, s, s)",
	};

	private TypedExpression? Fail(string message, ExpressionSyntax syntax) => FailAt(message, syntax.Start, syntax.End);

	private TypedExpression? FailAt(string message, int start, int end)
	{
		Error ??= new ExpressionError(DiagnosticCode.InvalidExpression, message, start, end, []);
		return null;
	}
}
