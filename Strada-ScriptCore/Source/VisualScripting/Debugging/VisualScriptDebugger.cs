using System.Collections.Generic;

namespace Strada.VisualScripting;

// Why a breakpoint cannot stop now (spec-slice4 section 2.6): no instance runs its document (for a library, no running
// program calls it), or the program they run has no node of its ID and Type (the document has errors and its last working
// version runs, or the node is new or retyped).
internal enum BreakpointInactivity : byte
{
	None,
	NotInProgram,
	NotRunning,
}

// How a resume ended (spec-slice4 section 7.8, Resume).
internal enum ResumeOutcome : byte
{
	// A continued chain stopped again: Stop is the stop it reached.
	Stopped,
	// Every continued chain ran to its end or suspended in a latent node.
	Ended,
	// Only read-only stops were resumed: they are dismissed.
	Dismissed,
}

// What a resume did: its outcome, whether an aborting error ended a continued chain (the engine reports it as a script
// exception), and the first stop it reached.
internal readonly record struct ResumeResult(ResumeOutcome Outcome, bool Failed, DebugStop? Stop);

// The editor's debugger in the VM (spec-slice4 section 7.5): the configuration and its version, the targets, the stops (at
// most MaxStops at once) and the current one, and StopRevision, which changes whenever a stop is made, resumed or ends.
// It decides how each run is traced (section 3.2), stops executions at their breakpoints (sections 2.2 and 4.1), snapshots
// them (section 5.1) and continues them (section 4.5). Main thread only, like the runtime.
internal sealed class VisualScriptDebugger : IVisualScriptDebugger
{
	// Spec-slice4 section 4.7: the stops held at once, and the changes a stop lists.
	internal const int MaxStops = 64;
	internal const int MaxChanges = 64;

	private readonly VisualScriptRuntime m_Runtime;
	private readonly List<DebugStop> m_Stops = [];
	// Every configured breakpoint by document and node, and the enabled ones the breakpoint tables are built from.
	private Dictionary<(ulong Document, uint Node), DebugBreakpoint> m_Breakpoints = [];
	private Dictionary<(ulong Document, uint Node), DebugBreakpoint> m_Enabled = [];
	// The changes each continued execution saw since its stop, by serial, while a resume runs: its next stop lists them.
	private readonly Dictionary<ulong, (List<DebugChange> Changes, int More)> m_Changes = [];
	// The first resumable stop the resume running now made.
	private DebugStop? m_Reached;
	private bool m_Resuming;
	// The error last reported: a nested execution's error is reported again by the executions it aborts.
	private ExecutionError? m_LastError;
	private int m_Version = 1;
	private int m_LastStopId;

	internal VisualScriptDebugger(VisualScriptRuntime runtime)
	{
		m_Runtime = runtime;
	}

	internal DebugConfiguration Configuration { get; private set; } = DebugConfiguration.Empty;

	// The stops in the order they were made.
	internal IReadOnlyList<DebugStop> Stops => m_Stops;

	// The stop the last action produced, else the oldest; null without stops.
	internal DebugStop? Current { get; private set; }

	internal ulong StopRevision { get; private set; }

	// The breakpoints reached beyond MaxStops since the last resume: they ran on, unsnapshotted.
	internal int Passed { get; private set; }

	// The configured breakpoints with their counters, in the configuration's order.
	internal IEnumerable<DebugBreakpoint> Breakpoints
	{
		get
		{
			foreach (DebugBreakpointSetting setting in Configuration.Breakpoints)
			{
				yield return m_Breakpoints[(setting.Document, setting.Node)];
			}
		}
	}

	// Takes a new configuration. Counters survive for breakpoints whose condition and hit count are unchanged; every cached
	// breakpoint table becomes stale.
	internal void Configure(DebugConfiguration configuration)
	{
		Dictionary<(ulong, uint), DebugBreakpoint> breakpoints = [];
		Dictionary<(ulong, uint), DebugBreakpoint> enabled = [];
		foreach (DebugBreakpointSetting setting in configuration.Breakpoints)
		{
			(ulong, uint) key = (setting.Document, setting.Node);
			if (!m_Breakpoints.TryGetValue(key, out DebugBreakpoint? breakpoint) || breakpoint.Setting.Condition != setting.Condition ||
				breakpoint.Setting.HitCount != setting.HitCount)
			{
				breakpoint = new DebugBreakpoint(setting);
			}
			breakpoint.Setting = setting;
			breakpoints[key] = breakpoint;
			if (setting.Enabled)
			{
				enabled[key] = breakpoint;
			}
		}
		m_Breakpoints = breakpoints;
		m_Enabled = enabled;
		Configuration = configuration;
		m_Version++;
	}

