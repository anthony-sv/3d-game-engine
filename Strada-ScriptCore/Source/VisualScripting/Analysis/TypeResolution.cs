using System;
using System.Collections.Generic;
using System.Reflection;

namespace Strada.VisualScripting;

// Resolves the wildcard pins of a graph (spec section 3.5) and types the operators (spec section 4.4) and Convert nodes.
// Nodes are resolved in data-flow order, so a node's wildcard inputs see the resolved types of the outputs linked into
// them; nodes on a cycle of wildcards are resolved last with what is known (pure cycles are an error of their own).
internal sealed class TypeResolution
{
	private readonly AnalysisContext m_Context;
	private readonly IReadOnlyDictionary<AnalyzedNode, VisualType> m_ConvertTargets;

	internal TypeResolution(AnalysisContext context, IReadOnlyDictionary<AnalyzedNode, VisualType> convertTargets)
	{
		m_Context = context;
		m_ConvertTargets = convertTargets;
	}

	private NodeLibrary Library => m_Context.Library;

	internal void Resolve(AnalyzedGraph graph)
	{
		Dictionary<AnalyzedNode, List<AnalyzedNode>> dependents = [];
		Dictionary<AnalyzedNode, int> waiting = [];
		List<AnalyzedNode> nodes = [];
		foreach (AnalyzedNode node in graph.Nodes)
		{
			if (HasWildcards(node))
			{
				nodes.Add(node);
				waiting[node] = 0;
			}
		}
		foreach (AnalyzedNode node in nodes)
		{
			HashSet<AnalyzedNode> sources = [];
			foreach (AnalyzedPin pin in node.Pins)
			{
				if (pin.IsInput && pin.Description.Source == WildcardSource.Links && pin.Source is { } link &&
					link.From.Description.Source != WildcardSource.None && waiting.ContainsKey(link.From.Node) && sources.Add(link.From.Node))
				{
					if (!dependents.TryGetValue(link.From.Node, out List<AnalyzedNode>? list))
					{
						list = [];
						dependents[link.From.Node] = list;
					}
					list.Add(node);
					waiting[node]++;
				}
			}
		}
		PriorityQueue<AnalyzedNode, int> ready = new();
		foreach (AnalyzedNode node in nodes)
		{
			if (waiting[node] == 0)
			{
				ready.Enqueue(node, node.Source.Index);
			}
		}
		HashSet<AnalyzedNode> resolved = [];
		while (ready.TryDequeue(out AnalyzedNode? node, out _))
		{
			ResolveNode(node);
			resolved.Add(node);
			foreach (AnalyzedNode dependent in dependents.GetValueOrDefault(node) ?? [])
			{
				if (--waiting[dependent] == 0)
				{
					ready.Enqueue(dependent, dependent.Source.Index);
				}
			}
		}
		foreach (AnalyzedNode node in nodes)
		{
			if (!resolved.Contains(node))
			{
				ResolveNode(node);
			}
		}
	}

	private static bool HasWildcards(AnalyzedNode node)
	{
		foreach (AnalyzedPin pin in node.Pins)
		{
			if (pin.Description.Source != WildcardSource.None)
			{
				return true;
			}
		}
		return false;
	}

	private void ResolveNode(AnalyzedNode node)
	{
		// Type variables: a Group shares one among its pins, any other wildcard pin resolved from links is its own.
		List<List<AnalyzedPin>> variables = [];
		Dictionary<string, List<AnalyzedPin>> groups = new(StringComparer.Ordinal);
		foreach (AnalyzedPin pin in node.Pins)
		{
			if (pin.Description.Source != WildcardSource.Links)
			{
				continue;
			}
			if (pin.Description.Group is { } group)
			{
				if (!groups.TryGetValue(group, out List<AnalyzedPin>? members))
				{
					members = [];
					groups[group] = members;
					variables.Add(members);
				}
				members.Add(pin);
			}
			else
			{
				variables.Add([pin]);
			}
		}
		foreach (List<AnalyzedPin> variable in variables)
		{
			VisualType type = ResolveVariable(node, variable);
			foreach (AnalyzedPin pin in variable)
			{
				pin.Type = type;
			}
		}
		BuiltinNode? builtin = node.Builtin;
		if (builtin == BuiltinNode.Reroute && node.FindPin("In", PinDirection.In) is { Type.IsExec: true })
		{
			// A Reroute of the flow passes executions on: it is not a pure node.
			node.Pure = false;
		}
		if (builtin is { } binary && BuiltinNodes.IsBinaryWildcardOperator(binary))
		{
			ApplyOperandRider(node);
		}
		foreach (AnalyzedPin pin in node.Pins)
		{
			if (pin.Description.Source == WildcardSource.ElementOf)
			{
				AnalyzedPin? array = node.FindPin(pin.Description.ElementOf!, PinDirection.In);
				pin.Type = array?.Type is { Kind: TypeKind.Array, Element: { } element, HasValues: true } ? element : VisualType.Wildcard;
			}
		}
		if (builtin is { } op && BuiltinNodes.IsOperator(op))
		{
			ResolveOperator(node, op);
		}
		else if (builtin == BuiltinNode.Convert)
		{
			ResolveConvert(node);
		}
	}

