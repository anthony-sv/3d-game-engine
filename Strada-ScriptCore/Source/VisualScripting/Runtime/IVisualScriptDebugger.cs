using System;

namespace Strada.VisualScripting;

// How one run of an execution is traced (spec-slice4 section 3.2), decided when it starts and at every resume. None runs it
// untraced: the interpreter then runs its Untraced instantiation, which pays nothing for the debugger.
[Flags]
internal enum DebugMode : byte
{
	None = 0,
	// Stop points are checked (spec-slice4 section 4.1).
	Break = 1,
	// Flow is reported (OnStep, OnEvaluated) and call frames are reported before they return to their pool.
	Record = 2,
	// Steps and values are reported for a recording.
	Values = 4,
}

// The debug hook, version 2 (spec-slice4 section 7.4): one debugger per runtime chooses how each run of an execution is
// traced, decides at the stop points of Break runs whether an execution stops there, and hears of the flow of Record and
// Values runs. Every traced run also stamps the frames it computes in (Frame.Stamps), whatever its modes. Nodes name their
// document through CompiledNode.Owner (Graph: 0 for the instance's own document, else the handle of the function library
// whose function the node is part of) and their ID in it. Main thread only, like the runtime.
internal interface IVisualScriptDebugger
{
	// When an execution starts (an event, a Tick or timeline output, a Send or Broadcast receiver, SendEvent) and when a fiber
	// or a debug fiber resumes: how this run is traced. Break needs the program's breakpoint tables (CompiledGraph.Debug),
	// which only VisualScriptDebugger builds: a Break run of a graph without them checks no stop point.
	DebugMode Attach(Execution execution);

	// Whether every stop point of a Break run asks ShouldStop, whatever the breakpoint tables hold (spec-slice4 section 4.1):
	// a step or a Break is armed.
	bool Armed { get; }

	// At a stop point of a Break run whose cheap checks matched (spec-slice4 section 4.1): true stops the execution there.
	bool ShouldStop(Execution execution);

	// A Record or Values run left the node through an exit (-1: its chain ended there, it suspended there, or it entered a
	// function): continuations as their Sequence or loop node, returns as the call's Then, a resumed fiber as its latent node,
	// a Tick as its Tween or Interval.
	void OnStep(Execution execution, CompiledNode node, int exit);

	// A Record or Values run evaluated a pure node: its outputs are slots[node.OutputBase..].
	void OnEvaluated(Execution execution, CompiledNode node, Slot[] slots);

	// A Record run's call frame is about to return to its pool.
	void OnFrameReleasing(Execution execution, Frame frame);

	// An execution ended in an aborting error, traced or not; its frames are intact. A nested execution's error is reported
	// again by the executions it aborts, with the same ExecutionError object.
	void OnAborted(Execution execution);

	// A debug fiber left its instance without running further (its instance died, a live edit cancelled it, the debugger
	// detached).
	void OnStoppedReleased(Execution execution, string reason);

	// A live edit carried a debug fiber over to the instance's new program (spec-slice4 section 4.8): it continues with it.
	void OnStoppedMigrated(Execution execution);

	// A traced run suspended in a latent node: the execution is a fiber of its instance now (spec-slice4 section 4.6, a step
	// that waits).
	void OnParked(Execution execution);

	// A traced execution returns to the runtime's pool: it ended, aborted, or its fiber or debug fiber was discarded or
	// released (spec-slice4 section 4.5, the end of a step).
	void OnReleased(Execution execution);
}
