using System;
using System.Collections.Generic;

namespace Strada.VisualScripting;

// What a wildcard pin accepts (spec section 3.5). None: the pin is not a wildcard.
internal enum PinConstraint
{
	None,
	Any,
	Numeric,
	Reference,
	Array,
	// The operands of formulas (spec-slice3 section 3.3.5): bool, the numeric primitives and Strada.Vector2, Vector3, Vector4.
	Expression,
}

// How a link turns the value of its output into a value of its input's type (spec section 3.4).
internal enum ConversionKind
{
	// Rule 1: the same type.
	Identity,
	// Rule 4: numeric widening.
	Widen,
	// Rule 5: a script class to a base class, a typed asset to Strada.Asset, a reference to System.Object; for links into
	// wildcard pins, any reference to a base class of it (a join, spec section 3.5).
	Upcast,
	// Rule 5: a value type to System.Object.
	Box,
	// Rule 5: a component to its entity.
	ComponentToEntity,
	// Rule 6: a typed asset to its handle (null gives AssetHandle.Invalid).
	AssetToHandle,
	// Rule 7: invariant-culture text (null gives "None").
	ToText,
}

// The compatibility rules of the type system: conversions between types, the joins of wildcard type variables and the
// wildcard constraints.
internal static class TypeRules
{
	// Rule 4: whether a numeric type widens to another.
	internal static bool Widens(VisualType from, VisualType to)
	{
		Type? source = from.ClrType;
		Type? target = to.ClrType;
		if (source == typeof(int))
		{
			return target == typeof(long) || target == typeof(float) || target == typeof(double);
		}
		if (source == typeof(uint))
		{
			return target == typeof(long) || target == typeof(ulong) || target == typeof(float) || target == typeof(double);
		}
		if (source == typeof(long) || source == typeof(ulong))
		{
			return target == typeof(float) || target == typeof(double);
		}
		return source == typeof(float) && target == typeof(double);
	}

	internal static bool WidensOrEquals(VisualType from, VisualType to) => ReferenceEquals(from, to) || Widens(from, to);

	// The conversion of a link from an output of one type to an input of another (rules 1, 2 and 4 to 7, in order); null
	// when the types are incompatible. Exec connects only to exec; an unresolved wildcard converts to nothing.
	internal static ConversionKind? GetConversion(VisualType from, VisualType to)
	{
		if (ReferenceEquals(from, to))
		{
			return from.IsWildcard ? null : ConversionKind.Identity;
		}
		if (!from.HasValues || !to.HasValues)
		{
			return null;
		}
		if (Contains(from.ImplicitTo, to))
		{
			return Classify(from, to);
		}
		return to.ClrType == typeof(string) ? ConversionKind.ToText : null;
	}

	// The conversion of a link into a wildcard pin whose type variable resolved to a join (spec section 3.5 rule 3): the
	// linked type itself, a numeric type the join widened, or a reference type the join is a base class of. Rule 7 never
	// applies to a join.
	internal static ConversionKind? GetJoinConversion(VisualType from, VisualType to)
	{
		if (ReferenceEquals(from, to))
		{
			return from.IsWildcard ? null : ConversionKind.Identity;
		}
		if (!from.HasValues || !to.HasValues)
		{
			return null;
		}
		if (Widens(from, to))
		{
			return ConversionKind.Widen;
		}
		return from.IsClrReference && to.IsClrReference && to.ClrType!.IsAssignableFrom(from.ClrType) ? ConversionKind.Upcast : null;
	}

