using System;
using System.Collections.Generic;

namespace Strada.VisualScripting;

// What a live edit carries from one program to another (spec section 5.6, spec-slice4 section 8.3), the same for every
// instance: made once per pair of programs, and its report is the receipt's Variables and States. Variables continue by ID
// (with the same type), then by name and type, each old variable once; the state of DoOnce, DoN, FlipFlop and Gate nodes,
// a Timeline's state and the stored outputs of impure and event nodes continue for nodes whose ID and type are unchanged
// (disabled only if they were), outputs also by pin name and type. What a plan does not carry starts as in a new instance.
// Main thread only, like the runtime that makes it.
internal sealed class CarryPlan
{
	// The carries of function graphs, made when a fiber's call frame first needs one: by the old body.
	private Dictionary<CompiledGraph, GraphCarry>? m_Functions;

	private CarryPlan(GraphProgram from, GraphProgram to, int[] variableSources, GraphCarry events)
	{
		From = from;
		To = to;
		VariableSources = variableSources;
		Events = events;
		Variables = VariableReport.Of(from, to, variableSources);
		(KeptStates, ResetStates) = ReportStates(from.EventGraph, to.EventGraph);
	}

	internal GraphProgram From { get; }

	internal GraphProgram To { get; }

	// Per variable of the new program: the index of the old variable it continues, or -1 (it starts at its value).
	internal int[] VariableSources { get; }

	// What the event graph's frame carries.
	internal GraphCarry Events { get; }

	internal VariableReport Variables { get; }

	// The event graph's nodes with state (DoOnce, DoN, FlipFlop, Gate and Timeline) that keep it.
	internal int KeptStates { get; }

	// The old event graph's nodes with state that lose it: their ID went, or names a node of another type, or the edit
	// disabled the node.
	internal IReadOnlyList<NodeNote> ResetStates { get; }

	internal static CarryPlan Make(GraphProgram from, GraphProgram to) =>
		new(from, to, MatchVariables(from, to), GraphCarry.Make(from.EventGraph, to.EventGraph));

	// The variables of an instance of the old program as the new program starts them.
	internal Slot[] CarryVariables(Slot[] values)
	{
		CompiledVariable[] variables = To.Variables;
		Slot[] carried = new Slot[variables.Length];
		for (int index = 0; index < variables.Length; index++)
		{
			carried[index] = VariableSources[index] >= 0 ? values[VariableSources[index]] : variables[index].Initial;
		}
		return carried;
	}

	// What a call frame of an old function carries into a frame of the new program's function of the same document and ID.
	internal GraphCarry ForFunction(CompiledFunction from, CompiledFunction to)
	{
		m_Functions ??= [];
		if (!m_Functions.TryGetValue(from.Body, out GraphCarry? carry))
		{
			carry = GraphCarry.Make(from.Body, to.Body);
			m_Functions[from.Body] = carry;
		}
		return carry;
	}

	// For each variable of the target program, the index of the source program's variable it continues (by ID with the same
	// type, then by name and type, each source variable continued once), or -1 for a new one.
	private static int[] MatchVariables(GraphProgram source, GraphProgram target)
	{
		Dictionary<uint, int> byId = [];
		Dictionary<string, int> byName = new(StringComparer.Ordinal);
		for (int index = 0; index < source.Variables.Length; index++)
		{
			byId[source.Variables[index].Id] = index;
			byName[source.Variables[index].Name] = index;
		}
		CompiledVariable[] variables = target.Variables;
		int[] matches = new int[variables.Length];
		bool[] taken = new bool[source.Variables.Length];
		for (int index = 0; index < variables.Length; index++)
		{
			matches[index] = byId.TryGetValue(variables[index].Id, out int old) && SameType(source.Variables[old], variables[index]) ? old : -1;
			if (matches[index] >= 0)
			{
				taken[matches[index]] = true;
			}
		}
		for (int index = 0; index < variables.Length; index++)
		{
			if (matches[index] < 0 && byName.TryGetValue(variables[index].Name, out int old) && !taken[old] &&
				SameType(source.Variables[old], variables[index]))
			{
				matches[index] = old;
				taken[old] = true;
			}
		}
		return matches;
	}

	internal static bool SameType(CompiledVariable a, CompiledVariable b) => a.Type.Id == b.Type.Id;

