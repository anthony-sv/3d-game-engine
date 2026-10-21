using System;
using System.Collections.Generic;

namespace Strada.VisualScripting;

// A graph running on an entity (spec section 5.1): its program, its variables, the persistent frame of its event graph
// (the outputs of impure and event nodes, and the state of DoOnce, DoN, FlipFlop, Gate and latent nodes), and its fibers.
internal sealed class GraphInstance
{
	// In the order their latent nodes became pending (Execution.PendingOrder).
	private readonly List<Execution> m_Fibers = [];
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
	internal IReadOnlyList<Execution> Fibers => m_Fibers;

	internal int FiberCount => m_Fibers.Count;

	internal Execution FiberAt(int index) => m_Fibers[index];

	// Adds a fiber at its place in the pending order: the end for a node that just became pending, the place it had for
	// one that keeps waiting (a WaitUntil checked again, a RetriggerableDelay's latest trigger).
	internal void AddFiber(Execution fiber)
	{
		int index = m_Fibers.Count;
		while (index > 0 && m_Fibers[index - 1].PendingOrder > fiber.PendingOrder)
		{
			index--;
		}
		m_Fibers.Insert(index, fiber);
	}

	internal bool RemoveFiber(Execution fiber) => m_Fibers.Remove(fiber);

	// A live edit: the instance runs the new program from now on, with the variables, frame and fibers carried over to it.
	internal void Swap(GraphProgram program, Slot[] variables, Frame frame, List<Execution> fibers)
	{
		Program = program;
		Variables = variables;
		Frame = frame;
		m_Fibers.Clear();
		m_Fibers.AddRange(fibers);
	}

	// Kills the instance: its fibers are handed back (to be released) and it never owns another.
	internal List<Execution> Kill()
	{
		IsAlive = false;
		List<Execution> fibers = [.. m_Fibers];
		m_Fibers.Clear();
		return fibers;
	}

	// Whether the node may log its once-per-instance warning now (spec section 5.3, non-aborting errors).
	internal bool ShouldWarn(uint node) => (m_Warned ??= []).Add(node);

	internal object? ReadVariable(int index) => SlotValues.ToValue(Variables[index], Program.Variables[index].Type);

	internal int FindVariable(string name) => Program.FindVariable(name);

	public override string ToString() => FormattableString.Invariant($"{Program.Name} on entity {Entity}");
}
