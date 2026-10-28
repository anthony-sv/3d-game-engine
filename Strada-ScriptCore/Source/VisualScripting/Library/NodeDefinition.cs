using System;
using System.Collections.Generic;
using System.Reflection;

namespace Strada.VisualScripting;

// The kinds of nodes (the library's Nodes[].Kind, spec section 6.1); Unknown only in analysis results.
internal enum NodeKind
{
	Event,
	Flow,
	Call,
	Pure,
	Latent,
	Variable,
	Function,
	Operator,
	Data,
	Reroute,
	Unknown,
}

// Every built-in node kind of spec sections 4.1 to 4.4 and slice 3's (spec-slice3 section 3.1). Values are never persisted
// (documents store type strings), so kinds are placed where the range predicates of BuiltinNodes stay true.
internal enum BuiltinNode
{
	EventCreate,
	EventUpdate,
	EventFixedUpdate,
	EventDestroy,
	EventCollisionEnter,
	EventCollisionExit,
	EventTriggerEnter,
	EventTriggerExit,
	EventClick,
	EventKeyPressed,
	EventKeyReleased,
	EventMouseButtonPressed,
	EventMouseButtonReleased,
	EventGamepadButtonPressed,
	EventGamepadButtonReleased,
	EventCustom,
	EventSend,
	FlowBranch,
	FlowSequence,
	FlowSwitch,
	FlowForLoop,
	FlowForEach,
	FlowWhile,
	FlowDoOnce,
	FlowDoN,
	FlowFlipFlop,
	FlowGate,
	FlowDelay,
	FlowRetriggerableDelay,
	FlowWaitFrames,
	FlowWaitUntil,
	FlowTween,
	FlowInterval,
	VariableGet,
	VariableSet,
	FunctionEntry,
	FunctionReturn,
	FunctionCall,
	Self,
	Literal,
	Reroute,
	Select,
	IsValid,
	Convert,
	Make,
	Break,
	FormatString,
	ArrayLength,
	ArrayGet,
	// Slice 3's pure kinds come before the operators, so IsOperator (>= OperatorAdd) stays a range.
	Expression,
	StringLength,
	StringIsEmpty,
	StringContains,
	StringStartsWith,
	StringEndsWith,
	StringIndexOf,
	StringSubstring,
	StringReplace,
	StringToUpper,
	StringToLower,
	StringTrim,
	StringSplit,
	StringJoin,
	StringToInt,
	StringToFloat,
	ArrayContains,
	ArrayIndexOf,
	ArrayMake,
	ArraySet,
	ArrayAdd,
	ArrayRemoveAt,
	ArrayInsert,
	ArrayAppend,
	OperatorAdd,
	OperatorSubtract,
	OperatorMultiply,
	OperatorDivide,
	OperatorModulo,
	OperatorNegate,
	OperatorEqual,
	OperatorNotEqual,
	OperatorLess,
	OperatorLessOrEqual,
	OperatorGreater,
	OperatorGreaterOrEqual,
	OperatorAnd,
	OperatorOr,
	OperatorXor,
	OperatorNot,
}

// A node kind of the library: a built-in kind or a reflected member. Pins are those of the default settings; analysis
// shapes the pins of each node of a document from its settings.
internal abstract class NodeDefinition
{
	private protected NodeDefinition(string type, string title, string category, NodeKind kind, bool pure, bool latent,
		IReadOnlyList<PinDescription> pins)
	{
		Type = type;
		Title = title;
		Category = category;
		Kind = kind;
		Pure = pure;
		Latent = latent;
		Pins = pins;
	}

	// The node type string documents store.
	internal string Type { get; }

	internal string Title { get; }

	internal string Subtitle { get; init; } = string.Empty;

	internal string Category { get; }

	internal string Description { get; init; } = string.Empty;

	internal IReadOnlyList<string> Keywords { get; init; } = [];

	internal NodeKind Kind { get; }

	internal bool Pure { get; }

	internal bool Latent { get; }

	internal IReadOnlyList<PinDescription> Pins { get; }

	internal IReadOnlyList<SettingDescription> Settings { get; init; } = [];

	// A node of a member of the game assembly that [VisualNode] marks (spec-slice3 section 3.7).
	internal bool Game { get; init; }

	internal SettingDescription? FindSetting(string name)
	{
		foreach (SettingDescription setting in Settings)
		{
			if (setting.Name == name)
			{
				return setting;
			}
		}
		return null;
	}

	public override string ToString() => Type;
}

