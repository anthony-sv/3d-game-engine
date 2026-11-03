using System;
using System.Collections.Generic;
using System.Linq;

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
	// A step's chain suspended in a latent node: the step goes on when its fiber resumes in a later frame.
	Waiting,
	// Only read-only stops were resumed: they are dismissed.
	Dismissed,
}

// What a resume runs (spec-slice4 section 4.5): Continue, or a step.
internal enum ResumeAction : byte
{
	Continue,
	StepInto,
	StepOver,
	StepOut,
}

// What a resume did: its outcome, whether an aborting error ended a continued chain (the engine reports it as a script
// exception), and the first stop it reached.
internal readonly record struct ResumeResult(ResumeOutcome Outcome, bool Failed, DebugStop? Stop);

// A step that runs (spec-slice4 section 4.5): its action, the stop it was made from, the chain it follows (the root's serial,
// RootSerial of every execution of the chain) and the logical depth of the stopped leaf. Waiting: the chain's root suspended
// in a latent node, and the step goes on when it resumes.
internal sealed class DebugStepRequest
{
	internal required ResumeAction Action { get; init; }

	internal required int Stop { get; init; }

	internal required ulong RootSerial { get; init; }

	internal required Execution Root { get; init; }

	internal required int Depth { get; init; }

	internal bool Waiting { get; set; }

	// The latent node the root last parked in while the step waits.
	internal CompiledNode? ParkedIn { get; set; }

	// The latent node the waiting root waits in; null while it does not wait.
	internal CompiledNode? WaitsIn => Waiting && Root.Serial == RootSerial && Root.SuspendedAt >= 0 ? Root.TopFrame.Graph.Nodes[Root.SuspendedAt] : null;
}

// The last call frame of a watched function (spec-slice4 section 5.5): copies of its slots and stamps as the call returned,
// its graph, and the entity whose execution made the call. Reused by the next call of the function.
internal sealed class DebugLastFrame
{
	internal DebugLastFrame(CompiledGraph graph)
	{
		Graph = graph;
		Slots = new Slot[graph.Template.Length];
	}

	internal CompiledGraph Graph { get; }

	internal Slot[] Slots { get; }

	internal ulong[]? Stamps { get; set; }

	internal ulong Entity { get; set; }
}

// The editor's debugger in the VM (spec-slice4 section 7.5): the configuration and its version, the targets, the stops (at
// most MaxStops at once) and the current one, the step request, the armed Break and Run to Node, the flow counted in watched
// graphs and the last frames of watched functions, the recording, and StopRevision, which changes whenever a stop is made,
// resumed, migrated or ends and whenever the step request waits, ends or is cancelled. It decides how each run is traced
// (section 3.2), stops executions at their breakpoints and steps (sections 2.2, 4.1 and 4.5), snapshots them (section 5.1),
// continues them (section 4.5) and counts the flow of Record runs (section 6.1). Main thread only, like the runtime.
internal sealed class VisualScriptDebugger : IVisualScriptDebugger
{
	// Spec-slice4 section 4.7: the stops held at once, and the changes a stop lists.
	internal const int MaxStops = 64;
	internal const int MaxChanges = 64;
	// Spec-slice4 section 6.1: the flow counters one graph and one take hold at most.
	internal const int MaxGraphEntries = 8192;
	internal const int MaxTakeEntries = 65_536;

	private readonly VisualScriptRuntime m_Runtime;
	private readonly List<DebugStop> m_Stops = [];
	// Every configured breakpoint by document and node, and the enabled ones the breakpoint tables are built from.
	private Dictionary<(ulong Document, uint Node), DebugBreakpoint> m_Breakpoints = [];
	private Dictionary<(ulong Document, uint Node), DebugBreakpoint> m_Enabled = [];
	// The changes each continued chain saw since its stop, by the chain's root serial, while a resume runs: its next stop lists
	// them.
	private readonly Dictionary<ulong, (List<DebugChange> Changes, int More)> m_Changes = [];
	// The first resumable stop the resume running now made.
	private DebugStop? m_Reached;
	private bool m_Resuming;
	// The error last reported: a nested execution's error is reported again by the executions it aborts.
	private ExecutionError? m_LastError;
	private int m_Version = 1;
	private int m_LastStopId;
	// The armed Break (section 4.5) and the document it is for (0: any graph).
	private bool m_BreakArmed;
	private ulong m_BreakDocument;
	// The armed Run to Node (section 2.5): a one-shot breakpoint in the tables.
	private DebugBreakpoint? m_RunTo;
	// The counters that fired since the last take, the take's sequence of first firings, its entries and the steps the caps
	// dropped (section 6.1).
	private readonly List<FlowCounters> m_Counted = [];
	private readonly int m_GraphEntries;
	private readonly int m_TakeEntries;
	private uint m_Sequence;
	private int m_Entries;
	private uint m_Dropped;
	// By document and function: the last call frame of each watched function (section 5.5).
	private readonly Dictionary<(ulong Document, uint Function), DebugLastFrame> m_LastFrames = [];

