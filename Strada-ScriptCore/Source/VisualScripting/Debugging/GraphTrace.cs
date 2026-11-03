using System.Collections.Generic;
using System.Runtime.CompilerServices;

namespace Strada.VisualScripting;

// A breakpoint while the debugger is attached: its setting, its counters (spec-slice4 section 2.4) and its condition compiled
// for each graph it is checked in. Hits are the stop points where its condition held, Reached every stop point where it was
// checked in a Break run of a targeted execution. A Run to Node (section 2.5) is a breakpoint of its own kind, counted
// nowhere.
internal sealed class DebugBreakpoint
{
	// By compiled graph: a condition is compiled against the scope of the program's graph (section 2.3), and goes with it.
	private readonly ConditionalWeakTable<CompiledGraph, CompiledCondition> m_Conditions = new();

	internal DebugBreakpoint(DebugBreakpointSetting setting, bool isRunTo = false)
	{
		Setting = setting;
		IsRunTo = isRunTo;
	}

	internal DebugBreakpointSetting Setting { get; set; }

	internal bool IsRunTo { get; }

	internal ulong Hits { get; set; }

	internal ulong Reached { get; set; }

	// Why the breakpoint never stops (its condition does not compile, or it is on a node no flow enters, which takes none);
	// null when it can.
	internal string? Error { get; private set; }

	internal bool HasCondition => Setting.Condition.Length > 0;

	// The breakpoint's condition for its node in a compiled graph of the program, compiled the first time it is needed there.
	internal CompiledCondition ConditionFor(CompiledGraph graph, CompiledNode node, GraphProgram program)
	{
		if (!m_Conditions.TryGetValue(graph, out CompiledCondition? condition))
		{
			condition = CompiledCondition.Compile(Setting.Condition, graph, node, program, condition: true);
			m_Conditions.Add(graph, condition);
			Error = condition.Error;
		}
		return condition;
	}

	// A condition on a node no flow enters is refused: the breakpoint never stops.
	internal void RefuseCondition()
	{
		Error = BreakpointConditions.PureCondition;
	}
}

// What the debugger checks in one compiled graph (spec-slice4 sections 4.1 and 7.5), built for its owner's configuration
// version: by node index, whether an Enter position stops (Breaks: the node's own enabled breakpoint of its Type, or one of
// a pure node its instruction evaluates, section 2.2) and whether a Continue position stops (ContinueBreaks: a While whose
// condition evaluates a pure breakpoint), with the breakpoints each checks, the node's own first.
internal sealed class GraphTrace
{
	internal GraphTrace(VisualScriptDebugger owner, int version, ulong document, uint function, int nodes)
	{
		Owner = owner;
		Version = version;
		Document = document;
		Function = function;
		Breaks = new bool[nodes];
		ContinueBreaks = new bool[nodes];
		EnterBreakpoints = new DebugBreakpoint[]?[nodes];
		ContinueBreakpoints = new DebugBreakpoint[]?[nodes];
		OwnBreakpoint = new bool[nodes];
	}

	internal VisualScriptDebugger Owner { get; }

	internal int Version { get; }

	// The document the graph's nodes belong to (the script graph's handle, or the library's) and its function (0 for an event
	// graph).
	internal ulong Document { get; }

	internal uint Function { get; }

	internal bool[] Breaks { get; }

	internal bool[] ContinueBreaks { get; }

	internal DebugBreakpoint[]?[] EnterBreakpoints { get; }

	internal DebugBreakpoint[]?[] ContinueBreakpoints { get; }

	// Whether EnterBreakpoints[node][0] is the node's own breakpoint (the others are pure nodes' breakpoints, which take no
	// hit count).
	internal bool[] OwnBreakpoint { get; }

	internal bool HasBreaks { get; set; }

	internal bool IsWatched { get; set; }
}

// What the debugger knows of one program for its owner's configuration version and the graph handle its instances run
// (spec-slice4 section 3.2): whether a Break run can stop in it and whether a window watches one of its graphs. Building it
// builds the GraphTrace of each of the program's graphs.
internal sealed class ProgramTrace
{
	private ProgramTrace(VisualScriptDebugger owner, int version, ulong handle)
	{
		Owner = owner;
		Version = version;
		Handle = handle;
	}

