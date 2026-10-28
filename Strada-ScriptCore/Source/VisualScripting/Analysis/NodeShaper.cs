using System;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Text;
using System.Text.Json;

namespace Strada.VisualScripting;

// Gives each node of a document its pins from its settings (spec sections 4.0 to 4.5), binds the settings, and reports
// what is wrong with them. Types of wildcard pins are resolved afterwards (TypeResolution).
internal sealed class NodeShaper
{
	internal const int MaxSequenceCount = 64;
	internal const int MinSequenceCount = 2;
	internal const int MaxPlaceholders = 64;
	// "Case:" plus the value is a pin name of at most 128 bytes.
	internal const int MaxCaseBytes = 123;
	internal const string CasePrefix = "Case:";
	// Flow.Timeline's template pins up to Finished: the event tracks' outputs follow them.
	private const int TimelineExecPinsEnd = 10;

	private readonly AnalysisContext m_Context;
	private readonly Dictionary<AnalyzedNode, VisualType> m_ConvertTargets;
	private readonly Dictionary<AnalyzedNode, ParsedFormula> m_Formulas;

	// convertTargets and formulas receive what Convert and Expression nodes' settings say, which type resolution completes.
	internal NodeShaper(AnalysisContext context, Dictionary<AnalyzedNode, VisualType> convertTargets, Dictionary<AnalyzedNode, ParsedFormula> formulas)
	{
		m_Context = context;
		m_ConvertTargets = convertTargets;
		m_Formulas = formulas;
	}

	private NodeLibrary Library => m_Context.Library;

	internal void Shape(AnalyzedNode node)
	{
		NodeDefinition? definition = node.Definition;
		if (definition == null)
		{
			node.Kind = NodeKind.Unknown;
			node.Title = UnknownTitle(node.Source.Type);
			node.PinsComplete = false;
			return;
		}
		node.Title = definition.Title;
		node.Subtitle = definition.Subtitle;
		node.Kind = definition.Kind;
		node.Pure = definition.Pure;
		node.Latent = definition.Latent;
		switch (definition)
		{
			case BuiltinNodeDefinition builtin:
				ShapeBuiltin(node, builtin);
				break;
			case MemberNodeDefinition member:
				ShapeMember(node, member);
				break;
		}
		CheckUnknownSettings(node, definition);
	}

	// A title from a type string the library does not know: its member or kind name in words ("Call:Strada.Foo.BarBaz(int)" →
	// "Bar Baz").
	internal static string UnknownTitle(string type)
	{
		string name = NodeResolver.LookupKey(type);
		int colon = name.IndexOf(':', StringComparison.Ordinal);
		if (colon >= 0)
		{
			name = name[(colon + 1)..];
		}
		int dot = name.LastIndexOf('.');
		if (dot >= 0 && dot + 1 < name.Length)
		{
			name = name[(dot + 1)..];
		}
		return name.Length > 0 ? Titles.Words(JsonText.Shorten(name)) : JsonText.Shorten(type);
	}

	private void ShapeBuiltin(AnalyzedNode node, BuiltinNodeDefinition definition)
	{
		switch (definition.Node)
		{
			case BuiltinNode.EventKeyPressed:
			case BuiltinNode.EventKeyReleased:
				InputEvent(node, definition, "Key", Library.BuiltinTypes.KeyCode);
				break;
			case BuiltinNode.EventMouseButtonPressed:
			case BuiltinNode.EventMouseButtonReleased:
				InputEvent(node, definition, "Button", Library.BuiltinTypes.MouseButton);
				break;
			case BuiltinNode.EventGamepadButtonPressed:
			case BuiltinNode.EventGamepadButtonReleased:
				InputEvent(node, definition, "Button", Library.BuiltinTypes.GamepadButton);
				break;
			case BuiltinNode.EventCustom:
			case BuiltinNode.EventSend:
				CustomEvent(node, definition.Node == BuiltinNode.EventSend);
				break;
			case BuiltinNode.FlowSequence:
				Sequence(node);
				break;
			case BuiltinNode.FlowSwitch:
				Switch(node);
				break;
			case BuiltinNode.FlowDoOnce:
				AddTemplatePins(node, definition);
				node.Binding = new GateBinding(!ReadBool(node, "StartClosed", false));
				break;
			case BuiltinNode.FlowGate:
				AddTemplatePins(node, definition);
				node.Binding = new GateBinding(ReadBool(node, "StartOpen", true));
				break;
			case BuiltinNode.FlowTween:
				AddTemplatePins(node, definition);
				Tween(node);
				break;
			case BuiltinNode.FlowTimeline:
				Timeline(node, definition);
				break;
			case BuiltinNode.VariableGet:
			case BuiltinNode.VariableSet:
				Variable(node, definition.Node == BuiltinNode.VariableSet);
				break;
			case BuiltinNode.FunctionEntry:
			case BuiltinNode.FunctionReturn:
				FunctionEnd(node, definition.Node == BuiltinNode.FunctionEntry);
				break;
			case BuiltinNode.FunctionCall:
				FunctionCall(node);
				break;
			case BuiltinNode.Literal:
				Literal(node);
				break;
			case BuiltinNode.Convert:
				ConvertNode(node);
				break;
			case BuiltinNode.Make:
			case BuiltinNode.Break:
				Struct(node, definition.Node == BuiltinNode.Make);
				break;
			case BuiltinNode.FormatString:
				FormatNode(node);
				break;
			case BuiltinNode.Expression:
				ExpressionNode(node);
				break;
			case BuiltinNode.ArrayMake:
				MakeArray(node);
				break;
			case BuiltinNode.OperatorAnd:
			case BuiltinNode.OperatorOr:
			case BuiltinNode.OperatorXor:
			case BuiltinNode.OperatorNot:
				AddTemplatePins(node, definition);
				node.Binding = new OperatorBinding(OperatorMode.Boolean, Library.Bool, null);
				break;
			default:
				AddTemplatePins(node, definition);
				break;
		}
	}

