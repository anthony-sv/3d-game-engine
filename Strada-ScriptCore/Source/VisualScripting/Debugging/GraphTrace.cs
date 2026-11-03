using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Text;

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

	// The flow counted in the graph while a window watches it (spec-slice4 section 6.1); null while none does.
	internal FlowCounters? Counters { get; set; }
}

// The flow of one watched compiled graph since the last take (spec-slice4 section 6.1): per flow counter
// (CompiledNode.ExitBase + exit; the node's extra counter, ExitBase + Exits.Length, counts runs without an exit and pure
// evaluations) how often it fired and the take's sequence number of its first firing, and the counters that fired, in that
// order, so a take walks only them. It belongs to its debugger and survives the configurations that keep the graph watched.
internal sealed class FlowCounters
{
	internal FlowCounters(CompiledGraph graph, ulong document, uint function)
	{
		Graph = graph;
		Document = document;
		Function = function;
		Counts = new uint[graph.CounterCount];
		FirstOrder = new uint[graph.CounterCount];
	}

	internal CompiledGraph Graph { get; }

	// The document holding the graph's nodes and the function it is (0: an event graph).
	internal ulong Document { get; }

	internal uint Function { get; }

	internal uint[] Counts { get; }

	internal uint[] FirstOrder { get; }

	internal List<int> Touched { get; } = [];

	// Whether the debugger lists the counters among those to take.
	internal bool Listed { get; set; }

	// Forgets what was counted (a take wrote it).
	internal void Reset()
	{
		foreach (int counter in Touched)
		{
			Counts[counter] = 0;
			FirstOrder[counter] = 0;
		}
		Touched.Clear();
		Listed = false;
	}
}

// The trace block of VisualScriptTakeTrace (spec-slice4 section 6.2): little-endian, a 32-byte header (the magic "VSDT",
// Version, StopRevision, the graph and string counts, Dropped, a reserved word), then per graph (ordered by document and
// function) its document, function and step count followed by its steps (node ID, exit as an index of the string table or
// NoExit, count, first order; ordered by first order), then the string table (each a byte length and UTF-8 bytes): the exit
// pin names. Compiled graphs of one document and function are merged by node ID and exit pin name.
internal static class TraceBlock
{
	internal const uint Version = 1;
	internal const uint NoExit = 0xFFFFFFFF;
	internal const int HeaderSize = 32;
	internal const int GraphSize = 16;
	internal const int StepSize = 16;

	internal static ReadOnlySpan<byte> Magic => "VSDT"u8;