	public DebugMode Attach(Execution execution)
	{
		if (m_Enabled.Count == 0 && Configuration.Watches.Count == 0)
		{
			return DebugMode.None;
		}
		GraphInstance instance = execution.Instance;
		if (!PassesTarget(instance))
		{
			return DebugMode.None;
		}
		ProgramTrace trace = TraceOf(execution.Program, instance.Graph);
		DebugMode mode = DebugMode.None;
		if (trace.HasBreakpoints)
		{
			mode |= DebugMode.Break;
		}
		if (trace.IsWatched)
		{
			mode |= DebugMode.Record;
		}
		return mode;
	}

	public bool ShouldStop(Execution execution)
	{
		GraphTrace trace = execution.TopFrame.Graph.Debug!;
		bool enter = execution.Node >= 0;
		int index = enter ? execution.Node : execution.Flow[execution.FlowCount - 1].Node;
		DebugBreakpoint[]? checks = enter ? trace.EnterBreakpoints[index] : trace.ContinueBreakpoints[index];
		DebugBreakpoint? stopping = null;
		for (int check = 0; check < (checks?.Length ?? 0); check++)
		{
			DebugBreakpoint breakpoint = checks![check];
			breakpoint.Reached++;
			breakpoint.Hits++;
			// A pure node's breakpoint takes no hit count: each consumer's instruction is a hit (spec-slice4 section 2.2).
			bool own = enter && check == 0 && trace.OwnBreakpoint[index];
			uint hitCount = own ? breakpoint.Setting.HitCount : 0;
			if (stopping == null && (hitCount == 0 || breakpoint.Hits >= hitCount))
			{
				stopping = breakpoint;
			}
		}
		if (stopping == null)
		{
			return false;
		}
		if (m_Stops.Count >= MaxStops)
		{
			Passed++;
			return false;
		}
		DebugPlace place = enter ? DebugPlace.Enter : DebugPlace.Continue;
		(ulong, uint) location = (stopping.Setting.Document, stopping.Setting.Node);
		IReadOnlyList<Execution> running = m_Runtime.Running;
		if (!execution.CanStop || !execution.Instance.IsAlive)
		{
			// Spec-slice4 section 4.4: it runs on; the stop shows where it was, and the executions below it.
			string message = !execution.Instance.IsAlive ? "its instance was destroyed, so it cannot wait"
				: execution.Parent != null ? "it runs nested in the execution that sent it its event"
				: "it runs nested in engine code another execution called";
			Add(DebugStopReason.Passed, place, execution, null, message, location, DebugSnapshot.Take(execution, place, running, running.Count - 2));
			return false;
		}
		Add(DebugStopReason.Breakpoint, place, execution, execution, string.Empty, location, DebugSnapshot.Take(execution, place, running, running.Count - 2));
		return true;
	}

	public void OnStep(Execution execution, CompiledNode node, int exit)
	{
		// This debugger keeps no flow: Record runs only stamp the frames they compute in.
	}

	public void OnEvaluated(Execution execution, CompiledNode node, Slot[] slots)
	{
		// As OnStep: nothing is kept of a pure node's values but its stamp.
	}

	public void OnFrameReleasing(Execution execution, Frame frame)
	{
		// No call frame is kept past its return.
	}

	public void OnAborted(Execution execution)
	{
		ExecutionError error = execution.Error!;
		if (ReferenceEquals(error, m_LastError))
		{
			return;
		}
		m_LastError = error;
		if (!Configuration.BreakOnErrors || !PassesTarget(execution.Instance))
		{
			return;
		}
		if (m_Stops.Count >= MaxStops)
		{
			Passed++;
			return;
		}
		IReadOnlyList<Execution> running = m_Runtime.Running;
		Add(DebugStopReason.Error, DebugPlace.Error, execution, null, error.Message, null,
			DebugSnapshot.Take(execution, DebugPlace.Error, running, running.Count - 1));
	}

	public void OnStoppedReleased(Execution execution, string reason)
	{
		if (FindStop(execution) is { } stop)
		{
			End(stop);
		}
	}

	// The resumable stop of a debug fiber; null when it has none.
	internal DebugStop? FindStop(Execution execution)
	{
		foreach (DebugStop stop in m_Stops)
		{
			if (ReferenceEquals(stop.Execution, execution) && stop.Serial == execution.Serial)
			{
				return stop;
			}
		}
		return null;
	}

	internal DebugStop? FindStop(int id)
	{
		foreach (DebugStop stop in m_Stops)
		{
			if (stop.Id == id)
			{
				return stop;
			}
		}
		return null;
	}

	// Continue (spec-slice4 section 4.5): the stop's chain runs on from where it stopped; a read-only stop is dismissed.
	internal ResumeResult Continue(DebugStop stop) => Resume([stop]);

	// Continue every stop: the resumable ones in the order they stopped, the read-only ones dismissed.
	internal ResumeResult ContinueAll() => Resume([.. m_Stops]);

