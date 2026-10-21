using System;
using System.Collections.Generic;

namespace Strada.VisualScripting;

// A graph running on an entity (spec section 5.1): its program, its variables, the persistent frame of its event graph
// (the outputs of impure and event nodes, and the state of DoOnce, DoN, FlipFlop and Gate nodes), and its fibers.
internal sealed class GraphInstance
{
	private readonly List<Execution> m_Fibers = [];
	private HashSet<uint>? m_Warned;

	internal GraphInstance(ulong entity, GraphProgram program)
	{
		Entity = entity;
		Program = program;
		Variables = new Slot[program.Variables.Length];
		for (int index = 0; index < Variables.Length; index++)
		{
			Variables[index] = program.Variables[index].Initial;
		}
		Frame = new Frame(program.EventGraph);
	}

	internal ulong Entity { get; }

	internal GraphProgram Program { get; }

	internal Slot[] Variables { get; }

	internal Frame Frame { get; }

	// False once the instance was destroyed or replaced: its executions run to their end, but none of them can wait.
	internal bool IsAlive { get; private set; } = true;

	// The suspended executions the instance owns, in the order they suspended.
	internal IReadOnlyList<Execution> Fibers => m_Fibers;

	internal void AddFiber(Execution fiber)
	{
		m_Fibers.Add(fiber);
	}

	internal bool RemoveFiber(Execution fiber) => m_Fibers.Remove(fiber);

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
