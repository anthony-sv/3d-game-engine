using System;
using System.Collections.Generic;

namespace Strada.VisualScripting;

// A graph running on an entity (spec section 5.1): its program, its variables, the persistent frame of its event graph
// (the outputs of impure and event nodes, and the state of DoOnce, DoN, FlipFlop, Gate and latent nodes), and its fibers.
internal sealed class GraphInstance
{
	// In the order their latent nodes became pending (spec section 5.2): a node that becomes pending goes last, a WaitUntil
	// checked again keeps its place, and the latest trigger of a RetriggerableDelay takes the place of the fiber it replaces.
	// Each fiber brings its own list node (Execution.FiberNode), so every change costs constant time and one Update's pass
	// over the fibers costs time linear in their number.
	private readonly LinkedList<Execution> m_Fibers = new();
	private HashSet<uint>? m_Warned;

	internal GraphInstance(ulong entity, ulong graph, GraphProgram program)
	{
		Entity = entity;
		Graph = graph;
		Program = program;
		Variables = new Slot[program.Variables.Length];
		for (int index = 0; index < Variables.Length; index++)
		{
			Variables[index] = program.Variables[index].Initial;
		}
		Frame = new Frame(program.EventGraph);
	}

	internal ulong Entity { get; }

	// The graph handle whose program the instance runs: a live edit of the handle swaps it (spec section 5.6).
	internal ulong Graph { get; }

	internal GraphProgram Program { get; private set; }

	internal Slot[] Variables { get; private set; }

	internal Frame Frame { get; private set; }

	// False once the instance was destroyed or replaced: its executions run to their end, but none of them can wait.
	internal bool IsAlive { get; private set; } = true;

	// The number of Update dispatches the instance started; a fiber advances and resumes only in dispatches after the one
	// running when it suspended (spec section 5.2).
	internal ulong Updates { get; set; }

	// The suspended executions the instance owns, in the order their latent nodes became pending.
	internal IReadOnlyCollection<Execution> Fibers => m_Fibers;

	internal int FiberCount => m_Fibers.Count;

	// The first fiber in the pending order; its Next is the one after it.
	internal LinkedListNode<Execution>? FirstFiber => m_Fibers.First;

	// Whether the execution is one of the instance's fibers now.
	internal bool HasFiber(Execution fiber) => ReferenceEquals(fiber.FiberNode.List, m_Fibers);

	// A latent node of the fiber became pending: the fiber goes last in the pending order.
	internal void AddFiber(Execution fiber)
	{
		m_Fibers.AddLast(fiber.FiberNode);
	}

	// The fiber is no longer one of the instance's (it resumed, was discarded or is released); nothing for one that is not.
	internal void RemoveFiber(Execution fiber)
	{
		if (HasFiber(fiber))
		{
			m_Fibers.Remove(fiber.FiberNode);
		}
	}

	// A RetriggerableDelay's latest trigger: the new fiber waits in the place of the one it replaces, which leaves.
	internal void ReplaceFiber(Execution replaced, Execution latest)
	{
		m_Fibers.AddBefore(replaced.FiberNode, latest.FiberNode);
		m_Fibers.Remove(replaced.FiberNode);
	}

	// A live edit: the instance runs the new program from now on, with the variables and frame carried over to it (the
	// fibers that survive the edit stay in their places).
	internal void Swap(GraphProgram program, Slot[] variables, Frame frame)
	{
		Program = program;
		Variables = variables;
		Frame = frame;
	}

	// The instance is destroyed or replaced: it never owns another fiber (the runtime releases the ones it has).
	internal void Kill()
	{
		IsAlive = false;
	}

	// Whether the node may log its once-per-instance warning now (spec section 5.3, non-aborting errors).
	internal bool ShouldWarn(uint node) => (m_Warned ??= []).Add(node);

	internal object? ReadVariable(int index) => SlotValues.ToValue(Variables[index], Program.Variables[index].Type);

	internal int FindVariable(string name) => Program.FindVariable(name);

	public override string ToString() => FormattableString.Invariant($"{Program.Name} on entity {Entity}");
}
