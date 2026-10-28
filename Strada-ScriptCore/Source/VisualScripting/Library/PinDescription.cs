namespace Strada.VisualScripting;

internal enum PinDirection
{
	In,
	Out,
}

// The role of an exec output of a latent node (spec section 4.0).
internal enum PinRole
{
	None,
	// Resumes the suspended execution once.
	Continuation,
	// Starts a new execution each time it fires.
	Tick,
}

// How a wildcard pin ("*") of a node gets its type (spec section 3.5).
internal enum WildcardSource
{
	// Not a wildcard; or a pin whose type a setting decides, left unresolved because that setting is missing or invalid (the
	// setting's diagnostic explains it, so the pin raises none of its own).
	None,
	// From the types of the outputs linked into the pins of its type variable (its Group, or the pin alone).
	Links,
	// The element type of the array pin named by ElementOf.
	ElementOf,
	// The array type of the elements of the pin named by ArrayOf (spec-slice3 sections 3.4.5 and 3.6).
	ArrayOf,
	// Computed by the node from its other pins (an operator's Result).
	Computed,
}

// A pin of a library node, or of a node of a document as analysis shapes it from its settings.
internal sealed class PinDescription
{
	internal PinDescription(string name, PinDirection direction, VisualType type)
	{
		Name = name;
		Direction = direction;
		Type = type;
	}

	internal string Name { get; }

	internal PinDirection Direction { get; }

	internal VisualType Type { get; }

	// A display name, when it differs from Name (switch cases).
	internal string? Label { get; init; }

	// Whether an unconnected input without a stored value takes Default (a value of Type) rather than the type default.
	internal bool HasDefault { get; init; }

	internal object? Default { get; init; }

	// An unconnected Target input means the entity running the graph; it takes no stored value.
	internal bool DefaultsToSelf { get; init; }

	internal PinRole Role { get; init; }

	internal PinConstraint Constraint { get; init; }

	// Wildcards of one node with the same Group share one type variable.
	internal string? Group { get; init; }

	// The pin's type is the element type of the named array pin.
	internal string? ElementOf { get; init; }

	// The pin's type is the array type whose elements have the type of the named pin.
	internal string? ArrayOf { get; init; }

	internal WildcardSource Source { get; init; }

	internal bool IsExec => Type.IsExec;

	internal bool IsInput => Direction == PinDirection.In;
}
