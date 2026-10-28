using System;
using System.Collections.Generic;

namespace Strada.VisualScripting;

// The easing curves of Flow.Tween (spec section 4.2), by their setting names.
internal enum Easing
{
	Linear,
	EaseIn,
	EaseOut,
	EaseInOut,
	EaseInCubic,
	EaseOutCubic,
	EaseInOutCubic,
	EaseOutBack,
	EaseOutBounce,
	EaseOutElastic,
}

// The built-in node kinds (spec sections 4.1 to 4.4) as library definitions: the pins of their default settings, their
// settings, titles, categories, descriptions and search keywords.
internal static class BuiltinNodes
{
	internal const string Events = "Events";
	internal const string Flow = "Flow";
	internal const string Variables = "Variables";
	internal const string Functions = "Functions";
	internal const string Operators = "Math/Operators";
	internal const string Utilities = "Utilities";
	internal const string Arrays = "Utilities/Array";
	internal const string Strings = "Utilities/String";
	internal const string Mathematics = "Math";
	internal const string Conversions = "Conversions";
	internal const string Structs = "Structs";

	// The group of the type variable Reroute, Select and Make Array share among their pins.
	internal const string ValueGroup = "T";

	// The group of the array pins of the array nodes that make a new array of their input's type.
	internal const string ArrayGroup = "A";

	internal const string ArraySubtitle = "Array";
	internal const string StringSubtitle = "String";

	// Array.Make's Count (spec-slice3 section 3.2).
	internal const int MaxMakeArrayCount = 64;

	internal static readonly string[] EasingNames =
	[
		nameof(Easing.Linear), nameof(Easing.EaseIn), nameof(Easing.EaseOut), nameof(Easing.EaseInOut), nameof(Easing.EaseInCubic),
		nameof(Easing.EaseOutCubic), nameof(Easing.EaseInOutCubic), nameof(Easing.EaseOutBack), nameof(Easing.EaseOutBounce),
		nameof(Easing.EaseOutElastic),
	];

	// The type strings of the built-in kinds.
	internal static string TypeOf(BuiltinNode node) => node switch
	{
		BuiltinNode.EventCreate => "Event.Create",
		BuiltinNode.EventUpdate => "Event.Update",
		BuiltinNode.EventFixedUpdate => "Event.FixedUpdate",
		BuiltinNode.EventDestroy => "Event.Destroy",
		BuiltinNode.EventCollisionEnter => "Event.CollisionEnter",
		BuiltinNode.EventCollisionExit => "Event.CollisionExit",
		BuiltinNode.EventTriggerEnter => "Event.TriggerEnter",
		BuiltinNode.EventTriggerExit => "Event.TriggerExit",
		BuiltinNode.EventClick => "Event.Click",
		BuiltinNode.EventKeyPressed => "Event.KeyPressed",
		BuiltinNode.EventKeyReleased => "Event.KeyReleased",
		BuiltinNode.EventMouseButtonPressed => "Event.MouseButtonPressed",
		BuiltinNode.EventMouseButtonReleased => "Event.MouseButtonReleased",
		BuiltinNode.EventGamepadButtonPressed => "Event.GamepadButtonPressed",
		BuiltinNode.EventGamepadButtonReleased => "Event.GamepadButtonReleased",
		BuiltinNode.EventCustom => "Event.Custom",
		BuiltinNode.EventSend => "Event.Send",
		BuiltinNode.FlowBranch => "Flow.Branch",
		BuiltinNode.FlowSequence => "Flow.Sequence",
		BuiltinNode.FlowSwitch => "Flow.Switch",
		BuiltinNode.FlowForLoop => "Flow.ForLoop",
		BuiltinNode.FlowForEach => "Flow.ForEach",
		BuiltinNode.FlowWhile => "Flow.While",
		BuiltinNode.FlowDoOnce => "Flow.DoOnce",
		BuiltinNode.FlowDoN => "Flow.DoN",
		BuiltinNode.FlowFlipFlop => "Flow.FlipFlop",
		BuiltinNode.FlowGate => "Flow.Gate",
		BuiltinNode.FlowDelay => "Flow.Delay",
		BuiltinNode.FlowRetriggerableDelay => "Flow.RetriggerableDelay",
		BuiltinNode.FlowWaitFrames => "Flow.WaitFrames",
		BuiltinNode.FlowWaitUntil => "Flow.WaitUntil",
		BuiltinNode.FlowTween => "Flow.Tween",
		BuiltinNode.FlowInterval => "Flow.Interval",
		BuiltinNode.FlowTimeline => "Flow.Timeline",
		BuiltinNode.VariableGet => "Variable.Get",
		BuiltinNode.VariableSet => "Variable.Set",
		BuiltinNode.FunctionEntry => "Function.Entry",
		BuiltinNode.FunctionReturn => "Function.Return",
		BuiltinNode.FunctionCall => "Function.Call",
		BuiltinNode.Self => "Self",
		BuiltinNode.Literal => "Literal",
		BuiltinNode.Reroute => "Reroute",
		BuiltinNode.Select => "Select",
		BuiltinNode.IsValid => "IsValid",
		BuiltinNode.Convert => "Convert",
		BuiltinNode.Make => "Make",
		BuiltinNode.Break => "Break",
		BuiltinNode.FormatString => "FormatString",
		BuiltinNode.ArrayLength => "Array.Length",
		BuiltinNode.ArrayGet => "Array.Get",
		BuiltinNode.Expression => "Expression",
		BuiltinNode.StringLength => "String.Length",
		BuiltinNode.StringIsEmpty => "String.IsEmpty",
		BuiltinNode.StringContains => "String.Contains",
		BuiltinNode.StringStartsWith => "String.StartsWith",
		BuiltinNode.StringEndsWith => "String.EndsWith",
		BuiltinNode.StringIndexOf => "String.IndexOf",
		BuiltinNode.StringSubstring => "String.Substring",
		BuiltinNode.StringReplace => "String.Replace",
		BuiltinNode.StringToUpper => "String.ToUpper",
		BuiltinNode.StringToLower => "String.ToLower",
		BuiltinNode.StringTrim => "String.Trim",
		BuiltinNode.StringSplit => "String.Split",
		BuiltinNode.StringJoin => "String.Join",
		BuiltinNode.StringToInt => "String.ToInt",
		BuiltinNode.StringToFloat => "String.ToFloat",
		BuiltinNode.ArrayContains => "Array.Contains",
		BuiltinNode.ArrayIndexOf => "Array.IndexOf",
		BuiltinNode.ArrayMake => "Array.Make",
		BuiltinNode.ArraySet => "Array.Set",
		BuiltinNode.ArrayAdd => "Array.Add",
		BuiltinNode.ArrayRemoveAt => "Array.RemoveAt",
		BuiltinNode.ArrayInsert => "Array.Insert",
		BuiltinNode.ArrayAppend => "Array.Append",
		BuiltinNode.OperatorAdd => "Operator.Add",
		BuiltinNode.OperatorSubtract => "Operator.Subtract",
		BuiltinNode.OperatorMultiply => "Operator.Multiply",
		BuiltinNode.OperatorDivide => "Operator.Divide",
		BuiltinNode.OperatorModulo => "Operator.Modulo",
		BuiltinNode.OperatorNegate => "Operator.Negate",
		BuiltinNode.OperatorEqual => "Operator.Equal",
		BuiltinNode.OperatorNotEqual => "Operator.NotEqual",
		BuiltinNode.OperatorLess => "Operator.Less",
		BuiltinNode.OperatorLessOrEqual => "Operator.LessOrEqual",
		BuiltinNode.OperatorGreater => "Operator.Greater",
		BuiltinNode.OperatorGreaterOrEqual => "Operator.GreaterOrEqual",
		BuiltinNode.OperatorAnd => "Operator.And",
		BuiltinNode.OperatorOr => "Operator.Or",
		BuiltinNode.OperatorXor => "Operator.Xor",
		BuiltinNode.OperatorNot => "Operator.Not",
		_ => throw new ArgumentOutOfRangeException(nameof(node), node, "a built-in node kind without a type string"),
	};

