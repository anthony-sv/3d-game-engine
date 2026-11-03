using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

namespace Strada.VisualScripting;

// Why a stop was made (spec-slice4 section 7.8, Stop).
internal enum DebugStopReason : byte
{
	Breakpoint,
	Step,
	Break,
	RunTo,
	Error,
	Passed,
}

// Where a snapshotted execution stood (spec-slice4 section 1): entering a node, continuing a flow entry (a Sequence or a
// loop), or failing at a node.
internal enum DebugPlace : byte
{
	Enter,
	Continue,
	Error,
}

// A value a stopped execution reads that something else changed while it was stopped (spec-slice4 section 4.7): a variable
// (Node 0, Name its name) or an output of an event or impure node of its instance's event graph (the node's ID and the pin),
// of the instance of the entity.
internal readonly record struct DebugChange(ulong Entity, uint Node, string Name);

// A literal input of a stopped execution's node that a live edit changed (spec-slice4 section 4.8): the pin, its type, and its
// literal before and after the edit.
internal readonly record struct DebugEdit(string Pin, VisualType Type, Slot Before, Slot After);

// A member of a stopped chain (spec-slice4 section 1): the execution and its serial (a pooled execution is the member only
// while its serial matches).
internal readonly record struct DebugChainMember(Execution Execution, ulong Serial)
{
	// Whether the member still waits as a debug fiber.
	internal bool IsWaiting => Execution.Serial == Serial && Execution.IsDebugFiber;
}

// One frame of a snapshotted execution, as it was (spec-slice4 section 5.1): its graph, copies of its slots and stamps, the
// call node in the caller's graph (-1 at the bottom) and where its flow-stack segment starts.
internal sealed record DebugSnapshotFrame(CompiledGraph Graph, Slot[] Slots, ulong[]? Stamps, int CallNode, int FlowBase);

// One execution of a snapshot: its instance's entity and graph handle, the program it ran, its frames (bottom first, as on its
// call stack), its instance's variables, its flow entries, where it stood (Node is the node it entered, failed at or waits in;
// for a Continue the Sequence or loop node, with Exit the exit it takes, -1 when its condition decides) and its start.
// Waiting: it runs the member above it nested, from its Send or Broadcast. NativeAbove: the title of the node through whose
// native code it runs the member above (null when it is the innermost or runs it nested from the VM).
internal sealed class DebugSnapshotMember
{
	internal required ulong Serial { get; init; }

	internal required ulong Entity { get; init; }

	internal required ulong Graph { get; init; }

	internal required GraphProgram Program { get; init; }

	internal required DebugSnapshotFrame[] Frames { get; init; }

	internal required Slot[] Variables { get; init; }

	internal required FlowEntry[] Flow { get; init; }

	internal required DebugPlace Place { get; init; }

	internal required CompiledNode? Node { get; init; }

	internal required int Entry { get; init; }

	internal required int Exit { get; init; }

	internal required ExecutionStart Start { get; init; }

	internal bool Waiting { get; init; }

	internal string? NativeAbove { get; init; }
}

// What a stop saw (spec-slice4 section 5.1): its executions, innermost first. Slots copy references, not objects.
internal sealed class DebugSnapshot
{
	internal DebugSnapshot(DebugSnapshotMember[] members)
	{
		Members = members;
	}

	internal DebugSnapshotMember[] Members { get; }

	// The snapshot of an execution at its stop point or failing node, with the executions below it on the host stack
	// (running, innermost first) down to the bottom: VM-nested senders waiting in their Send, executions nested through
	// native code below the title of the member node that called it.
	internal static DebugSnapshot Take(Execution execution, DebugPlace place, IReadOnlyList<Execution> running, int below)
	{
		List<DebugSnapshotMember> members = [Capture(execution, place, waiting: false, nativeAbove: null)];
		Execution above = execution;
		for (int index = below; index >= 0; index--)
		{
			Execution member = running[index];
			bool nested = ReferenceEquals(above.Parent, member);
			members.Add(Capture(member, PlaceOf(member), nested, nested ? null : PositionOf(member)?.Title));
			above = member;
		}
		return new DebugSnapshot([.. members]);
	}

	private static DebugPlace PlaceOf(Execution execution) => execution.Node < 0 && execution.FlowCount > execution.Top.FlowBase ? DebugPlace.Continue : DebugPlace.Enter;