	internal VisualScriptDebugger Owner { get; }

	internal int Version { get; }

	internal ulong Handle { get; }

	internal bool HasBreakpoints { get; private set; }

	internal bool IsWatched { get; private set; }

	internal bool IsCurrent(VisualScriptDebugger owner, int version, ulong handle) => ReferenceEquals(Owner, owner) && Version == version && Handle == handle;

	// breakpoints: the enabled breakpoints by document and node. runTo: the armed Run to Node, or null. watches: the watched
	// (document, function) pairs.
	internal static ProgramTrace Build(VisualScriptDebugger owner, int version, GraphProgram program, ulong handle,
		IReadOnlyDictionary<(ulong Document, uint Node), DebugBreakpoint> breakpoints, DebugBreakpoint? runTo,
		IReadOnlySet<(ulong Document, uint Function)> watches)
	{
		ProgramTrace trace = new(owner, version, handle);
		PureClosures closures = new(handle, breakpoints, runTo);
		List<CompiledGraph> graphs = [program.EventGraph];
		foreach (CompiledFunction function in program.Functions)
		{
			graphs.Add(function.Body);
		}
		foreach (CompiledGraph graph in graphs)
		{
			GraphTrace graphTrace = new(owner, version, DocumentOf(graph, handle), graph.Function?.Id ?? 0, graph.Nodes.Length);
			graphTrace.IsWatched = watches.Contains((graphTrace.Document, graphTrace.Function));
			if (breakpoints.Count > 0 || runTo != null)
			{
				FillBreaks(program, graph, graphTrace, closures);
			}
			graph.Debug = graphTrace;
			trace.HasBreakpoints |= graphTrace.HasBreaks;
			trace.IsWatched |= graphTrace.IsWatched;
		}
		program.Debug = trace;
		return trace;
	}

	// The document a compiled graph's nodes belong to: its library's handle, or the handle of the script graph the program runs
	// for.
	internal static ulong DocumentOf(CompiledGraph graph, ulong handle) => graph.Graph != 0 ? graph.Graph : handle;

	// A node's own breakpoint: an enabled one of its document, ID and Type (spec-slice4 section 2.6). The Make and Break nodes
	// of split pins have their node's ID and are not it.
	internal static DebugBreakpoint? OwnOf(CompiledNode node, ulong handle, IReadOnlyDictionary<(ulong Document, uint Node), DebugBreakpoint> breakpoints) =>
		!node.IsSplit && breakpoints.TryGetValue((DocumentOf(node.Owner, handle), node.Id), out DebugBreakpoint? breakpoint) &&
		breakpoint.Setting.Type == node.Type ? breakpoint : null;

	// Whether the armed Run to Node is on the node: its document, ID and Type.
	internal static bool IsRunTo(CompiledNode node, ulong handle, DebugBreakpoint? runTo) => runTo != null && !node.IsSplit &&
		runTo.Setting.Document == DocumentOf(node.Owner, handle) && runTo.Setting.Node == node.Id && runTo.Setting.Type == node.Type;

	// The breakpoints each stop point of the graph checks: an impure node's own (its condition compiled now, so that State
	// reports one that does not compile before the node is reached), the Run to Node on it, then those of the pure nodes its
	// instruction evaluates.
	private static void FillBreaks(GraphProgram program, CompiledGraph graph, GraphTrace trace, PureClosures closures)
	{
		List<DebugBreakpoint> found = [];
		foreach (CompiledNode node in graph.Nodes)
		{
			// The Entry and Return of a pure function are evaluated through the plans of its callers' calls (PureClosures).
			if (!BreakpointConditions.IsEntered(node))
			{
				continue;
			}
			found.Clear();
			DebugBreakpoint? own = OwnOf(node, closures.Handle, closures.Breakpoints);
			if (own != null)
			{
				if (own.HasCondition)
				{
					own.ConditionFor(graph, node, program);
				}
				found.Add(own);
			}
			if (IsRunTo(node, closures.Handle, closures.RunTo))
			{
				found.Add(closures.RunTo!);
			}
			int owned = found.Count;
			closures.AddPlanBreakpoints(graph, node, found);
			if (found.Count == 0)
			{
				continue;
			}
			trace.Breaks[node.Index] = true;
			trace.EnterBreakpoints[node.Index] = [.. found];
			trace.OwnBreakpoint[node.Index] = own != null;
			trace.HasBreaks = true;
			if (node.Op == Opcode.While && found.Count > owned)
			{
				// The condition is evaluated again at each Continue(While): its pure breakpoints stop there too.
				trace.ContinueBreaks[node.Index] = true;
				trace.ContinueBreakpoints[node.Index] = [.. found.GetRange(owned, found.Count - owned)];
			}
		}
	}

