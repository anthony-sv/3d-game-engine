using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace Strada.VisualScripting;

// The punctuation of formulas (spec-slice3 section 3.3.2).
internal enum ExpressionPunctuation : byte
{
	None,
	Plus,
	Minus,
	Star,
	Slash,
	Percent,
	OpenParenthesis,
	CloseParenthesis,
	Comma,
	Question,
	Colon,
	Dot,
	Bang,
	Less,
	LessOrEqual,
	Greater,
	GreaterOrEqual,
	Equal,
	NotEqual,
	And,
	Or,
}

internal enum ExpressionTokenKind : byte
{
	End,
	Identifier,
	Number,
	Punctuation,
}

// A token of a formula: its UTF-16 range, and for numbers their value and type (Int, Long, ULong, Float or Double).
internal readonly record struct ExpressionToken(ExpressionTokenKind Kind, int Start, int End, ExpressionPunctuation Punctuation, SlotKind NumberKind,
	Slot Number);

internal enum ExpressionSyntaxKind : byte
{
	Literal,
	Input,
	Unary,
	Binary,
	Conditional,
	Component,
	Call,
}

// What a call calls: a function of the fixed set, a vector constructor (vec2, vec3, vec4) or a numeric cast.
internal enum ExpressionCalleeKind : byte
{
	Function,
	Construct,
	Cast,
}

// A name that may be called, with the number of arguments it takes. Function is the function of Function callees; Type is
// the vector a constructor makes or the type a cast gives.
internal sealed record ExpressionCallee(string Name, ExpressionCalleeKind Kind, ExpressionFunction Function, SlotKind Type, int Arity);

// A node of a formula's syntax tree, with the UTF-16 range of the text it covers.
internal sealed class ExpressionSyntax
{
	internal ExpressionSyntax(ExpressionSyntaxKind kind, int start, int end)
	{
		Kind = kind;
		Start = start;
		End = end;
	}

	internal ExpressionSyntaxKind Kind { get; }

	internal int Start { get; }

	internal int End { get; }

	// Unary and binary operators.
	internal ExpressionPunctuation Operator { get; init; }

	internal ExpressionSyntax[] Operands { get; init; } = [];

	// Literals: the value and its type (Bool, Int, Long, ULong, Float or Double).
	internal SlotKind LiteralKind { get; init; }

	internal Slot Literal { get; init; }

	// Inputs: the index of the input among the formula's inputs.
	internal int Input { get; init; }

	// Components: the component's name as written, and its range.
	internal string Name { get; init; } = string.Empty;

	internal int NameStart { get; init; }

	internal int NameEnd { get; init; }

	internal ExpressionCallee? Callee { get; init; }
}

// The first problem of a formula: its code (InvalidExpression or ReservedName), its message and the UTF-16 range it names.
internal sealed record ExpressionError(DiagnosticCode Code, string Message, int Start, int End, IReadOnlyList<string> Suggestions);

// A formula as the parser read it: its inputs (spec-slice3 section 3.3.1's input rule), and its syntax tree or its first
// problem. Steps counts what the lexer and the parser did, which is linear in the formula's length.
internal sealed class ParsedFormula
{
	internal ParsedFormula(string text, IReadOnlyList<string> inputs, ExpressionSyntax? root, ExpressionError? error, int steps)
	{
		Text = text;
		Inputs = inputs;
		Root = root;
		Error = error;
		Steps = steps;
	}

	internal string Text { get; }

	internal IReadOnlyList<string> Inputs { get; }

	internal ExpressionSyntax? Root { get; }

	internal ExpressionError? Error { get; }

	internal int Steps { get; }
}