	private ResumeResult Resume(List<DebugStop> stops)
	{
		Passed = 0;
		m_Resuming = true;
		m_Reached = null;
		bool continued = false;
		bool failed = false;
		try
		{
			foreach (DebugStop stop in stops)
			{
				if (!m_Stops.Contains(stop))
				{
					continue;
				}
				End(stop);
				if (stop.Execution is not { } execution || execution.Serial != stop.Serial || !execution.Instance.HasDebugFiber(execution))
				{
					continue;
				}
				List<DebugChange> changes = DebugSnapshot.Changes(stop.Leaf, execution.Instance, MaxChanges, out int more);
				m_Changes[execution.Serial] = (changes, more);
				continued = true;
				failed |= m_Runtime.ContinueStopped(execution);
			}
		}
		finally
		{
			m_Resuming = false;
			m_Changes.Clear();
		}
		DebugStop? reached = m_Reached;
		m_Reached = null;
		if (reached != null)
		{
			Current = reached;
		}
		ResumeOutcome outcome = reached != null ? ResumeOutcome.Stopped : continued ? ResumeOutcome.Ended : ResumeOutcome.Dismissed;
		return new ResumeResult(outcome, failed, reached);
	}

	// The programs the instances run, each once, with the graph handle they run for: what InactivityOf reads.
	internal List<(GraphProgram Program, ulong Handle)> RunningPrograms()
	{
		HashSet<GraphProgram> seen = [];
		List<(GraphProgram, ulong)> programs = [];
		foreach (GraphInstance instance in m_Runtime.Instances)
		{
			if (seen.Add(instance.Program))
			{
				programs.Add((instance.Program, instance.Graph));
			}
		}
		return programs;
	}

	// Whether a breakpoint can stop now, and why not (spec-slice4 section 2.6), from the running programs.
	internal static BreakpointInactivity InactivityOf(DebugBreakpointSetting setting, List<(GraphProgram Program, ulong Handle)> programs)
	{
		bool running = false;
		foreach ((GraphProgram program, ulong handle) in programs)
		{
			if (handle == setting.Document)
			{
				running = true;
				if (HasNode(program.EventGraph, setting))
				{
					return BreakpointInactivity.None;
				}
			}
			foreach (CompiledFunction function in program.Functions)
			{
				if ((function.Graph != 0 ? function.Graph : handle) != setting.Document)
				{
					continue;
				}
				running = true;
				if (HasNode(function.Body, setting))
				{
					return BreakpointInactivity.None;
				}
			}
		}
		return running ? BreakpointInactivity.NotInProgram : BreakpointInactivity.NotRunning;
	}

	private static bool HasNode(CompiledGraph graph, DebugBreakpointSetting setting)
	{
		int index = graph.FindNode(setting.Node);
		return index >= 0 && graph.Nodes[index].Type == setting.Type;
	}

	// Whether the instance passes its graph's target (spec-slice4 section 3.1): any instance, or the chosen entity.
	internal bool PassesTarget(GraphInstance instance) =>
		!Configuration.Targets.TryGetValue(instance.Graph, out ulong entity) || entity == 0 || entity == instance.Entity;

	private ProgramTrace TraceOf(GraphProgram program, ulong handle) =>
		program.Debug is { } trace && trace.IsCurrent(this, m_Version, handle) ? trace
			: ProgramTrace.Build(this, m_Version, program, handle, m_Enabled, Configuration.Watches);

	private void Add(DebugStopReason reason, DebugPlace place, Execution execution, Execution? resumable, string message,
		(ulong Document, uint Node)? breakpoint, DebugSnapshot snapshot)
	{
		List<DebugChange> changed = [];
		int more = 0;
		if (m_Changes.Remove(execution.Serial, out (List<DebugChange> Changes, int More) seen))
		{
			(changed, more) = seen;
		}
		DebugStop stop = new()
		{
			Id = ++m_LastStopId,
			Reason = reason,
			Snapshot = snapshot,
			Execution = resumable,
			Serial = execution.Serial,
			Entity = execution.Instance.Entity,
			Graph = execution.Instance.Graph,
			Place = place,
			Message = message,
			Breakpoint = breakpoint,
			Changed = changed,
			ChangedMore = more,
		};
		m_Stops.Add(stop);
		StopRevision++;
		if (resumable != null && m_Resuming && m_Reached == null)
		{
			m_Reached = stop;
		}
		Current ??= stop;
	}

	// The stop ends: it was resumed, dismissed or released.
	private void End(DebugStop stop)
	{
		m_Stops.Remove(stop);
		StopRevision++;
		if (ReferenceEquals(Current, stop))
		{
			Current = m_Stops.Count > 0 ? m_Stops[0] : null;
		}
	}
}
