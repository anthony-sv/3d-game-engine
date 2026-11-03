using System;
using System.Collections.Generic;

namespace Strada.VisualScripting;

// Live edit (spec section 5.6): a new working program of a graph handle replaces the program of every instance of the
// handle, as soon as no dispatch or execution of the old program is running. An instance keeps what still means the same
// in the new program: its variables by ID (then by name and type), the state and stored outputs of nodes whose ID and type
// are unchanged, and its fibers, when every position they hold still exists. Positions are carried over by node ID. A node
// the edit disables counts as removed, and one it enables again as new: neither carries anything over.
internal sealed partial class VisualScriptRuntime
{
	// Swaps the instances of every handle waiting for it whose old program is not running; the others swap when it ends.
	private void ApplySwaps()
	{
		List<ulong>? swapped = null;
		foreach (ulong graph in m_PendingSwaps)
		{
			if (TrySwap(graph))
			{
				(swapped ??= []).Add(graph);
			}
		}
		foreach (ulong graph in swapped ?? [])
		{
			m_PendingSwaps.Remove(graph);
		}
	}

	// True when no instance of the handle waits any more.
	private bool TrySwap(ulong graph)
	{
		if (!m_Graphs.TryGetValue(graph, out GraphEntry? entry) || entry.Effective is not { } program)
		{
			return true;
		}
		bool waiting = false;
		foreach (GraphInstance instance in m_Instances.Values)
		{
			if (instance.Graph != graph || ReferenceEquals(instance.Program, program))
			{
				continue;
			}
			if (instance.Program.Activity > 0)
			{
				waiting = true;
				continue;
			}
			Migrate(instance, program);
		}
		return !waiting;
	}

	private void Migrate(GraphInstance instance, GraphProgram target)
	{
		Slot[] variables = CarryVariables(instance.Program, instance.Variables, target);
		Frame frame = new(target.EventGraph);
		CarryNodes(instance.Frame, frame);
		// The fibers that survive keep their places in the pending order; releasing a cancelled one takes it out, so the next
		// fiber is read first.
		LinkedListNode<Execution>? next = instance.FirstFiber;
		while (next != null)
		{
			Execution fiber = next.Value;
			next = next.Next;
			if (FindMissing(fiber, target) is not { } problem)
			{
				Move(fiber, target, frame);
				continue;
			}
			CompiledNode latent = fiber.TopFrame.Graph.Nodes[fiber.SuspendedAt];
			string entity = FormattableString.Invariant($"{m_Environment.GetEntityName(instance.Entity)} ({instance.Entity})");
			m_Environment.Log(VisualScriptLogLevel.Warn,
				FormattableString.Invariant($"{target.Name} on {entity}: the execution waiting in {Naming(latent)} is cancelled: {problem}"));
			Release(fiber);
		}
		// Stopped executions migrate as fibers do (spec-slice4 section 4.8), in the order they stopped; a cancelled one ends its
		// stop, and its chain loses it.
		int index = 0;
		while (index < instance.DebugFibers.Count)
		{
			Execution stopped = instance.DebugFibers[index];
			if (FindMissing(stopped, target) is not { } problem)
			{
				Move(stopped, target, frame);
				m_Debugger?.OnStoppedMigrated(stopped);
				index++;
				continue;
			}
			string entity = FormattableString.Invariant($"{m_Environment.GetEntityName(instance.Entity)} ({instance.Entity})");
			m_Environment.Log(VisualScriptLogLevel.Warn,
				FormattableString.Invariant($"{target.Name} on {entity}: the stopped execution at {Naming(StopNode(stopped))} is cancelled: {problem}"));
			ReleaseDebugFiber(stopped, problem);
		}
		instance.Swap(target, variables, frame);
	}

	// Variables match by ID (with the same type), then by name and type, and keep their values; the others start as they do
	// in a new instance (spec section 5.1): at their Value, else at the type default.
	private static Slot[] CarryVariables(GraphProgram source, Slot[] values, GraphProgram target)
	{
		int[] matches = MatchVariables(source, target);
		CompiledVariable[] variables = target.Variables;
		Slot[] carried = new Slot[variables.Length];
		for (int index = 0; index < variables.Length; index++)
		{
			carried[index] = matches[index] >= 0 ? values[matches[index]] : variables[index].Initial;
		}
		return carried;
	}