	private static void AddTemplatePins(AnalyzedNode node, NodeDefinition definition)
	{
		foreach (PinDescription pin in definition.Pins)
		{
			node.AddPin(pin);
		}
	}

	private void InputEvent(AnalyzedNode node, NodeDefinition definition, string setting, VisualType enumType)
	{
		AddTemplatePins(node, definition);
		if (!node.Source.Settings.TryGet(setting, out JsonElement value))
		{
			m_Context.ReportNode(DiagnosticCode.MissingSetting, node,
				$"{definition.Title} needs the setting {setting}: an enumerator name of {enumType.Id}");
			return;
		}
		DecodedLiteral decoded = LiteralCodec.Decode(enumType, value);
		if (!decoded.IsValid)
		{
			m_Context.ReportNode(DiagnosticCode.InvalidSetting, node, $"{setting}: {decoded.Message}");
			return;
		}
		node.Binding = new InputEventBinding(decoded.Value!);
	}

	private void CustomEvent(AnalyzedNode node, bool send)
	{
		string? name = ReadName(node, "Name");
		List<AnalyzedParameter>? parameters = ReadParameters(node);
		if (send)
		{
			node.AddPin(BuiltinNodes.ExecIn());
			node.AddPin(BuiltinNodes.Target(Library.Entity));
		}
		else
		{
			node.AddPin(BuiltinNodes.ExecOut());
		}
		foreach (AnalyzedParameter parameter in parameters ?? [])
		{
			node.AddPin(DeclaredPin(parameter.Name, send ? PinDirection.In : PinDirection.Out, parameter.Type));
		}
		if (send)
		{
			node.AddPin(BuiltinNodes.ExecOut());
		}
		if (parameters == null)
		{
			node.PinsComplete = false;
		}
		if (name != null)
		{
			node.Title = send ? "Send " + name : name;
			if (parameters != null)
			{
				node.Binding = new CustomEventBinding(name, parameters);
			}
		}
	}

	// The Parameters setting of custom events: [{ "Name", "Type" }]; null when it is invalid.
	private List<AnalyzedParameter>? ReadParameters(AnalyzedNode node)
	{
		List<AnalyzedParameter> parameters = [];
		if (!node.Source.Settings.TryGet("Parameters", out JsonElement list))
		{
			return parameters;
		}
		if (list.ValueKind != JsonValueKind.Array)
		{
			m_Context.ReportNode(DiagnosticCode.InvalidSetting, node, "Parameters: expected an array of { \"Name\", \"Type\" } objects");
			return null;
		}
		bool valid = true;
		HashSet<string> names = new(StringComparer.Ordinal);
		int index = 0;
		foreach (JsonElement element in list.EnumerateArray())
		{
			string path = $"Parameters[{AnalysisContext.Format(index++)}]";
			if (element.ValueKind != JsonValueKind.Object)
			{
				m_Context.ReportNode(DiagnosticCode.InvalidSetting, node, $"{path}: expected an object with Name and Type");
				valid = false;
				continue;
			}
			string? name = null;
			string? typeId = null;
			foreach (JsonProperty property in element.EnumerateObject())
			{
				if (!JsonText.TryGetName(property, out string? key))
				{
					key = "?";
				}
				if (key == "Name" && JsonText.TryGetString(property.Value, out string? text))
				{
					name = text;
				}
				else if (key == "Type" && JsonText.TryGetString(property.Value, out string? typeText))
				{
					typeId = typeText;
				}
				else if (key is not ("Name" or "Type"))
				{
					m_Context.ReportNode(DiagnosticCode.InvalidSetting, node, $"{path}: a parameter has only Name and Type, not {JsonText.Quote(key)}");
					valid = false;
				}
			}
			if (name == null || typeId == null)
			{
				m_Context.ReportNode(DiagnosticCode.InvalidSetting, node, $"{path}: expected a Name and a Type, both strings");
				valid = false;
				continue;
			}
			if (!CheckDeclaredName(node, name, $"{path}.Name"))
			{
				valid = false;
				continue;
			}
			if (!names.Add(name))
			{
				m_Context.ReportNode(DiagnosticCode.InvalidSetting, node, $"{path}: a parameter named {name} comes earlier");
				valid = false;
				continue;
			}
			VisualType? type = Declarations.ResolveType(m_Context, typeId, false, (code, message, suggestions) =>
				m_Context.ReportNode(code, node, $"{path}.Type: {message}", suggestions));
			parameters.Add(new AnalyzedParameter(name, typeId, type));
		}
		return valid ? parameters : null;
	}

