using System;
using System.Collections.Generic;

namespace Strada.VisualScripting;

internal enum DiagnosticSeverity
{
	Error,
	Warning,
}

// The stable diagnostic codes (spec section 6.2), written by their names. ImpureInPureFunction and NotInFunction are the
// VM's additions: an impure node in a pure function's graph, and Function.Entry or Function.Return in the event graph.
// DisabledIgnored (a Warning): a node whose Enabled is "Disabled" but which cannot be disabled (Function.Entry,
// Function.Return and pure nodes), so it runs. UnusedSplit (a Warning): an entry of a node's Split that splits nothing (it
// names no pin of its direction, a pin whose type cannot split, or a field pin of an entry that did not split).
internal enum DiagnosticCode
{
	InvalidDocument,
	UnknownNodeType,
	AmbiguousNode,
	NodeUpdated,
	UnknownPin,
	UnusedInput,
	MultipleExecLinks,
	MultipleDataLinks,
	TypeMismatch,
	UnresolvedWildcard,
	NoOperator,
	NoConversion,
	InvalidValue,
	EntityValue,
	ZeroQuaternion,
	InvalidSetting,
	MissingSetting,
	UnknownSetting,
	MissingTypeArgument,
	InvalidCase,
	DuplicateEvent,
	EventInFunction,
	EventInputLinked,
	InvalidName,
	ReservedName,
	UnknownType,
	InvalidType,
	NotExposable,
	UnknownVariable,
	UnknownFunction,
	MissingEntry,
	DuplicateEntry,
	MissingReturn,
	DuplicateReturn,
	LatentInPureContext,
	TickInFunction,
	PureCycle,
	PureRecursion,
	ImpureInPureFunction,
	NotInFunction,
	DisabledIgnored,
	UnusedSplit,
}

// A finding of the analysis. Function is 0 for the event graph and document variables, else the function's ID; the other
// locations are optional: a node (with a pin and its direction), a link, or a variable or local. Its severity is its code's,
// except on a disabled node, where every finding is a Warning (Demote).
internal sealed class Diagnostic
{
	internal Diagnostic(DiagnosticCode code, uint function, string message)
	{
		Code = code;
		Function = function;
		Message = message;
		Severity = GetSeverity(code);
	}

	internal DiagnosticSeverity Severity { get; private set; }

	internal DiagnosticCode Code { get; }

	internal uint Function { get; }

	internal uint? Node { get; init; }

	internal string? Pin { get; init; }

	internal PinDirection? Direction { get; init; }

	internal DocumentLink? Link { get; init; }

	internal uint? Variable { get; init; }

	internal string Message { get; }

	// Candidates for unknown names, nearest first; every candidate of an ambiguous node.
	internal IReadOnlyList<string> Suggestions { get; init; } = [];

	// The severity of a code: an Error unless it is one of the Warning codes.
	internal static DiagnosticSeverity GetSeverity(DiagnosticCode code) => code switch
	{
		DiagnosticCode.NodeUpdated or DiagnosticCode.UnusedInput or DiagnosticCode.ZeroQuaternion or DiagnosticCode.UnknownSetting or
			DiagnosticCode.DisabledIgnored or DiagnosticCode.UnusedSplit => DiagnosticSeverity.Warning,
		_ => DiagnosticSeverity.Error,
	};

	// The finding is located on a disabled node (its node, or an end of its link): it is a Warning whatever its code, because
	// a disabled node never makes a program broken.
	internal void Demote()
	{
		Severity = DiagnosticSeverity.Warning;
	}

	public override string ToString() => FormattableString.Invariant($"{Severity} {Code} (function {Function}, node {Node}): {Message}");
}