	internal static bool IsEvent(BuiltinNode node) => node <= BuiltinNode.EventCustom;

	internal static bool IsInputEvent(BuiltinNode node) => node is >= BuiltinNode.EventKeyPressed and <= BuiltinNode.EventGamepadButtonReleased;

	// The latent built-in kinds (spec section 4.0); Function.Call is latent when its function is.
	internal static bool IsLatent(BuiltinNode node) => node is >= BuiltinNode.FlowDelay and <= BuiltinNode.FlowTimeline;

	// The latent kinds whose outputs start executions of their own (Tick roles).
	internal static bool HasTicks(BuiltinNode node) => node is BuiltinNode.FlowTween or BuiltinNode.FlowInterval or BuiltinNode.FlowTimeline;

	internal static bool IsOperator(BuiltinNode node) => node >= BuiltinNode.OperatorAdd;

	// The binary operators whose operands are wildcards (the rider of spec section 3.5 rule 4 applies to them).
	internal static bool IsBinaryWildcardOperator(BuiltinNode node) =>
		node is >= BuiltinNode.OperatorAdd and <= BuiltinNode.OperatorGreaterOrEqual and not BuiltinNode.OperatorNegate;

	internal static List<BuiltinNodeDefinition> Create(BuiltinTypes types)
	{
		List<BuiltinNodeDefinition> nodes = [];
		AddEvents(nodes, types);
		AddFlow(nodes, types);
		AddLatent(nodes, types);
		AddVariablesAndFunctions(nodes);
		AddData(nodes, types);
		AddExpression(nodes);
		AddStrings(nodes, types);
		AddArrays(nodes, types);
		AddOperators(nodes, types);
		return nodes;
	}