	private void Sequence(AnalyzedNode node)
	{
		int count = MinSequenceCount;
		if (node.Source.Settings.TryGet("Count", out JsonElement value))
		{
			if (value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out int stored) && stored is >= MinSequenceCount and <= MaxSequenceCount)
			{
				count = stored;
			}
			else
			{
				m_Context.ReportNode(DiagnosticCode.InvalidSetting, node,
					$"Count: expected an integer from {AnalysisContext.Format(MinSequenceCount)} to {AnalysisContext.Format(MaxSequenceCount)}, not {JsonText.Excerpt(value)}");
				node.PinsComplete = false;
			}
		}
		node.AddPin(BuiltinNodes.ExecIn());
		for (int index = 0; index < count; index++)
		{
			node.AddPin(BuiltinNodes.ExecOut("Then" + AnalysisContext.Format(index)));
		}
		node.Binding = new SequenceBinding(count);
	}

	private void Switch(AnalyzedNode node)
	{
		node.AddPin(BuiltinNodes.ExecIn());
		VisualType? type = ReadType(node, "Type", "a switch selects on int, string or an enum", static candidate =>
			candidate.ClrType == typeof(int) || candidate.ClrType == typeof(string) || candidate.Kind == TypeKind.Enum);
		if (type == null)
		{
			node.AddPin(BuiltinNodes.Wildcard("Selection", PinDirection.In, PinConstraint.Any, WildcardSource.None));
			node.AddPin(BuiltinNodes.ExecOut("Default"));
			node.PinsComplete = false;
			return;
		}
		node.AddPin(new PinDescription("Selection", PinDirection.In, type));
		List<SwitchCase> cases = [];
		List<string> labels = [];
		if (node.Source.Settings.TryGet("Cases", out JsonElement list))
		{
			if (list.ValueKind != JsonValueKind.Array)
			{
				m_Context.ReportNode(DiagnosticCode.InvalidCase, node, $"Cases: expected an array of case values of {type.Id}");
				node.PinsComplete = false;
			}
			else
			{
				int index = 0;
				foreach (JsonElement element in list.EnumerateArray())
				{
					string path = $"Cases[{AnalysisContext.Format(index++)}]";
					if (!TryReadCase(node, type, element, path, out object? value, out string? label))
					{
						node.PinsComplete = false;
						continue;
					}
					if (labels.Contains(label))
					{
						m_Context.ReportNode(DiagnosticCode.InvalidCase, node, $"{path}: the case {label} comes earlier");
						node.PinsComplete = false;
						continue;
					}
					labels.Add(label);
					cases.Add(new SwitchCase(value, CasePrefix + label));
				}
			}
		}
		else if (type.Kind == TypeKind.Enum)
		{
			// Every enumerator, in declaration order; enumerators sharing a value keep the first name.
			HashSet<ulong> values = [];
			foreach (EnumeratorInfo enumerator in type.Enumerators)
			{
				if (values.Add(LiteralCodec.ToBits(enumerator.Value)))
				{
					labels.Add(enumerator.Name);
					cases.Add(new SwitchCase(enumerator.Value, CasePrefix + enumerator.Name));
				}
			}
		}
		for (int index = 0; index < cases.Count; index++)
		{
			node.AddPin(new PinDescription(cases[index].Pin, PinDirection.Out, VisualType.Exec) { Label = labels[index] });
		}
		node.AddPin(BuiltinNodes.ExecOut("Default"));
		node.Binding = new SwitchBinding(type, cases);
	}

	// A case value and its canonical text: integers in decimal, enum values by enumerator name (decimal without one),
	// strings verbatim (1 to 123 bytes without control characters).
	private bool TryReadCase(AnalyzedNode node, VisualType type, JsonElement element, string path, out object value, out string label)
	{
		value = string.Empty;
		label = string.Empty;
		if (type.ClrType == typeof(string))
		{
			if (!JsonText.TryGetString(element, out string? text))
			{
				m_Context.ReportNode(DiagnosticCode.InvalidCase, node, $"{path}: expected a string, not {JsonText.Excerpt(element)}");
				return false;
			}
			int bytes = JsonText.ByteCount(text);
			if (bytes == 0 || bytes > MaxCaseBytes || JsonText.HasControlCharacter(text))
			{
				m_Context.ReportNode(DiagnosticCode.InvalidCase, node,
					$"{path}: a string case has 1 to {AnalysisContext.Format(MaxCaseBytes)} bytes without control characters");
				return false;
			}
			value = text;
			label = text;
			return true;
		}
		DecodedLiteral decoded = LiteralCodec.Decode(type, element);
		if (!decoded.IsValid || decoded.Value == null)
		{
			m_Context.ReportNode(DiagnosticCode.InvalidCase, node, $"{path}: {decoded.Message}");
			return false;
		}
		value = decoded.Value;
		label = type.Kind == TypeKind.Enum ? EnumLabel(type, decoded.Value) : AnalysisContext.Format((int)decoded.Value);
		return true;
	}

	private static string EnumLabel(VisualType type, object value)
	{
		ulong bits = LiteralCodec.ToBits(value);
		foreach (EnumeratorInfo enumerator in type.Enumerators)
		{
			if (LiteralCodec.ToBits(enumerator.Value) == bits)
			{
				return enumerator.Name;
			}
		}
		TypeCode underlying = Type.GetTypeCode(Enum.GetUnderlyingType(value.GetType()));
		return underlying is TypeCode.UInt64 or TypeCode.UInt32 or TypeCode.UInt16 or TypeCode.Byte
			? bits.ToString(CultureInfo.InvariantCulture)
			: AnalysisContext.Format(System.Convert.ToInt64(value, CultureInfo.InvariantCulture));
	}

	private void Tween(AnalyzedNode node)
	{
		Easing easing = Easing.Linear;
		if (node.Source.Settings.TryGet("Easing", out JsonElement value))
		{
			// The names are those of the enum, in its order.
			int index = JsonText.TryGetString(value, out string? name) ? Array.IndexOf(BuiltinNodes.EasingNames, name) : -1;
			if (index >= 0)
			{
				easing = (Easing)index;
			}
			else
			{
				m_Context.ReportNode(DiagnosticCode.InvalidSetting, node,
					$"Easing: expected one of {string.Join(", ", BuiltinNodes.EasingNames)}, not {JsonText.Excerpt(value)}");
			}
		}
		node.Binding = new TweenBinding(easing);
	}

	// The template's inputs, Update and Finished, an exec output per event track, Time, Direction and Playing, then a data
	// output per curve track (spec-slice3 section 3.8.1). A setting with problems keeps the pins of its valid tracks.
	private void Timeline(AnalyzedNode node, NodeDefinition definition)
	{
		TimelineDefinition? timeline = TimelineDefinition.Empty;
		List<TimelineTrackPin> tracks = [];
		if (node.Source.Settings.TryGet("Timeline", out JsonElement setting))
		{
			timeline = TimelineReader.Read(setting, Library, (code, message) => m_Context.ReportNode(code, node, message), tracks);
		}
		for (int index = 0; index < TimelineExecPinsEnd; index++)
		{
			node.AddPin(definition.Pins[index]);
		}
		foreach (TimelineTrackPin track in tracks)
		{
			if (track.Type == null)
			{
				node.AddPin(BuiltinNodes.ExecOut(track.Name, PinRole.Tick));
			}
		}
		for (int index = TimelineExecPinsEnd; index < definition.Pins.Count; index++)
		{
			node.AddPin(definition.Pins[index]);
		}
		foreach (TimelineTrackPin track in tracks)
		{
			if (track.Type is { } type)
			{
				node.AddPin(new PinDescription(track.Name, PinDirection.Out, type));
			}
		}
		if (timeline == null)
		{
			node.PinsComplete = false;
			return;
		}
		node.Binding = new TimelineBinding(timeline);
	}

	private void Variable(AnalyzedNode node, bool set)
	{
		AnalyzedVariable? variable = ReadVariable(node);
		VisualType? type = variable?.Type;
		if (set)
		{
			node.AddPin(BuiltinNodes.ExecIn());
			node.AddPin(DeclaredPin("Value", PinDirection.In, type));
			node.AddPin(BuiltinNodes.ExecOut());
		}
		node.AddPin(DeclaredPin("Value", PinDirection.Out, type));
		if (variable != null)
		{
			node.Title = (set ? "Set " : "Get ") + variable.Name;
			node.Binding = new VariableBinding(variable);
		}
	}

	private AnalyzedVariable? ReadVariable(AnalyzedNode node)
	{
		if (!ReadId(node, "Variable", "variable", out uint id))
		{
			return null;
		}
		AnalyzedFunction? function = node.Graph.Owner;
		AnalyzedVariable? variable = m_Context.FindVariable(id, function);
		if (variable != null)
		{
			return variable;
		}
		List<string> candidates = [];
		foreach (AnalyzedVariable candidate in m_Context.Variables)
		{
			candidates.Add(AnalysisContext.Format(candidate.ID));
		}
		foreach (AnalyzedVariable local in function?.Locals ?? [])
		{
			candidates.Add(AnalysisContext.Format(local.ID));
		}
		AnalyzedFunction? owner = m_Context.FindLocalOwner(id);
		string message = owner != null
			? $"variable {AnalysisContext.Format(id)} is a local of {owner.Name}: a local works only in its own function's graph"
			: $"there is no variable {AnalysisContext.Format(id)}";
		m_Context.ReportNode(DiagnosticCode.UnknownVariable, node, message, m_Context.Suggest(AnalysisContext.Format(id), candidates));
		return null;
	}

	private void FunctionEnd(AnalyzedNode node, bool entry)
	{
		AnalyzedFunction? function = node.Graph.Owner;
		if (function == null)
		{
			m_Context.ReportNode(DiagnosticCode.NotInFunction, node, $"{node.Title} belongs in the graph of a function, not in the event graph");
			node.PinsComplete = false;
			return;
		}
		if (!function.Pure)
		{
			node.AddPin(entry ? BuiltinNodes.ExecOut() : BuiltinNodes.ExecIn());
		}
		foreach (AnalyzedParameter parameter in entry ? function.Inputs : function.Outputs)
		{
			node.AddPin(DeclaredPin(parameter.Name, entry ? PinDirection.Out : PinDirection.In, parameter.Type));
		}
		node.Binding = new FunctionBinding(function);
	}

	private void FunctionCall(AnalyzedNode node)
	{
		AnalyzedFunction? function = ReadFunction(node);
		if (function == null)
		{
			node.PinsComplete = false;
			return;
		}
		if (!function.Pure)
		{
			node.AddPin(BuiltinNodes.ExecIn());
		}
		foreach (AnalyzedParameter parameter in function.Inputs)
		{
			node.AddPin(DeclaredPin(parameter.Name, PinDirection.In, parameter.Type));
		}
		if (!function.Pure)
		{
			node.AddPin(BuiltinNodes.ExecOut());
		}
		foreach (AnalyzedParameter parameter in function.Outputs)
		{
			node.AddPin(DeclaredPin(parameter.Name, PinDirection.Out, parameter.Type));
		}
		node.Title = function.Name;
		node.Pure = function.Pure;
		node.Latent = function.Latent;
		node.Binding = new FunctionBinding(function);
	}

	private AnalyzedFunction? ReadFunction(AnalyzedNode node)
	{
		if (!ReadId(node, "Function", "function", out uint id))
		{
			return null;
		}
		AnalyzedFunction? function = m_Context.FindFunction(id);
		if (function != null)
		{
			return function;
		}
		List<string> candidates = [];
		foreach (AnalyzedFunction candidate in m_Context.Functions)
		{
			candidates.Add(AnalysisContext.Format(candidate.ID));
		}
		m_Context.ReportNode(DiagnosticCode.UnknownFunction, node, $"there is no function {AnalysisContext.Format(id)}",
			m_Context.Suggest(AnalysisContext.Format(id), candidates));
		return null;
	}

	private void Literal(AnalyzedNode node)
	{
		VisualType? type = ReadType(node, "Type", "a literal has a type with a literal form (numbers, text, vectors, colors, enums, handles)",
			static candidate => candidate.HasLiteral);
		node.AddPin(DeclaredPin("Value", PinDirection.Out, type));
		if (type == null)
		{
			return;
		}
		node.Subtitle = type.Title;
		object? value = LiteralCodec.DefaultOf(type);
		if (node.Source.Settings.TryGet("Value", out JsonElement stored))
		{
			DecodedLiteral decoded = LiteralCodec.Decode(type, stored);
			if (!ReportLiteral(node, decoded, "Value"))
			{
				return;
			}
			value = decoded.Value;
		}
		node.Binding = new LiteralBinding(type, value);
	}

	private void ConvertNode(AnalyzedNode node)
	{
		VisualType? type = ReadType(node, "To", "Convert converts to a type with values", static candidate => candidate.HasValues);
		node.AddPin(BuiltinNodes.Wildcard("Value", PinDirection.In, PinConstraint.Any, WildcardSource.Links));
		node.AddPin(DeclaredPin("Result", PinDirection.Out, type));
		if (type != null)
		{
			node.Title = "To " + type.Title;
			m_ConvertTargets[node] = type;
		}
	}

	private void Struct(AnalyzedNode node, bool make)
	{
		VisualType? type = make
			? ReadType(node, "Type", "Make builds structs whose public fields are all writable (vectors, quaternions, colors, matrices, rays)",
				static candidate => candidate.Makeable)
			: ReadType(node, "Type", "Break splits structs (vectors, quaternions, colors, matrices, rays, raycast hits, asset handles)",
				static candidate => candidate.Breakable);
		if (type == null)
		{
			node.AddPin(DeclaredPin("Value", make ? PinDirection.Out : PinDirection.In, null));
			node.PinsComplete = false;
			return;
		}
		if (!make)
		{
			node.AddPin(new PinDescription("Value", PinDirection.In, type));
		}
		foreach (StructMember field in type.Fields)
		{
			node.AddPin(new PinDescription(field.Name, make ? PinDirection.In : PinDirection.Out, field.Type));
		}
		if (make)
		{
			node.AddPin(new PinDescription("Value", PinDirection.Out, type));
		}
		node.Title = (make ? "Make " : "Break ") + type.Title;
		node.Binding = new StructBinding(type);
	}

	private void FormatNode(AnalyzedNode node)
	{
		string format = string.Empty;
		if (node.Source.Settings.TryGet("Format", out JsonElement value))
		{
			if (!JsonText.TryGetString(value, out string? text))
			{
				m_Context.ReportNode(DiagnosticCode.InvalidSetting, node, $"Format: expected a string, not {JsonText.Excerpt(value)}");
				node.AddPin(new PinDescription("Result", PinDirection.Out, Library.String));
				node.PinsComplete = false;
				return;
			}
			format = text;
		}
		List<FormatSegment> segments = [];
		List<string> placeholders = [];
		bool valid = ParseFormat(node, format, segments, placeholders);
		foreach (string placeholder in placeholders)
		{
			node.AddPin(BuiltinNodes.Wildcard(placeholder, PinDirection.In, PinConstraint.Any, WildcardSource.Links));
		}
		node.AddPin(new PinDescription("Result", PinDirection.Out, Library.String));
		if (valid)
		{
			node.Binding = new FormatBinding(segments, placeholders);
		}
		else
		{
			node.PinsComplete = false;
		}
	}

	// "{Name}" inserts a placeholder's value, "{{" and "}}" are braces (spec section 4.4).
	private bool ParseFormat(AnalyzedNode node, string format, List<FormatSegment> segments, List<string> placeholders)
	{
		StringBuilder text = new();
		int index = 0;
		while (index < format.Length)
		{
			char character = format[index];
			if (character == '{' && index + 1 < format.Length && format[index + 1] == '{')
			{
				text.Append('{');
				index += 2;
				continue;
			}
			if (character == '}' && index + 1 < format.Length && format[index + 1] == '}')
			{
				text.Append('}');
				index += 2;
				continue;
			}
			if (character == '}')
			{
				m_Context.ReportNode(DiagnosticCode.InvalidSetting, node,
					$"Format: the }} at {AnalysisContext.Format(index)} closes no placeholder (write }}}} for a brace)");
				return false;
			}
			if (character != '{')
			{
				text.Append(character);
				index++;
				continue;
			}
			int end = format.IndexOfAny(['{', '}'], index + 1);
			if (end < 0 || format[end] != '}')
			{
				m_Context.ReportNode(DiagnosticCode.InvalidSetting, node,
					$"Format: the {{ at {AnalysisContext.Format(index)} opens a placeholder that is never closed (write {{{{ for a brace)");
				return false;
			}
			string name = format[(index + 1)..end];
			if (!CheckDeclaredName(node, name, "Format placeholder"))
			{
				return false;
			}
			if (text.Length > 0)
			{
				segments.Add(new FormatSegment(text.ToString(), -1));
				text.Clear();
			}
			int placeholder = placeholders.IndexOf(name);
			if (placeholder < 0)
			{
				if (placeholders.Count == MaxPlaceholders)
				{
					m_Context.ReportNode(DiagnosticCode.InvalidSetting, node,
						$"Format: at most {AnalysisContext.Format(MaxPlaceholders)} distinct placeholders");
					return false;
				}
				placeholder = placeholders.Count;
				placeholders.Add(name);
			}
			segments.Add(new FormatSegment(null, placeholder));
			index = end + 1;
		}
		if (text.Length > 0)
		{
			segments.Add(new FormatSegment(text.ToString(), -1));
		}
		return true;
	}

	// An input per input name of the formula (each its own type variable, typed by its link, a float when unlinked), then
	// Result, which the formula's type decides (spec-slice3 sections 3.3.1 and 3.3.4). While a formula does not read, the
	// node has the inputs it names and an untyped Result, and links naming other pins attach as unresolved pins.
	private void ExpressionNode(AnalyzedNode node)
	{
		const string setting = "Formula";
		if (!node.Source.Settings.TryGet(setting, out JsonElement value))
		{
			m_Context.ReportSetting(DiagnosticCode.MissingSetting, node, setting, string.Empty, null,
				$"{node.Title} needs the setting {setting}: a formula such as a * b + sin(t)");
			ShapeBrokenExpression(node, []);
			return;
		}
		if (!JsonText.TryGetString(value, out string? formula))
		{
			m_Context.ReportSetting(DiagnosticCode.InvalidSetting, node, setting, string.Empty, null, $"{setting}: expected a string, not {JsonText.Excerpt(value)}");
			ShapeBrokenExpression(node, []);
			return;
		}
		node.Subtitle = ExpressionParser.Subtitle(formula);
		ParsedFormula parsed = ExpressionParser.Parse(formula, name => m_Context.Suggest(name, ExpressionParser.CalleeNames, transpositions: true));
		if (parsed.Error is { } error)
		{
			m_Context.ReportSetting(error.Code, node, setting, formula, (error.Start, error.End), error.Message, error.Suggestions);
			ShapeBrokenExpression(node, parsed.Inputs);
			return;
		}
		foreach (string input in parsed.Inputs)
		{
			node.AddPin(BuiltinNodes.Wildcard(input, PinDirection.In, PinConstraint.Expression, WildcardSource.Links));
		}
		node.AddPin(BuiltinNodes.Wildcard("Result", PinDirection.Out, PinConstraint.Any, WildcardSource.Computed));
		m_Formulas[node] = parsed;
	}

	private static void ShapeBrokenExpression(AnalyzedNode node, IReadOnlyList<string> inputs)
	{
		foreach (string input in inputs)
		{
			node.AddPin(BuiltinNodes.Wildcard(input, PinDirection.In, PinConstraint.Expression, WildcardSource.None));
		}
		node.AddPin(BuiltinNodes.Wildcard("Result", PinDirection.Out, PinConstraint.Any, WildcardSource.None));
		node.PinsComplete = false;
	}

	// Array.Make (spec-slice3 section 3.4.5): Count elements of the type Type names, or of the type their links join to (then
	// Array is the array of Element0's type).
	private void MakeArray(AnalyzedNode node)
	{
		int count = 2;
		if (node.Source.Settings.TryGet("Count", out JsonElement value))
		{
			if (value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out int stored) && stored is >= 0 and <= BuiltinNodes.MaxMakeArrayCount)
			{
				count = stored;
			}
			else
			{
				m_Context.ReportNode(DiagnosticCode.InvalidSetting, node,
					$"Count: expected an integer from 0 to {AnalysisContext.Format(BuiltinNodes.MaxMakeArrayCount)}, not {JsonText.Excerpt(value)}");
				node.PinsComplete = false;
			}
		}
		if (node.Source.Settings.TryGet("Type", out _))
		{
			VisualType? type = ReadType(node, "Type", "Make Array makes arrays of a type with values that is not an array",
				static candidate => candidate.ArrayType != null);
			for (int index = 0; index < count; index++)
			{
				node.AddPin(DeclaredPin(ElementPin(index), PinDirection.In, type));
			}
			node.AddPin(DeclaredPin("Array", PinDirection.Out, type?.ArrayType));
			if (type == null)
			{
				node.PinsComplete = false;
				return;
			}
			node.Title = $"Make {type.Title} Array";
			node.Binding = new MakeArrayBinding(count);
			return;
		}
		if (count == 0)
		{
			m_Context.ReportNode(DiagnosticCode.MissingSetting, node, "Make Array with no elements needs the setting Type: the element type");
			node.AddPin(DeclaredPin("Array", PinDirection.Out, null));
			return;
		}
		for (int index = 0; index < count; index++)
		{
			node.AddPin(BuiltinNodes.Wildcard(ElementPin(index), PinDirection.In, PinConstraint.Any, WildcardSource.Links, BuiltinNodes.ValueGroup));
		}
		node.AddPin(BuiltinNodes.ArrayOf("Array", ElementPin(0)));
		node.Binding = new MakeArrayBinding(count);
	}

	internal static string ElementPin(int index) => "Element" + AnalysisContext.Format(index);

	private void ShapeMember(AnalyzedNode node, MemberNodeDefinition definition)
	{
		if (!definition.IsGeneric)
		{
			AddTemplatePins(node, definition);
			node.Binding = new MemberBinding(definition, definition.Member, []);
			return;
		}
		if (definition.GenericInference && !node.Source.Settings.TryGet("TypeArguments", out _))
		{
			// Links decide the types (TypeResolution closes the method once they are resolved), unless one decides none.
			AddTemplatePins(node, definition);
			if (definition.RequiresTypeArguments)
			{
				ReportMissingTypeArguments(node, definition);
			}
			return;
		}
		List<VisualType>? arguments = ReadTypeArguments(node, definition);
		if (arguments == null)
		{
			AddUnresolvedPins(node, definition);
			return;
		}
		Type[] clrTypes = new Type[arguments.Count];
		for (int index = 0; index < clrTypes.Length; index++)
		{
			clrTypes[index] = arguments[index].ClrType!;
		}
		MethodInfo closed;
		try
		{
			closed = definition.Method!.MakeGenericMethod(clrTypes);
		}
		catch (ArgumentException exception)
		{
			m_Context.ReportNode(DiagnosticCode.InvalidSetting, node, $"TypeArguments: {exception.Message}");
			AddUnresolvedPins(node, definition);
			return;
		}
		MemberShape? shape = MemberNodes.Shape(definition.MemberKind, closed, definition.Pure, Library.FindType, Library.Entity);
		if (shape == null)
		{
			m_Context.ReportNode(DiagnosticCode.InvalidSetting, node, "TypeArguments: the member has no node form for these type arguments");
			AddUnresolvedPins(node, definition);
			return;
		}
		foreach (PinDescription pin in shape.Pins)
		{
			node.AddPin(pin);
		}
		node.Binding = new MemberBinding(definition, closed, arguments);
	}

	// The template pins of a generic method whose TypeArguments are invalid: its pins of generic types are left unresolved
	// (the setting's diagnostic explains them), so links decide nothing.
	private static void AddUnresolvedPins(AnalyzedNode node, MemberNodeDefinition definition)
	{
		foreach (PinDescription pin in definition.Pins)
		{
			node.AddPin(pin.Source == WildcardSource.None ? pin : BuiltinNodes.Wildcard(pin.Name, pin.Direction, pin.Constraint, WildcardSource.None));
		}
	}

	// MissingTypeArgument for an inferred generic method a type parameter of which appears in no input.
	private void ReportMissingTypeArguments(AnalyzedNode node, MemberNodeDefinition definition)
	{
		Type[] parameters = definition.Method!.GetGenericArguments();
		List<string> undecided = [];
		for (int index = 0; index < parameters.Length; index++)
		{
			if (definition.GenericAnchors[index] == null)
			{
				undecided.Add(parameters[index].Name);
			}
		}
		int arity = parameters.Length;
		m_Context.ReportNode(DiagnosticCode.MissingTypeArgument, node,
			$"{definition.Title} needs TypeArguments: {AnalysisContext.Format(arity)} type ID{(arity == 1 ? "" : "s")}, as no input decides {string.Join(", ", undecided)}");
	}

	private List<VisualType>? ReadTypeArguments(AnalyzedNode node, MemberNodeDefinition definition)
	{
		IReadOnlyList<VisualType> domain = Library.DomainOf(definition.GenericDomain);
		string domainText = definition.GenericDomain switch
		{
			TypeConstraint.Component => "a component class",
			TypeConstraint.Asset => "an asset class",
			TypeConstraint.Script => "a script class of the game",
			_ => "a type with values",
		};
		List<string> candidates = definition.GenericInference ? ValueTypeIds() : Ids(domain);
		int arity = definition.GenericArity;
		if (!node.Source.Settings.TryGet("TypeArguments", out JsonElement list))
		{
			m_Context.ReportNode(DiagnosticCode.MissingTypeArgument, node,
				$"{definition.Title} needs TypeArguments: {AnalysisContext.Format(arity)} type ID{(arity == 1 ? "" : "s")}, each {domainText}",
				candidates);
			return null;
		}
		if (list.ValueKind != JsonValueKind.Array || list.GetArrayLength() != arity)
		{
			m_Context.ReportNode(DiagnosticCode.InvalidSetting, node,
				$"TypeArguments: expected an array of {AnalysisContext.Format(arity)} type ID{(arity == 1 ? "" : "s")}, not {JsonText.Excerpt(list)}");
			return null;
		}
		List<VisualType> arguments = [];
		foreach (JsonElement element in list.EnumerateArray())
		{
			if (!JsonText.TryGetString(element, out string? id))
			{
				m_Context.ReportNode(DiagnosticCode.InvalidSetting, node, $"TypeArguments: expected type IDs, not {JsonText.Excerpt(element)}");
				return null;
			}
			VisualType? type = Library.FindType(id);
			if (type == null)
			{
				string problem = Declarations.IsNestedArray(id) ? $"{JsonText.Quote(id)}: {Declarations.NestedArrayMessage}" : $"unknown type {JsonText.Quote(id)}";
				m_Context.ReportNode(DiagnosticCode.UnknownType, node, $"TypeArguments: {problem}", m_Context.Suggest(id, candidates));
				return null;
			}
			if (definition.GenericInference ? !type.HasValues : !ContainsType(domain, type))
			{
				m_Context.ReportNode(DiagnosticCode.InvalidSetting, node, $"TypeArguments: {type.Id} is not {domainText}",
					m_Context.Suggest(id, candidates));
				return null;
			}
			arguments.Add(type);
		}
		return arguments;
	}

	private void CheckUnknownSettings(AnalyzedNode node, NodeDefinition definition)
	{
		foreach (KeyValuePair<string, JsonElement> setting in node.Source.Settings.Members)
		{
			if (definition.FindSetting(setting.Key) != null)
			{
				continue;
			}
			List<string> names = [];
			foreach (SettingDescription description in definition.Settings)
			{
				names.Add(description.Name);
			}
			string known = names.Count > 0 ? $"its settings are {string.Join(", ", names)}" : "it has no settings";
			m_Context.ReportNode(DiagnosticCode.UnknownSetting, node, $"{definition.Type} has no setting {JsonText.Quote(setting.Key)}: {known}",
				m_Context.Suggest(setting.Key, names));
		}
	}

	// A pin typed by a declaration (a variable, parameter or setting type); a wildcard no link resolves when the declared
	// type is invalid.
	private static PinDescription DeclaredPin(string name, PinDirection direction, VisualType? type) =>
		type != null ? new PinDescription(name, direction, type) : BuiltinNodes.Wildcard(name, direction, PinConstraint.Any, WildcardSource.None);

	// A Type setting: missing → MissingSetting (when required), not a type ID → UnknownType, not accepted → InvalidSetting.
	private VisualType? ReadType(AnalyzedNode node, string setting, string requirement, Func<VisualType, bool> accepts)
	{
		if (!node.Source.Settings.TryGet(setting, out JsonElement value))
		{
			m_Context.ReportNode(DiagnosticCode.MissingSetting, node, $"{node.Title} needs the setting {setting}: a type ID");
			return null;
		}
		if (!JsonText.TryGetString(value, out string? id))
		{
			m_Context.ReportNode(DiagnosticCode.InvalidSetting, node, $"{setting}: expected a type ID, not {JsonText.Excerpt(value)}");
			return null;
		}
		VisualType? type = Library.FindType(id);
		if (type == null && Declarations.IsNestedArray(id))
		{
			m_Context.ReportNode(DiagnosticCode.UnknownType, node, $"{setting}: {JsonText.Quote(id)}: {Declarations.NestedArrayMessage}");
			return null;
		}
		if (type == null)
		{
			List<string> candidates = [];
			foreach (VisualType candidate in Library.Types)
			{
				if (accepts(candidate))
				{
					candidates.Add(candidate.Id);
				}
			}
			m_Context.ReportNode(DiagnosticCode.UnknownType, node, $"{setting}: unknown type {JsonText.Quote(id)}", m_Context.Suggest(id, candidates));
			return null;
		}
		if (!accepts(type))
		{
			m_Context.ReportNode(DiagnosticCode.InvalidSetting, node, $"{setting}: {requirement}, not {type.Id}");
			return null;
		}
		return type;
	}

	private bool ReadBool(AnalyzedNode node, string setting, bool defaultValue)
	{
		if (!node.Source.Settings.TryGet(setting, out JsonElement value))
		{
			return defaultValue;
		}
		if (value.ValueKind is JsonValueKind.True or JsonValueKind.False)
		{
			return value.GetBoolean();
		}
		m_Context.ReportNode(DiagnosticCode.InvalidSetting, node, $"{setting}: expected true or false, not {JsonText.Excerpt(value)}");
		return defaultValue;
	}

	// A required ID setting (Variable, Function).
	private bool ReadId(AnalyzedNode node, string setting, string what, out uint id)
	{
		id = 0;
		if (!node.Source.Settings.TryGet(setting, out JsonElement value))
		{
			m_Context.ReportNode(DiagnosticCode.MissingSetting, node, $"{node.Title} needs the setting {setting}: the ID of a {what}");
			return false;
		}
		if (value.ValueKind != JsonValueKind.Number || !value.TryGetUInt32(out id))
		{
			m_Context.ReportNode(DiagnosticCode.InvalidSetting, node, $"{setting}: expected the ID of a {what}, not {JsonText.Excerpt(value)}");
			return false;
		}
		return true;
	}

	// A required name setting (custom events): the name rule of spec section 1.3.
	private string? ReadName(AnalyzedNode node, string setting)
	{
		if (!node.Source.Settings.TryGet(setting, out JsonElement value))
		{
			m_Context.ReportNode(DiagnosticCode.MissingSetting, node, $"{node.Title} needs the setting {setting}: the event's name");
			return null;
		}
		if (!JsonText.TryGetString(value, out string? name))
		{
			m_Context.ReportNode(DiagnosticCode.InvalidSetting, node, $"{setting}: expected a name, not {JsonText.Excerpt(value)}");
			return null;
		}
		string? problem = Names.Check(name);
		if (problem != null)
		{
			m_Context.ReportNode(DiagnosticCode.InvalidName, node, $"{setting} {JsonText.Quote(name)}: {problem}");
			return null;
		}
		return name;
	}

	// A user-declared pin name (custom-event parameters, placeholders): InvalidName or ReservedName when it breaks the rules.
	private bool CheckDeclaredName(AnalyzedNode node, string name, string what)
	{
		string? problem = Names.Check(name);
		if (problem != null)
		{
			m_Context.ReportNode(DiagnosticCode.InvalidName, node, $"{what} {JsonText.Quote(name)}: {problem}");
			return false;
		}
		if (Names.IsReserved(name))
		{
			m_Context.ReportNode(DiagnosticCode.ReservedName, node, $"{what} {JsonText.Quote(name)}: {name} is a reserved pin name ({Names.ReservedList})");
			return false;
		}
		return true;
	}

	// Reports what is wrong with a decoded literal; false when it is not a value.
	private bool ReportLiteral(AnalyzedNode node, DecodedLiteral decoded, string what)
	{
		if (decoded.WasZeroQuaternion)
		{
			m_Context.ReportNode(DiagnosticCode.ZeroQuaternion, node, $"{what}: a quaternion of length zero is the identity");
		}
		switch (decoded.Problem)
		{
			case LiteralProblem.InvalidValue:
				m_Context.ReportNode(DiagnosticCode.InvalidValue, node, decoded.Describe(what));
				return false;
			case LiteralProblem.EntityValue:
				m_Context.ReportNode(DiagnosticCode.EntityValue, node, decoded.Describe(what));
				return false;
			default:
				return true;
		}
	}

	private static bool ContainsType(IReadOnlyList<VisualType> types, VisualType type)
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

	// The IDs of the types with values: the type arguments of inferred generic methods.
	private List<string> ValueTypeIds()
	{
		List<string> ids = [];
		foreach (VisualType type in Library.Types)
		{
			if (type.HasValues)
			{
				ids.Add(type.Id);
			}
		}
		return ids;
	}

	private static List<string> Ids(IReadOnlyList<VisualType> types)
	{
		List<string> ids = [];
		foreach (VisualType type in types)
		{
			ids.Add(type.Id);
		}
		return ids;
	}
}
