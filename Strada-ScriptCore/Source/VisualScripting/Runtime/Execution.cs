using System;

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
}

// An aborting error, ready to be logged once (spec section 5.3, Errors).
internal sealed record ExecutionError(string Message);

// One execution (spec section 5.3): its own flow stack and call stack, and the position of the flow. All of it lives on
// the heap, so an execution can stop at any node and continue later, as a fiber, exactly where it was. Executions are
// pooled by their runtime: starting one does not allocate once the pool holds enough.
internal sealed class Execution
{
	private const int InitialCapacity = 8;

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

	internal ExecutionError? Error { get; set; }

	// Where a fiber continues: the ID of the latent node it waits in and the pin it continues at. Positions that outlive a
	// program are node IDs and pin names (spec section 5.6).
	internal (uint Node, string Pin) SuspendedPosition
	{
		get
		{
			if (SuspendedAt < 0)
			{
				throw new InvalidOperationException("the execution is not suspended");
			}
			CompiledNode node = TopFrame.Graph.Nodes[SuspendedAt];
			return (node.Id, node.ExitPins[node.ContinuationExit]);
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

	internal FlowEntry PopFlow()
	{
		FlowEntry entry = Flow[--FlowCount];
		Flow[FlowCount] = default;
		return entry;
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
		Error = null;
		Instance = null!;
		Program = null!;
	}
}