	// The node an execution that runs now evaluates: the member, Send or call node whose run reached the one above it.
	private static CompiledNode? PositionOf(Execution execution)
	{
		CompiledGraph graph = execution.TopFrame.Graph;
		if (execution.Current is { } current && ReferenceEquals(current.Owner, graph))
		{
			return current;
		}
		return execution.Node >= 0 ? graph.Nodes[execution.Node] : null;
	}

	private static DebugSnapshotMember Capture(Execution execution, DebugPlace place, bool waiting, string? nativeAbove)
	{
		DebugSnapshotFrame[] frames = new DebugSnapshotFrame[execution.CallCount];
		for (int index = 0; index < frames.Length; index++)
		{
			CallRecord record = execution.Calls[index];
			Frame frame = record.Frame;
			frames[index] = new DebugSnapshotFrame(frame.Graph, (Slot[])frame.Slots.Clone(), (ulong[]?)frame.Stamps?.Clone(), record.CallNode, record.FlowBase);
		}
		CompiledGraph top = execution.TopFrame.Graph;
		CompiledNode? node;
		int exit = -1;
		switch (place)
		{
			case DebugPlace.Continue:
				FlowEntry entry = execution.Flow[execution.FlowCount - 1];
				node = top.Nodes[entry.Node];
				exit = ContinueExit(entry, node);
				break;
			case DebugPlace.Error:
				node = PositionOf(execution);
				break;
			default:
				node = waiting ? PositionOf(execution) : top.Nodes[execution.Node];
				break;
		}
		return new DebugSnapshotMember
		{
			Serial = execution.Serial,
			Entity = execution.Instance.Entity,
			Graph = execution.Instance.Graph,
			Program = execution.Program,
			Frames = frames,
			Variables = (Slot[])execution.Instance.Variables.Clone(),
			Flow = execution.Flow.AsSpan(0, execution.FlowCount).ToArray(),
			Place = place,
			Node = node,
			Entry = place == DebugPlace.Enter && !waiting ? execution.Entry : -1,
			Exit = exit,
			Start = execution.Start,
			Waiting = waiting,
			NativeAbove = nativeAbove,
		};
	}

	// The exit a Continue position takes: a Sequence's next output, a loop's Body or Completed; -1 for a While, whose
	// condition decides.
	internal static int ContinueExit(in FlowEntry entry, CompiledNode node) => entry.Kind switch
	{
		FlowKind.Sequence => entry.Index,
		FlowKind.ForLoop => entry.Break || entry.Index == entry.Last ? 1 : 0,
		FlowKind.ForEach => entry.Break || entry.Index + 1 >= entry.Items!.Length ? 1 : 0,
		_ => -1,
	};

	// What changed in the live values a stopped execution reads since the snapshot (spec-slice4 section 4.7): its instance's
	// variables and the outputs of event and impure nodes of the instance's event-graph frame. Values compare by their bits,
	// objects by reference. After a live edit (section 4.8) the snapshot's values are read across it as the edit carried them:
	// variables by ID, then name and type, outputs by node ID and type and by pin name and type; what the edit added or reset
	// is no change made elsewhere. Each change is listed once (members of one instance see the same ones), at most max; more
	// counts the others.
	internal static void AddChanges(DebugSnapshotMember member, GraphInstance instance, List<DebugChange> changes, int max, ref int more)
	{
		CompiledVariable[] variables = instance.Program.Variables;
		// Null while no edit replaced the program: the variables are where they were.
		int[]? matches = ReferenceEquals(member.Program, instance.Program) ? null
			: VisualScriptRuntime.MatchVariables(member.Program, instance.Program);
		for (int index = 0; index < variables.Length; index++)
		{
			int old = matches?[index] ?? index;
			if (old >= 0 && old < member.Variables.Length && !SameSlot(member.Variables[old], instance.Variables[index]))
			{
				Add(changes, new DebugChange(instance.Entity, 0, variables[index].Name), max, ref more);
			}
		}
		DebugSnapshotFrame events = member.Frames[0];
		Slot[] live = instance.Frame.Slots;
		bool edited = !ReferenceEquals(events.Graph, instance.Frame.Graph);
		foreach (CompiledNode node in instance.Frame.Graph.Nodes)
		{
			if (node.Pure || node.IsSplit)
			{
				continue;
			}
			CompiledNode? before = edited ? VisualScriptRuntime.Find(events.Graph, node) : node;
			if (before == null)
			{
				continue;
			}
			for (int output = 0; output < node.OutputCount; output++)
			{
				int match = edited ? VisualScriptRuntime.MatchOutput(before, node, output) : output;
				if (match >= 0 && !SameSlot(events.Slots[before.OutputBase + match], live[node.OutputBase + output]))
				{
					Add(changes, new DebugChange(instance.Entity, node.Id, node.OutputPins[output]), max, ref more);
				}
			}
		}
	}