	private static (int Kept, IReadOnlyList<NodeNote> Reset) ReportStates(CompiledGraph from, CompiledGraph to)
	{
		int kept = 0;
		List<NodeNote> reset = [];
		foreach (CompiledNode old in from.Nodes)
		{
			if (!HasState(old))
			{
				continue;
			}
			if (VisualScriptRuntime.Find(to, old) != null)
			{
				kept++;
				continue;
			}
			// A disabled node compiles to no state, so a node that has state and keeps its type lost it by being disabled.
			int index = to.FindNode(old.Id);
			string reason = index < 0 ? StateReset.Removed : to.Nodes[index].Type != old.Type ? StateReset.Changed : StateReset.Disabled;
			reset.Add(NodeNote.Of(old, reason));
		}
		return (kept, reset);
	}

	// Whether a node keeps state between executions that a live edit carries: a DoOnce, DoN, FlipFlop or Gate, or a Timeline.
	// A latent node's state is the fiber that waits in it, which moves with the fiber.
	private static bool HasState(CompiledNode node) => !node.IsSplit && ((node.Op != Opcode.Latent && node.StateSlot >= 0) || node.TimelineSlot >= 0);
}

// The reasons a node's state does not carry over (the receipt's States.Reset[].Reason).
internal static class StateReset
{
	internal const string Removed = "Removed";
	internal const string Changed = "Changed";
	internal const string Disabled = "Disabled";
}

// What one frame of a compiled graph carries into a frame of the graph that replaces it: slot copies in the order they
// apply (node state, then stored outputs), and the Timelines whose state continues with its time clamped into the new
// length (spec-slice3 section 3.8.6). The next advance writes a Timeline's outputs with its new curves.
internal sealed class GraphCarry
{
	private GraphCarry(SlotCarry[] states, TimelineCarry[] timelines, OutputCarry[] outputs)
	{
		States = states;
		Timelines = timelines;
		Outputs = outputs;
	}

	internal SlotCarry[] States { get; }

	internal TimelineCarry[] Timelines { get; }

	internal OutputCarry[] Outputs { get; }

	internal static GraphCarry Make(CompiledGraph source, CompiledGraph target)
	{
		List<SlotCarry> states = [];
		List<TimelineCarry> timelines = [];
		List<OutputCarry> outputs = [];
		foreach (CompiledNode node in target.Nodes)
		{
			// The Make and Break nodes of split pins have no state: the node whose pins they split keeps its outputs.
			if (node.IsSplit || VisualScriptRuntime.Find(source, node) is not { } old)
			{
				continue;
			}
			if (node.Op != Opcode.Latent && node.StateSlot >= 0 && old.StateSlot >= 0)
			{
				states.Add(new SlotCarry(node.StateSlot, old.StateSlot));
			}
			if (node.TimelineSlot >= 0 && old.TimelineSlot >= 0)
			{
				timelines.Add(new TimelineCarry(node.TimelineSlot, old.TimelineSlot, node.Timeline!.Definition.Length, node.Id));
			}
			if (node.Pure)
			{
				continue;
			}
			for (int output = 0; output < node.OutputCount; output++)
			{
				int match = VisualScriptRuntime.MatchOutput(old, node, output);
				if (match >= 0)
				{
					outputs.Add(new OutputCarry(node.OutputBase + output, old.OutputBase + match, node.Index, output));
				}
			}
		}
		return new GraphCarry([.. states], [.. timelines], [.. outputs]);
	}

	// Carries a frame of the old graph into a frame of the new one; a Timeline that has state reports its time, and the time
	// it was clamped from, to timelines when it is given.
	internal void Apply(Frame from, Frame to, List<TimelineNote>? timelines)
	{
		Slot[] source = from.Slots;
		Slot[] target = to.Slots;
		foreach (SlotCarry state in States)
		{
			target[state.To] = source[state.From];
		}
		foreach (TimelineCarry timeline in Timelines)
		{
			if (source[timeline.From].Object is not TimelineState state)
			{
				continue;
			}
			double time = state.Time;
			state.Time = Math.Clamp(time, 0.0, timeline.Length);
			target[timeline.To] = Slot.FromObject(state);
			timelines?.Add(new TimelineNote(timeline.Node, state.Time, time > timeline.Length || time < 0.0 ? time : null));
		}
		foreach (OutputCarry output in Outputs)
		{
			target[output.To] = source[output.From];
		}
	}
}

// A slot of the new frame and the slot of the old frame it continues.
internal readonly record struct SlotCarry(int To, int From);

// A Timeline's state slot, the old one it continues, the new length its time is clamped into and the node's ID.
internal readonly record struct TimelineCarry(int To, int From, double Length, uint Node);