	// The type of a type variable from the outputs linked into its inputs (rules 2 and 3). A Reroute whose In is linked to
	// an exec output is exec; one whose In is unconnected takes the type of the inputs its Out leads to.
	private VisualType ResolveVariable(AnalyzedNode node, List<AnalyzedPin> variable)
	{
		bool reroute = node.Builtin == BuiltinNode.Reroute;
		bool linked = false;
		VisualType? joined = null;
		AnalyzedPin? first = null;
		foreach (AnalyzedPin pin in variable)
		{
			if (!pin.IsInput || pin.Source is not { } link)
			{
				continue;
			}
			linked = true;
			VisualType type = link.From.Type;
			if (type.IsExec && reroute)
			{
				return VisualType.Exec;
			}
			if (!type.HasValues)
			{
				continue;
			}
			if (joined == null)
			{
				joined = type;
				first = pin;
				continue;
			}
			VisualType? join = TypeRules.Join(joined, type);
			if (join == null)
			{
				m_Context.ReportNode(DiagnosticCode.TypeMismatch, node,
					$"{first!.Name} is {joined.Id} and {pin.Name} is {type.Id}: {Describe(variable)} take one type, and these have no common type");
				return VisualType.Wildcard;
			}
			joined = join;
		}
		if (joined != null)
		{
			return joined;
		}
		return reroute && !linked ? ResolveDownstream(node) : VisualType.Wildcard;
	}

	private static string Describe(List<AnalyzedPin> variable)
	{
		List<string> names = [];
		foreach (AnalyzedPin pin in variable)
		{
			names.Add(pin.Name);
		}
		return string.Join(", ", names);
	}

	// A Reroute with an unconnected In: the type of the first typed input its Out leads to, through other Reroutes. The walk
	// enters a Reroute only through the link its In takes its value from (any other link into it is an Error
	// MultipleDataLinks): each Reroute is entered from one Reroute at most, so all the walks together visit it at most once.
	private static VisualType ResolveDownstream(AnalyzedNode reroute)
	{
		HashSet<AnalyzedNode> visited = [reroute];
		Queue<AnalyzedNode> pending = new();
		pending.Enqueue(reroute);
		while (pending.TryDequeue(out AnalyzedNode? current))
		{
			AnalyzedPin? output = current.FindPin("Out", PinDirection.Out);
			foreach (AnalyzedLink link in output?.Links ?? [])
			{
				AnalyzedPin target = link.To;
				if (target.Node.Builtin == BuiltinNode.Reroute && target.Name == "In")
				{
					if (ReferenceEquals(target.Source, link) && visited.Add(target.Node))
					{
						pending.Enqueue(target.Node);
					}
					continue;
				}
				if (target.Description.Source == WildcardSource.None && !target.Type.IsWildcard)
				{
					return target.Type;
				}
			}
		}
		return VisualType.Wildcard;
	}

	// Rule 4's rider: when exactly one operand is linked and the other stores a literal, the literal has the linked type.
	private static void ApplyOperandRider(AnalyzedNode node)
	{
		AnalyzedPin? a = node.FindPin("A", PinDirection.In);
		AnalyzedPin? b = node.FindPin("B", PinDirection.In);
		if (a == null || b == null || (a.Source == null) == (b.Source == null))
		{
			return;
		}
		(AnalyzedPin linked, AnalyzedPin other) = a.Source != null ? (a, b) : (b, a);
		if (linked.Type.HasValues && !other.Type.HasValues && StoresLiteral(node, other.Name))
		{
			other.Type = linked.Type;
		}
	}

	internal static bool StoresLiteral(AnalyzedNode node, string pin)
	{
		foreach (KeyValuePair<string, System.Text.Json.JsonElement> input in node.Source.Inputs.Members)
		{
			if (node.Rename(PinDirection.In, input.Key) == pin)
			{
				return true;
			}
		}
		return false;
	}