	private static void AddEvents(List<BuiltinNodeDefinition> nodes, BuiltinTypes types)
	{
		Event(nodes, BuiltinNode.EventCreate, "Create",
			"Runs once when the scene starts, or when the entity starts running the graph during play, after every new instance exists.",
			["start", "begin", "init"], [ExecOut()]);
		Event(nodes, BuiltinNode.EventUpdate, "Update", "Runs every frame with the seconds since the previous frame (scaled by the time scale).",
			["tick", "frame", "every frame"], [ExecOut(), Out("DeltaTime", types.Float)]);
		Event(nodes, BuiltinNode.EventFixedUpdate, "Fixed Update", "Runs before every physics step with the seconds per step.",
			["physics", "step", "fixed"], [ExecOut(), Out("FixedDeltaTime", types.Float)]);
		Event(nodes, BuiltinNode.EventDestroy, "Destroy", "Runs when the entity is destroyed or the scene stops.", ["end", "stop", "remove"],
			[ExecOut()]);
		Event(nodes, BuiltinNode.EventCollisionEnter, "Collision Enter",
			"Runs when a collider of the entity starts touching a collider of Other.", ["contact", "hit", "touch", "physics"],
			[ExecOut(), Out("Other", types.Entity)]);
		Event(nodes, BuiltinNode.EventCollisionExit, "Collision Exit", "Runs when the colliders of the entity stop touching those of Other.",
			["contact", "touch", "physics"], [ExecOut(), Out("Other", types.Entity)]);
		Event(nodes, BuiltinNode.EventTriggerEnter, "Trigger Enter",
			"Runs when a trigger collider of either entity starts overlapping a collider of the other.", ["overlap", "sensor", "physics"],
			[ExecOut(), Out("Other", types.Entity)]);
		Event(nodes, BuiltinNode.EventTriggerExit, "Trigger Exit", "Runs when the overlap between a trigger collider and Other ends.",
			["overlap", "sensor", "physics"], [ExecOut(), Out("Other", types.Entity)]);
		Event(nodes, BuiltinNode.EventClick, "Click",
			"Runs when a UI button of the entity or of one of its children is clicked, before the frame's Update; Button is the clicked button.",
			["ui", "button", "press"], [ExecOut(), Out("Button", types.Entity)]);
		InputEvent(nodes, BuiltinNode.EventKeyPressed, "Key Pressed", "Runs in the frame the key goes down.", "Key", types.KeyCode, false);
		InputEvent(nodes, BuiltinNode.EventKeyReleased, "Key Released", "Runs in the frame the key goes up.", "Key", types.KeyCode, false);
		InputEvent(nodes, BuiltinNode.EventMouseButtonPressed, "Mouse Button Pressed", "Runs in the frame the mouse button goes down.", "Button",
			types.MouseButton, false);
		InputEvent(nodes, BuiltinNode.EventMouseButtonReleased, "Mouse Button Released", "Runs in the frame the mouse button goes up.",
			"Button", types.MouseButton, false);
		InputEvent(nodes, BuiltinNode.EventGamepadButtonPressed, "Gamepad Button Pressed",
			"Runs in the frame the button of the gamepad (0 to 15) goes down.", "Button", types.GamepadButton, true, types.Int);
		InputEvent(nodes, BuiltinNode.EventGamepadButtonReleased, "Gamepad Button Released",
			"Runs in the frame the button of the gamepad (0 to 15) goes up.", "Button", types.GamepadButton, true, types.Int);
		Event(nodes, BuiltinNode.EventCustom, "Custom Event",
			"Runs when Event.Send or a C# script raises the event by its name; its outputs are the event's parameters.",
			["custom", "message", "receive"], [ExecOut()], CustomEventSettings());
		Add(nodes, new BuiltinNodeDefinition(BuiltinNode.EventSend, TypeOf(BuiltinNode.EventSend), "Send Event", Events, NodeKind.Call, false,
			false, [ExecIn(), Target(types.Entity), ExecOut()])
		{
			Description = "Raises the custom event of that name on the Target entity's graph (Self when unconnected) and continues when it ends or waits.",
			Keywords = ["send", "message", "raise", "event"],
			Settings = CustomEventSettings(),
		});
	}

	private static void AddFlow(List<BuiltinNodeDefinition> nodes, BuiltinTypes types)
	{
		FlowNode(nodes, BuiltinNode.FlowBranch, "Branch", "Continues at True or False depending on Condition.", ["if", "else", "condition"],
			[ExecIn(), In("Condition", types.Bool), ExecOut("True"), ExecOut("False")]);
		FlowNode(nodes, BuiltinNode.FlowSequence, "Sequence",
			"Runs its outputs one after another: each starts when the chain of the one before it has ended.", ["order", "then", "multiple"],
			[ExecIn(), ExecOut("Then0"), ExecOut("Then1")],
			[new SettingDescription("Count", SettingKind.Int) { Default = 2, Min = 2, Max = 64 }]);
		FlowNode(nodes, BuiltinNode.FlowSwitch, "Switch", "Continues at the case whose value equals Selection, else at Default.",
			["case", "select", "match"],
			[ExecIn(), Wildcard("Selection", PinDirection.In, PinConstraint.Any, WildcardSource.None), ExecOut("Default")],
			[
				new SettingDescription("Type", SettingKind.Type) { Required = true, Constraint = TypeConstraint.Switch },
				new SettingDescription("Cases", SettingKind.Cases),
			]);
		FlowNode(nodes, BuiltinNode.FlowForLoop, "For Loop", "Runs Body for each index from First to Last inclusive, then Completed.",
			["for", "loop", "repeat", "count"],
			[ExecIn(), In("First", types.Int, 0), In("Last", types.Int, 0), ExecIn("Break"), ExecOut("Body"), Out("Index", types.Int),
				ExecOut("Completed")]);
		FlowNode(nodes, BuiltinNode.FlowForEach, "For Each", "Runs Body for each element of a snapshot of Array, then Completed.",
			["each", "loop", "array", "iterate"],
			[
				ExecIn(), Wildcard("Array", PinDirection.In, PinConstraint.Array, WildcardSource.Links), ExecIn("Break"), ExecOut("Body"),
				Wildcard("Element", PinDirection.Out, PinConstraint.Any, WildcardSource.ElementOf, elementOf: "Array"), Out("Index", types.Int),
				ExecOut("Completed"),
			]);
		FlowNode(nodes, BuiltinNode.FlowWhile, "While", "Runs Body as long as Condition is true, checking it before every iteration.",
			["loop", "repeat", "until"], [ExecIn(), In("Condition", types.Bool), ExecOut("Body"), ExecOut("Completed")]);
		FlowNode(nodes, BuiltinNode.FlowDoOnce, "Do Once", "Continues at Completed the first time, and again only after Reset.",
			["once", "first", "single"], [ExecIn(), ExecIn("Reset"), ExecOut("Completed")],
			[new SettingDescription("StartClosed", SettingKind.Bool) { Default = false }]);
		FlowNode(nodes, BuiltinNode.FlowDoN, "Do N", "Continues at Exit the first N times, counting them in Counter, until Reset.",
			["count", "times", "limit"], [ExecIn(), In("N", types.Int, 1), ExecIn("Reset"), ExecOut("Exit"), Out("Counter", types.Int)]);
		FlowNode(nodes, BuiltinNode.FlowFlipFlop, "Flip Flop", "Alternates between A and B, starting with A.", ["toggle", "alternate"],
			[ExecIn(), ExecOut("A"), ExecOut("B"), Out("IsA", types.Bool)]);
		FlowNode(nodes, BuiltinNode.FlowGate, "Gate", "Continues from Enter at Exit while the gate is open; Open, Close and Toggle change it.",
			["open", "close", "block"], [ExecIn("Enter"), ExecIn("Open"), ExecIn("Close"), ExecIn("Toggle"), ExecOut("Exit")],
			[new SettingDescription("StartOpen", SettingKind.Bool) { Default = true }]);
	}