	// The type a type variable takes from two of its linked types (spec section 3.5 rule 3). Equal types give that type.
	// Numeric types give the narrowest type both widen to; two integer types join only to an integer type, so int with
	// ulong is an error, as in C#. Reference types give their nearest common base class; System.Object only when one of them
	// is System.Object (unrelated classes do not join). Null when the types do not join.
	internal static VisualType? Join(VisualType a, VisualType b)
	{
		if (ReferenceEquals(a, b))
		{
			return a.IsWildcard ? null : a;
		}
		if (!a.HasValues || !b.HasValues)
		{
			return null;
		}
		if (a.IsNumeric && b.IsNumeric)
		{
			return JoinNumbers(a, b);
		}
		if (!a.IsClrReference || !b.IsClrReference)
		{
			return null;
		}
		for (VisualType? candidate = a; candidate != null; candidate = candidate.Base)
		{
			if (IsSelfOrBase(candidate, b))
			{
				return candidate;
			}
		}
		if (a.ClrType == typeof(object))
		{
			return a;
		}
		return b.ClrType == typeof(object) ? b : null;
	}

	// Whether a type satisfies a wildcard constraint (exec and the wildcard satisfy none).
	internal static bool Satisfies(VisualType type, PinConstraint constraint) => constraint switch
	{
		PinConstraint.Numeric => type.IsNumeric,
		PinConstraint.Reference => type.IsReference,
		PinConstraint.Array => type.HasValues && type.Kind == TypeKind.Array,
		PinConstraint.Expression => type.HasValues && (type.Kind is TypeKind.Bool or TypeKind.Integer or TypeKind.Float ||
			type.ClrType == typeof(Vector2) || type.ClrType == typeof(Vector3) || type.ClrType == typeof(Vector4)),
		_ => type.HasValues,
	};

	internal static string Describe(PinConstraint constraint) => constraint switch
	{
		PinConstraint.Numeric => "a number",
		PinConstraint.Reference => "a reference (an entity, component, asset, object or array)",
		PinConstraint.Array => "an array",
		PinConstraint.Expression => "a bool, a number or a vector (Vector2, Vector3, Vector4)",
		_ => "a value",
	};

	private static ConversionKind Classify(VisualType from, VisualType to)
	{
		if (Widens(from, to))
		{
			return ConversionKind.Widen;
		}
		if (to.ClrType == typeof(AssetHandle))
		{
			return ConversionKind.AssetToHandle;
		}
		if (from.Kind == TypeKind.Component && to.ClrType == typeof(Entity))
		{
			return ConversionKind.ComponentToEntity;
		}
		return to.ClrType == typeof(object) && from.ClrType!.IsValueType ? ConversionKind.Box : ConversionKind.Upcast;
	}

	private static VisualType? JoinNumbers(VisualType a, VisualType b)
	{
		bool integers = a.Kind == TypeKind.Integer && b.Kind == TypeKind.Integer;
		VisualType? best = null;
		foreach (VisualType candidate in Candidates(a))
		{
			if ((integers && candidate.Kind != TypeKind.Integer) || !WidensOrEquals(b, candidate))
			{
				continue;
			}
			if (best == null || Rank(candidate) < Rank(best))
			{
				best = candidate;
			}
		}
		return best;
	}

	// A numeric type and the types it widens to.
	private static IEnumerable<VisualType> Candidates(VisualType type)
	{
		yield return type;
		foreach (VisualType target in type.ImplicitTo)
		{
			if (Widens(type, target))
			{
				yield return target;
			}
		}
	}

	// Narrower numeric types first.
	private static int Rank(VisualType type)
	{
		Type? clrType = type.ClrType;
		if (clrType == typeof(int))
		{
			return 0;
		}
		if (clrType == typeof(uint))
		{
			return 1;
		}
		if (clrType == typeof(long))
		{
			return 2;
		}
		if (clrType == typeof(ulong))
		{
			return 3;
		}
		return clrType == typeof(float) ? 4 : 5;
	}

	private static bool IsSelfOrBase(VisualType candidate, VisualType type)
	{
		for (VisualType? current = type; current != null; current = current.Base)
		{
			if (ReferenceEquals(current, candidate))
			{
				return true;
			}
		}
		return false;
	}

	private static bool Contains(IReadOnlyList<VisualType> types, VisualType type)
	{
		foreach (VisualType candidate in types)
		{
			if (ReferenceEquals(candidate, type))
			{
				return true;
			}
		}
		return false;
	}
}
