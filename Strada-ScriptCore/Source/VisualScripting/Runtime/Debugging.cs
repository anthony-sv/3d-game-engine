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

	// The last working program of every loaded graph handle, with the handle: what a node of a document runs as when the graph
	// next runs (a graph whose game assembly changed has none until it compiles again).
	internal IEnumerable<(ulong Handle, GraphProgram Program)> EffectivePrograms
	{
		get
		{
			foreach ((ulong handle, GraphEntry entry) in m_Graphs)
			{
				if (entry.Effective is { } program)
				{
					yield return (handle, program);
				}
			}
		}
	}

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

	// Continues a stopped chain from its root (spec-slice4 sections 4.3, 4.5 and 4.12): with the budget it had left,
	// Time.DeltaTime as it read it and the run modes of each member decided again as it continues. The leaf passes the stop
	// point it stands at, so every resume runs at least one instruction; a leaf that waits inside its Send (its receiver was
	// released) is inside its instruction already. The chain may stop again (debug fibers again), suspend, end or abort (the
	// error logged once); the root must not be used after.
	internal ExecutionResult ContinueStopped(Execution root, Execution leaf)
	{
		root.Instance.RemoveDebugFiber(root);
		leaf.SkipStop = leaf.Delivery == null;
		Trace(root);
		float deltaTime = m_Environment.GetDeltaTime();
		m_Environment.SetDeltaTime(root.SavedDeltaTime);
		GraphProgram program = root.Program;
		Enter(program);
		try
		{
			ExecutionResult result = Execute(root, true, root.SavedSteps);
			Finish(root, result);
			return result;
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

	// A debug fiber leaves without running further, and its chain loses it (spec-slice4 section 4.3): its receiver, if it still
	// waits, becomes the root of a chain of its own, with the budget and Time.DeltaTime every member saved when it stopped; the
	// sender above it counts the delivery as delivered when it continues.
	private void ReleaseDebugFiber(Execution stopped, string reason)
	{
		stopped.Instance.RemoveDebugFiber(stopped);
		if (stopped.Delivery is { IsWaiting: true } delivery)
		{
			Execution root = delivery.Receiver;
			root.Parent = null;
			for (Execution? member = root; member != null; member = member.Delivery is { IsWaiting: true } next ? next.Receiver : null)
			{
				member.RootSerial = root.Serial;
			}
		}
		m_Debugger?.OnStoppedReleased(stopped, reason);
		Release(stopped);
	}

	// The node a stopped execution stands at: the node it enters (the Send or Broadcast a waiting sender waits in), or the
	// Sequence or loop whose flow entry it continues.
	internal static CompiledNode StopNode(Execution stopped)
	{
		CompiledGraph graph = stopped.TopFrame.Graph;
		return graph.Nodes[stopped.Node >= 0 ? stopped.Node : stopped.Flow[stopped.FlowCount - 1].Node];
	}

	// The document a node belongs to: its library's handle, or the instance's graph handle for the program's own nodes.
	private static ulong DocumentOf(GraphInstance instance, CompiledNode node) => node.Owner.Graph != 0 ? node.Owner.Graph : instance.Graph;
}