	private static void AddLatent(List<BuiltinNodeDefinition> nodes, BuiltinTypes types)
	{
		Latent(nodes, BuiltinNode.FlowDelay, "Delay", "Waits Duration seconds of scene time, then continues at Completed.",
			["wait", "timer", "seconds", "sleep"], [ExecIn(), In("Duration", types.Float, 0.2f), ExecOut("Completed", PinRole.Continuation)]);
		Latent(nodes, BuiltinNode.FlowRetriggerableDelay, "Retriggerable Delay",
			"Waits Duration seconds; reaching it again while it waits starts the wait over, and only the latest execution continues.",
			["debounce", "wait", "restart"], [ExecIn(), In("Duration", types.Float, 0.2f), ExecOut("Completed", PinRole.Continuation)]);
		Latent(nodes, BuiltinNode.FlowWaitFrames, "Wait Frames", "Waits for Frames updates, then continues at Completed.",
			["wait", "frames", "next frame"], [ExecIn(), In("Frames", types.Int, 1), ExecOut("Completed", PinRole.Continuation)]);
		Latent(nodes, BuiltinNode.FlowWaitUntil, "Wait Until", "Continues at Completed as soon as Condition is true, checking it once per update.",
			["wait", "condition", "until"], [ExecIn(), In("Condition", types.Bool), ExecOut("Completed", PinRole.Continuation)]);
		Latent(nodes, BuiltinNode.FlowTween, "Tween",
			"Runs Update every frame for Duration seconds with Alpha eased from 0 to 1, then continues at Completed; Stop cancels it.",
			["animate", "ease", "interpolate", "lerp", "timeline"],
			[
				ExecIn(), ExecIn("Stop"), In("Duration", types.Float, 1.0f), ExecOut("Update", PinRole.Tick), Out("Alpha", types.Float),
				ExecOut("Completed", PinRole.Continuation),
			],
			[new SettingDescription("Easing", SettingKind.Choice) { Default = nameof(Easing.Linear), Choices = EasingNames }]);
		Latent(nodes, BuiltinNode.FlowInterval, "Interval",
			"Runs Tick every Interval seconds, Count times (endlessly when Count is 0), then continues at Completed; Stop cancels it.",
			["repeat", "timer", "tick", "every"],
			[
				ExecIn("Start"), ExecIn("Stop"), In("Interval", types.Float, 1.0f), In("Count", types.Int, 0), ExecOut("Tick", PinRole.Tick),
				Out("Index", types.Int), ExecOut("Completed", PinRole.Continuation),
			]);
		// Its pins without tracks; each event track adds an exec output after Finished, each curve track a data output after
		// Playing (spec-slice3 section 3.8.1).
		Latent(nodes, BuiltinNode.FlowTimeline, "Timeline",
			"Plays curves over Length seconds: Update fires every frame while it plays with each curve's value at Time, event tracks fire at their keys, Finished at the end; Rate scales its speed.",
			["timeline", "curve", "animate", "keyframe", "sequence"],
			[
				ExecIn("Play"), ExecIn("PlayFromStart"), ExecIn("Reverse"), ExecIn("ReverseFromEnd"), ExecIn("Stop"), ExecIn("SetTime"),
				In("NewTime", types.Float, 0.0f), In("Rate", types.Float, 1.0f), ExecOut("Update", PinRole.Tick), ExecOut("Finished", PinRole.Tick),
				Out("Time", types.Float), Out("Direction", types.TimelineDirection), Out("Playing", types.Bool),
			],
			[new SettingDescription("Timeline", SettingKind.Timeline)]);
	}

	private static void AddVariablesAndFunctions(List<BuiltinNodeDefinition> nodes)
	{
		SettingDescription variable = new("Variable", SettingKind.Variable) { Required = true };
		Add(nodes, new BuiltinNodeDefinition(BuiltinNode.VariableGet, TypeOf(BuiltinNode.VariableGet), "Get Variable", Variables, NodeKind.Variable,
			true, false, [Wildcard("Value", PinDirection.Out, PinConstraint.Any, WildcardSource.None)])
		{
			Description = "Reads a variable of the graph, or a local of the function.",
			Keywords = ["get", "read"],
			Settings = [variable],
		});
		Add(nodes, new BuiltinNodeDefinition(BuiltinNode.VariableSet, TypeOf(BuiltinNode.VariableSet), "Set Variable", Variables, NodeKind.Variable,
			false, false,
			[
				ExecIn(), Wildcard("Value", PinDirection.In, PinConstraint.Any, WildcardSource.None), ExecOut(),
				Wildcard("Value", PinDirection.Out, PinConstraint.Any, WildcardSource.None),
			])
		{
			Description = "Writes a variable of the graph or a local of the function, then outputs the value it wrote.",
			Keywords = ["set", "write", "assign"],
			Settings = [variable],
		});
		Add(nodes, new BuiltinNodeDefinition(BuiltinNode.FunctionEntry, TypeOf(BuiltinNode.FunctionEntry), "Function Entry", Functions,
			NodeKind.Function, false, false, [ExecOut()])
		{
			Description = "Where a call of the function starts; its outputs are the function's inputs.",
			Keywords = ["start", "parameters", "inputs"],
		});
		Add(nodes, new BuiltinNodeDefinition(BuiltinNode.FunctionReturn, TypeOf(BuiltinNode.FunctionReturn), "Return", Functions,
			NodeKind.Function, false, false, [ExecIn()])
		{
			Description = "Ends the call; its inputs are the function's outputs.",
			Keywords = ["return", "result", "outputs", "end"],
		});
		Add(nodes, new BuiltinNodeDefinition(BuiltinNode.FunctionCall, TypeOf(BuiltinNode.FunctionCall), "Call Function", Functions,
			NodeKind.Function, false, false, [ExecIn(), ExecOut()])
		{
			Description = "Calls a function of the graph with its inputs and outputs its results.",
			Keywords = ["call", "invoke", "run"],
			Settings = [new SettingDescription("Function", SettingKind.Function) { Required = true }],
		});
	}