// Reads formulas (spec-slice3 sections 3.3.2 to 3.3.4 and 3.3.9). The checks run in a fixed order and the first problem is
// the formula's: the caps of the text, an empty formula, the lexer's problems, the inputs' names, then the syntax. Inputs are
// found whatever the problem, except for a formula over the byte cap, which is not read at all.
internal static class ExpressionParser
{
	internal const int MaxBytes = 4096;
	internal const int MaxParts = 1024;
	internal const int MaxNesting = 64;
	internal const int MaxInputs = 32;
	internal const int MaxIdentifierBytes = Names.MaxBytes;
	internal const int MaxSubtitleLength = 60;

	private static readonly string[] s_Constants = ["true", "false", "pi", "tau"];

	private static readonly ExpressionCallee[] s_Callees =
	[
		Function("abs", ExpressionFunction.Abs, 1),
		Function("sign", ExpressionFunction.Sign, 1),
		Function("min", ExpressionFunction.Min, 2),
		Function("max", ExpressionFunction.Max, 2),
		Function("clamp", ExpressionFunction.Clamp, 3),
		Function("floor", ExpressionFunction.Floor, 1),
		Function("ceil", ExpressionFunction.Ceil, 1),
		Function("round", ExpressionFunction.Round, 1),
		Function("trunc", ExpressionFunction.Trunc, 1),
		Function("frac", ExpressionFunction.Frac, 1),
		Function("sqrt", ExpressionFunction.Sqrt, 1),
		Function("exp", ExpressionFunction.Exp, 1),
		Function("log", ExpressionFunction.Log, 1),
		Function("log2", ExpressionFunction.Log2, 1),
		Function("log10", ExpressionFunction.Log10, 1),
		Function("pow", ExpressionFunction.Pow, 2),
		Function("atan2", ExpressionFunction.Atan2, 2),
		Function("sin", ExpressionFunction.Sin, 1),
		Function("cos", ExpressionFunction.Cos, 1),
		Function("tan", ExpressionFunction.Tan, 1),
		Function("asin", ExpressionFunction.Asin, 1),
		Function("acos", ExpressionFunction.Acos, 1),
		Function("atan", ExpressionFunction.Atan, 1),
		Function("radians", ExpressionFunction.Radians, 1),
		Function("degrees", ExpressionFunction.Degrees, 1),
		Function("lerp", ExpressionFunction.Lerp, 3),
		Function("smoothstep", ExpressionFunction.Smoothstep, 3),
		Function("step", ExpressionFunction.Step, 2),
		Function("length", ExpressionFunction.Length, 1),
		Function("normalize", ExpressionFunction.Normalize, 1),
		Function("dot", ExpressionFunction.Dot, 2),
		Function("distance", ExpressionFunction.Distance, 2),
		Function("cross", ExpressionFunction.Cross, 2),
		new("vec2", ExpressionCalleeKind.Construct, ExpressionFunction.Abs, SlotKind.Vector2, 2),
		new("vec3", ExpressionCalleeKind.Construct, ExpressionFunction.Abs, SlotKind.Vector3, 3),
		new("vec4", ExpressionCalleeKind.Construct, ExpressionFunction.Abs, SlotKind.Vector4, 4),
		Cast("int", SlotKind.Int),
		Cast("uint", SlotKind.UInt),
		Cast("long", SlotKind.Long),
		Cast("ulong", SlotKind.ULong),
		Cast("float", SlotKind.Float),
		Cast("double", SlotKind.Double),
	];

	private static readonly string[] s_CalleeNames = Array.ConvertAll(s_Callees, static callee => callee.Name);

	// The names a call may name: the functions, the vector constructors and the casts.
	internal static IReadOnlyList<string> CalleeNames => s_CalleeNames;