	private static void Add(List<DebugChange> changes, DebugChange change, int max, ref int more)
	{
		if (changes.Contains(change))
		{
			return;
		}
		if (changes.Count < max)
		{
			changes.Add(change);
		}
		else
		{
			more++;
		}
	}

	// The unlinked (literal) inputs of a frame's node whose literal a live edit changed (spec-slice4 section 4.8): node is the
	// node as the snapshot saw it and current the compiled graph the execution runs now. Before is the literal of the program
	// the snapshot was taken in, After the current one; a pin linked in either program, or of another type now, is no edited
	// literal.
	internal static List<DebugEdit> EditedInputs(CompiledNode node, CompiledGraph current)
	{
		List<DebugEdit> edits = [];
		int index = current.FindNode(node.Id);
		if (index < 0 || ReferenceEquals(current, node.Owner))
		{
			return edits;
		}
		CompiledNode now = current.Nodes[index];
		for (int input = 0; input < now.InputPins.Length; input++)
		{
			int before = Array.IndexOf(node.InputPins, now.InputPins[input]);
			if (before < 0 || IsLinked(now, input) || IsLinked(node, before) || node.InputTypes[before].Id != now.InputTypes[input].Id)
			{
				continue;
			}
			Slot old = node.Owner.Template[node.InputBase + before];
			Slot edited = current.Template[now.InputBase + input];
			if (!SameSlot(old, edited))
			{
				edits.Add(new DebugEdit(now.InputPins[input], now.InputTypes[input], old, edited));
			}
		}
		return edits;
	}

	// Whether a staged input of a node takes its value from a link (an operand loads it) or means Self.
	private static bool IsLinked(CompiledNode node, int input)
	{
		if (input == 0 && node.TargetIsSelf)
		{
			return true;
		}
		int slot = node.InputBase + input;
		foreach (Operand operand in node.Operands)
		{
			if (operand.Target == slot)
			{
				return true;
			}
		}
		return false;
	}

	// Equal bits of the value part and the same object.
	internal static bool SameSlot(Slot a, Slot b)
	{
		ref byte left = ref Unsafe.As<Slot, byte>(ref a);
		ref byte right = ref Unsafe.As<Slot, byte>(ref b);
		return Unsafe.ReadUnaligned<ulong>(ref left) == Unsafe.ReadUnaligned<ulong>(ref right) &&
			Unsafe.ReadUnaligned<ulong>(ref Unsafe.Add(ref left, 8)) == Unsafe.ReadUnaligned<ulong>(ref Unsafe.Add(ref right, 8)) &&
			ReferenceEquals(a.Object, b.Object);
	}
}

// A stop (spec-slice4 sections 1 and 4.2): resumable (its chain's executions are debug fibers until it continues or they are
// released), passed (a breakpoint an execution that cannot stop ran past) or an error stop; the last two are read-only. A
// chain that loses a member (section 4.3) changes the stop: its snapshot, chain and message follow what is left.
internal sealed class DebugStop
{
	internal required int Id { get; init; }

	internal required DebugStopReason Reason { get; init; }

	// The snapshot's members are the chain's, in the chain's order, for a resumable stop.
	internal required DebugSnapshot Snapshot { get; set; }

	// The chain of a resumable stop, from its leaf (where the flow stands) to its root; empty for a read-only stop.
	internal List<DebugChainMember> Chain { get; set; } = [];

	// The leaf's serial (the stopped execution's, for a read-only stop).
	internal required ulong Serial { get; set; }

	internal bool Resumable => Chain.Count > 0;

	// The leaf and the root of a resumable stop's chain.
	internal Execution? Execution => Chain.Count > 0 ? Chain[0].Execution : null;

	internal Execution? Root => Chain.Count > 0 ? Chain[^1].Execution : null;

	internal required ulong Entity { get; set; }

	internal required ulong Graph { get; set; }

	internal required DebugPlace Place { get; init; }

	// The error of an error stop, why a passed stop could not stop, what became of a chain that lost a member; "" otherwise.
	internal required string Message { get; set; }

	// The breakpoint that stopped it (its document and node); null for other reasons.
	internal (ulong Document, uint Node)? Breakpoint { get; init; }

	internal bool Migrated { get; set; }

	internal IReadOnlyList<DebugChange> Changed { get; init; } = [];

	internal int ChangedMore { get; init; }

	internal DebugSnapshotMember Leaf => Snapshot.Members[0];
}