	private static void AddData(List<BuiltinNodeDefinition> nodes, BuiltinTypes types)
	{
		DataNode(nodes, BuiltinNode.Self, "Self", Utilities, "The entity running the graph.", ["this", "me", "owner"],
			[Out("Self", types.Entity)]);
		DataNode(nodes, BuiltinNode.Literal, "Literal", Utilities, "A constant value of a type.", ["constant", "value", "number", "text"],
			[Wildcard("Value", PinDirection.Out, PinConstraint.Any, WildcardSource.None)],
			[
				new SettingDescription("Type", SettingKind.Type) { Required = true, Constraint = TypeConstraint.Literal },
				new SettingDescription("Value", SettingKind.Value),
			]);
		Add(nodes, new BuiltinNodeDefinition(BuiltinNode.Reroute, TypeOf(BuiltinNode.Reroute), "Reroute", Utilities, NodeKind.Reroute, true,
			false,
			[
				Wildcard("In", PinDirection.In, PinConstraint.Any, WildcardSource.Links, ValueGroup),
				Wildcard("Out", PinDirection.Out, PinConstraint.Any, WildcardSource.Links, ValueGroup),
			])
		{
			Description = "Passes a value, or the flow, through unchanged, to route links.",
			Keywords = ["wire", "knot", "pass", "route"],
		});
		DataNode(nodes, BuiltinNode.Select, "Select", Utilities, "Outputs True or False depending on Condition (both inputs are evaluated).",
			["choose", "pick", "ternary", "if"],
			[
				In("Condition", types.Bool), Wildcard("True", PinDirection.In, PinConstraint.Any, WildcardSource.Links, ValueGroup),
				Wildcard("False", PinDirection.In, PinConstraint.Any, WildcardSource.Links, ValueGroup),
				Wildcard("Result", PinDirection.Out, PinConstraint.Any, WildcardSource.Links, ValueGroup),
			]);
		DataNode(nodes, BuiltinNode.IsValid, "Is Valid",
			Utilities, "Whether a reference is set: an entity that exists, a component its entity still has, a valid asset, any other object.",
			["null", "none", "exists", "valid"],
			[Wildcard("Value", PinDirection.In, PinConstraint.Reference, WildcardSource.Links), Out("Result", types.Bool)]);
		DataNode(nodes, BuiltinNode.Convert, "Convert", Conversions,
			"Converts a value to the type To: numbers, enums and text, entities to script classes, and the types' own conversions.",
			["cast", "parse", "to", "change type"],
			[
				Wildcard("Value", PinDirection.In, PinConstraint.Any, WildcardSource.Links),
				Wildcard("Result", PinDirection.Out, PinConstraint.Any, WildcardSource.None),
			],
			[new SettingDescription("To", SettingKind.Type) { Required = true, Constraint = TypeConstraint.Any }]);
		DataNode(nodes, BuiltinNode.Make, "Make", Structs, "Builds a struct from its fields.", ["construct", "struct", "compose", "new"],
			[Wildcard("Value", PinDirection.Out, PinConstraint.Any, WildcardSource.None)],
			[new SettingDescription("Type", SettingKind.Type) { Required = true, Constraint = TypeConstraint.Makeable }]);
		DataNode(nodes, BuiltinNode.Break, "Break", Structs, "Splits a struct into its fields.", ["split", "fields", "decompose"],
			[Wildcard("Value", PinDirection.In, PinConstraint.Any, WildcardSource.None)],
			[new SettingDescription("Type", SettingKind.Type) { Required = true, Constraint = TypeConstraint.Breakable }]);
		DataNode(nodes, BuiltinNode.FormatString, "Format String", Utilities,
			"Builds text from Format, replacing each {Name} with its input's value as text; {{ and }} are braces.",
			["text", "format", "interpolate", "string", "concatenate"], [Out("Result", types.String)],
			[new SettingDescription("Format", SettingKind.String) { Default = string.Empty }]);
		DataNode(nodes, BuiltinNode.ArrayLength, "Length", Arrays, "The number of elements of an array.", ["count", "size", "array"],
			[Wildcard("Array", PinDirection.In, PinConstraint.Array, WildcardSource.Links), Out("Length", types.Int)], subtitle: ArraySubtitle);
		DataNode(nodes, BuiltinNode.ArrayGet, "Get", Arrays, "The element of an array at Index (from 0).", ["index", "element", "item", "array"],
			[
				Wildcard("Array", PinDirection.In, PinConstraint.Array, WildcardSource.Links), In("Index", types.Int),
				Wildcard("Element", PinDirection.Out, PinConstraint.Any, WildcardSource.ElementOf, elementOf: "Array"),
			], subtitle: ArraySubtitle);
	}

	// The formula node (spec-slice3 section 3.3): its inputs are the names its Formula uses, so the template has only Result.
	private static void AddExpression(List<BuiltinNodeDefinition> nodes)
	{
		Add(nodes, new BuiltinNodeDefinition(BuiltinNode.Expression, TypeOf(BuiltinNode.Expression), "Expression", Mathematics, NodeKind.Operator, true,
			false, [Wildcard("Result", PinDirection.Out, PinConstraint.Any, WildcardSource.Computed)])
		{
			Description = "Computes a formula such as a * b + sin(t): each name it uses is an input (a float unless linked), and Result is its value.",
			Keywords = ["formula", "math", "calculate", "equation", "expr", "="],
			Settings = [new SettingDescription("Formula", SettingKind.Expression) { Default = string.Empty, Required = true }],
		});
	}