	// For each variable of the target program, the index of the source program's variable it continues (by ID with the same
	// type, then by name and type, each source variable continued once), or -1 for a new one. The debugger's Changed compare
	// (spec-slice4 section 4.7) reads variables across a live edit by the same rule.
	internal static int[] MatchVariables(GraphProgram source, GraphProgram target)
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

	private static bool SameType(CompiledVariable a, CompiledVariable b) => a.Type.Id == b.Type.Id;

	// The state of DoOnce, DoN, FlipFlop and Gate nodes and the stored outputs of impure and event nodes, for nodes whose ID
	// and type are unchanged; outputs also keep their pin's name and type. Latent nodes become pending again with their
	// fibers (Move); a Timeline keeps its state, its time clamped into its new length (spec-slice3 section 3.8.6), and the
	// next advance writes its outputs with its new curves. Everything else starts from the new frame's template.
	private static void CarryNodes(Frame from, Frame to)
	{
		CompiledGraph source = from.Graph;
		foreach (CompiledNode node in to.Graph.Nodes)
		{
			// The Make and Break nodes of split pins have no state: the node whose pins they split keeps its outputs.
			if (node.IsSplit)
			{
				continue;
			}
			if (Find(source, node) is not { } old)
			{
				continue;
			}
			if (node.Op != Opcode.Latent && node.StateSlot >= 0 && old.StateSlot >= 0)
			{
				to.Slots[node.StateSlot] = from.Slots[old.StateSlot];
			}
			if (node.TimelineSlot >= 0 && old.TimelineSlot >= 0 && from.Slots[old.TimelineSlot].Object is TimelineState state)
			{
				state.Time = Math.Clamp(state.Time, 0.0, node.Timeline!.Definition.Length);
				to.Slots[node.TimelineSlot] = Slot.FromObject(state);
			}
			if (node.Pure)
			{
				continue;
			}
			for (int output = 0; output < node.OutputCount; output++)
			{
				int match = MatchOutput(old, node, output);
				if (match >= 0)
				{
					to.Slots[node.OutputBase + output] = from.Slots[old.OutputBase + match];
				}
			}
		}
	}

	// The output of the old node a new node's output continues: the one of the same pin name and type; -1 when there is none.
	internal static int MatchOutput(CompiledNode old, CompiledNode node, int output)
	{
		int match = Array.IndexOf(old.OutputPins, node.OutputPins[output]);
		return match >= 0 && old.OutputTypes[match].Id == node.OutputTypes[output].Id ? match : -1;
	}

	// Why the fiber or debug fiber cannot continue in the new program: a node its position (a fiber's latent node, a stopped
	// execution's stop point), flow stack or call frames hold is gone or has another type, or a function it is in changed its
	// parameters or locals. Null when it can.
	private static string? FindMissing(Execution fiber, GraphProgram target)
	{
		CompiledGraph caller = null!;
		CompiledGraph newCaller = null!;
		for (int index = 0; index < fiber.CallCount; index++)
		{
			CallRecord record = fiber.Calls[index];
			CompiledGraph graph = record.Frame.Graph;
			CompiledGraph newGraph = target.EventGraph;
			if (index > 0)
			{
				CompiledFunction function = graph.Function!;
				CompiledFunction? newFunction = target.FindFunction(function.Graph, function.Id);
				if (newFunction == null || newFunction.Pure || !SameSignature(function, newFunction))
				{
					return $"the edit changed the function {function.Name}";
				}
				CompiledNode call = caller.Nodes[record.CallNode];
				if (Find(newCaller, call) is not { } newCall || newCall.Function is not { } callee || callee.Graph != function.Graph ||
					callee.Id != function.Id)
				{
					return Removed(call, newCaller);
				}
				newGraph = newFunction.Body;
			}
			int end = index + 1 < fiber.CallCount ? fiber.Calls[index + 1].FlowBase : fiber.FlowCount;
			for (int flow = record.FlowBase; flow < end; flow++)
			{
				FlowEntry entry = fiber.Flow[flow];
				CompiledNode node = graph.Nodes[entry.Node];
				// A Sequence entry continues at Then<Index>, which must still be an output.
				if (Find(newGraph, node) is not { } moved || (entry.Kind == FlowKind.Sequence && entry.Index >= moved.Count))
				{
					return Removed(node, newGraph);
				}
			}
			caller = graph;
			newCaller = newGraph;
		}
		if (fiber.SuspendedAt >= 0)
		{
			CompiledNode latent = caller.Nodes[fiber.SuspendedAt];
			return Find(newCaller, latent) == null ? Removed(latent, newCaller) : null;
		}
		// A Continue position is its top flow entry, checked above.
		return fiber.Node >= 0 ? FindMissingStop(fiber, caller, newCaller) : null;
	}

