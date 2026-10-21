using System;
using System.Collections.Generic;
using System.Reflection;
using Strada.Interop;

namespace Strada.VisualScripting;

// The kinds of types (the library's Types[].Kind, spec section 3.1).
internal enum TypeKind
{
	Exec,
	Bool,
	Integer,
	Float,
	String,
	Vector,
	Quaternion,
	Color,
	Matrix,
	Entity,
	Component,
	Asset,
	Enum,
	Struct,
	Class,
	Array,
	Object,
}

// An enumerator of an enum type; Value is boxed as the enum type.
internal sealed record EnumeratorInfo(string Name, object Value);

// A public field or get-only property of a struct, which Make sets and Break reads (spec section 4.4).
internal sealed record StructMember(string Name, VisualType Type, MemberInfo Member);

// A type of the visual-scripting type system (spec section 3): exec, the wildcard "*" of templates and unresolved pins, or a
// CLR type named by its type ID. The library builder fills in the derived facts while it builds the library; afterwards a
// type never changes and is shared by every runtime that uses the library.
internal sealed class VisualType
{
	internal VisualType(string id, string title, TypeKind kind, Type? clrType)
	{
		Id = id;
		Title = title;
		Kind = kind;
		ClrType = clrType;
	}

	// Exec pins (control flow).
	internal static VisualType Exec { get; } = new(TypeIds.Exec, "Exec", TypeKind.Exec, null);

	// A wildcard pin whose type is not known (yet): templates, and pins analysis could not resolve. It has no values, so it
	// satisfies no constraint and converts to nothing; its Kind means nothing.
	internal static VisualType Wildcard { get; } = new(TypeIds.Wildcard, "Wildcard", TypeKind.Object, null);

	internal string Id { get; }

	internal string Title { get; }

	internal TypeKind Kind { get; }

	// Null for exec and the wildcard.
	internal Type? ClrType { get; }

	internal bool IsExec => ReferenceEquals(this, Exec);

	internal bool IsWildcard => ReferenceEquals(this, Wildcard);

	// A type with values: neither exec nor the wildcard.
	internal bool HasValues => ClrType != null;

	// The numeric primitives (int, uint, long, ulong, float, double).
	internal bool IsNumeric => HasValues && Kind is TypeKind.Integer or TypeKind.Float;

	// The kinds of the Reference constraint (spec section 3.5).
	internal bool IsReference => HasValues && Kind is TypeKind.Entity or TypeKind.Component or TypeKind.Asset or TypeKind.Class or
		TypeKind.Array or TypeKind.Object;

	// Whether values of the type are CLR references (strings included).
	internal bool IsClrReference => ClrType is { IsValueType: false };

	// A concrete asset class (Mesh, Material, ...), as opposed to Strada.Asset itself.
	internal bool IsTypedAsset => Kind == TypeKind.Asset && ClrType != typeof(Asset);

	// Strada.Script and the game's script classes (Kind Entity, but not Strada.Entity itself).
	internal bool IsScriptClass => Kind == TypeKind.Entity && ClrType != typeof(Entity);

	// The script field type of exposable types (spec section 3.3); None otherwise.
	internal ScriptFieldType FieldType { get; set; }

	// The engine asset type of concrete asset classes; None otherwise.
	internal NativeAssetType AssetType { get; set; }

	// Whether documents can hold values of the type (spec section 2.1).
	internal bool HasLiteral { get; set; }

	// Enum types: the enumerators in declaration order.
	internal IReadOnlyList<EnumeratorInfo> Enumerators { get; set; } = [];

	internal bool IsFlags { get; set; }

	// Structs whose public instance fields are all writable (spec section 4.4).
	internal bool Makeable { get; set; }

	// Make types and readonly structs that break into their get-only properties.
	internal bool Breakable { get; set; }

	// The members Make sets and Break reads, in declaration order; empty unless the type is makeable or breakable.
	internal IReadOnlyList<StructMember> Fields { get; set; } = [];

	// Classes: the nearest base class that is a library type, other than System.Object.
	internal VisualType? Base { get; set; }

	// Arrays: the element type.
	internal VisualType? Element { get; set; }

	// The targets of spec section 3.4 rules 4, 5 and 6: the widened types (narrowest first), the base classes up to
	// Strada.Entity or Strada.Asset (nearest first) or a component's entity, Strada.AssetHandle, and System.Object last.
	// Rule 7 (to string) is implied.
	internal IReadOnlyList<VisualType> ImplicitTo { get; set; } = [];

	public override string ToString() => Id;
}
