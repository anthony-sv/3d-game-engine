using System;
using System.Collections.Generic;

namespace Strada.VisualScripting;

// Live edit (spec section 5.6): a new working program of a graph handle replaces the program of every instance of the
// handle, as soon as no dispatch or execution of the old program is running. An instance keeps what still means the same
// in the new program: its variables by ID (then by name and type), the state and stored outputs of nodes whose ID and type
// are unchanged, and its fibers, when every position they hold still exists. Positions are carried over by node ID.
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
				FormattableString.Invariant($"{target.Name} on {entity}: the execution waiting in {latent.Title} (node {latent.Id}) is cancelled: {problem}"));
			Release(fiber);
		}
		instance.Swap(target, variables, frame);
	}

	// Variables match by ID (with the same type), then by name and type, and keep their values; the others start as they do
	// in a new instance (spec section 5.1): at their Value, else at the type default.
	private static Slot[] CarryVariables(GraphProgram source, Slot[] values, GraphProgram target)
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
		Slot[] carried = new Slot[variables.Length];
		for (int index = 0; index < variables.Length; index++)
		{
			if (matches[index] < 0 && byName.TryGetValue(variables[index].Name, out int old) && !taken[old] &&
				SameType(source.Variables[old], variables[index]))
			{
				matches[index] = old;
				taken[old] = true;
			}
			carried[index] = matches[index] >= 0 ? values[matches[index]] : variables[index].Initial;
		}
		return carried;
	}

	private static bool SameType(CompiledVariable a, CompiledVariable b) => a.Type.Id == b.Type.Id;

	// The state of DoOnce, DoN, FlipFlop and Gate nodes and the stored outputs of impure and event nodes, for nodes whose ID
	// and type are unchanged; outputs also keep their pin's name and type. Latent nodes become pending again with their
	// fibers (Move). Everything else starts from the new frame's template.
	private static void CarryNodes(Frame from, Frame to)
	{
		CompiledGraph source = from.Graph;
		foreach (CompiledNode node in to.Graph.Nodes)
		{
			int index = source.FindNode(node.Id);
			if (index < 0 || source.Nodes[index].Type != node.Type)
			{
				continue;
			}
			CompiledNode old = source.Nodes[index];
			if (node.Op != Opcode.Latent && node.StateSlot >= 0 && old.StateSlot >= 0)
			{
				to.Slots[node.StateSlot] = from.Slots[old.StateSlot];
			}
			if (node.Pure)
			{
				continue;
			}
			for (int output = 0; output < node.OutputCount; output++)
			{
				int match = Array.IndexOf(old.OutputPins, node.OutputPins[output]);
				if (match >= 0 && old.OutputTypes[match].Id == node.OutputTypes[output].Id)
				{
					to.Slots[node.OutputBase + output] = from.Slots[old.OutputBase + match];
				}
			}
		}
	}

	// Why the fiber cannot continue in the new program: a node its latent position, flow stack or call frames hold is gone
	// or has another type, or a function it is in changed its parameters or locals. Null when it can.
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
				CompiledFunction? newFunction = target.FindFunction(function.Id);
				if (newFunction == null || newFunction.Pure || !SameSignature(function, newFunction))
				{
					return $"the edit changed the function {function.Name}";
				}
				CompiledNode call = caller.Nodes[record.CallNode];
				if (Find(newCaller, call) is not { } newCall || newCall.Function?.Id != function.Id)
				{
					return Removed(call);
				}
				newGraph = newFunction.Graph;
			}
			int end = index + 1 < fiber.CallCount ? fiber.Calls[index + 1].FlowBase : fiber.FlowCount;
			for (int flow = record.FlowBase; flow < end; flow++)
			{
				FlowEntry entry = fiber.Flow[flow];
				CompiledNode node = graph.Nodes[entry.Node];
				// A Sequence entry continues at Then<Index>, which must still be an output.
				if (Find(newGraph, node) is not { } moved || (entry.Kind == FlowKind.Sequence && entry.Index >= moved.Count))
				{
					return Removed(node);
				}
			}
			caller = graph;
			newCaller = newGraph;
		}
		CompiledNode latent = caller.Nodes[fiber.SuspendedAt];
		return Find(newCaller, latent) == null ? Removed(latent) : null;
	}

	// The node of the new graph with the ID and type of an old one; null when there is none.
	private static CompiledNode? Find(CompiledGraph graph, CompiledNode old)
	{
		int index = graph.FindNode(old.Id);
		return index >= 0 && graph.Nodes[index].Type == old.Type ? graph.Nodes[index] : null;
	}

	private static string Removed(CompiledNode node) => FormattableString.Invariant($"the edit removed or changed {node.Title} (node {node.Id})");

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

	// Carries a fiber FindMissing accepted over to the new program: its call frames are new frames of the new functions
	// (node state, stored outputs and locals carried by ID), its positions are the new programs' nodes of the same IDs, and
	// its latent node is pending with it again.
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
				CompiledFunction newFunction = target.FindFunction(function.Id)!;
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
		CompiledNode latent = newCaller.Nodes[newCaller.FindNode(caller.Nodes[fiber.SuspendedAt].Id)];
		fiber.SuspendedAt = latent.Index;
		fiber.Current = latent;
		fiber.Program = target;
		fiber.TopFrame.Slots[latent.StateSlot] = Slot.FromObject(fiber);
	}

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