	// The text nodes (spec-slice3 section 3.5): ordinal comparisons, invariant case mappings, lengths and indices in UTF-16
	// code units; none of them stops an execution on its own data.
	private static void AddStrings(List<BuiltinNodeDefinition> nodes, BuiltinTypes types)
	{
		StringNode(nodes, BuiltinNode.StringLength, "Length", "The number of UTF-16 code units of Text, as C#'s string.Length counts them.",
			["count", "size", "characters", "string"], [In("Text", types.String), Out("Length", types.Int)]);
		StringNode(nodes, BuiltinNode.StringIsEmpty, "Is Empty", "Whether Text has no characters.", ["empty", "blank", "string"],
			[In("Text", types.String), Out("Result", types.Bool)]);
		StringNode(nodes, BuiltinNode.StringContains, "Contains", "Whether Substring occurs in Text (ordinal, case-sensitive; an empty Substring always does).",
			["find", "search", "includes", "string"], [In("Text", types.String), In("Substring", types.String), Out("Result", types.Bool)]);
		StringNode(nodes, BuiltinNode.StringStartsWith, "Starts With", "Whether Text begins with Prefix (ordinal, case-sensitive).",
			["prefix", "begins", "string"], [In("Text", types.String), In("Prefix", types.String), Out("Result", types.Bool)]);
		StringNode(nodes, BuiltinNode.StringEndsWith, "Ends With", "Whether Text ends with Suffix (ordinal, case-sensitive).",
			["suffix", "ends", "string"], [In("Text", types.String), In("Suffix", types.String), Out("Result", types.Bool)]);
		StringNode(nodes, BuiltinNode.StringIndexOf, "Index Of",
			"The first index from StartIndex (clamped to the text) where Substring occurs in Text (ordinal), or -1 when it does not.",
			["find", "search", "position", "string"],
			[In("Text", types.String), In("Substring", types.String), In("StartIndex", types.Int, 0), Out("Index", types.Int)]);
		StringNode(nodes, BuiltinNode.StringSubstring, "Substring",
			"Length characters of Text from Start (all the rest when Length is negative), clamped to the text.",
			["slice", "part", "cut", "mid", "string"],
			[In("Text", types.String), In("Start", types.Int, 0), In("Length", types.Int, -1), Out("Result", types.String)]);
		StringNode(nodes, BuiltinNode.StringReplace, "Replace", "Text with every occurrence of Old replaced by New, left to right (ordinal).",
			["substitute", "swap", "string"],
			[In("Text", types.String), In("Old", types.String), In("New", types.String), Out("Result", types.String)]);
		StringNode(nodes, BuiltinNode.StringToUpper, "To Upper", "Text in upper case (the invariant culture's mapping).",
			["uppercase", "capitals", "case", "string"], [In("Text", types.String), Out("Result", types.String)]);
		StringNode(nodes, BuiltinNode.StringToLower, "To Lower", "Text in lower case (the invariant culture's mapping).",
			["lowercase", "case", "string"], [In("Text", types.String), Out("Result", types.String)]);
		StringNode(nodes, BuiltinNode.StringTrim, "Trim", "Text without its leading and trailing white space.", ["strip", "whitespace", "string"],
			[In("Text", types.String), Out("Result", types.String)]);
		StringNode(nodes, BuiltinNode.StringSplit, "Split",
			"The parts of Text between the occurrences of Separator (ordinal); RemoveEmpty leaves out the empty parts.",
			["separate", "tokens", "parts", "string"],
			[In("Text", types.String), In("Separator", types.String, ","), In("RemoveEmpty", types.Bool, false), Out("Parts", types.StringArray)]);
		StringNode(nodes, BuiltinNode.StringJoin, "Join", "The elements of Parts as text, with Separator between them.",
			["concatenate", "combine", "list", "string"],
			[
				Wildcard("Parts", PinDirection.In, PinConstraint.Array, WildcardSource.Links), In("Separator", types.String, ", "),
				Out("Result", types.String),
			]);
		StringNode(nodes, BuiltinNode.StringToInt, "To Int",
			"Reads Text as an integer (an optional sign and digits, white space around); Success is false, and Value 0, when it is none.",
			["parse", "number", "integer", "convert", "string"], [In("Text", types.String), Out("Value", types.Int), Out("Success", types.Bool)]);
		StringNode(nodes, BuiltinNode.StringToFloat, "To Float",
			"Reads Text as a finite number (a sign, digits, a decimal point, an exponent, white space around); Success is false, and Value 0, when it is none.",
			["parse", "number", "decimal", "convert", "string"], [In("Text", types.String), Out("Value", types.Float), Out("Success", types.Bool)]);
	}

