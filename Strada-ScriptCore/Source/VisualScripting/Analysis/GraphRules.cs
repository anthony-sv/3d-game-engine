using System;
using System.Collections.Generic;

namespace Strada.VisualScripting;

// The rules about whole graphs (spec sections 4.0 to 4.3): events, the shape of functions, what pure contexts may hold,
// pure cycles and pure recursion.
internal static class GraphRules
{
	internal static void Check(AnalysisContext context, IReadOnlyList<AnalyzedFunction> functions, AnalyzedGraph eventGraph)
	{
		CheckEvents(context, eventGraph);
		foreach (AnalyzedFunction function in functions)
		{
			CheckFunction(context, function);
		}
		foreach (AnalyzedFunction function in functions)
		{
			CheckPureCycles(context, function.Graph);
		}
		CheckPureCycles(context, eventGraph);
		CheckPureRecursion(context, functions);
	}

	// Each standard event once per document, input events once per key or button, custom events once per name.
	private static void CheckEvents(AnalysisContext context, AnalyzedGraph graph)
	{
		Dictionary<string, AnalyzedNode> first = new(StringComparer.Ordinal);
		foreach (AnalyzedNode node in graph.Nodes)
		{
			if (node.Builtin is not { } builtin || !BuiltinNodes.IsEvent(builtin))
			{
				continue;
			}
			string? key = builtin switch
			{
				BuiltinNode.EventCustom => node.Binding is CustomEventBinding custom ? "Custom:" + custom.Name : null,
				_ when BuiltinNodes.IsInputEvent(builtin) => node.Binding is InputEventBinding input
					? BuiltinNodes.TypeOf(builtin) + ":" + AnalysisContext.Format((long)LiteralCodec.ToBits(input.Value))
					: null,
				_ => BuiltinNodes.TypeOf(builtin),
			};
			if (key == null)
			{
				continue;
			}
			if (first.TryGetValue(key, out AnalyzedNode? earlier))
			{
				context.ReportNode(DiagnosticCode.DuplicateEvent, node,
					$"{node.Title} is handled by node {AnalysisContext.Format(earlier.ID)} already: an event has one node per graph");
				continue;
			}
			first[key] = node;
		}
	}

	private static void CheckFunction(AnalysisContext context, AnalyzedFunction function)
	{
		List<AnalyzedNode> entries = [];
		List<AnalyzedNode> returns = [];
		foreach (AnalyzedNode node in function.Graph.Nodes)
		{
			switch (node.Builtin)
			{
				case BuiltinNode.FunctionEntry:
					entries.Add(node);
					continue;
				case BuiltinNode.FunctionReturn:
					returns.Add(node);
					continue;
				case { } builtin when BuiltinNodes.IsEvent(builtin):
					context.ReportNode(DiagnosticCode.EventInFunction, node, $"{node.Title} is an event: events belong in the event graph");
					continue;
				case { } builtin when BuiltinNodes.HasTicks(builtin):
					context.ReportNode(DiagnosticCode.TickInFunction, node,
						$"{node.Title} starts executions of its own on every tick: it belongs in the event graph, where they have no caller to return to");
					break;
			}
			if (function.Pure && node.Kind != NodeKind.Unknown && !node.Pure)
			{
				if (node.Latent)
				{
					context.ReportNode(DiagnosticCode.LatentInPureContext, node,
						$"{node.Title} waits, and {function.Name} is a pure function: pure functions return at once");
				}
				else
				{
					context.ReportNode(DiagnosticCode.ImpureInPureFunction, node,
						$"{node.Title} has exec pins, and {function.Name} is a pure function: its graph holds only pure nodes");
				}
			}
		}
		if (entries.Count == 0)
		{
			context.ReportFunction(DiagnosticCode.MissingEntry, function.ID, $"{function.Name} has no Function.Entry node");
		}
		for (int index = 1; index < entries.Count; index++)
		{
			context.ReportNode(DiagnosticCode.DuplicateEntry, entries[index],
				$"{function.Name} has its Function.Entry in node {AnalysisContext.Format(entries[0].ID)} already");
		}
		if (!function.Pure)
		{
			return;
		}
		if (returns.Count == 0)
		{
			context.ReportFunction(DiagnosticCode.MissingReturn, function.ID, $"{function.Name} is pure and has no Function.Return node");
		}
		for (int index = 1; index < returns.Count; index++)
		{
			context.ReportNode(DiagnosticCode.DuplicateReturn, returns[index],
				$"{function.Name} is pure and has its Function.Return in node {AnalysisContext.Format(returns[0].ID)} already");
		}
	}

