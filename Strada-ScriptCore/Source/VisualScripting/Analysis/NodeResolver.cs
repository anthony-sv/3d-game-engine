using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Text.Json;

namespace Strada.VisualScripting;

// Finds the library node of a node's stored type string (spec section 4.5, decision 13): an exact match; the redirect
// table; the only overload of the member (by declaring type and member name, or by type for constructors) whose pins take
// every pin name the node stores; otherwise the node is unknown, or ambiguous between several overloads.
internal static class NodeResolver
{
	// Redirect chains longer than this are treated as broken (a table cycle).
	private const int MaxRedirectHops = 16;

	internal static void Resolve(AnalysisContext context, AnalyzedNode node, IReadOnlyList<(PinDirection Direction, string Pin)> storedPins)
	{
		NodeLibrary library = context.Library;
		string type = node.Source.Type;
		NodeDefinition? exact = library.FindNode(type);
		if (exact != null)
		{
			node.Resolution = NodeResolution.Exact;
			node.Definition = exact;
			return;
		}

		if (library.FindRedirect(type) is { } redirect && FollowRedirects(library, redirect, out NodeDefinition? redirected, out List<PinRename> renames))
		{
			Resolved(context, node, NodeResolution.Redirect, redirected, renames);
			return;
		}

		if (MemberNodeDefinition.TryParsePrefix(type, out _))
		{
			IReadOnlyList<MemberNodeDefinition> overloads = library.FindMembers(LookupKey(type));
			List<MemberNodeDefinition> fitting = [];
			foreach (MemberNodeDefinition overload in overloads)
			{
				if (AcceptsPins(overload, node, storedPins))
				{
					fitting.Add(overload);
				}
			}
			if (fitting.Count == 1)
			{
				Resolved(context, node, NodeResolution.Unique, fitting[0], []);
				return;
			}
			if (fitting.Count > 1)
			{
				List<string> candidates = [];
				foreach (MemberNodeDefinition candidate in fitting)
				{
					candidates.Add(candidate.Type);
				}
				candidates.Sort(StringComparer.Ordinal);
				context.ReportNode(DiagnosticCode.AmbiguousNode, node,
					$"\"{JsonText.Shorten(type)}\" matches {AnalysisContext.Format(fitting.Count)} overloads: store the full type string of one",
					candidates);
				return;
			}
			if (overloads.Count > 0)
			{
				List<string> candidates = [];
				foreach (MemberNodeDefinition candidate in overloads)
				{
					candidates.Add(candidate.Type);
				}
				candidates.Sort(StringComparer.Ordinal);
				context.ReportNode(DiagnosticCode.UnknownNodeType, node,
					$"no overload of {LookupKey(type)} has every pin the node's links and inputs name", context.Suggest(type, candidates, false));
				return;
			}
		}
		context.ReportNode(DiagnosticCode.UnknownNodeType, node, $"unknown node type \"{JsonText.Shorten(type)}\"",
			context.Suggest(type, library.NodeTypes));
	}

	// "Call:Strada.Input.IsKeyDown(Strada.KeyCode)" → "Call:Strada.Input.IsKeyDown"; "Call:Strada.Entity.GetComponent<T>()" →
	// "Call:Strada.Entity.GetComponent"; "New:Strada.Vector3(float)" → "New:Strada.Vector3". The signature and the generic
	// parameters are the only places '(' and '<' appear.
	internal static string LookupKey(string type)
	{
		int end = type.Length;
		int parenthesis = type.IndexOf('(', StringComparison.Ordinal);
		if (parenthesis >= 0)
		{
			end = parenthesis;
		}
		int angle = type.IndexOf('<', StringComparison.Ordinal);
		if (angle >= 0 && angle < end)
		{
			end = angle;
		}
		return type[..end];
	}

	private static void Resolved(AnalysisContext context, AnalyzedNode node, NodeResolution resolution, NodeDefinition definition,
		IReadOnlyList<PinRename> renames)
	{
		node.Resolution = resolution;
		node.Definition = definition;
		node.CanonicalType = definition.Type;
		node.PinRenames = renames;
		context.ReportNode(DiagnosticCode.NodeUpdated, node, $"node updated: {node.Source.Type} → {definition.Type}");
	}

	private static bool FollowRedirects(NodeLibrary library, NodeRedirect first, [NotNullWhen(true)] out NodeDefinition? definition,
		out List<PinRename> renames)
	{
		renames = [];
		NodeRedirect redirect = first;
		for (int hop = 0; hop < MaxRedirectHops; hop++)
		{
			Compose(renames, redirect.PinRenames);
			if (library.FindNode(redirect.NewType) is { } target)
			{
				definition = target;
				return true;
			}
			if (library.FindRedirect(redirect.NewType) is not { } next)
			{
				break;
			}
			redirect = next;
		}
		definition = null;
		return false;
	}

	// Applies the renames of a later redirect to the renames collected so far (A → B, then B → C gives A → C).
	private static void Compose(List<PinRename> renames, IReadOnlyList<PinRename> next)
	{
		foreach (PinRename rename in next)
		{
			bool chained = false;
			for (int index = 0; index < renames.Count; index++)
			{
				if (renames[index].Direction == rename.Direction && renames[index].To == rename.From)
				{
					renames[index] = renames[index] with { To = rename.To };
					chained = true;
				}
			}
			if (!chained)
			{
				renames.Add(rename);
			}
		}
	}

	private static bool AcceptsPins(MemberNodeDefinition candidate, AnalyzedNode node, IReadOnlyList<(PinDirection Direction, string Pin)> storedPins)
	{
		foreach ((PinDirection direction, string pin) in storedPins)
		{
			if (!HasPin(candidate, pin, direction))
			{
				return false;
			}
		}
		foreach (KeyValuePair<string, JsonElement> input in node.Source.Inputs.Members)
		{
			if (!HasPin(candidate, input.Key, PinDirection.In))
			{
				return false;
			}
		}
		return true;
	}

	private static bool HasPin(NodeDefinition definition, string name, PinDirection direction)
	{
		foreach (PinDescription pin in definition.Pins)
		{
			if (pin.Direction == direction && pin.Name == name)
			{
				return true;
			}
		}
		return false;
	}
}