	// Reads a formula. suggest gives the names nearest to an unknown function's among CalleeNames.
	internal static ParsedFormula Parse(string text, Func<string, IReadOnlyList<string>> suggest)
	{
		if (JsonText.ByteCount(text) > MaxBytes)
		{
			ExpressionError tooLong = new(DiagnosticCode.InvalidExpression,
				FormattableString.Invariant($"a formula has at most {MaxBytes} bytes"), 0, text.Length, []);
			return new ParsedFormula(text, [], null, tooLong, 0);
		}
		Lexer lexer = new(text);
		lexer.Run();
		List<ExpressionToken> tokens = lexer.Tokens;
		int steps = lexer.Steps;
		List<string> inputs = [];
		ExpressionError? error = null;
		if (lexer.Error == null && tokens.Count == 1)
		{
			error = new ExpressionError(DiagnosticCode.InvalidExpression, "the formula is empty", 0, 0, []);
		}
		error ??= lexer.Error;
		error = FindInputs(text, tokens, inputs, error);
		if (error != null)
		{
			return new ParsedFormula(text, inputs, null, error, steps);
		}
		Parser parser = new(text, tokens, inputs, suggest);
		ExpressionSyntax? root = parser.Parse();
		return new ParsedFormula(text, inputs, root, root == null ? parser.Error : null, steps + parser.Steps);
	}

	// The callee a name calls; null when it calls nothing.
	internal static ExpressionCallee? FindCallee(string name)
	{
		foreach (ExpressionCallee callee in s_Callees)
		{
			if (callee.Name == name)
			{
				return callee;
			}
		}
		return null;
	}

	// The subtitle of an Expression node: the formula with its runs of white space as one space, at most 60 characters.
	internal static string Subtitle(string formula)
	{
		StringBuilder text = new();
		bool space = false;
		foreach (char character in formula)
		{
			if (IsWhiteSpace(character))
			{
				space = true;
				continue;
			}
			if (space && text.Length > 0)
			{
				text.Append(' ');
			}
			space = false;
			text.Append(character);
		}
		string collapsed = text.ToString();
		StringInfo elements = new(collapsed);
		if (elements.LengthInTextElements <= MaxSubtitleLength)
		{
			return collapsed;
		}
		return elements.SubstringByTextElements(0, MaxSubtitleLength - 1) + "…";
	}

	// "line L, column C" of a UTF-16 index: both from 1, columns counted in code points (a tab is one column).
	internal static string Position(string text, int index)
	{
		int line = 1;
		int column = 1;
		for (int at = 0; at < index && at < text.Length; at++)
		{
			char character = text[at];
			if (character == '\n')
			{
				line++;
				column = 1;
			}
			else if (!char.IsLowSurrogate(character) || at == 0 || !char.IsHighSurrogate(text[at - 1]))
			{
				column++;
			}
		}
		return FormattableString.Invariant($"line {line}, column {column}");
	}

	internal static bool IsWhiteSpace(char character) => character is ' ' or '\t' or '\n' or '\r';

	private static ExpressionCallee Function(string name, ExpressionFunction function, int arity) =>
		new(name, ExpressionCalleeKind.Function, function, SlotKind.Float, arity);

	private static ExpressionCallee Cast(string name, SlotKind type) => new(name, ExpressionCalleeKind.Cast, ExpressionFunction.Abs, type, 1);

	private static bool IsConstant(string name) => Array.IndexOf(s_Constants, name) >= 0;

	// The input rule (spec-slice3 section 3.3.1): an identifier is an input name unless a '(' follows it, a '.' comes before it,
	// or it is true, false, pi or tau. Names longer than an identifier may be are none (the lexer reported them); the reserved
	// pin names and Result are refused, and the first 32 distinct names are the inputs.
	private static ExpressionError? FindInputs(string text, List<ExpressionToken> tokens, List<string> inputs, ExpressionError? error)
	{
		for (int index = 0; index < tokens.Count; index++)
		{
			ExpressionToken token = tokens[index];
			if (token.Kind != ExpressionTokenKind.Identifier || token.End - token.Start > MaxIdentifierBytes)
			{
				continue;
			}
			if (tokens[index + 1].Punctuation == ExpressionPunctuation.OpenParenthesis ||
				(index > 0 && tokens[index - 1].Punctuation == ExpressionPunctuation.Dot))
			{
				continue;
			}
			string name = text[token.Start..token.End];
			if (IsConstant(name) || inputs.Contains(name))
			{
				continue;
			}
			if (Names.IsReserved(name))
			{
				error ??= new ExpressionError(DiagnosticCode.ReservedName,
					$"{name} is a reserved pin name ({Names.ReservedList}): name the input otherwise", token.Start, token.End, []);
				continue;
			}
			if (name == "Result")
			{
				error ??= new ExpressionError(DiagnosticCode.ReservedName, "Result names the node's output: name the input otherwise", token.Start,
					token.End, []);
				continue;
			}
			if (inputs.Count == MaxInputs)
			{
				error ??= new ExpressionError(DiagnosticCode.InvalidExpression,
					FormattableString.Invariant($"a formula uses at most {MaxInputs} inputs"), token.Start, token.End, []);
				continue;
			}
			inputs.Add(name);
		}
		return error;
	}