	internal static byte[] Write(ulong stopRevision, uint dropped, IEnumerable<FlowCounters> counted)
	{
		SortedDictionary<(ulong Document, uint Function), Dictionary<(uint Node, string? Exit), (uint Count, uint FirstOrder)>> graphs = [];
		foreach (FlowCounters counters in counted)
		{
			if (counters.Touched.Count == 0)
			{
				continue;
			}
			if (!graphs.TryGetValue((counters.Document, counters.Function), out Dictionary<(uint, string?), (uint, uint)>? steps))
			{
				steps = [];
				graphs[(counters.Document, counters.Function)] = steps;
			}
			// Only the counters that fired: a take costs what ran, never the graph's size.
			foreach (int counter in counters.Touched)
			{
				CompiledNode node = NodeOf(counters.Graph, counter);
				int exit = counter - node.ExitBase;
				(uint, string?) key = (node.Id, exit < node.Exits.Length ? node.ExitPins[exit] : null);
				uint count = counters.Counts[counter];
				uint first = counters.FirstOrder[counter];
				steps[key] = steps.TryGetValue(key, out (uint Count, uint FirstOrder) merged)
					? (Saturate((ulong)merged.Count + count), Math.Min(merged.FirstOrder, first))
					: (count, first);
			}
		}
		Dictionary<string, uint> strings = new(StringComparer.Ordinal);
		List<byte[]> texts = [];
		int size = HeaderSize;
		List<(ulong Document, uint Function, List<(uint Node, uint Exit, uint Count, uint FirstOrder)> Steps)> blocks = [];
		foreach (((ulong document, uint function), Dictionary<(uint, string?), (uint, uint)> steps) in graphs)
		{
			List<(uint Node, string? Exit, uint Count, uint FirstOrder)> sorted = [.. steps.Select(static step => (step.Key.Item1, step.Key.Item2, step.Value.Item1,
				step.Value.Item2))];
			sorted.Sort(static (a, b) => a.FirstOrder != b.FirstOrder ? a.FirstOrder.CompareTo(b.FirstOrder) : a.Node.CompareTo(b.Node));
			// Exit names enter the string table in the order the block names them.
			List<(uint Node, uint Exit, uint Count, uint FirstOrder)> ordered = [];
			foreach ((uint node, string? exit, uint count, uint first) in sorted)
			{
				uint index = NoExit;
				if (exit != null && !strings.TryGetValue(exit, out index))
				{
					index = (uint)texts.Count;
					strings[exit] = index;
					texts.Add(Encoding.UTF8.GetBytes(exit));
					size += 4 + texts[^1].Length;
				}
				ordered.Add((node, index, count, first));
			}
			blocks.Add((document, function, ordered));
			size += GraphSize + (ordered.Count * StepSize);
		}
		byte[] block = new byte[size];
		Span<byte> span = block;
		Magic.CopyTo(span);
		BinaryPrimitives.WriteUInt32LittleEndian(span[4..], Version);
		BinaryPrimitives.WriteUInt64LittleEndian(span[8..], stopRevision);
		BinaryPrimitives.WriteUInt32LittleEndian(span[16..], (uint)blocks.Count);
		BinaryPrimitives.WriteUInt32LittleEndian(span[20..], (uint)texts.Count);
		BinaryPrimitives.WriteUInt32LittleEndian(span[24..], dropped);
		int offset = HeaderSize;
		foreach ((ulong document, uint function, List<(uint Node, uint Exit, uint Count, uint FirstOrder)> steps) in blocks)
		{
			BinaryPrimitives.WriteUInt64LittleEndian(span[offset..], document);
			BinaryPrimitives.WriteUInt32LittleEndian(span[(offset + 8)..], function);
			BinaryPrimitives.WriteUInt32LittleEndian(span[(offset + 12)..], (uint)steps.Count);
			offset += GraphSize;
			foreach ((uint node, uint exit, uint count, uint first) in steps)
			{
				BinaryPrimitives.WriteUInt32LittleEndian(span[offset..], node);
				BinaryPrimitives.WriteUInt32LittleEndian(span[(offset + 4)..], exit);
				BinaryPrimitives.WriteUInt32LittleEndian(span[(offset + 8)..], count);
				BinaryPrimitives.WriteUInt32LittleEndian(span[(offset + 12)..], first);
				offset += StepSize;
			}
		}
		foreach (byte[] text in texts)
		{
			BinaryPrimitives.WriteUInt32LittleEndian(span[offset..], (uint)text.Length);
			text.CopyTo(span[(offset + 4)..]);
			offset += 4 + text.Length;
		}
		return block;
	}

	private static uint Saturate(ulong count) => count > uint.MaxValue ? uint.MaxValue : (uint)count;

	// The node a flow counter belongs to: the last one whose counters start at or before it (nodes hold them in order).
	internal static CompiledNode NodeOf(CompiledGraph graph, int counter)
	{
		CompiledNode[] nodes = graph.Nodes;
		int low = 0;
		int high = nodes.Length - 1;
		while (low < high)
		{
			int middle = low + ((high - low + 1) / 2);
			if (nodes[middle].ExitBase <= counter)
			{
				low = middle;
			}
			else
			{
				high = middle - 1;
			}
		}
		return nodes[low];
	}
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
			if (graphTrace.IsWatched)
			{
				// What was counted since the last take stays with the graph across configurations.
				graphTrace.Counters = graph.Debug is { Counters: { } counted } previous && ReferenceEquals(previous.Owner, owner) ? counted
					: new FlowCounters(graph, graphTrace.Document, graphTrace.Function);
			}
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