	private void ResolveOperator(AnalyzedNode node, BuiltinNode op)
	{
		AnalyzedPin? a = node.FindPin("A", PinDirection.In);
		AnalyzedPin? b = node.FindPin("B", PinDirection.In);
		AnalyzedPin? result = node.FindPin("Result", PinDirection.Out);
		if (a == null || result == null || !a.Type.HasValues || (b != null && !b.Type.HasValues))
		{
			return;
		}
		switch (op)
		{
			case BuiltinNode.OperatorNegate:
				Negate(node, a.Type, result);
				break;
			case BuiltinNode.OperatorEqual:
			case BuiltinNode.OperatorNotEqual:
				Equality(node, a.Type, b!.Type);
				break;
			case BuiltinNode.OperatorLess:
			case BuiltinNode.OperatorLessOrEqual:
			case BuiltinNode.OperatorGreater:
			case BuiltinNode.OperatorGreaterOrEqual:
				Comparison(node, op, a.Type, b!.Type);
				break;
			default:
				Arithmetic(node, op, a.Type, b!.Type, result);
				break;
		}
	}

	// (1) numeric primitives at their join; (2) Add with a string operand concatenates; (3) the operator method of the
	// operand types.
	private void Arithmetic(AnalyzedNode node, BuiltinNode op, VisualType a, VisualType b, AnalyzedPin result)
	{
		if (a.IsNumeric && b.IsNumeric)
		{
			VisualType? join = TypeRules.Join(a, b);
			if (join == null)
			{
				m_Context.ReportNode(DiagnosticCode.TypeMismatch, node,
					$"{a.Id} and {b.Id} have no common numeric type (convert one of them with Convert)");
				return;
			}
			result.Type = join;
			node.Binding = new OperatorBinding(OperatorMode.Numeric, join, null);
			return;
		}
		if (op == BuiltinNode.OperatorAdd && (a.ClrType == typeof(string) || b.ClrType == typeof(string)))
		{
			result.Type = Library.String;
			node.Binding = new OperatorBinding(OperatorMode.Concatenate, Library.String, null);
			return;
		}
		string name = op switch
		{
			BuiltinNode.OperatorAdd => "op_Addition",
			BuiltinNode.OperatorSubtract => "op_Subtraction",
			BuiltinNode.OperatorMultiply => "op_Multiply",
			BuiltinNode.OperatorDivide => "op_Division",
			_ => "op_Modulus",
		};
		if (FindOperator(node, name, a, b) is { } method)
		{
			result.Type = Library.FindType(method.ReturnType)!;
			node.Binding = new OperatorBinding(OperatorMode.Method, null, method);
		}
	}

	// C# unary minus on numbers (uint becomes long; ulong has none), else op_UnaryNegation.
	private void Negate(AnalyzedNode node, VisualType a, AnalyzedPin result)
	{
		if (a.IsNumeric)
		{
			if (a.ClrType == typeof(ulong))
			{
				m_Context.ReportNode(DiagnosticCode.NoOperator, node, "ulong values cannot be negated (convert to long or double first)");
				return;
			}
			VisualType type = a.ClrType == typeof(uint) ? Library.Long : a;
			result.Type = type;
			node.Binding = new OperatorBinding(OperatorMode.Numeric, type, null);
			return;
		}
		if (FindOperator(node, "op_UnaryNegation", a, null) is { } method)
		{
			result.Type = Library.FindType(method.ReturnType)!;
			node.Binding = new OperatorBinding(OperatorMode.Method, null, method);
		}
	}

	private void Comparison(AnalyzedNode node, BuiltinNode op, VisualType a, VisualType b)
	{
		if (a.IsNumeric && b.IsNumeric)
		{
			VisualType? join = TypeRules.Join(a, b);
			if (join == null)
			{
				m_Context.ReportNode(DiagnosticCode.TypeMismatch, node,
					$"{a.Id} and {b.Id} have no common numeric type (convert one of them with Convert)");
				return;
			}
			node.Binding = new OperatorBinding(OperatorMode.Numeric, join, null);
			return;
		}
		string name = op switch
		{
			BuiltinNode.OperatorLess => "op_LessThan",
			BuiltinNode.OperatorLessOrEqual => "op_LessThanOrEqual",
			BuiltinNode.OperatorGreater => "op_GreaterThan",
			_ => "op_GreaterThanOrEqual",
		};
		if (FindOperator(node, name, a, b, Library.Bool) is { } method)
		{
			node.Binding = new OperatorBinding(OperatorMode.Method, null, method);
		}
	}