	// Splits a formula into tokens, ending with an End token. It goes on after a problem, so every identifier is found; the
	// first problem is kept.
	private sealed class Lexer
	{
		private readonly string m_Text;
		private int m_At;

		internal Lexer(string text)
		{
			m_Text = text;
		}

		internal List<ExpressionToken> Tokens { get; } = [];

		internal ExpressionError? Error { get; private set; }

		internal int Steps { get; private set; }

		internal void Run()
		{
			while (m_At < m_Text.Length)
			{
				Steps++;
				char character = m_Text[m_At];
				if (IsWhiteSpace(character))
				{
					m_At++;
				}
				else if (IsLetter(character) || character == '_')
				{
					Identifier();
				}
				else if (IsDigit(character))
				{
					Number();
				}
				else
				{
					Punctuation(character);
				}
			}
			Tokens.Add(new ExpressionToken(ExpressionTokenKind.End, m_Text.Length, m_Text.Length, ExpressionPunctuation.None, default, default));
		}

		private void Identifier()
		{
			int start = m_At;
			while (m_At < m_Text.Length && (IsLetter(m_Text[m_At]) || IsDigit(m_Text[m_At]) || m_Text[m_At] == '_'))
			{
				Steps++;
				m_At++;
			}
			if (m_At - start > MaxIdentifierBytes)
			{
				Fail(FormattableString.Invariant($"{JsonText.Shorten(m_Text[start..m_At])} is too long: a name has at most {MaxIdentifierBytes} bytes"),
					start, m_At);
			}
			Tokens.Add(new ExpressionToken(ExpressionTokenKind.Identifier, start, m_At, ExpressionPunctuation.None, default, default));
		}