	// Why a stopped execution's Enter position has no continuation in the new graph (spec-slice4 sections 4.8 and 7.6): its
	// node is gone, or no longer has the exec input it is entered through; a sender waiting in its Send or Broadcast also needs
	// the node to raise the same event in the same way, since its staged arguments were made for the old parameter types and
	// its receivers were found by the old event and graph.
	private static string? FindMissingStop(Execution stopped, CompiledGraph graph, CompiledGraph newGraph)
	{
		CompiledNode node = graph.Nodes[stopped.Node];
		if (Find(newGraph, node) is not { } moved || MapEntry(node, moved, stopped.Entry) < 0)
		{
			return Removed(node, newGraph);
		}
		if (stopped.Delivery is not { } delivery)
		{
			return null;
		}
		CompiledNode sender = graph.Nodes[delivery.Node];
		if (Find(newGraph, sender) is not { } newSender)
		{
			return Removed(sender, newGraph);
		}
		bool same = sender.EventName == newSender.EventName && SameTypes(sender.EventParameters, newSender.EventParameters) &&
			sender.TargetGraph == newSender.TargetGraph && sender.IncludeSelf == newSender.IncludeSelf;
		return same ? null : $"the edit changed the event {Naming(sender)} raises";
	}

	// The exec input of the new node that an old node's entry enters, by its name; -1 when the new node has none of that name.
	// The entry of a node without exec inputs (an event, a function's Entry) stays.
	private static int MapEntry(CompiledNode old, CompiledNode moved, int entry) =>
		entry < old.EntryPins.Length ? Array.IndexOf(moved.EntryPins, old.EntryPins[entry]) : entry;

	// The node of the other graph with the ID and type of a node, enabled or disabled as it was; null when there is none.
	internal static CompiledNode? Find(CompiledGraph graph, CompiledNode old)
	{
		int index = graph.FindNode(old.Id);
		return index >= 0 && IsSameNode(graph.Nodes[index], old) ? graph.Nodes[index] : null;
	}

	// Whether a node of the new program continues one of the old: the same type, and disabled only if it was.
	private static bool IsSameNode(CompiledNode old, CompiledNode node) => old.Type == node.Type && old.IsDisabled == node.IsDisabled;

	// Why a node the fiber holds has no continuation in the new graph.
	private static string Removed(CompiledNode node, CompiledGraph graph)
	{
		int index = graph.FindNode(node.Id);
		bool disabled = !node.IsDisabled && index >= 0 && graph.Nodes[index].IsDisabled && graph.Nodes[index].Type == node.Type;
		return disabled ? $"the edit disabled {Naming(node)}" : $"the edit removed or changed {Naming(node)}";
	}

	// "Delay (node 12)", and for a node of a library function "Delay (node 12) of Scripts/Motion.sgraph": node IDs collide
	// across documents (spec-slice3 section 4.9), and the messages name the program's own document already.
	private static string Naming(CompiledNode node) => node.Owner.Graph != 0
		? FormattableString.Invariant($"{node.Title} (node {node.Id}) of {node.Owner.Name}")
		: FormattableString.Invariant($"{node.Title} (node {node.Id})");