	// Pure nodes evaluate their inputs first, so a data cycle among pure nodes could never be evaluated. One diagnostic per
	// cycle (a strongly connected component), on its first node.
	private static void CheckPureCycles(AnalysisContext context, AnalyzedGraph graph)
	{
		List<AnalyzedNode> pure = [];
		Dictionary<AnalyzedNode, List<AnalyzedNode>> edges = [];
		foreach (AnalyzedNode node in graph.Nodes)
		{
			if (node.Pure && node.Kind != NodeKind.Unknown)
			{
				pure.Add(node);
				edges[node] = [];
			}
		}
		foreach (AnalyzedLink link in graph.Links)
		{
			if (link.IsValid && !link.IsExec && edges.TryGetValue(link.From.Node, out List<AnalyzedNode>? targets) && edges.ContainsKey(link.To.Node))
			{
				targets.Add(link.To.Node);
			}
		}
		foreach (List<AnalyzedNode> component in StronglyConnected(pure, edges))
		{
			bool selfLoop = component.Count == 1 && edges[component[0]].Contains(component[0]);
			if (component.Count < 2 && !selfLoop)
			{
				continue;
			}
			component.Sort(static (a, b) => a.Source.Index.CompareTo(b.Source.Index));
			List<string> ids = [];
			foreach (AnalyzedNode node in component)
			{
				ids.Add(AnalysisContext.Format(node.ID));
			}
			context.ReportNode(DiagnosticCode.PureCycle, component[0],
				$"the pure nodes {string.Join(", ", ids)} feed each other: store a value in a variable to break the cycle");
		}
	}

	// A pure function that calls itself, directly or through other pure functions, could never return (pure calls evaluate
	// their inputs eagerly): an Error on each such call.
	private static void CheckPureRecursion(AnalysisContext context, IReadOnlyList<AnalyzedFunction> functions)
	{
		List<AnalyzedFunction> pure = [];
		Dictionary<AnalyzedFunction, List<AnalyzedFunction>> calls = [];
		foreach (AnalyzedFunction function in functions)
		{
			if (function.Pure)
			{
				pure.Add(function);
				calls[function] = [];
			}
		}
		foreach (AnalyzedFunction function in pure)
		{
			foreach (AnalyzedNode node in function.Graph.Nodes)
			{
				if (node.Builtin == BuiltinNode.FunctionCall && node.Binding is FunctionBinding { Function: { Pure: true } callee })
				{
					calls[function].Add(callee);
				}
			}
		}
		Dictionary<AnalyzedFunction, int> componentOf = [];
		int index = 0;
		foreach (List<AnalyzedFunction> component in StronglyConnected(pure, calls))
		{
			foreach (AnalyzedFunction function in component)
			{
				componentOf[function] = index;
			}
			index++;
		}
		foreach (AnalyzedFunction function in pure)
		{
			foreach (AnalyzedNode node in function.Graph.Nodes)
			{
				// A call within its own component is on a cycle (a call of the function itself is a cycle of one).
				if (node.Builtin == BuiltinNode.FunctionCall && node.Binding is FunctionBinding { Function: { Pure: true } callee } &&
					componentOf[callee] == componentOf[function])
				{
					context.ReportNode(DiagnosticCode.PureRecursion, node,
						callee == function ? $"the pure function {function.Name} calls itself: pure functions cannot recurse"
							: $"the pure functions {function.Name} and {callee.Name} call each other: pure functions cannot recurse");
				}
			}
		}
	}

	// Tarjan's algorithm without recursion (graphs may hold thousands of nodes); components in discovery order.
	private static List<List<T>> StronglyConnected<T>(List<T> vertices, Dictionary<T, List<T>> edges)
		where T : notnull
	{
		Dictionary<T, int> order = [];
		Dictionary<T, int> low = [];
		HashSet<T> onStack = [];
		Stack<T> stack = new();
		List<List<T>> components = [];
		int counter = 0;
		foreach (T root in vertices)
		{
			if (order.ContainsKey(root))
			{
				continue;
			}
			Stack<(T Vertex, int Next)> work = new();
			work.Push((root, 0));
			order[root] = low[root] = counter++;
			stack.Push(root);
			onStack.Add(root);
			while (work.Count > 0)
			{
				(T vertex, int next) = work.Pop();
				List<T> targets = edges[vertex];
				if (next < targets.Count)
				{
					work.Push((vertex, next + 1));
					T target = targets[next];
					if (!order.ContainsKey(target))
					{
						order[target] = low[target] = counter++;
						stack.Push(target);
						onStack.Add(target);
						work.Push((target, 0));
					}
					else if (onStack.Contains(target))
					{
						low[vertex] = Math.Min(low[vertex], order[target]);
					}
					continue;
				}
				if (low[vertex] == order[vertex])
				{
					List<T> component = [];
					T member;
					do
					{
						member = stack.Pop();
						onStack.Remove(member);
						component.Add(member);
					}
					while (!EqualityComparer<T>.Default.Equals(member, vertex));
					components.Add(component);
				}
				if (work.Count > 0)
				{
					T parent = work.Peek().Vertex;
					low[parent] = Math.Min(low[parent], low[vertex]);
				}
			}
		}
		return components;
	}
}
