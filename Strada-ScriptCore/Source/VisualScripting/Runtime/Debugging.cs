using System.Collections.Generic;

namespace Strada.VisualScripting;

// The runtime's side of the debugger (spec-slice4 sections 4 and 7.5): the executions running on the host stack, how each
// run is traced, debug fibers continued and released, and the protocol behind VisualScriptDebug.
internal sealed partial class VisualScriptRuntime
{
	// The executions running now, outermost first (Execute pushes and pops them): what lies below a stop on the host stack.
	private readonly List<Execution> m_Running = [];

	internal IReadOnlyList<Execution> Running => m_Running;

	internal IVisualScriptDebugger? Debugger => m_Debugger;

	internal IEnumerable<GraphInstance> Instances => m_Instances.Values;

	// Attaches the debugger that traces executions from now on; null detaches it. Replacing or detaching a debugger releases
	// every debug fiber without running it (spec-slice4 section 4.11). Answers how many it released.
	internal int AttachDebugger(IVisualScriptDebugger? debugger)
	{
		int released = 0;
		if (m_Debugger != null && !ReferenceEquals(debugger, m_Debugger))
		{
			foreach (GraphInstance instance in m_Instances.Values)
			{
				released += ReleaseDebugFibers(instance, "the debugger detached");
			}
		}
		m_Debugger = debugger;
		return released;
	}

	// VisualScriptDebug (spec-slice4 sections 7.7 and 7.8): the reply to any request.
	internal HostReply Debug(System.ReadOnlySpan<byte> request) => HostReply.Success(DebugProtocol.Handle(this, request));

	// Continues a debug fiber where it stopped (spec-slice4 sections 4.5 and 4.12): with the budget it had left, Time.DeltaTime
	// as it read it and its run modes decided again, its first stop point passed. True when it ended in an aborting error
	// (logged once, released). The execution may stop again (a debug fiber again), suspend, or end; it must not be used after.
	internal bool ContinueStopped(Execution stopped)
	{
		stopped.Instance.RemoveDebugFiber(stopped);
		stopped.SkipStop = true;
		Trace(stopped);
		float deltaTime = m_Environment.GetDeltaTime();
		m_Environment.SetDeltaTime(stopped.SavedDeltaTime);
		GraphProgram program = stopped.Program;
		Enter(program);
		try
		{
			return Finish(stopped, Execute(stopped, true, stopped.SavedSteps));
		}
		finally
		{
			Leave(program);
			m_Environment.SetDeltaTime(deltaTime);
		}
	}

	// How the execution's run is traced, as the attached debugger decides (spec-slice4 section 3.2).
	private void Trace(Execution execution)
	{
		DebugMode mode = m_Debugger?.Attach(execution) ?? DebugMode.None;
		execution.Mode = mode;
		execution.Debugger = mode == DebugMode.None ? null : m_Debugger;
	}

	// Releases the instance's debug fibers without running them, in the order they stopped; answers how many.
	private int ReleaseDebugFibers(GraphInstance instance, string reason)
	{
		int released = 0;
		while (instance.DebugFibers.Count > 0)
		{
			ReleaseDebugFiber(instance.DebugFibers[0], reason);
			released++;
		}
		return released;
	}

	private void ReleaseDebugFiber(Execution stopped, string reason)
	{
		stopped.Instance.RemoveDebugFiber(stopped);
		m_Debugger?.OnStoppedReleased(stopped, reason);
		Release(stopped);
	}

	// The node a stopped execution stands at: the node it enters, or the Sequence or loop whose flow entry it continues.
	private static CompiledNode StopNode(Execution stopped)
	{
		CompiledGraph graph = stopped.TopFrame.Graph;
		return graph.Nodes[stopped.Node >= 0 ? stopped.Node : stopped.Flow[stopped.FlowCount - 1].Node];
	}

	// The document a node belongs to: its library's handle, or the instance's graph handle for the program's own nodes.
	private static ulong DocumentOf(GraphInstance instance, CompiledNode node) => node.Owner.Graph != 0 ? node.Owner.Graph : instance.Graph;
}