	// The array nodes of spec-slice3 section 3.4.5: none changes an array in place; the ones that "change" one make a new one.
	private static void AddArrays(List<BuiltinNodeDefinition> nodes, BuiltinTypes types)
	{
		ArrayNode(nodes, BuiltinNode.ArrayContains, "Contains", "Whether an element of Array equals Value.", ["find", "search", "includes", "array"],
			[ArrayInput("Array", null), ElementInput(), Out("Result", types.Bool)]);
		ArrayNode(nodes, BuiltinNode.ArrayIndexOf, "Index Of",
			"The first index from StartIndex (clamped to the array) whose element equals Value, or -1 when there is none.",
			["find", "search", "position", "array"], [ArrayInput("Array", null), ElementInput(), In("StartIndex", types.Int, 0), Out("Index", types.Int)]);
		ArrayNode(nodes, BuiltinNode.ArrayMake, "Make Array", "A new array of the elements in order, of the type Type or of the type their links join to.",
			["new", "list", "create", "build", "array"],
			[
				Wildcard("Element0", PinDirection.In, PinConstraint.Any, WildcardSource.Links, ValueGroup),
				Wildcard("Element1", PinDirection.In, PinConstraint.Any, WildcardSource.Links, ValueGroup),
				ArrayOf("Array", "Element0"),
			],
			[
				new SettingDescription("Type", SettingKind.Type) { Constraint = TypeConstraint.Any },
				new SettingDescription("Count", SettingKind.Int) { Default = 2, Min = 0, Max = MaxMakeArrayCount },
			]);
		ArrayNode(nodes, BuiltinNode.ArraySet, "Set Element", "A copy of Array with the element at Index replaced by Value.",
			["replace", "assign", "write", "array"],
			[ArrayInput("Array", ArrayGroup), In("Index", types.Int), ElementInput(), ArrayResult()]);
		ArrayNode(nodes, BuiltinNode.ArrayAdd, "Add Element", "A copy of Array with Value appended.", ["append", "push", "insert", "array"],
			[ArrayInput("Array", ArrayGroup), ElementInput(), ArrayResult()]);
		ArrayNode(nodes, BuiltinNode.ArrayRemoveAt, "Remove Element", "A copy of Array without the element at Index.",
			["delete", "erase", "remove", "array"], [ArrayInput("Array", ArrayGroup), In("Index", types.Int), ArrayResult()]);
		ArrayNode(nodes, BuiltinNode.ArrayInsert, "Insert Element",
			"A copy of Array with Value inserted before the element at Index (an Index of the array's length appends).", ["insert", "add", "array"],
			[ArrayInput("Array", ArrayGroup), In("Index", types.Int), ElementInput(), ArrayResult()]);
		ArrayNode(nodes, BuiltinNode.ArrayAppend, "Append Arrays", "A new array of A's elements, then B's.", ["concatenate", "join", "combine", "array"],
			[ArrayInput("A", ArrayGroup), ArrayInput("B", ArrayGroup), ArrayResult()]);
	}

	private static void AddOperators(List<BuiltinNodeDefinition> nodes, BuiltinTypes types)
	{
		Arithmetic(nodes, BuiltinNode.OperatorAdd, "Add", "A + B: numbers, text (joined, the other value as text) or values with an addition operator.",
			["+", "plus", "sum", "concatenate"]);
		Arithmetic(nodes, BuiltinNode.OperatorSubtract, "Subtract", "A - B.", ["-", "minus", "difference"]);
		Arithmetic(nodes, BuiltinNode.OperatorMultiply, "Multiply", "A * B.", ["*", "times", "product", "scale"]);
		Arithmetic(nodes, BuiltinNode.OperatorDivide, "Divide", "A / B (an integer division by zero stops the execution).",
			["/", "quotient", "over"]);
		Arithmetic(nodes, BuiltinNode.OperatorModulo, "Modulo", "The remainder of A / B.", ["%", "remainder", "mod"]);
		Operator(nodes, BuiltinNode.OperatorNegate, "Negate", "-A.", ["-", "minus", "invert", "opposite"],
			[
				Wildcard("A", PinDirection.In, PinConstraint.Any, WildcardSource.Links),
				Wildcard("Result", PinDirection.Out, PinConstraint.Any, WildcardSource.Computed),
			]);
		Comparison(nodes, BuiltinNode.OperatorEqual, "Equal", "Whether A equals B.", ["==", "equals", "same"], types.Bool);
		Comparison(nodes, BuiltinNode.OperatorNotEqual, "Not Equal", "Whether A differs from B.", ["!=", "different", "unequal"], types.Bool);
		Comparison(nodes, BuiltinNode.OperatorLess, "Less", "Whether A < B.", ["<", "smaller", "below"], types.Bool);
		Comparison(nodes, BuiltinNode.OperatorLessOrEqual, "Less Or Equal", "Whether A <= B.", ["<=", "at most"], types.Bool);
		Comparison(nodes, BuiltinNode.OperatorGreater, "Greater", "Whether A > B.", [">", "larger", "above"], types.Bool);
		Comparison(nodes, BuiltinNode.OperatorGreaterOrEqual, "Greater Or Equal", "Whether A >= B.", [">=", "at least"], types.Bool);
		Operator(nodes, BuiltinNode.OperatorAnd, "And", "Whether A and B are both true (both are evaluated).", ["&&", "both", "all"],
			[In("A", types.Bool), In("B", types.Bool), Out("Result", types.Bool)]);
		Operator(nodes, BuiltinNode.OperatorOr, "Or", "Whether A or B is true (both are evaluated).", ["||", "either", "any"],
			[In("A", types.Bool), In("B", types.Bool), Out("Result", types.Bool)]);
		Operator(nodes, BuiltinNode.OperatorXor, "Xor", "Whether exactly one of A and B is true.", ["^", "exclusive", "different"],
			[In("A", types.Bool), In("B", types.Bool), Out("Result", types.Bool)]);
		Operator(nodes, BuiltinNode.OperatorNot, "Not", "Whether A is false.", ["!", "invert", "negate"], [In("A", types.Bool), Out("Result", types.Bool)]);
	}

	private static SettingDescription[] CustomEventSettings() =>
	[
		new SettingDescription("Name", SettingKind.String) { Required = true },
		new SettingDescription("Parameters", SettingKind.Parameters) { Default = System.Array.Empty<object>() },
	];

	private static void Event(List<BuiltinNodeDefinition> nodes, BuiltinNode node, string title, string description, string[] keywords,
		PinDescription[] pins, SettingDescription[]? settings = null)
	{
		Add(nodes, new BuiltinNodeDefinition(node, TypeOf(node), title, Events, NodeKind.Event, false, false, pins)
		{
			Description = description,
			Keywords = keywords,
			Settings = settings ?? [],
		});
	}

	private static void InputEvent(List<BuiltinNodeDefinition> nodes, BuiltinNode node, string title, string description, string setting,
		VisualType enumType, bool gamepad, VisualType? intType = null)
	{
		PinDescription[] pins = gamepad ? [In("Gamepad", intType!, 0), ExecOut()] : [ExecOut()];
		Event(nodes, node, title, description, ["input", "press", "release", gamepad ? "gamepad" : setting == "Key" ? "keyboard" : "mouse"], pins,
			[new SettingDescription(setting, SettingKind.Enum) { Required = true, Type = enumType.Id }]);
	}