	// The equality table of spec section 4.4.
	private void Equality(AnalyzedNode node, VisualType a, VisualType b)
	{
		if (a.IsNumeric && b.IsNumeric)
		{
			VisualType? numeric = TypeRules.Join(a, b);
			if (numeric == null)
			{
				m_Context.ReportNode(DiagnosticCode.TypeMismatch, node, $"{a.Id} and {b.Id} have no common numeric type (convert one of them with Convert)");
				return;
			}
			node.Binding = new OperatorBinding(OperatorMode.Numeric, numeric, null);
			return;
		}
		if (a.Kind == TypeKind.Enum || b.Kind == TypeKind.Enum)
		{
			if (!ReferenceEquals(a, b))
			{
				m_Context.ReportNode(DiagnosticCode.TypeMismatch, node, $"{a.Id} and {b.Id}: enum values compare with values of the same enum");
				return;
			}
			node.Binding = new OperatorBinding(OperatorMode.ValueEquality, a, null);
			return;
		}
		VisualType? join = TypeRules.Join(a, b);
		if (join == null)
		{
			m_Context.ReportNode(DiagnosticCode.TypeMismatch, node, $"{a.Id} and {b.Id} cannot be compared: they have no common type");
			return;
		}
		OperatorMode mode = join.Kind switch
		{
			TypeKind.Bool or TypeKind.String => OperatorMode.ValueEquality,
			TypeKind.Entity => OperatorMode.EntityEquality,
			TypeKind.Asset => OperatorMode.AssetEquality,
			TypeKind.Component => OperatorMode.ComponentEquality,
			_ => OperatorMode.ObjectEquality,
		};
		MethodInfo? method = null;
		if (mode == OperatorMode.ObjectEquality)
		{
			// Structs use the equality operator they declare; other classes one declared on them or a base class.
			method = FindEquality(join);
			mode = method != null ? OperatorMode.Method : OperatorMode.ObjectEquality;
		}
		node.Binding = new OperatorBinding(mode, join, method);
	}

	private MethodInfo? FindEquality(VisualType type)
	{
		for (Type? current = type.ClrType; current != null && current != typeof(object) && current != typeof(ValueType); current = current.BaseType)
		{
			foreach (MethodInfo method in current.GetMethods(BindingFlags.Public | BindingFlags.Static | BindingFlags.DeclaredOnly))
			{
				ParameterInfo[] parameters = method.GetParameters();
				if (method.Name == "op_Equality" && method.ReturnType == typeof(bool) && parameters.Length == 2 &&
					parameters[0].ParameterType.IsAssignableFrom(type.ClrType) && parameters[1].ParameterType.IsAssignableFrom(type.ClrType))
				{
					return method;
				}
			}
			if (type.ClrType!.IsValueType)
			{
				break;
			}
		}
		return null;
	}

	// The operator method named name on the operand types and their base types: parameters that take the operands as they
	// are first, then with numeric widening (rule 4). None, or more than one, is an Error NoOperator. A method whose types
	// are not library types does not count; for comparisons the method must return bool.
	private MethodInfo? FindOperator(AnalyzedNode node, string name, VisualType a, VisualType? b, VisualType? returns = null)
	{
		int arity = b == null ? 1 : 2;
		List<MethodInfo> candidates = [];
		CollectOperators(a.ClrType!, name, arity, candidates);
		if (b != null)
		{
			CollectOperators(b.ClrType!, name, arity, candidates);
		}
		List<MethodInfo> usable = [];
		foreach (MethodInfo candidate in candidates)
		{
			if (Library.FindType(candidate.ReturnType) is { } result && (returns == null || ReferenceEquals(result, returns)) &&
				AllParametersAreLibraryTypes(candidate))
			{
				usable.Add(candidate);
			}
		}
		foreach (bool widen in new[] { false, true })
		{
			List<MethodInfo> matches = [];
			foreach (MethodInfo candidate in usable)
			{
				ParameterInfo[] parameters = candidate.GetParameters();
				if (Accepts(parameters[0].ParameterType, a, widen) && (b == null || Accepts(parameters[1].ParameterType, b, widen)))
				{
					matches.Add(candidate);
				}
			}
			if (matches.Count == 1)
			{
				return matches[0];
			}
			if (matches.Count > 1)
			{
				m_Context.ReportNode(DiagnosticCode.NoOperator, node, $"{Operands(a, b)} match {AnalysisContext.Format(matches.Count)} {name} operators");
				return null;
			}
		}
		m_Context.ReportNode(DiagnosticCode.NoOperator, node, $"there is no {node.Title} operator for {Operands(a, b)}");
		return null;
	}

	private static string Operands(VisualType a, VisualType? b) => b == null ? a.Id : $"{a.Id} and {b.Id}";

