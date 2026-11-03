using System;
using System.Collections.Generic;

namespace Strada.VisualScripting;

// The slots of one graph's nodes: the persistent frame of an instance's event graph, or the frame of one function call.
internal sealed class Frame
{
	internal Frame(CompiledGraph graph)
	{
		Graph = graph;
		Slots = new Slot[graph.Template.Length];
		Array.Copy(graph.Template, Slots, Slots.Length);
	}

	internal CompiledGraph Graph { get; }

	internal Slot[] Slots { get; }

	// By node index: the Serial of the execution whose traced run last evaluated the node in this frame (spec-slice4 section
	// 5.2), 0 for none. Made by the first traced run that uses the frame; null while no traced run did. A pooled call frame
	// clears it when it returns to its pool.
	internal ulong[]? Stamps { get; set; }

	// The stamps of the frame, made on first use.
	internal ulong[] StampsOrNew => Stamps ??= new ulong[Graph.Nodes.Length];
}

// The kinds of flow-stack entries (spec section 5.3): the continuations Sequence and the loops push.
internal enum FlowKind : byte
{
	Sequence,
	ForLoop,
	ForEach,
	While,
}

// A continuation on the flow stack. Loop entries hold their loop's state, so it travels with the execution.
internal struct FlowEntry
{
	internal FlowKind Kind;

	internal int Node;

	// Sequence: the next output. ForLoop: the current index. ForEach: the current element's index.
	internal int Index;

	// ForLoop: the last index.
	internal int Last;

	internal bool Break;

	// ForEach: the snapshot of the array.
	internal Array? Items;
}

// A frame on an execution's call stack: the event graph's frame at the bottom, then one per function call (impure calls,
// and pure calls while a plan evaluates them).
internal struct CallRecord
{
	internal Frame Frame;

	// The Function.Call node in the caller's graph; -1 at the bottom.
	internal int CallNode;

	// The flow-stack entries below this belong to the callers: the frame's segment starts here.
	internal int FlowBase;

	// Pure calls: the caller's plan and where it continues.
	internal int[]? ResumePlan;

	internal int ResumeIndex;
}

// How a run of an execution ended.
internal enum ExecutionResult
{
	Ended,
	// A latent node suspended it: the execution is a fiber of its instance now.
	Suspended,
	// An aborting error stopped it (the error is in Execution.Error).
	Aborted,
	// The debugger stopped it before an instruction (spec-slice4 section 7.1): it is a debug fiber of its instance now, and
	// only the debugger resumes it.
	Stopped,
}

// What started an execution (spec-slice4 section 5.1).
internal enum StartKind : byte
{
	// An event node: a lifecycle, input or custom event the engine dispatched.
	Event,
	// A Tween's or Interval's Tick output.
	Tick,
	// An exec output of a Timeline (an event track, Update or Finished).
	Timeline,
	// A fiber resumed at its latent node: Origin is the start of the execution that suspended.
	Resume,
	// VisualScriptComponent.SendEvent from C#.
	SendEvent,
	// An Event.Send or Event.Broadcast of another execution: Origin is that node.
	Delivery,
}

// The start of an execution: its kind, the node it started at (an event node, the Tween, Interval or Timeline whose exit
// Exit fired, the latent node a fiber resumed in) and, for a resume or a delivery, the node it came from (its ID and the
// handle of its document; 0 when unknown).
internal readonly record struct ExecutionStart(StartKind Kind, CompiledNode? Node, int Exit, uint OriginNode, ulong OriginDocument);

// An aborting error (spec section 5.3, Errors; spec-slice4 section 8.2): the graph the instance runs and the entity, the
// failing node (the document holding it, the function it is in, 0 for the event graph, its ID and title), the bare message
// and the calls that led to the node (innermost last, at most MaxCalls), and Text, the line logged once.
internal sealed record ExecutionError(ulong Graph, ulong Entity, ulong Document, uint Function, uint Node, string Title, string Message,
	LiveCallFrame[] Calls, string Text)
{
	internal const int MaxCalls = 8;
}

// What one running Event.Broadcast node keeps apart from its frame: its receiver snapshot and a copy of its arguments. A
// broadcast whose receiver stopped keeps it until the broadcast ends (spec-slice4 section 4.3).
internal sealed class BroadcastScratch
{
	internal List<GraphInstance> Receivers { get; } = [];

	internal Slot[] Arguments { get; set; } = [];
}