	// The pure nodes an instruction evaluates (spec-slice4 section 2.2): its plan (a Timeline's per-input plans), and through
	// pure calls, transitively, each pure function's Entry, Return and the plan of its Return. Each function's closure is found
	// once per build.
	private sealed class PureClosures
	{
		private readonly Dictionary<CompiledFunction, DebugBreakpoint[]> m_Functions = [];
		private readonly HashSet<CompiledFunction> m_Open = [];

		internal PureClosures(ulong handle, IReadOnlyDictionary<(ulong Document, uint Node), DebugBreakpoint> breakpoints, DebugBreakpoint? runTo)
		{
			Handle = handle;
			Breakpoints = breakpoints;
			RunTo = runTo;
		}

		internal ulong Handle { get; }

		internal IReadOnlyDictionary<(ulong Document, uint Node), DebugBreakpoint> Breakpoints { get; }

		internal DebugBreakpoint? RunTo { get; }

		// Adds the breakpoints of the pure nodes the impure node's instruction evaluates to found, each once.
		internal void AddPlanBreakpoints(CompiledGraph graph, CompiledNode node, List<DebugBreakpoint> found)
		{
			if (node.PureSources.Length > 0)
			{
				AddPlan(graph, node.Plan ??= Plans.Of(graph, node), found);
			}
			for (int input = 0; input < node.InputSources.Length; input++)
			{
				if (node.InputSources[input].Length > 0)
				{
					AddPlan(graph, node.InputPlans[input] ??= Plans.Of(graph, node.InputSources[input]), found);
				}
			}
		}

		private void AddPlan(CompiledGraph graph, int[] plan, List<DebugBreakpoint> found)
		{
			foreach (int index in plan)
			{
				CompiledNode pure = graph.Nodes[index];
				AddPure(pure, found);
				if (pure.Op == Opcode.PureCall)
				{
					foreach (DebugBreakpoint breakpoint in OfFunction(pure.Function!))
					{
						AddUnique(breakpoint, found);
					}
				}
			}
		}

		private DebugBreakpoint[] OfFunction(CompiledFunction function)
		{
			if (m_Functions.TryGetValue(function, out DebugBreakpoint[]? known))
			{
				return known;
			}
			if (!m_Open.Add(function))
			{
				// A call of a function whose closure is being found: its breakpoints are counted where the closure started.
				return [];
			}
			CompiledGraph body = function.Body;
			List<DebugBreakpoint> found = [];
			AddPure(body.Nodes[function.EntryNode], found);
			CompiledNode returned = body.Nodes[function.ReturnNode];
			AddPure(returned, found);
			AddPlanBreakpoints(body, returned, found);
			m_Open.Remove(function);
			DebugBreakpoint[] closure = [.. found];
			m_Functions[function] = closure;
			return closure;
		}

		// The breakpoint of a node no flow enters, and the Run to Node on it. A condition there is refused: the breakpoint
		// never stops (spec-slice4 section 2.2).
		private void AddPure(CompiledNode node, List<DebugBreakpoint> found)
		{
			if (OwnOf(node, Handle, Breakpoints) is { } breakpoint)
			{
				if (breakpoint.HasCondition)
				{
					breakpoint.RefuseCondition();
				}
				else
				{
					AddUnique(breakpoint, found);
				}
			}
			if (IsRunTo(node, Handle, RunTo))
			{
				AddUnique(RunTo, found);
			}
		}

		private static void AddUnique(DebugBreakpoint? breakpoint, List<DebugBreakpoint> found)
		{
			if (breakpoint != null && !found.Contains(breakpoint))
			{
				found.Add(breakpoint);
			}
		}
	}
}