	private static void FlowNode(List<BuiltinNodeDefinition> nodes, BuiltinNode node, string title, string description, string[] keywords,
		PinDescription[] pins, SettingDescription[]? settings = null)
	{
		Add(nodes, new BuiltinNodeDefinition(node, TypeOf(node), title, Flow, NodeKind.Flow, false, false, pins)
		{
			Description = description,
			Keywords = keywords,
			Settings = settings ?? [],
		});
	}

	private static void Latent(List<BuiltinNodeDefinition> nodes, BuiltinNode node, string title, string description, string[] keywords,
		PinDescription[] pins, SettingDescription[]? settings = null)
	{
		Add(nodes, new BuiltinNodeDefinition(node, TypeOf(node), title, Flow, NodeKind.Latent, false, true, pins)
		{
			Description = description,
			Keywords = keywords,
			Settings = settings ?? [],
		});
	}

	private static void DataNode(List<BuiltinNodeDefinition> nodes, BuiltinNode node, string title, string category, string description,
		string[] keywords, PinDescription[] pins, SettingDescription[]? settings = null, string subtitle = "")
	{
		Add(nodes, new BuiltinNodeDefinition(node, TypeOf(node), title, category, NodeKind.Data, true, false, pins)
		{
			Subtitle = subtitle,
			Description = description,
			Keywords = keywords,
			Settings = settings ?? [],
		});
	}

	private static void StringNode(List<BuiltinNodeDefinition> nodes, BuiltinNode node, string title, string description, string[] keywords,
		PinDescription[] pins)
	{
		DataNode(nodes, node, title, Strings, description, keywords, pins, subtitle: StringSubtitle);
	}

	private static void ArrayNode(List<BuiltinNodeDefinition> nodes, BuiltinNode node, string title, string description, string[] keywords,
		PinDescription[] pins, SettingDescription[]? settings = null)
	{
		DataNode(nodes, node, title, Arrays, description, keywords, pins, settings, ArraySubtitle);
	}

	// An array input typed by its links: a type variable of its own, or the group the node's array pins share.
	private static PinDescription ArrayInput(string name, string? group) =>
		Wildcard(name, PinDirection.In, PinConstraint.Array, WildcardSource.Links, group);

	// The Value of a node's Array input: an element of it.
	private static PinDescription ElementInput() =>
		Wildcard("Value", PinDirection.In, PinConstraint.Any, WildcardSource.ElementOf, elementOf: "Array");

	// The array a node makes from its array inputs: of their type.
	private static PinDescription ArrayResult() => Wildcard("Result", PinDirection.Out, PinConstraint.Array, WildcardSource.Links, ArrayGroup);

	private static void Arithmetic(List<BuiltinNodeDefinition> nodes, BuiltinNode node, string title, string description, string[] keywords)
	{
		Operator(nodes, node, title, description, keywords,
			[
				Wildcard("A", PinDirection.In, PinConstraint.Any, WildcardSource.Links),
				Wildcard("B", PinDirection.In, PinConstraint.Any, WildcardSource.Links),
				Wildcard("Result", PinDirection.Out, PinConstraint.Any, WildcardSource.Computed),
			]);
	}

	private static void Comparison(List<BuiltinNodeDefinition> nodes, BuiltinNode node, string title, string description, string[] keywords,
		VisualType boolType)
	{
		Operator(nodes, node, title, description, keywords,
			[
				Wildcard("A", PinDirection.In, PinConstraint.Any, WildcardSource.Links),
				Wildcard("B", PinDirection.In, PinConstraint.Any, WildcardSource.Links), Out("Result", boolType),
			]);
	}

	private static void Operator(List<BuiltinNodeDefinition> nodes, BuiltinNode node, string title, string description, string[] keywords,
		PinDescription[] pins)
	{
		Add(nodes, new BuiltinNodeDefinition(node, TypeOf(node), title, Operators, NodeKind.Operator, true, false, pins)
		{
			Description = description,
			Keywords = keywords,
		});
	}

	private static void Add(List<BuiltinNodeDefinition> nodes, BuiltinNodeDefinition node)
	{
		nodes.Add(node);
	}

	internal static PinDescription ExecIn(string name = "Execute") => new(name, PinDirection.In, VisualType.Exec);

	internal static PinDescription ExecOut(string name = "Then", PinRole role = PinRole.None) =>
		new(name, PinDirection.Out, VisualType.Exec) { Role = role };

	internal static PinDescription In(string name, VisualType type) => new(name, PinDirection.In, type);

	internal static PinDescription In(string name, VisualType type, object defaultValue) =>
		new(name, PinDirection.In, type) { HasDefault = true, Default = defaultValue };

	internal static PinDescription Out(string name, VisualType type) => new(name, PinDirection.Out, type);

	internal static PinDescription Target(VisualType entity) => new("Target", PinDirection.In, entity) { DefaultsToSelf = true };

	internal static PinDescription Wildcard(string name, PinDirection direction, PinConstraint constraint, WildcardSource source,
		string? group = null, string? elementOf = null) =>
		new(name, direction, VisualType.Wildcard) { Constraint = constraint, Source = source, Group = group, ElementOf = elementOf };

	// An output whose type is the array of the type of the input named element.
	internal static PinDescription ArrayOf(string name, string element) =>
		new(name, PinDirection.Out, VisualType.Wildcard) { Constraint = PinConstraint.Array, Source = WildcardSource.ArrayOf, ArrayOf = element };
}

// The library types the built-in kinds use.
internal sealed record BuiltinTypes(VisualType Bool, VisualType Int, VisualType Float, VisualType String, VisualType Entity,
	VisualType KeyCode, VisualType MouseButton, VisualType GamepadButton, VisualType StringArray, VisualType TimelineDirection);