		// digits ["." digits] [("e" | "E") ["+" | "-"] digits] ["d" | "D"], each optional part only when it is complete.
		private void Number()
		{
			int start = m_At;
			SkipDigits();
			bool real = false;
			if (m_At + 1 < m_Text.Length && m_Text[m_At] == '.' && IsDigit(m_Text[m_At + 1]))
			{
				m_At++;
				SkipDigits();
				real = true;
			}
			if (m_At < m_Text.Length && m_Text[m_At] is 'e' or 'E')
			{
				int exponent = m_At + 1;
				if (exponent < m_Text.Length && m_Text[exponent] is '+' or '-')
				{
					exponent++;
				}
				if (exponent < m_Text.Length && IsDigit(m_Text[exponent]))
				{
					m_At = exponent;
					SkipDigits();
					real = true;
				}
			}
			int digitsEnd = m_At;
			bool isDouble = false;
			if (m_At < m_Text.Length && m_Text[m_At] is 'd' or 'D')
			{
				m_At++;
				isDouble = true;
			}
			string written = m_Text[start..m_At];
			string digits = m_Text[start..digitsEnd];
			SlotKind kind;
			Slot value;
			if (isDouble || real)
			{
				double number = double.Parse(digits, NumberStyles.Float, CultureInfo.InvariantCulture);
				if (isDouble)
				{
					kind = SlotKind.Double;
					value = Slot.FromDouble(number);
					if (double.IsInfinity(number))
					{
						Fail($"{JsonText.Shorten(written)} is too large for a double", start, m_At);
					}
				}
				else
				{
					// Read as the double the text denotes, narrowed to float, as every float of the engine is read.
					kind = SlotKind.Float;
					value = Slot.FromFloat((float)number);
					if (Math.Abs(number) > float.MaxValue)
					{
						Fail($"{JsonText.Shorten(written)} is too large for a float: write {JsonText.Shorten(written)}d for a double", start, m_At);
					}
				}
			}
			else if (int.TryParse(digits, NumberStyles.None, CultureInfo.InvariantCulture, out int integer))
			{
				kind = SlotKind.Int;
				value = Slot.FromInt(integer);
			}
			else if (long.TryParse(digits, NumberStyles.None, CultureInfo.InvariantCulture, out long wide))
			{
				kind = SlotKind.Long;
				value = Slot.FromLong(wide);
			}
			else if (ulong.TryParse(digits, NumberStyles.None, CultureInfo.InvariantCulture, out ulong unsigned))
			{
				kind = SlotKind.ULong;
				value = Slot.FromULong(unsigned);
			}
			else
			{
				kind = SlotKind.Int;
				value = default;
				Fail($"{JsonText.Shorten(written)} is too large for an integer", start, m_At);
			}
			Tokens.Add(new ExpressionToken(ExpressionTokenKind.Number, start, m_At, ExpressionPunctuation.None, kind, value));
		}

		private void SkipDigits()
		{
			while (m_At < m_Text.Length && IsDigit(m_Text[m_At]))
			{
				Steps++;
				m_At++;
			}
		}

		private void Punctuation(char character)
		{
			int start = m_At;
			char next = m_At + 1 < m_Text.Length ? m_Text[m_At + 1] : '\0';
			(ExpressionPunctuation punctuation, int length) = character switch
			{
				'+' => (ExpressionPunctuation.Plus, 1),
				'-' => (ExpressionPunctuation.Minus, 1),
				'*' => (ExpressionPunctuation.Star, 1),
				'/' => (ExpressionPunctuation.Slash, 1),
				'%' => (ExpressionPunctuation.Percent, 1),
				'(' => (ExpressionPunctuation.OpenParenthesis, 1),
				')' => (ExpressionPunctuation.CloseParenthesis, 1),
				',' => (ExpressionPunctuation.Comma, 1),
				'?' => (ExpressionPunctuation.Question, 1),
				':' => (ExpressionPunctuation.Colon, 1),
				'.' => (ExpressionPunctuation.Dot, 1),
				'!' when next == '=' => (ExpressionPunctuation.NotEqual, 2),
				'!' => (ExpressionPunctuation.Bang, 1),
				'<' when next == '=' => (ExpressionPunctuation.LessOrEqual, 2),
				'<' => (ExpressionPunctuation.Less, 1),
				'>' when next == '=' => (ExpressionPunctuation.GreaterOrEqual, 2),
				'>' => (ExpressionPunctuation.Greater, 1),
				'=' when next == '=' => (ExpressionPunctuation.Equal, 2),
				'&' when next == '&' => (ExpressionPunctuation.And, 2),
				'|' when next == '|' => (ExpressionPunctuation.Or, 2),
				_ => (ExpressionPunctuation.None, 0),
			};
			if (punctuation == ExpressionPunctuation.None)
			{
				// One code point, a surrogate pair included.
				int end = start + (char.IsHighSurrogate(character) && char.IsLowSurrogate(next) ? 2 : 1);
				Fail($"unexpected '{m_Text[start..end]}'", start, end);
				m_At = end;
				return;
			}
			m_At += length;
			Tokens.Add(new ExpressionToken(ExpressionTokenKind.Punctuation, start, m_At, punctuation, default, default));
		}

		private void Fail(string message, int start, int end)
		{
			Error ??= new ExpressionError(DiagnosticCode.InvalidExpression, message, start, end, []);
		}