// What a sender waits for inside its Event.Send or Event.Broadcast while the receiver it runs nested is stopped (spec-slice4
// section 4.3): the Send or Broadcast node (an index of the sender's top frame's graph), the receiver and its serial (it is
// the receiver only while both match and it is still a debug fiber), and for a Broadcast its kept scratch, the receiver it
// delivers to next and how many received the event so far, the stopped one included.
internal sealed class PendingDelivery
{
	internal PendingDelivery(int node, Execution receiver)
	{
		Node = node;
		Receiver = receiver;
		ReceiverSerial = receiver.Serial;
	}

	internal int Node { get; set; }

	internal Execution Receiver { get; }

	internal ulong ReceiverSerial { get; }

	internal BroadcastScratch? Scratch { get; set; }

	internal int NextReceiver { get; set; }

	internal int Count { get; set; }

	// Whether the receiver still waits to continue: it was not released (its instance died, an edit cancelled it) meanwhile.
	internal bool IsWaiting => Receiver.Serial == ReceiverSerial && Receiver.IsDebugFiber;
}

// One execution (spec section 5.3): its own flow stack and call stack, and the position of the flow. All of it lives on
// the heap, so an execution can stop at any node and continue later, as a fiber, exactly where it was. Executions are
// pooled by their runtime: starting one does not allocate once the pool holds enough.
internal sealed class Execution
{
	// The entries of a latent node below its exec inputs (0 and up): a fiber runs the node again to continue at its
	// continuation, or to check a WaitUntil's condition first; a Tick execution starts at a Tween or Interval and leaves
	// through its Tick output, or at a Timeline and leaves through its TickExit.
	internal const int ResumeEntry = -1;
	internal const int RecheckEntry = -2;
	internal const int TickEntry = -3;

	private const int InitialCapacity = 8;

	internal Execution()
	{
		FiberNode = new LinkedListNode<Execution>(this);
	}

	// The execution's number, new each time a pooled execution starts: a reference kept across dispatches still means the
	// same execution only while it matches.
	internal ulong Serial { get; set; }

	// The execution's place in its instance's fibers (GraphInstance) while it is one; made once and reused with the execution,
	// so becoming a fiber, resuming and being discarded cost constant time and allocate nothing.
	internal LinkedListNode<Execution> FiberNode { get; }

	// Whether the execution runs on the host stack now (VisualScriptRuntime.Execute). A fiber keeps its place among its
	// instance's fibers while its WaitUntil condition is checked, and a RetriggerableDelay's latest trigger takes its place
	// as it suspends: whatever runs meanwhile never releases or advances a running execution.
	internal bool IsRunning { get; set; }

	// How the current run is traced (spec-slice4 section 3.2), and the debugger tracing it: non-null exactly when Mode is not
	// None.
	internal DebugMode Mode { get; set; }

	internal IVisualScriptDebugger? Debugger { get; set; }

	// The execution whose Event.Send or Event.Broadcast runs this one nested (a receiver); null for one the engine started or
	// one nested through native code.
	internal Execution? Parent { get; set; }

	// The Serial of the outermost execution of the receivers' chain: the execution's own for one without a Parent.
	internal ulong RootSerial { get; set; }

	// Whether the execution may stop at a breakpoint in its current run (spec-slice4 section 4.4): it is outermost on the host
	// stack, or a receiver whose sender can stop (they stop together, as a chain). One that cannot stop passes its
	// breakpoints, snapshotted.
	internal bool CanStop { get; set; }

	// A stopped execution: the steps its budget had left and Time.DeltaTime as it read it (spec-slice4 section 4.2), both
	// restored when it continues.
	internal int SavedSteps { get; set; }

	internal float SavedDeltaTime { get; set; }

	// The first stop point of the run passes: every resume runs at least one instruction (spec-slice4 section 4.1).
	internal bool SkipStop { get; set; }

	internal ExecutionStart Start { get; set; }

	// A sender stopped inside its Send or Broadcast because its receiver stopped (spec-slice4 section 4.3): the run that
	// continues it finishes the delivery first.
	internal PendingDelivery? Delivery { get; set; }

	// Whether the execution is one of its instance's debug fibers now (GraphInstance.AddDebugFiber).
	internal bool IsDebugFiber { get; set; }

	internal GraphInstance Instance { get; set; } = null!;

	// The program the execution started with (an instance's program may be replaced while it runs).
	internal GraphProgram Program { get; set; } = null!;

	internal CallRecord[] Calls { get; private set; } = new CallRecord[InitialCapacity];

	internal int CallCount { get; set; }

	internal FlowEntry[] Flow { get; private set; } = new FlowEntry[InitialCapacity];

	internal int FlowCount { get; set; }

	// The node the flow enters next and its exec input; Node is -1 when the chain ended.
	internal int Node { get; set; } = -1;

