using System.Collections.Generic;
using System.Reflection;

namespace Strada.VisualScripting;

// Splits the struct pins a node's Split lists (spec-slice2 section 3.3): each entry replaces the pin of its path by one
// field pin per field of its type, "<Pin>.<Field>" in field declaration order, typed by the field's type; a nested entry
// splits a field pin of an earlier one. An input splits when its resolved type is makeable, an output when it is
// breakable. An entry that names no pin of its direction, a pin whose type cannot split (or is a wildcard not resolved),
// or a field pin of an entry that did not split is ignored with a Warning UnusedSplit. Nodes whose pins are not complete
// (unknown nodes, broken settings) split nothing and report nothing: their pins are only what links and inputs name.
internal static class PinSplitter
{
	// Whether the node splits once its wildcards are resolved (after its links are attached) rather than right after it is
	// shaped: the types of its other pins are final when it is shaped.
	internal static bool SplitsLate(AnalyzedNode node) => node.PinsComplete && TypeResolution.HasWildcards(node);

	// Whether the node lists pins to split.
	internal static bool HasEntries(AnalyzedNode node) => node.Source.Split.Inputs.Count > 0 || node.Source.Split.Outputs.Count > 0;

	internal static void Split(AnalysisContext context, AnalyzedNode node)
	{
		if (!node.PinsComplete)
		{
			return;
		}
		Split(context, node, PinDirection.In);
		Split(context, node, PinDirection.Out);
	}

	// Whether a link endpoint or Inputs key may name a field pin of an entry of the node's Split once it is split.
	internal static bool MayNameField(AnalyzedNode node, string name, PinDirection direction)
	{
		foreach (string entry in node.Source.Split.Of(direction))
		{
			if (AnalyzedNode.IsPathUnder(name, node.Rename(direction, entry)))
			{
				return true;
			}
		}
		return false;
	}

	private static void Split(AnalysisContext context, AnalyzedNode node, PinDirection direction)
	{
		string side = direction == PinDirection.In ? "input" : "output";
		foreach (string entry in node.Source.Split.Of(direction))
		{
			string path = node.Rename(direction, entry);
			AnalyzedPin? pin = node.FindShownPin(path, direction);
			if (pin == null)
			{
				int dot = path.LastIndexOf('.');
				string message = node.FindSplitPin(path, direction) != null ? $"{path} is split by an earlier entry"
					: dot > 0 && node.FindSplitPin(path[..dot], direction) == null ? $"{path} is a field of {path[..dot]}, which is not split"
					: $"{node.Title} has no {side} pin {JsonText.Quote(path)}";
				Unused(context, node, path, direction, message);
				continue;
			}
			VisualType type = pin.Type;
			if (!type.HasValues || type.Fields.Count == 0 || !(direction == PinDirection.In ? type.Makeable : type.Breakable))
			{
				Unused(context, node, path, direction, type.IsExec ? $"{path} is an exec pin"
					: !type.HasValues ? $"the type of {path} is not resolved"
					: direction == PinDirection.In ? $"{path} is {type.Id}: inputs split only structs whose public fields are all writable"
					: $"{path} is {type.Id}: outputs split only structs with public fields or readonly structs with get-only properties");
				continue;
			}
			if (pin.Links.Count > 0)
			{
				// A node that splits late found its links before it split: a link that names the split pin itself reaches
				// no pin (its fields take the links).
				foreach (AnalyzedLink link in pin.Links)
				{
					link.IsValid = false;
					context.ReportPin(DiagnosticCode.UnknownPin, node, path, direction, UnknownPinMessage(node, path, direction), link.Document);
				}
			}
			object? value = direction == PinDirection.In ? UnconnectedValue(pin) : null;
			List<(PinDescription, StructMember)> fields = [];
			foreach (StructMember field in type.Fields)
			{
				string name = $"{path}.{field.Name}";
				fields.Add((direction == PinDirection.In
					? new PinDescription(name, direction, field.Type) { HasDefault = true, Default = FieldOf(value, field) }
					: new PinDescription(name, direction, field.Type), field));
			}
			pin.Split(fields);
		}
	}

	// The UnknownPin message for a link endpoint that names no pin: why, when the name is a split pin itself or a field
	// path of a pin that is not split.
	internal static string UnknownPinMessage(AnalyzedNode node, string name, PinDirection direction)
	{
		string side = direction == PinDirection.In ? "input" : "output";
		if (node.FindSplitPin(name, direction) != null)
		{
			return $"{node.Title} has no {side} pin {JsonText.Quote(name)}: it is split, so links reach its fields";
		}
		if (FindUnsplitParent(node, name, direction) is { } parent)
		{
			return $"{node.Title} has no {side} pin {JsonText.Quote(name)}: {parent} is not split";
		}
		string other = node.FindShownPin(name, direction == PinDirection.In ? PinDirection.Out : PinDirection.In) != null
			? $" ({name} is an {(direction == PinDirection.In ? "output" : "input")})"
			: string.Empty;
		return $"{node.Title} has no {side} pin {JsonText.Quote(name)}{other}";
	}

	// The pin a path reads as a field path of when it names no pin: the longest path before a '.' that names a pin of the
	// node, which is not split. Null when there is none.
	internal static string? FindUnsplitParent(AnalyzedNode node, string name, PinDirection direction)
	{
		for (int dot = name.LastIndexOf('.'); dot > 0; dot = name.LastIndexOf('.', dot - 1))
		{
			string parent = name[..dot];
			if (node.FindShownPin(parent, direction) != null)
			{
				return parent;
			}
		}
		return null;
	}

	// The value an unconnected input takes without a literal: the pin's default, else its type's default; a field pin's is
	// that field of its parent's (a field pin's default is set when it is split).
	private static object? UnconnectedValue(AnalyzedPin pin) =>
		pin.Description.HasDefault && ReferenceEquals(pin.Description.Type, pin.Type) && pin.Description.Default != null
			? pin.Description.Default
			: LiteralCodec.DefaultOf(pin.Type);

	// A field of a struct value: what the field pin of an unconnected split input takes without a literal, so splitting a pin
	// changes nothing until a field is given a value. Graphs hold no quaternion of length zero (spec section 2.2): a field
	// of a zero struct that is one takes the identity, its type's default.
	private static object? FieldOf(object? value, StructMember field)
	{
		object? member = value == null ? null : field.Member switch
		{
			FieldInfo info => info.GetValue(value),
			PropertyInfo property => property.GetValue(value),
			_ => null,
		};
		return member switch
		{
			null => LiteralCodec.DefaultOf(field.Type),
			Quaternion { X: 0.0f, Y: 0.0f, Z: 0.0f, W: 0.0f } => LiteralCodec.DefaultOf(field.Type),
			_ => member,
		};
	}

	private static void Unused(AnalysisContext context, AnalyzedNode node, string path, PinDirection direction, string why)
	{
		string side = direction == PinDirection.In ? "Inputs" : "Outputs";
		context.ReportPin(DiagnosticCode.UnusedSplit, node, path, direction, $"Split.{side} {JsonText.Quote(path)} is ignored: {why}");
	}
}