		private static bool IsLetter(char character) => character is >= 'A' and <= 'Z' or >= 'a' and <= 'z';

		private static bool IsDigit(char character) => character is >= '0' and <= '9';
	}

	// The grammar of spec-slice3 section 3.3.3 by recursive descent: binary operators of a level are a loop, so only nesting
	// recurses (parentheses, calls, unary operators and the branches of ?:), which MaxNesting bounds.
	private sealed class Parser
	{
		private readonly string m_Text;
		private readonly List<ExpressionToken> m_Tokens;
		private readonly List<string> m_Inputs;
		private readonly Func<string, IReadOnlyList<string>> m_Suggest;
		private int m_Index;
		private int m_Nesting;
		private int m_Parts;

		internal Parser(string text, List<ExpressionToken> tokens, List<string> inputs, Func<string, IReadOnlyList<string>> suggest)
		{
			m_Text = text;
			m_Tokens = tokens;
			m_Inputs = inputs;
			m_Suggest = suggest;
		}

		internal ExpressionError? Error { get; private set; }

		internal int Steps { get; private set; }

		private ExpressionToken Current => m_Tokens[m_Index];

		internal ExpressionSyntax? Parse()
		{
			ExpressionSyntax? root = Conditional();
			if (root == null)
			{
				return null;
			}
			if (Current.Kind != ExpressionTokenKind.End)
			{
				return Fail($"unexpected '{TextOf(Current)}' after the formula", Current.Start, Current.End);
			}
			return root;
		}

		private ExpressionToken Next()
		{
			Steps++;
			ExpressionToken token = m_Tokens[m_Index];
			if (token.Kind != ExpressionTokenKind.End)
			{
				m_Index++;
			}
			return token;
		}

		private bool At(ExpressionPunctuation punctuation) => Current.Kind == ExpressionTokenKind.Punctuation && Current.Punctuation == punctuation;

		// conditional = or [ "?" conditional ":" conditional ]
		private ExpressionSyntax? Conditional()
		{
			Steps++;
			ExpressionSyntax? condition = Or();
			if (condition == null || !At(ExpressionPunctuation.Question))
			{
				return condition;
			}
			ExpressionToken question = Next();
			ExpressionSyntax? whenTrue = Nested(question, Conditional);
			if (whenTrue == null)
			{
				return null;
			}
			if (!At(ExpressionPunctuation.Colon))
			{
				return Fail($"expected ':' for the '?' at {Position(m_Text, question.Start)}", question.Start, question.End);
			}
			Next();
			ExpressionSyntax? whenFalse = Nested(question, Conditional);
			if (whenFalse == null)
			{
				return null;
			}
			return Make(new ExpressionSyntax(ExpressionSyntaxKind.Conditional, condition.Start, whenFalse.End)
			{
				Operands = [condition, whenTrue, whenFalse],
			}, question);
		}

		private ExpressionSyntax? Or() => Binary(And, ExpressionPunctuation.Or);

		private ExpressionSyntax? And() => Binary(Equality, ExpressionPunctuation.And);

		private ExpressionSyntax? Equality() => Binary(Relation, ExpressionPunctuation.Equal, ExpressionPunctuation.NotEqual);

		private ExpressionSyntax? Relation() => Binary(Additive, ExpressionPunctuation.Less, ExpressionPunctuation.LessOrEqual, ExpressionPunctuation.Greater,
			ExpressionPunctuation.GreaterOrEqual);

		private ExpressionSyntax? Additive() => Binary(Term, ExpressionPunctuation.Plus, ExpressionPunctuation.Minus);

		private ExpressionSyntax? Term() => Binary(Unary, ExpressionPunctuation.Star, ExpressionPunctuation.Slash, ExpressionPunctuation.Percent);

