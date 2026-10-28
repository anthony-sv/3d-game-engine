using System.Collections.Generic;

namespace Strada.VisualScripting;

// The kinds of node settings (the library's Settings[].Kind, spec section 6.1).
internal enum SettingKind
{
	Bool,
	Int,
	String,
	// A type ID, restricted by a TypeConstraint.
	Type,
	// TypeArguments: one type ID per type parameter of a generic method.
	TypeList,
	// A variable or local ID.
	Variable,
	// A function ID.
	Function,
	// [{ "Name", "Type" }].
	Parameters,
	// A switch's case values.
	Cases,
	// A literal of the type the sibling Type setting names.
	Value,
	// An enumerator of the enum type named by Type.
	Enum,
	// One of Choices.
	Choice,
	// A formula (spec-slice3 section 3.3): a string the node's pins follow.
	Expression,
	// A timeline's length, loop and tracks (spec-slice3 section 3.8.2): an object the node's pins follow.
	Timeline,
}

// Which types a Type or TypeList setting accepts.
internal enum TypeConstraint
{
	None,
	// Any type with values.
	Any,
	// A type with a literal form (spec section 2.1).
	Literal,
	Exposable,
	Struct,
	Makeable,
	Breakable,
	Enum,
	// int, string or an enum.
	Switch,
	// The registered component classes.
	Component,
	// The concrete asset classes.
	Asset,
	// The game's script classes.
	Script,
}

// A setting of a node kind. Default is null, a bool, an int, a string or an empty list (written as []).
internal sealed class SettingDescription
{
	internal SettingDescription(string name, SettingKind kind)
	{
		Name = name;
		Kind = kind;
	}

	internal string Name { get; }

	internal SettingKind Kind { get; }

	internal object? Default { get; init; }

	internal bool Required { get; init; }

	internal int? Min { get; init; }

	internal int? Max { get; init; }

	internal TypeConstraint Constraint { get; init; }

	// Enum settings: the enum type's ID.
	internal string? Type { get; init; }

	// Choice settings.
	internal IReadOnlyList<string> Choices { get; init; } = [];
}
