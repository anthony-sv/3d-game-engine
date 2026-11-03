using System.Collections.Generic;

namespace Strada.VisualScripting;

// A breakpoint while the debugger is attached: its setting, its counters (spec-slice4 section 2.4) and the version of the
// setting they count for. Hits are the stop points where its condition held, Reached every stop point where it was checked
// in a Break run.
internal sealed class DebugBreakpoint
{
	internal DebugBreakpoint(DebugBreakpointSetting setting)
	{
		Setting = setting;
	}

	internal DebugBreakpointSetting Setting { get; set; }

	internal ulong Hits { get; set; }

	internal ulong Reached { get; set; }
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

	// breakpoints: the enabled breakpoints by document and node. watches: the watched (document, function) pairs.
	internal static ProgramTrace Build(VisualScriptDebugger owner, int version, GraphProgram program, ulong handle,
		IReadOnlyDictionary<(ulong Document, uint Node), DebugBreakpoint> breakpoints, IReadOnlySet<(ulong Document, uint Function)> watches)
	{
		ProgramTrace trace = new(owner, version, handle);
		PureClosures closures = new(handle, breakpoints);
		List<CompiledGraph> graphs = [program.EventGraph];
		foreach (CompiledFunction function in program.Functions)
		{
			graphs.Add(function.Body);
		}
		foreach (CompiledGraph graph in graphs)
		{
			GraphTrace graphTrace = new(owner, version, DocumentOf(graph, handle), graph.Function?.Id ?? 0, graph.Nodes.Length);
			graphTrace.IsWatched = watches.Contains((graphTrace.Document, graphTrace.Function));
			if (breakpoints.Count > 0)
			{
				FillBreaks(graph, graphTrace, breakpoints, closures);
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

	private static void FillBreaks(CompiledGraph graph, GraphTrace trace, IReadOnlyDictionary<(ulong Document, uint Node), DebugBreakpoint> breakpoints,
		PureClosures closures)
	{
		List<DebugBreakpoint> found = [];
		foreach (CompiledNode node in graph.Nodes)
		{
			if (node.Pure)
			{
				continue;
			}
			found.Clear();
			DebugBreakpoint? own = OwnOf(node, closures.Handle, breakpoints);
			if (own != null)
			{
				found.Add(own);
			}
			closures.AddPlanBreakpoints(graph, node, found);
			if (found.Count == 0)
			{
				continue;
			}
			trace.Breaks[node.Index] = true;
			trace.EnterBreakpoints[node.Index] = [.. found];
			trace.OwnBreakpoint[node.Index] = own != null;
			trace.HasBreaks = true;
			if (node.Op == Opcode.While && found.Count > (own != null ? 1 : 0))
			{
				// The condition is evaluated again at each Continue(While): its pure breakpoints stop there too.
				trace.ContinueBreaks[node.Index] = true;
				trace.ContinueBreakpoints[node.Index] = [.. found.GetRange(own != null ? 1 : 0, found.Count - (own != null ? 1 : 0))];
			}
		}
	}

	// The pure nodes an instruction evaluates (spec-slice4 section 2.2): its plan (a Timeline's per-input plans), and through
	// pure calls, transitively, each pure function's Entry, Return and the plan of its Return. Each function's closure is found
	// once per build.
	private sealed class PureClosures
	{
		private readonly IReadOnlyDictionary<(ulong Document, uint Node), DebugBreakpoint> m_Breakpoints;
		private readonly Dictionary<CompiledFunction, DebugBreakpoint[]> m_Functions = [];
		private readonly HashSet<CompiledFunction> m_Open = [];

		internal PureClosures(ulong handle, IReadOnlyDictionary<(ulong Document, uint Node), DebugBreakpoint> breakpoints)
		{
			Handle = handle;
			m_Breakpoints = breakpoints;
		}

		internal ulong Handle { get; }

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
				AddUnique(OwnOf(pure, Handle, m_Breakpoints), found);
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
			AddUnique(OwnOf(body.Nodes[function.EntryNode], Handle, m_Breakpoints), found);
			CompiledNode returned = body.Nodes[function.ReturnNode];
			AddUnique(OwnOf(returned, Handle, m_Breakpoints), found);
			AddPlanBreakpoints(body, returned, found);
			m_Open.Remove(function);
			DebugBreakpoint[] closure = [.. found];
			m_Functions[function] = closure;
			return closure;
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