		// A left-associative level: operand { operator operand }.
		private ExpressionSyntax? Binary(Func<ExpressionSyntax?> operand, params ReadOnlySpan<ExpressionPunctuation> operators)
		{
			Steps++;
			ExpressionSyntax? left = operand();
			while (left != null && Current.Kind == ExpressionTokenKind.Punctuation && operators.Contains(Current.Punctuation))
			{
				ExpressionToken op = Next();
				ExpressionSyntax? right = operand();
				if (right == null)
				{
					return null;
				}
				left = Make(new ExpressionSyntax(ExpressionSyntaxKind.Binary, left.Start, right.End) { Operator = op.Punctuation, Operands = [left, right] }, op);
			}
			return left;
		}

		// unary = ("-" | "+" | "!") unary | postfix
		private ExpressionSyntax? Unary()
		{
			Steps++;
			if (At(ExpressionPunctuation.Minus) || At(ExpressionPunctuation.Plus) || At(ExpressionPunctuation.Bang))
			{
				ExpressionToken op = Next();
				ExpressionSyntax? operand = Nested(op, Unary);
				if (operand == null)
				{
					return null;
				}
				return Make(new ExpressionSyntax(ExpressionSyntaxKind.Unary, op.Start, operand.End) { Operator = op.Punctuation, Operands = [operand] }, op);
			}
			return Postfix();
		}

		// postfix = primary { "." identifier }
		private ExpressionSyntax? Postfix()
		{
			Steps++;
			ExpressionSyntax? value = Primary();
			while (value != null && At(ExpressionPunctuation.Dot))
			{
				ExpressionToken dot = Next();
				if (Current.Kind != ExpressionTokenKind.Identifier)
				{
					return Fail("expected a component (X, Y, Z or W) after '.'", dot.Start, dot.End);
				}
				ExpressionToken name = Next();
				value = Make(new ExpressionSyntax(ExpressionSyntaxKind.Component, value.Start, name.End)
				{
					Operands = [value],
					Name = TextOf(name),
					NameStart = name.Start,
					NameEnd = name.End,
				}, name);
			}
			return value;
		}

		private ExpressionSyntax? Primary()
		{
			Steps++;
			ExpressionToken token = Current;
			switch (token.Kind)
			{
				case ExpressionTokenKind.Number:
					Next();
					return Make(new ExpressionSyntax(ExpressionSyntaxKind.Literal, token.Start, token.End) { LiteralKind = token.NumberKind, Literal = token.Number },
						token);
				case ExpressionTokenKind.Identifier:
					return Name();
				case ExpressionTokenKind.Punctuation when token.Punctuation == ExpressionPunctuation.OpenParenthesis:
					Next();
					ExpressionSyntax? inner = Nested(token, Conditional);
					if (inner == null)
					{
						return null;
					}
					if (!At(ExpressionPunctuation.CloseParenthesis))
					{
						return Fail($"expected ')' to close the '(' at {Position(m_Text, token.Start)}", token.Start, token.End);
					}
					Next();
					return inner;
				case ExpressionTokenKind.Punctuation when token.Punctuation == ExpressionPunctuation.Dot && m_Tokens[m_Index + 1] is
				{ Kind: ExpressionTokenKind.Number } number && number.Start == token.End:
					string fraction = TextOf(number);
					return Fail($"write 0.{fraction}, not .{fraction}", token.Start, number.End);
			}
			if (m_Index > 0)
			{
				ExpressionToken previous = m_Tokens[m_Index - 1];
				return Fail($"expected a value after '{TextOf(previous)}'", previous.Start, previous.End);
			}
			return Fail($"expected a value, not '{TextOf(token)}'", token.Start, token.End);
		}