	private static bool SameSignature(CompiledFunction a, CompiledFunction b)
	{
		if (!SameTypes(a.InputTypes, b.InputTypes) || !SameTypes(a.OutputTypes, b.OutputTypes) || a.Locals.Length != b.Locals.Length)
		{
			return false;
		}
		for (int index = 0; index < a.Locals.Length; index++)
		{
			if (a.Locals[index].Type.Id != b.Locals[index].Type.Id)
			{
				return false;
			}
		}
		return true;
	}

	// Carries a fiber or debug fiber FindMissing accepted over to the new program: its call frames are new frames of the new
	// functions (node state, stored outputs and locals carried by ID), its positions are the new programs' nodes of the same
	// IDs (a stopped execution's stop node, entry and pending delivery included), and a fiber's latent node is pending with it
	// again.
	private static void Move(Execution fiber, GraphProgram target, Frame eventFrame)
	{
		CompiledGraph caller = null!;
		CompiledGraph newCaller = null!;
		for (int index = 0; index < fiber.CallCount; index++)
		{
			ref CallRecord record = ref fiber.Calls[index];
			Frame old = record.Frame;
			CompiledGraph graph = old.Graph;
			Frame moved = eventFrame;
			if (index > 0)
			{
				CompiledFunction function = graph.Function!;
				CompiledFunction newFunction = target.FindFunction(function.Graph, function.Id)!;
				moved = newFunction.AcquireFrame();
				CarryNodes(old, moved);
				CarryLocals(function, old, newFunction, moved);
				function.ReleaseFrame(old);
				record.CallNode = newCaller.FindNode(caller.Nodes[record.CallNode].Id);
			}
			int end = index + 1 < fiber.CallCount ? fiber.Calls[index + 1].FlowBase : fiber.FlowCount;
			for (int flow = record.FlowBase; flow < end; flow++)
			{
				ref FlowEntry entry = ref fiber.Flow[flow];
				entry.Node = moved.Graph.FindNode(graph.Nodes[entry.Node].Id);
			}
			record.Frame = moved;
			caller = graph;
			newCaller = moved.Graph;
		}
		fiber.Program = target;
		// The start names the new program's node of its ID while the edit kept it (Describe reads a stopped execution's start
		// event in its moved frame, spec-slice4 section 5.3); a removed one stays as it was, which only names it.
		if (fiber.Start.Node is { } started && GraphOf(target, started.Owner) is { } startGraph && Find(startGraph, started) is { } start)
		{
			fiber.Start = fiber.Start with { Node = start };
		}
		if (fiber.SuspendedAt >= 0)
		{
			CompiledNode latent = newCaller.Nodes[newCaller.FindNode(caller.Nodes[fiber.SuspendedAt].Id)];
			fiber.SuspendedAt = latent.Index;
			fiber.Current = latent;
			fiber.TopFrame.Slots[latent.StateSlot] = Slot.FromObject(fiber);
			return;
		}
		if (fiber.Node >= 0)
		{
			CompiledNode old = caller.Nodes[fiber.Node];
			CompiledNode node = newCaller.Nodes[newCaller.FindNode(old.Id)];
			fiber.Entry = MapEntry(old, node, fiber.Entry);
			fiber.Node = node.Index;
		}
		if (fiber.Delivery is { } delivery)
		{
			delivery.Node = newCaller.FindNode(caller.Nodes[delivery.Node].Id);
		}
		fiber.Current = StopNode(fiber);
	}

	// The new program's graph of an old compiled graph: the event graph, or the body of the function of the same document and
	// ID; null when the edit removed the function.
	private static CompiledGraph? GraphOf(GraphProgram target, CompiledGraph old) =>
		old.Function is { } function ? target.FindFunction(function.Graph, function.Id)?.Body : target.EventGraph;

	// Locals of a surviving call frame match by ID.
	private static void CarryLocals(CompiledFunction function, Frame from, CompiledFunction newFunction, Frame to)
	{
		foreach (LocalSlot local in newFunction.Locals)
		{
			foreach (LocalSlot old in function.Locals)
			{
				if (old.Id == local.Id)
				{
					to.Slots[local.Slot] = from.Slots[old.Slot];
					break;
				}
			}
		}
	}
}