	private static void CollectOperators(Type type, string name, int arity, List<MethodInfo> candidates)
	{
		for (Type? current = type; current != null && current != typeof(object) && current != typeof(ValueType) && current != typeof(Enum);
			current = current.BaseType)
		{
			foreach (MethodInfo method in current.GetMethods(BindingFlags.Public | BindingFlags.Static | BindingFlags.DeclaredOnly))
			{
				if (method.Name == name && !method.IsGenericMethodDefinition && method.GetParameters().Length == arity && !candidates.Contains(method))
				{
					candidates.Add(method);
				}
			}
		}
	}

	private bool AllParametersAreLibraryTypes(MethodInfo method)
	{
		foreach (ParameterInfo parameter in method.GetParameters())
		{
			if (parameter.ParameterType.IsByRef || Library.FindType(parameter.ParameterType) == null)
			{
				return false;
			}
		}
		return true;
	}

	// Whether an operator parameter takes an operand: the same type or a base class of a reference; with widening, also a
	// numeric type the operand widens to.
	private bool Accepts(Type parameter, VisualType operand, bool widen)
	{
		Type type = operand.ClrType!;
		if (parameter == type || (!type.IsValueType && parameter.IsAssignableFrom(type)))
		{
			return true;
		}
		return widen && Library.FindType(parameter) is { } target && TypeRules.Widens(operand, target);
	}

	// How Convert converts its Value to To (spec section 4.4); NoConversion for any other pair.
	private void ResolveConvert(AnalyzedNode node)
	{
		AnalyzedPin? value = node.FindPin("Value", PinDirection.In);
		if (value == null || !value.Type.HasValues || !m_ConvertTargets.TryGetValue(node, out VisualType? to))
		{
			return;
		}
		VisualType from = value.Type;
		ConvertBinding? binding = PlanConversion(from, to);
		if (binding == null)
		{
			m_Context.ReportNode(DiagnosticCode.NoConversion, node, $"there is no conversion from {from.Id} to {to.Id}");
			return;
		}
		node.Binding = binding;
	}

	private ConvertBinding? PlanConversion(VisualType from, VisualType to)
	{
		if (ReferenceEquals(from, to))
		{
			return new ConvertBinding(from, to, ConvertPlan.Identity, null, null);
		}
		if (from.IsNumeric && to.IsNumeric)
		{
			return new ConvertBinding(from, to, ConvertPlan.NumericCast, null, null);
		}
		if (from.Kind == TypeKind.Enum && to.Kind == TypeKind.Integer)
		{
			return new ConvertBinding(from, to, ConvertPlan.EnumToInteger, null, null);
		}
		if (from.Kind == TypeKind.Integer && to.Kind == TypeKind.Enum)
		{
			return new ConvertBinding(from, to, ConvertPlan.IntegerToEnum, null, null);
		}
		if (from.ClrType == typeof(string))
		{
			ConvertPlan? parse = to.Kind switch
			{
				TypeKind.Bool => ConvertPlan.ParseBool,
				TypeKind.Integer or TypeKind.Float => ConvertPlan.ParseNumber,
				TypeKind.Enum => ConvertPlan.ParseEnum,
				_ => null,
			};
			if (parse is { } plan)
			{
				return new ConvertBinding(from, to, plan, null, null);
			}
		}
		if (to.ClrType == typeof(string))
		{
			return new ConvertBinding(from, to, ConvertPlan.ToText, ConversionKind.ToText, null);
		}
		if (TypeRules.GetConversion(from, to) is { } link)
		{
			return new ConvertBinding(from, to, ConvertPlan.Link, link, null);
		}
		if (from.Kind == TypeKind.Entity && to.IsScriptClass)
		{
			return new ConvertBinding(from, to, ConvertPlan.ScriptInstance, null, null);
		}
		MethodInfo? method = FindUserConversion(from, to);
		return method != null ? new ConvertBinding(from, to, ConvertPlan.Method, null, method) : null;
	}

	// op_Implicit or op_Explicit declared on either type, from exactly the one type to exactly the other.
	private static MethodInfo? FindUserConversion(VisualType from, VisualType to)
	{
		foreach (Type declaring in new[] { from.ClrType!, to.ClrType! })
		{
			foreach (MethodInfo method in declaring.GetMethods(BindingFlags.Public | BindingFlags.Static | BindingFlags.DeclaredOnly))
			{
				if (method.Name is "op_Implicit" or "op_Explicit" && method.ReturnType == to.ClrType && method.GetParameters() is [{ } parameter] &&
					parameter.ParameterType == from.ClrType)
				{
					return method;
				}
			}
		}
		return null;
	}
}