// A stored output of the new frame: its slot, the old slot it continues, and the new node (its index) and output it is.
internal readonly record struct OutputCarry(int To, int From, int Node, int Output);

// A Timeline of an instance after an edit: its node's ID, its time, and the time beyond its new length it had (null when it
// was within it).
internal readonly record struct TimelineNote(uint Node, double Time, double? ClampedFrom);

// A node a receipt names: its document (0 for the graph's own), the function holding it (0 for the event graph), its ID and
// title, why it is named, and for a cancelled execution whether it was stopped.
internal sealed record NodeNote(ulong Document, uint Function, uint Node, string Title, string Reason, bool Stopped = false)
{
	internal static NodeNote Of(CompiledNode node, string reason, bool stopped = false) =>
		new(node.Owner.Graph, node.Owner.Function?.Id ?? 0, node.Id, node.Title, reason, stopped);
}

// Why a variable of the new program starts at its value although an old variable has its ID or its name: the old one has
// another type, or another new variable continues it.
internal enum VariableResetReason : byte
{
	TypeChanged,
	New,
}

internal readonly record struct MatchedVariable(string Name, string From);

// From and To are the type IDs of a TypeChanged variable; null for New.
internal readonly record struct ResetVariable(string Name, VariableResetReason Reason, string? From, string? To);

// What a plan does with the variables (the receipt's Variables): kept under their name, continued from a variable of another
// name (a rename the ID follows), reset, added (no old variable has the ID or the name) and removed (no new variable
// continues it, and none resets for its type).
internal sealed class VariableReport
{
	internal static VariableReport None { get; } = new();

	internal IReadOnlyList<string> Kept { get; private init; } = [];

	internal IReadOnlyList<MatchedVariable> Matched { get; private init; } = [];

	internal IReadOnlyList<ResetVariable> Reset { get; private init; } = [];

	internal IReadOnlyList<string> Added { get; private init; } = [];

	internal IReadOnlyList<string> Removed { get; private init; } = [];

	// The variables of a program that replaces none: every one is added.
	internal static VariableReport AllAdded(GraphProgram program)
	{
		List<string> added = [];
		foreach (CompiledVariable variable in program.Variables)
		{
			added.Add(variable.Name);
		}
		return new VariableReport { Added = added };
	}

	internal static VariableReport Of(GraphProgram from, GraphProgram to, int[] sources)
	{
		Dictionary<uint, int> byId = [];
		Dictionary<string, int> byName = new(StringComparer.Ordinal);
		for (int index = 0; index < from.Variables.Length; index++)
		{
			byId[from.Variables[index].Id] = index;
			byName[from.Variables[index].Name] = index;
		}
		List<string> kept = [];
		List<MatchedVariable> matched = [];
		List<ResetVariable> reset = [];
		List<string> added = [];
		bool[] accounted = new bool[from.Variables.Length];
		for (int index = 0; index < to.Variables.Length; index++)
		{
			CompiledVariable variable = to.Variables[index];
			if (sources[index] >= 0)
			{
				CompiledVariable old = from.Variables[sources[index]];
				accounted[sources[index]] = true;
				if (old.Name == variable.Name)
				{
					kept.Add(variable.Name);
				}
				else
				{
					matched.Add(new MatchedVariable(variable.Name, old.Name));
				}
				continue;
			}
			int sameId = byId.GetValueOrDefault(variable.Id, -1);
			int sameName = byName.GetValueOrDefault(variable.Name, -1);
			int retyped = sameId >= 0 && !CarryPlan.SameType(from.Variables[sameId], variable) ? sameId
				: sameName >= 0 && !CarryPlan.SameType(from.Variables[sameName], variable) ? sameName
				: -1;
			if (retyped >= 0)
			{
				accounted[retyped] = true;
				reset.Add(new ResetVariable(variable.Name, VariableResetReason.TypeChanged, from.Variables[retyped].Type.Id, variable.Type.Id));
			}
			else if (sameId >= 0 || sameName >= 0)
			{
				reset.Add(new ResetVariable(variable.Name, VariableResetReason.New, null, null));
			}
			else
			{
				added.Add(variable.Name);
			}
		}
		List<string> removed = [];
		for (int index = 0; index < from.Variables.Length; index++)
		{
			if (!accounted[index])
			{
				removed.Add(from.Variables[index].Name);
			}
		}
		return new VariableReport { Kept = kept, Matched = matched, Reset = reset, Added = added, Removed = removed };
	}
}