	internal VisualScriptDebugger(VisualScriptRuntime runtime)
		: this(runtime, MaxGraphEntries, MaxTakeEntries)
	{
	}

	// A debugger whose flow counters hold at most graphEntries per graph and takeEntries per take (the tests reach the caps
	// with small graphs).
	internal VisualScriptDebugger(VisualScriptRuntime runtime, int graphEntries, int takeEntries)
	{
		m_Runtime = runtime;
		m_GraphEntries = graphEntries;
		m_TakeEntries = takeEntries;
	}

	internal DebugConfiguration Configuration { get; private set; } = DebugConfiguration.Empty;

	// The stops in the order they were made.
	internal IReadOnlyList<DebugStop> Stops => m_Stops;

	// The stop the last action produced, else the oldest; null without stops.
	internal DebugStop? Current { get; private set; }

	internal ulong StopRevision { get; private set; }

	// The breakpoints reached beyond MaxStops since the last resume: they ran on, unsnapshotted.
	internal int Passed { get; private set; }

	// The step that runs or waits; null without one.
	internal DebugStepRequest? Stepping { get; private set; }

	public bool Armed => Stepping != null || m_BreakArmed;

	// graph.trace's recording while one is armed (Record Start to Stop).
	internal DebugRecording? Recording { get; private set; }

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
		// The last frames of functions no window watches any more are stale from now on.
		foreach ((ulong Document, uint Function) function in m_LastFrames.Keys.Where(key => !configuration.Watches.Contains(key)).ToList())
		{
			m_LastFrames.Remove(function);
		}
	}

	public DebugMode Attach(Execution execution)
	{
		// A step traces the executions of its chain whatever their instances (section 4.6, Send).
		bool followed = Stepping != null && execution.RootSerial == Stepping.RootSerial;
		bool tabled = m_Enabled.Count > 0 || m_RunTo != null;
		if (!followed && !tabled && !m_BreakArmed && Configuration.Watches.Count == 0 && Recording == null)
		{
			return DebugMode.None;
		}
		DebugMode mode = followed ? DebugMode.Break : DebugMode.None;
		if (Recording != null && Recording.Admits(execution, this))
		{
			// A recording has a filter of its own (section 3.2, Values).
			mode |= DebugMode.Values;
		}
		GraphInstance instance = execution.Instance;
		if (!PassesTarget(instance))
		{
			return mode;
		}
		if (tabled || Configuration.Watches.Count > 0)
		{
			ProgramTrace trace = TraceOf(execution.Program, instance.Graph);
			if (trace.HasBreakpoints)
			{
				mode |= DebugMode.Break;
			}
			if (trace.IsWatched)
			{
				mode |= DebugMode.Record;
			}
		}
		if (m_BreakArmed && (m_BreakDocument == 0 || instance.Graph == m_BreakDocument || Compiles(execution.Program, m_BreakDocument)))
		{
			mode |= DebugMode.Break;
		}
		return mode;
	}

	public bool ShouldStop(Execution execution)
	{
		bool enter = execution.Node >= 0;
		int index = enter ? execution.Node : execution.Flow[execution.FlowCount - 1].Node;
		GraphInstance instance = execution.Instance;
		bool targeted = PassesTarget(instance);
		DebugBreakpoint? stopping = null;
		bool runTo = false;
		string message = string.Empty;
		if (targeted && TablesOf(execution) is { } trace)
		{
			DebugBreakpoint[]? checks = enter ? trace.EnterBreakpoints[index] : trace.ContinueBreakpoints[index];
			for (int check = 0; check < (checks?.Length ?? 0); check++)
			{
				DebugBreakpoint breakpoint = checks![check];
				if (breakpoint.IsRunTo)
				{
					runTo = true;
					continue;
				}
				breakpoint.Reached++;
				// Only a node's own breakpoint takes a condition and a hit count: a pure node's counts each consumer's
				// instruction as a hit (section 2.2).
				bool own = enter && check == 0 && trace.OwnBreakpoint[index];
				string failure = string.Empty;
				if (own && breakpoint.HasCondition && !Holds(breakpoint, execution, out failure))
				{
					continue;
				}
				breakpoint.Hits++;
				uint hitCount = own ? breakpoint.Setting.HitCount : 0;
				if (stopping == null && (hitCount == 0 || breakpoint.Hits >= hitCount))
				{
					stopping = breakpoint;
					message = failure;
				}
			}
		}
		DebugStopReason reason;
		if (stopping != null)
		{
			reason = DebugStopReason.Breakpoint;
		}
		else if (StepReaches(execution))
		{
			reason = DebugStopReason.Step;
		}
		else if (targeted && BreakReaches(execution))
		{
			reason = DebugStopReason.Break;
		}
		else if (runTo)
		{
			reason = DebugStopReason.RunTo;
		}
		else
		{
			return false;
		}
		if (m_Stops.Count >= MaxStops)
		{
			Passed++;
			return false;
		}
		DebugPlace place = enter ? DebugPlace.Enter : DebugPlace.Continue;
		(ulong, uint)? location = stopping != null ? (stopping.Setting.Document, stopping.Setting.Node)
			: reason == DebugStopReason.RunTo ? (m_RunTo!.Setting.Document, m_RunTo.Setting.Node)
			: null;
		IReadOnlyList<Execution> running = m_Runtime.Running;
		DebugSnapshot snapshot = DebugSnapshot.Take(execution, place, running, running.Count - 2);
		if (!execution.CanStop || !instance.IsAlive)
		{
			// Section 4.4: it runs on; the stop shows where it was, and the executions below it. Steps and Breaks never get
			// here: they stop only executions that can stop.
			string why = !instance.IsAlive ? "its instance was destroyed, so it cannot wait"
				: execution.Parent != null ? "it runs nested in the execution that sent it its event"
				: "it runs nested in engine code another execution called";
			Add(DebugStopReason.Passed, place, execution, false, why, location, snapshot);
			return false;
		}
		Add(reason, place, execution, true, message, location, snapshot);
		return true;
	}

	public void OnStep(Execution execution, CompiledNode node, int exit)
	{
		if ((execution.Mode & DebugMode.Record) != 0 && node.Owner.Debug is { Counters: { } counters } && !node.IsSplit)
		{
			Count(counters, node.ExitBase + (exit >= 0 ? exit : node.Exits.Length));
		}
		if ((execution.Mode & DebugMode.Values) != 0)
		{
			Recording?.OnStep(execution, node, exit);
		}
	}

	public void OnEvaluated(Execution execution, CompiledNode node, Slot[] slots)
	{
		if ((execution.Mode & DebugMode.Record) != 0 && node.Owner.Debug is { Counters: { } counters } && !node.IsSplit)
		{
			// A pure node's evaluation counts in its extra counter.
			Count(counters, node.ExitBase + node.Exits.Length);
		}
		if ((execution.Mode & DebugMode.Values) != 0)
		{
			Recording?.OnEvaluated(execution, node, slots);
		}
	}

	// A watched function's call frame is copied as it returns (section 5.5): the last call wins.
	public void OnFrameReleasing(Execution execution, Frame frame)
	{
		CompiledGraph graph = frame.Graph;
		if (graph.Debug is not { IsWatched: true } trace || !ReferenceEquals(trace.Owner, this))
		{
			return;
		}
		(ulong, uint) key = (trace.Document, trace.Function);
		if (!m_LastFrames.TryGetValue(key, out DebugLastFrame? last) || !ReferenceEquals(last.Graph, graph))
		{
			last = new DebugLastFrame(graph);
			m_LastFrames[key] = last;
		}
		Array.Copy(frame.Slots, last.Slots, last.Slots.Length);
		if (frame.Stamps is { } stamps)
		{
			last.Stamps ??= new ulong[stamps.Length];
			Array.Copy(stamps, last.Stamps, stamps.Length);
		}
		else if (last.Stamps != null)
		{
			Array.Clear(last.Stamps);
		}
		last.Entity = execution.Instance.Entity;
	}

	// The last call frame of a function of a document that returned since a window watched it; null when none did.
	internal DebugLastFrame? FindLastFrame(ulong document, uint function) => m_LastFrames.GetValueOrDefault((document, function));

	// A counter fired (section 6.1): its first firing in a take records the take's order and lists it, within the caps.
	private void Count(FlowCounters counters, int counter)
	{
		uint[] counts = counters.Counts;
		uint count = counts[counter];
		if (count != 0)
		{
			counts[counter] = count == uint.MaxValue ? count : count + 1;
			return;
		}
		if (counters.Touched.Count >= m_GraphEntries || m_Entries >= m_TakeEntries)
		{
			m_Dropped = m_Dropped == uint.MaxValue ? m_Dropped : m_Dropped + 1;
			return;
		}
		counts[counter] = 1;
		counters.FirstOrder[counter] = ++m_Sequence;
		counters.Touched.Add(counter);
		m_Entries++;
		if (!counters.Listed)
		{
			counters.Listed = true;
			m_Counted.Add(counters);
		}
	}

	// VisualScriptTakeTrace (section 6.2): the block of what was counted since the last take, which starts the next one.
	internal byte[] TakeTrace()
	{
		byte[] block = TraceBlock.Write(StopRevision, m_Dropped, m_Counted);
		foreach (FlowCounters counters in m_Counted)
		{
			counters.Reset();
		}
		m_Counted.Clear();
		m_Sequence = 0;
		m_Entries = 0;
		m_Dropped = 0;
		return block;
	}

	// The runtime detached the debugger or replaced it (spec-slice4 section 7.8, Detach), after releasing its debug fibers:
	// what it still holds is dropped, so nothing of the ended session (read-only stops and their snapshots, the recording, the
	// last frames, the counted flow, the step and the armed Break and Run to Node) stays reachable through it.
	internal void Detached()
	{
		m_Stops.Clear();
		Current = null;
		Passed = 0;
		Stepping = null;
		m_BreakArmed = false;
		m_BreakDocument = 0;
		m_RunTo = null;
		m_Changes.Clear();
		m_Reached = null;
		m_LastError = null;
		Recording = null;
		m_LastFrames.Clear();
		m_Counted.Clear();
		m_Sequence = 0;
		m_Entries = 0;
		m_Dropped = 0;
		m_Version++;
	}

	// Record Start: a recording replaces the one armed before (whose steps are dropped); Values runs report to it from the
	// next execution start or resume on.
	internal void StartRecording(DebugRecordingSettings settings)
	{
		Recording = new DebugRecording(settings);
	}

	// Record Stop: the recording ends; null when none was armed.
	internal DebugRecording? StopRecording()
	{
		DebugRecording? recording = Recording;
		Recording = null;
		return recording;
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
		Add(DebugStopReason.Error, DebugPlace.Error, execution, false, error.Text, null,
			DebugSnapshot.Take(execution, DebugPlace.Error, running, running.Count - 1));
	}

	// A member of a stopped chain leaves it (section 4.3): the members below it (its receivers) keep the stop, with the sender
	// gone; the members above it (its senders) go on after their delivery, as the stop when nothing is left below, else as a
	// stop of their own. A stop with no member left ends.
	public void OnStoppedReleased(Execution execution, string reason)
	{
		if (FindMember(execution, out int member) is not { } stop)
		{
			return;
		}
		bool destroyed = reason == VisualScriptRuntime.InstanceDestroyed;
		List<DebugChainMember> below = stop.Chain.GetRange(0, member);
		List<DebugChainMember> above = stop.Chain.GetRange(member + 1, stop.Chain.Count - member - 1);
		DebugSnapshotMember[] members = stop.Snapshot.Members;
		string sendersMessage = destroyed ? "the receiver was destroyed: this execution continues after its delivery"
			: "the receiver was cancelled: this execution continues after its delivery";
		if (below.Count == 0 && above.Count == 0)
		{
			End(stop);
			return;
		}
		if (below.Count == 0)
		{
			Reshape(stop, above, members[(member + 1)..], sendersMessage);
			StopRevision++;
			return;
		}
		Reshape(stop, below, members[..member],
			destroyed ? "the sender was destroyed: this execution continues alone" : "the sender was cancelled: this execution continues alone");
		if (above.Count > 0)
		{
			DebugSnapshotMember[] upper = members[(member + 1)..];
			m_Stops.Add(new DebugStop
			{
				Id = ++m_LastStopId,
				Reason = stop.Reason,
				Snapshot = new DebugSnapshot(upper),
				Chain = above,
				Serial = above[0].Serial,
				Entity = upper[0].Entity,
				Graph = upper[0].Graph,
				Place = DebugPlace.Enter,
				Message = sendersMessage,
			});
		}
		StopRevision++;
	}

	public void OnStoppedMigrated(Execution execution)
	{
		if (FindMember(execution, out _) is { } stop)
		{
			stop.Migrated = true;
			StopRevision++;
		}
	}

	public void OnParked(Execution execution)
	{
		if (Stepping is { } step && execution.Serial == step.RootSerial)
		{
			// The step's chain waits in a latent node (section 4.6): it goes on when the fiber resumes. StopRevision changes
			// when the step starts waiting and when it waits in another latent node, never when a WaitUntil whose condition is
			// still false parks it again in its place (section 6.2): the step request is unchanged then.
			bool waited = step.Waiting;
			CompiledNode? parkedIn = step.ParkedIn;
			step.Waiting = true;
			step.ParkedIn = step.WaitsIn;
			if (!waited || !ReferenceEquals(parkedIn, step.ParkedIn))
			{
				StopRevision++;
			}
		}
	}

	public void OnReleased(Execution execution)
	{
		if (Stepping is { } step && execution.Serial == step.RootSerial)
		{
			// The step's chain ended, aborted, or its waiting fiber was discarded: the step ends without a stop.
			Stepping = null;
			StopRevision++;
		}
	}

	// The stop whose chain holds the execution, and the member's index in it; null when none does.
	private DebugStop? FindMember(Execution execution, out int member)
	{
		foreach (DebugStop stop in m_Stops)
		{
			for (int index = 0; index < stop.Chain.Count; index++)
			{
				if (ReferenceEquals(stop.Chain[index].Execution, execution) && stop.Chain[index].Serial == execution.Serial)
				{
					member = index;
					return stop;
				}
			}
		}
		member = -1;
		return null;
	}

	// The stop's chain is what is left of it: its leaf, entity, graph and snapshot follow.
	private static void Reshape(DebugStop stop, List<DebugChainMember> chain, DebugSnapshotMember[] members, string message)
	{
		stop.Chain = chain;
		stop.Snapshot = new DebugSnapshot(members);
		stop.Serial = chain[0].Serial;
		stop.Entity = members[0].Entity;
		stop.Graph = members[0].Graph;
		stop.Message = message;
	}

	// The resumable stop of a debug fiber that is its leaf; null when it has none.
	internal DebugStop? FindStop(Execution execution)
	{
		foreach (DebugStop stop in m_Stops)
		{
			if (stop.Resumable && ReferenceEquals(stop.Execution, execution) && stop.Serial == execution.Serial)
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

	// Continue (section 4.5): the stop's chain runs on from where it stopped; a read-only stop is dismissed.
	internal ResumeResult Continue(DebugStop stop) => Resume([stop], ResumeAction.Continue);

	// Continue every stop: the resumable ones in the order they stopped, the read-only ones dismissed.
	internal ResumeResult ContinueAll() => Resume([.. m_Stops], ResumeAction.Continue);

	// Step Into, Over or Out from a resumable stop (sections 4.5 and 4.6): its chain runs until the step reaches a stop point
	// (a new stop, reason Step), the chain ends, or its root suspends in a latent node (Waiting).
	internal ResumeResult Step(DebugStop stop, ResumeAction action) => Resume([stop], action);

	// Break (section 4.5): the first stop point of a targeted execution that can stop, from now on, stops it; with a document,
	// only a stop point of an execution whose instance runs that script graph, or one at that library's nodes.
	internal void ArmBreak(ulong document)
	{
		m_BreakArmed = true;
		m_BreakDocument = document;
	}

	// Cancels the armed Break and the step request (section 4.5).
	internal void CancelBreak()
	{
		m_BreakArmed = false;
		m_BreakDocument = 0;
		if (Stepping != null)
		{
			Stepping = null;
			StopRevision++;
		}
	}

	// Run to Node (section 2.5): a one-shot breakpoint on the node of the document, removed at the next stop of any kind. False
	// when no loaded or running program has the node.
	internal bool ArmRunTo(ulong document, uint node)
	{
		foreach ((GraphProgram program, ulong handle) in Programs())
		{
			if (BreakpointConditions.Find(program, handle, document, node) is ({ } _, { } found))
			{
				m_RunTo = new DebugBreakpoint(new DebugBreakpointSetting(document, node, found.Type, true, string.Empty, 0), isRunTo: true);
				m_Version++;
				return true;
			}
		}
		return false;
	}

	private ResumeResult Resume(List<DebugStop> stops, ResumeAction action)
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
				// The root continues the chain and the leaf passes its stop point; the members between them are each checked
				// as their sender finishes its delivery (a released one counts as delivered).
				if (!stop.Resumable || !stop.Chain[0].IsWaiting || !stop.Chain[^1].IsWaiting)
				{
					continue;
				}
				Execution root = stop.Root!;
				Execution leaf = stop.Execution!;
				List<DebugChange> changes = [];
				int more = 0;
				for (int member = 0; member < stop.Chain.Count; member++)
				{
					DebugSnapshot.AddChanges(stop.Snapshot.Members[member], stop.Chain[member].Execution.Instance, changes, MaxChanges, ref more);
				}
				m_Changes[root.Serial] = (changes, more);
				if (action != ResumeAction.Continue)
				{
					Stepping = new DebugStepRequest { Action = action, Stop = stop.Id, RootSerial = root.Serial, Root = root, Depth = Depth(leaf) };
				}
				continued = true;
				failed |= m_Runtime.ContinueStopped(root, leaf) == ExecutionResult.Aborted;
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
			return new ResumeResult(ResumeOutcome.Stopped, failed, reached);
		}
		if (action != ResumeAction.Continue && Stepping is { Waiting: true })
		{
			return new ResumeResult(ResumeOutcome.Waiting, failed, null);
		}
		return new ResumeResult(continued ? ResumeOutcome.Ended : ResumeOutcome.Dismissed, failed, null);
	}

	// The logical depth of an execution (section 1): its frames and those of the senders it runs nested in.
	internal static int Depth(Execution execution)
	{
		int depth = 0;
		for (Execution? member = execution; member != null; member = member.Parent)
		{
			depth += member.CallCount;
		}
		return depth;
	}

	// Whether the step stops the execution at this stop point (section 4.5): an execution of its chain that can stop, at any
	// depth (Into), at most the depth it stepped from (Over), or above it (Out).
	private bool StepReaches(Execution execution)
	{
		if (Stepping is not { } step || execution.RootSerial != step.RootSerial || !execution.CanStop || !execution.Instance.IsAlive)
		{
			return false;
		}
		return step.Action switch
		{
			ResumeAction.StepInto => true,
			ResumeAction.StepOver => Depth(execution) <= step.Depth,
			_ => Depth(execution) < step.Depth,
		};
	}

	// Whether the armed Break stops the execution at this stop point: one that can stop, of the Break's graph.
	private bool BreakReaches(Execution execution) => m_BreakArmed && execution.CanStop && execution.Instance.IsAlive && BreakAdmits(execution);

	// Whether an execution at its stop point is of the Break's graph: any graph; an instance that runs the script graph; or a
	// stop point at the library's nodes.
	private bool BreakAdmits(Execution execution) => m_BreakDocument == 0 || execution.Instance.Graph == m_BreakDocument ||
		execution.TopFrame.Graph.Graph == m_BreakDocument;

	private static bool Compiles(GraphProgram program, ulong library)
	{
		foreach (CompiledFunction function in program.Functions)
		{
			if (function.Graph == library)
			{
				return true;
			}
		}
		return false;
	}

	// Whether the breakpoint's condition holds at the execution's stop point (section 2.3). One that does not compile never
	// holds; one that fails at run time holds, failure saying why.
	private static bool Holds(DebugBreakpoint breakpoint, Execution execution, out string failure)
	{
		failure = string.Empty;
		CompiledGraph graph = execution.TopFrame.Graph;
		CompiledCondition condition = breakpoint.ConditionFor(graph, graph.Nodes[execution.Node], execution.Program);
		if (condition.Error != null)
		{
			return false;
		}
		try
		{
			return condition.Evaluate(ConditionValues.Of(execution)).Bool;
		}
		catch (VisualScriptException exception)
		{
			failure = $"the condition failed: {exception.Message}";
			return true;
		}
	}

	// The breakpoint tables of the execution's graph for this configuration (built again when the configuration changed since
	// the run attached); null when no breakpoint and no Run to Node is set.
	private GraphTrace? TablesOf(Execution execution)
	{
		if (m_Enabled.Count == 0 && m_RunTo == null)
		{
			return null;
		}
		CompiledGraph graph = execution.TopFrame.Graph;
		if (graph.Debug is not { } trace || !ReferenceEquals(trace.Owner, this) || trace.Version != m_Version)
		{
			TraceOf(execution.Program, execution.Instance.Graph);
			trace = graph.Debug;
		}
		return trace;
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

	// The programs instances run and the last working program of each loaded graph, with the graph handle each runs for.
	internal List<(GraphProgram Program, ulong Handle)> Programs()
	{
		List<(GraphProgram Program, ulong Handle)> programs = RunningPrograms();
		foreach ((ulong handle, GraphProgram program) in m_Runtime.EffectivePrograms)
		{
			if (!programs.Contains((program, handle)))
			{
				programs.Add((program, handle));
			}
		}
		return programs;
	}

	// Whether a breakpoint can stop now, and why not (section 2.6), from the running programs.
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

	// Whether the instance passes its graph's target (section 3.1): any instance, or the chosen entity.
	internal bool PassesTarget(GraphInstance instance) =>
		!Configuration.Targets.TryGetValue(instance.Graph, out ulong entity) || entity == 0 || entity == instance.Entity;

	private ProgramTrace TraceOf(GraphProgram program, ulong handle) =>
		program.Debug is { } trace && trace.IsCurrent(this, m_Version, handle) ? trace
			: ProgramTrace.Build(this, m_Version, program, handle, m_Enabled, m_RunTo, Configuration.Watches);

	// Records a stop. A resumable stop holds its chain, from the execution up through the senders it runs nested in; it ends
	// the step of its chain and an armed Break of its graph. Any stop removes the Run to Node.
	private void Add(DebugStopReason reason, DebugPlace place, Execution execution, bool resumable, string message, (ulong Document, uint Node)? breakpoint,
		DebugSnapshot snapshot)
	{
		List<DebugChange> changed = [];
		int more = 0;
		if (resumable && m_Changes.Remove(execution.RootSerial, out (List<DebugChange> Changes, int More) seen))
		{
			(changed, more) = seen;
		}
		List<DebugChainMember> chain = [];
		if (resumable)
		{
			for (Execution? member = execution; member != null; member = member.Parent)
			{
				chain.Add(new DebugChainMember(member, member.Serial));
			}
		}
		DebugStop stop = new()
		{
			Id = ++m_LastStopId,
			Reason = reason,
			Snapshot = snapshot,
			Chain = chain,
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
		if (resumable)
		{
			if (Stepping != null && execution.RootSerial == Stepping.RootSerial)
			{
				Stepping = null;
			}
			if (m_BreakArmed && BreakAdmits(execution))
			{
				m_BreakArmed = false;
				m_BreakDocument = 0;
			}
			if (m_Resuming && m_Reached == null)
			{
				m_Reached = stop;
			}
		}
		if (m_RunTo != null)
		{
			m_RunTo = null;
			m_Version++;
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