		// An identifier: a call when a '(' follows it, else a constant or an input.
		private ExpressionSyntax? Name()
		{
			ExpressionToken token = Next();
			string name = TextOf(token);
			if (At(ExpressionPunctuation.OpenParenthesis))
			{
				return Call(token, name);
			}
			switch (name)
			{
				case "true":
				case "false":
					return Make(new ExpressionSyntax(ExpressionSyntaxKind.Literal, token.Start, token.End)
					{
						LiteralKind = SlotKind.Bool,
						Literal = Slot.FromBool(name == "true"),
					}, token);
				case "pi":
				case "tau":
					return Make(new ExpressionSyntax(ExpressionSyntaxKind.Literal, token.Start, token.End)
					{
						LiteralKind = SlotKind.Float,
						Literal = Slot.FromFloat(name == "pi" ? MathF.PI : 2.0f * MathF.PI),
					}, token);
			}
			return Make(new ExpressionSyntax(ExpressionSyntaxKind.Input, token.Start, token.End) { Input = m_Inputs.IndexOf(name) }, token);
		}

		// call = (function | cast) "(" [ conditional { "," conditional } ] ")"
		private ExpressionSyntax? Call(ExpressionToken nameToken, string name)
		{
			if (IsConstant(name))
			{
				return Fail($"{name} is a constant, not a function", nameToken.Start, nameToken.End);
			}
			ExpressionCallee? callee = FindCallee(name);
			if (callee == null)
			{
				IReadOnlyList<string> suggestions = m_Suggest(name);
				string advice = suggestions.Count > 0 ? $": did you mean {suggestions[0]}?" : string.Empty;
				return FailWith(DiagnosticCode.InvalidExpression, $"unknown function {name}{advice}", nameToken.Start, nameToken.End, suggestions);
			}
			ExpressionToken open = Next();
			List<ExpressionSyntax> arguments = [];
			if (!At(ExpressionPunctuation.CloseParenthesis))
			{
				while (true)
				{
					ExpressionSyntax? argument = Nested(open, Conditional);
					if (argument == null)
					{
						return null;
					}
					arguments.Add(argument);
					if (At(ExpressionPunctuation.Comma))
					{
						Next();
						continue;
					}
					if (At(ExpressionPunctuation.CloseParenthesis))
					{
						break;
					}
					return Fail($"expected ')' to close the '(' at {Position(m_Text, open.Start)}", open.Start, open.End);
				}
			}
			ExpressionToken close = Next();
			if (arguments.Count != callee.Arity)
			{
				return Fail(FormattableString.Invariant(
					$"{name} takes {callee.Arity} argument{(callee.Arity == 1 ? "" : "s")}, not {arguments.Count}"), nameToken.Start, close.End);
			}
			return Make(new ExpressionSyntax(ExpressionSyntaxKind.Call, nameToken.Start, close.End) { Operands = [.. arguments], Callee = callee }, nameToken);
		}

		// A part of the formula one level deeper than its parent, which opener opens.
		private ExpressionSyntax? Nested(ExpressionToken opener, Func<ExpressionSyntax?> part)
		{
			if (m_Nesting == MaxNesting)
			{
				return Fail(FormattableString.Invariant($"a formula nests at most {MaxNesting} levels deep"), opener.Start, opener.End);
			}
			m_Nesting++;
			ExpressionSyntax? result = part();
			m_Nesting--;
			return result;
		}

		// Counts a new node of the tree against MaxParts; token is where the part that is one too many is.
		private ExpressionSyntax? Make(ExpressionSyntax syntax, ExpressionToken token)
		{
			if (++m_Parts > MaxParts)
			{
				return Fail(FormattableString.Invariant($"a formula has at most {MaxParts} parts"), token.Start, token.End);
			}
			return syntax;
		}

		private string TextOf(ExpressionToken token) => token.Kind == ExpressionTokenKind.End ? "the end" : m_Text[token.Start..token.End];

		private ExpressionSyntax? Fail(string message, int start, int end) => FailWith(DiagnosticCode.InvalidExpression, message, start, end, []);

		private ExpressionSyntax? FailWith(DiagnosticCode code, string message, int start, int end, IReadOnlyList<string> suggestions)
		{
			Error ??= new ExpressionError(code, message, start, end, suggestions);
			return null;
		}
	}
}