internal sealed class BuiltinNodeDefinition : NodeDefinition
{
	internal BuiltinNodeDefinition(BuiltinNode node, string type, string title, string category, NodeKind kind, bool pure, bool latent,
		IReadOnlyList<PinDescription> pins)
		: base(type, title, category, kind, pure, latent, pins)
	{
		Node = node;
	}

	internal BuiltinNode Node { get; }
}

// The member kinds of reflected nodes and their type-string prefixes (spec section 4.5).
internal enum MemberNodeKind
{
	// "Call:": a method.
	Call,
	// "Get:": a property getter or a field read.
	Get,
	// "Set:": a property setter or a field write.
	Set,
	// "New:": a constructor.
	New,
}

// How an instance member reaches the object it runs on.
internal enum TargetKind
{
	// A static member: no Target.
	None,
	// A member of Strada.Entity: the Target entity, Self when unconnected.
	Entity,
	// A member of a component class: the Target entity's component of the declaring type, fetched at call time; the Target
	// entity is Self when unconnected.
	Component,
	// A member of any other type: the Target value of the declaring type (a struct by value; null aborts for a class).
	Value,
	// An instance member of a script class of the game (spec-slice3 section 3.7.3): the Target entity's script instance,
	// fetched at call time, which must be of the declaring class; the Target entity is Self when unconnected.
	Script,
}

// A parameter of a reflected method or constructor and the pin it maps to: an input, or an output for out parameters.
internal sealed record MemberParameter(ParameterInfo Parameter, string Pin, PinDirection Direction);

// Where an inferred generic method's type parameter comes from (spec-slice3 section 3.6): the element type of the input Pin
// when IsArray (the first input of type T[]), else the type of the input Pin (the first input of type T).
internal sealed record GenericAnchor(string Pin, bool IsArray);

// A node made from a public member of the scripting API.
internal sealed class MemberNodeDefinition : NodeDefinition
{
	internal MemberNodeDefinition(MemberNodeKind memberKind, MemberInfo member, string type, string title, string category, bool pure,
		IReadOnlyList<PinDescription> pins)
		: base(type, title, category, pure ? NodeKind.Pure : NodeKind.Call, pure, false, pins)
	{
		MemberKind = memberKind;
		Member = member;
	}

	internal MemberNodeKind MemberKind { get; }

	// The MethodInfo (a generic method definition for generic methods), PropertyInfo, FieldInfo or ConstructorInfo.
	internal MemberInfo Member { get; }

	internal Type DeclaringType => Member.DeclaringType!;

	internal TargetKind Target { get; init; }

	// Methods and constructors: their parameters in order.
	internal IReadOnlyList<MemberParameter> Parameters { get; init; } = [];

	// Generic methods: the domain of their type parameters (Component, Asset or Script); None otherwise.
	internal TypeConstraint GenericDomain { get; init; }

	// Generic methods whose type parameters are all unconstrained: their types are inferred from links (spec-slice3 section
	// 3.6) unless TypeArguments names them.
	internal bool GenericInference { get; init; }

	// Inferred generic methods: per type parameter, the input it is inferred from; null for one no input decides (it appears
	// only in results), which makes TypeArguments required.
	internal IReadOnlyList<GenericAnchor?> GenericAnchors { get; init; } = [];

	internal bool IsGeneric => GenericDomain != TypeConstraint.None || GenericInference;

	// An inferred generic method that links cannot close: a type parameter appears in no input.
	internal bool RequiresTypeArguments
	{
		get
		{
			foreach (GenericAnchor? anchor in GenericAnchors)
			{
				if (anchor == null)
				{
					return true;
				}
			}
			return false;
		}
	}

	// The prefix, declaring type and member name ("Call:Strada.Input.IsKeyDown"; "New:Strada.Vector3"): overloads share it,
	// and stored types resolve by it when no exact match exists (spec section 4.5).
	internal string LookupKey { get; init; } = string.Empty;

	internal MethodInfo? Method => Member as MethodInfo;

	internal int GenericArity => Member is MethodInfo { IsGenericMethodDefinition: true } method ? method.GetGenericArguments().Length : 0;

	internal static string Prefix(MemberNodeKind kind) => kind switch
	{
		MemberNodeKind.Call => "Call:",
		MemberNodeKind.Get => "Get:",
		MemberNodeKind.Set => "Set:",
		_ => "New:",
	};

	internal static bool TryParsePrefix(string type, out MemberNodeKind kind)
	{
		foreach (MemberNodeKind candidate in Enum.GetValues<MemberNodeKind>())
		{
			if (type.StartsWith(Prefix(candidate), StringComparison.Ordinal))
			{
				kind = candidate;
				return true;
			}
		}
		kind = MemberNodeKind.Call;
		return false;
	}
}