	internal int Entry { get; set; }

	// The node being evaluated, for the messages of errors.
	internal CompiledNode? Current { get; set; }

	// A suspended execution: the latent node it waits in (its frame is the top of the call stack).
	internal int SuspendedAt { get; set; } = -1;

	// The number of the instance's Update dispatches when the fiber's latent node became pending: only later ones advance
	// or resume it (spec section 5.2).
	internal ulong Since { get; set; }

	// What a fiber's latent node waits for (spec section 5.3, Timing). Delay and RetriggerableDelay: the time summed and
	// Duration. Tween: the time elapsed and Duration. Interval: the timer and Interval, Count, and the next Index.
	// WaitFrames: the updates left.
	internal double WaitTime { get; set; }

	internal double WaitLimit { get; set; }

	internal int WaitCount { get; set; }

	internal long WaitIndex { get; set; }

	internal ExecutionError? Error { get; set; }

	// A tick execution of a Timeline: the exit it leaves the node through (spec-slice3 section 3.8.5); -1 otherwise.
	internal int TickExit { get; set; } = -1;

	// Where a fiber continues: the ID of the latent node it waits in and the pin it continues at, "" for a Timeline's run,
	// which never continues. Positions that outlive a program are node IDs and pin names (spec section 5.6).
	internal (uint Node, string Pin) SuspendedPosition
	{
		get
		{
			if (SuspendedAt < 0)
			{
				throw new InvalidOperationException("the execution is not suspended");
			}
			CompiledNode node = TopFrame.Graph.Nodes[SuspendedAt];
			return (node.Id, node.ContinuationExit >= 0 ? node.ExitPins[node.ContinuationExit] : string.Empty);
		}
	}

	internal ref CallRecord Top => ref Calls[CallCount - 1];

	internal Frame TopFrame => Calls[CallCount - 1].Frame;

	// The function frames on the call stack (the bottom is the event graph's frame).
	internal int FunctionDepth => CallCount - 1;

	internal void PushCall(Frame frame, int callNode, int[]? resumePlan, int resumeIndex)
	{
		if (CallCount == Calls.Length)
		{
			CallRecord[] calls = Calls;
			Array.Resize(ref calls, calls.Length * 2);
			Calls = calls;
		}
		Calls[CallCount++] = new CallRecord
		{
			Frame = frame,
			CallNode = callNode,
			FlowBase = FlowCount,
			ResumePlan = resumePlan,
			ResumeIndex = resumeIndex,
		};
	}

	internal CallRecord PopCall()
	{
		CallRecord record = Calls[--CallCount];
		Calls[CallCount] = default;
		return record;
	}

	internal void PushFlow(in FlowEntry entry)
	{
		if (FlowCount == Flow.Length)
		{
			FlowEntry[] flow = Flow;
			Array.Resize(ref flow, flow.Length * 2);
			Flow = flow;
		}
		Flow[FlowCount++] = entry;
	}

	// Drops the entry on top: its continuation is done.
	internal void PopFlow()
	{
		Flow[--FlowCount] = default;
	}

	// Drops the entries of the top frame's segment (Function.Return discards the call's pending continuations).
	internal void TruncateFlow(int count)
	{
		Array.Clear(Flow, count, FlowCount - count);
		FlowCount = count;
	}

	// The index of the innermost entry of a loop node in the top frame's segment; -1 when the loop is not active there.
	internal int FindLoop(int node)
	{
		int floor = Calls[CallCount - 1].FlowBase;
		for (int index = FlowCount - 1; index >= floor; index--)
		{
			if (Flow[index].Node == node && Flow[index].Kind is FlowKind.ForLoop or FlowKind.ForEach)
			{
				return index;
			}
		}
		return -1;
	}

	internal void Reset()
	{
		Array.Clear(Calls, 0, CallCount);
		Array.Clear(Flow, 0, FlowCount);
		CallCount = 0;
		FlowCount = 0;
		Node = -1;
		Entry = 0;
		Current = null;
		SuspendedAt = -1;
		Since = 0;
		WaitTime = 0.0;
		WaitLimit = 0.0;
		WaitCount = 0;
		WaitIndex = 0;
		Error = null;
		TickExit = -1;
		Mode = DebugMode.None;
		Debugger = null;
		Parent = null;
		RootSerial = 0;
		CanStop = false;
		SavedSteps = 0;
		SavedDeltaTime = 0.0f;
		SkipStop = false;
		Start = default;
		Delivery = null;
		IsDebugFiber = false;
		Instance = null!;
		Program = null!;
	}
}
