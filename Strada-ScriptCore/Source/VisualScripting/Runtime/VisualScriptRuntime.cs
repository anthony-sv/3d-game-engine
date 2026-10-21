using System;
using System.Collections.Generic;
using System.Reflection;
using Strada.Interop;

namespace Strada.VisualScripting;

// The statuses of the runtime's operations; the values are the engine's ScriptStatus (Host's constants).
internal enum VisualScriptStatus
{
	Success = 0,
	Failure = 1,
	NotFound = 2,
}

// The visual-script VM (spec section 5): the programs loaded for graph handles, the graph instances of entities and their
// fibers, the invokers of the members its programs call, and the executions it runs. All execution state lives in
// per-execution objects, so the runtime is re-entrant: a graph may start another execution of any graph (Event.Send, or
// an engine call that runs scripts) while it runs. Main thread only.
internal sealed partial class VisualScriptRuntime
{
	// Spec section 5.3: the steps of an outermost execution and its nested ones, and the active executions plus function
	// frames.
	internal const int MaxSteps = 1_000_000;
	internal const int MaxDepth = 64;

	private readonly IVisualScriptEnvironment m_Environment;
	private readonly Func<NodeLibrary> m_Library;
	private readonly Dictionary<ulong, GraphEntry> m_Graphs = [];
	private readonly Dictionary<ulong, GraphInstance> m_Instances = [];
	private readonly Stack<Execution> m_ExecutionPool = new();
	// By member and kind: a property's or field's getter and setter share their MemberInfo.
	private readonly Dictionary<(MemberInfo Member, MemberNodeKind Kind), MemberInvoker> m_Invokers = [];
	private readonly Dictionary<Type, MemberInvoker> m_Makers = [];
	private readonly Dictionary<Type, MemberInvoker> m_Breakers = [];
	// Active executions plus the function frames on their call stacks.
	private int m_Depth;
	// Steps left to the outermost active execution, which its nested executions share.
	private int m_StepsRemaining;

	// library gives the node library documents are analyzed and compiled with (spec section 5.1: built when first needed,
	// shared by every runtime).
	internal VisualScriptRuntime(IVisualScriptEnvironment environment, Func<NodeLibrary> library)
	{
		m_Environment = environment;
		m_Library = library;
	}

	// Whether an execution of this runtime is running (on the host stack, not suspended).
	internal bool IsExecuting => m_Depth > 0;

	internal int InstanceCount => m_Instances.Count;

	// Analyzes and compiles a document for a graph handle and returns its diagnostics. A program is stored whatever the
	// document holds; a broken one keeps the handle's working program running (spec section 5.1). name is the display name
	// in messages (the asset path).
	internal IReadOnlyList<Diagnostic> Load(ulong graph, string name, ReadOnlySpan<byte> document)
	{
		AnalyzedDocument analyzed = GraphAnalyzer.Analyze(m_Library(), document);
		GraphProgram program = ProgramCompiler.Compile(analyzed, name);
		if (!m_Graphs.TryGetValue(graph, out GraphEntry? entry))
		{
			entry = new GraphEntry();
			m_Graphs[graph] = entry;
		}
		entry.Latest = program;
		if (!program.IsBroken)
		{
			entry.Effective = program;
		}
		return analyzed.Diagnostics;
	}

	// Forgets the handle and its programs; instances keep the program they run until they are destroyed. False when the
	// handle was not loaded.
	internal bool Unload(ulong graph) => m_Graphs.Remove(graph);

	// The last program loaded for the handle (broken or not), and the last working one.
	internal GraphProgram? FindLatestProgram(ulong graph) => m_Graphs.TryGetValue(graph, out GraphEntry? entry) ? entry.Latest : null;

	internal GraphProgram? FindEffectiveProgram(ulong graph) => m_Graphs.TryGetValue(graph, out GraphEntry? entry) ? entry.Effective : null;

	// Creates the entity's instance of the graph (replacing the one it has, whose fibers die): variables from their values,
	// then the overrides of the component (stored-map JSON, spec section 5.4; empty for none).
	internal VisualScriptStatus CreateInstance(ulong entity, ulong graph, ReadOnlySpan<byte> variables)
	{
		if (!m_Graphs.TryGetValue(graph, out GraphEntry? entry))
		{
			return VisualScriptStatus.NotFound;
		}
		if (entry.Effective is not { } program)
		{
			GraphProgram latest = entry.Latest!;
			Diagnostic first = latest.FirstError!;
			string location = first.Node is { } node ? FormattableString.Invariant($" (node {node})")
				: first.Variable is { } variable ? FormattableString.Invariant($" (variable {variable})")
				: string.Empty;
			string errors = latest.ErrorCount == 1 ? "1 error" : FormattableString.Invariant($"{latest.ErrorCount} errors");
			m_Environment.Log(VisualScriptLogLevel.Error, $"{latest.Name} cannot run: {errors}, first: {first.Message}{location}");
			return VisualScriptStatus.Failure;
		}
		GraphInstance instance = new(entity, program);
		ApplyVariables(instance, variables);
		if (m_Instances.Remove(entity, out GraphInstance? replaced))
		{
			Kill(replaced);
		}
		m_Instances[entity] = instance;
		return VisualScriptStatus.Success;
	}

	// Removes the entity's instance without running Event.Destroy; false when it has none.
	internal bool DestroyInstance(ulong entity)
	{
		if (!m_Instances.Remove(entity, out GraphInstance? instance))
		{
			return false;
		}
		Kill(instance);
		return true;
	}

	// Removes every instance; no events run.
	internal void DestroyAllInstances()
	{
		List<GraphInstance> instances = [.. m_Instances.Values];
		m_Instances.Clear();
		foreach (GraphInstance instance in instances)
		{
			Kill(instance);
		}
	}

	internal GraphInstance? FindInstance(ulong entity) => m_Instances.GetValueOrDefault(entity);

	// Dispatches a lifecycle event to the entity's instance (spec section 5.2). Failure when an execution it started
	// ended in an aborting error (each logged once); NotFound without an instance.
	internal VisualScriptStatus Invoke(ulong entity, ScriptEvent scriptEvent, float timeStep, ulong other)
	{
		if (!m_Instances.TryGetValue(entity, out GraphInstance? instance))
		{
			return VisualScriptStatus.NotFound;
		}
		GraphProgram program = instance.Program;
		bool failed = false;
		switch (scriptEvent)
		{
			case ScriptEvent.Create:
				failed = Start(instance, program.Events[(int)ScriptEvent.Create], default);
				break;
			case ScriptEvent.Update:
				m_Environment.SetDeltaTime(timeStep);
				foreach (InputEvent input in program.InputEvents)
				{
					if (IsTriggered(input))
					{
						failed |= Start(instance, input.Node, default);
					}
				}
				failed |= Start(instance, program.Events[(int)ScriptEvent.Update], Slot.FromFloat(timeStep));
				break;
			case ScriptEvent.FixedUpdate:
				failed = Start(instance, program.Events[(int)ScriptEvent.FixedUpdate], Slot.FromFloat(timeStep));
				break;
			case ScriptEvent.Destroy:
				m_Instances.Remove(entity);
				Kill(instance);
				failed = Start(instance, program.Events[(int)ScriptEvent.Destroy], default);
				break;
			case ScriptEvent.CollisionEnter:
			case ScriptEvent.CollisionExit:
			case ScriptEvent.TriggerEnter:
			case ScriptEvent.TriggerExit:
			case ScriptEvent.Click:
				failed = Start(instance, program.Events[(int)scriptEvent], Slot.FromObject(m_Environment.GetEntity(other)));
				break;
			default:
				return VisualScriptStatus.Failure;
		}
		return failed ? VisualScriptStatus.Failure : VisualScriptStatus.Success;
	}

	// Continues a fiber of an instance at the continuation of the latent node it waits in, as an outermost execution with
	// a budget of its own (spec section 5.3). NotFound when the fiber is not one of its instance's (it finished, or its
	// instance died).
	internal VisualScriptStatus Resume(Execution fiber)
	{
		if (fiber.SuspendedAt < 0 || !fiber.Instance.RemoveFiber(fiber))
		{
			return VisualScriptStatus.NotFound;
		}
		CompiledNode node = fiber.TopFrame.Graph.Nodes[fiber.SuspendedAt];
		fiber.SuspendedAt = -1;
		Follow(fiber, node, node.ContinuationExit);
		return Finish(fiber, Execute(fiber, true)) ? VisualScriptStatus.Failure : VisualScriptStatus.Success;
	}

	private bool IsTriggered(InputEvent input) => input.Kind switch
	{
		BuiltinNode.EventKeyPressed => m_Environment.IsKeyPressed((KeyCode)input.Value),
		BuiltinNode.EventKeyReleased => m_Environment.IsKeyReleased((KeyCode)input.Value),
		BuiltinNode.EventMouseButtonPressed => m_Environment.IsMouseButtonPressed((MouseButton)input.Value),
		BuiltinNode.EventMouseButtonReleased => m_Environment.IsMouseButtonReleased((MouseButton)input.Value),
		BuiltinNode.EventGamepadButtonPressed => m_Environment.IsGamepadButtonPressed((GamepadButton)input.Value, input.Gamepad),
		_ => m_Environment.IsGamepadButtonReleased((GamepadButton)input.Value, input.Gamepad),
	};

	// Starts an execution at an event node of the instance (none runs without one), its first output set to value. True when
	// it ended in an aborting error. An execution is outermost, with a budget of its own, unless another one is running.
	private bool Start(GraphInstance instance, int eventNode, Slot value)
	{
		if (eventNode < 0)
		{
			return false;
		}
		CompiledNode node = instance.Program.EventGraph.Nodes[eventNode];
		if (node.OutputCount > 0)
		{
			instance.Frame.Slots[node.OutputBase] = value;
		}
		Execution execution = Acquire(instance, eventNode);
		return Finish(execution, Execute(execution, !IsExecuting));
	}

	// What happens to an execution after it ran: one that ended or aborted is released (an abort logged once); a suspended
	// one stays with its instance as a fiber. True for an abort.
	private bool Finish(Execution execution, ExecutionResult result)
	{
		switch (result)
		{
			case ExecutionResult.Suspended:
				execution.Instance.AddFiber(execution);
				return false;
			case ExecutionResult.Aborted:
				m_Environment.Log(VisualScriptLogLevel.Error, execution.Error!.Message);
				Release(execution);
				return true;
			default:
				Release(execution);
				return false;
		}
	}

	// A new execution of the instance that starts at a node of its event graph.
	private Execution Acquire(GraphInstance instance, int node)
	{
		Execution execution = m_ExecutionPool.TryPop(out Execution? pooled) ? pooled : new Execution();
		execution.Instance = instance;
		execution.Program = instance.Program;
		execution.PushCall(instance.Frame, -1, null, 0);
		execution.Node = node;
		execution.Entry = 0;
		return execution;
	}

	// Returns the frames of an execution's function calls to their pools and the execution to the runtime's pool.
	private void Release(Execution execution)
	{
		for (int index = execution.CallCount - 1; index > 0; index--)
		{
			Frame frame = execution.Calls[index].Frame;
			frame.Graph.Function!.ReleaseFrame(frame);
		}
		execution.Reset();
		m_ExecutionPool.Push(execution);
	}

	// An instance that is destroyed or replaced: its fibers die (spec section 5.1).
	private void Kill(GraphInstance instance)
	{
		foreach (Execution fiber in instance.Kill())
		{
			Release(fiber);
		}
	}

	// The invoker of a member, built the first time a node of the runtime calls it.
	private MemberInvoker GetInvoker(MemberInfo member, MemberNodeKind kind)
	{
		if (!m_Invokers.TryGetValue((member, kind), out MemberInvoker? invoker))
		{
			invoker = MemberInvokers.ForMember(member, kind);
			m_Invokers[(member, kind)] = invoker;
		}
		return invoker;
	}

	private MemberInvoker GetStructInvoker(VisualType type, bool make)
	{
		Dictionary<Type, MemberInvoker> cache = make ? m_Makers : m_Breakers;
		if (!cache.TryGetValue(type.ClrType!, out MemberInvoker? invoker))
		{
			invoker = make ? MemberInvokers.ForMake(type) : MemberInvokers.ForBreak(type);
			cache[type.ClrType!] = invoker;
		}
		return invoker;
	}

	// The programs of a graph handle.
	private sealed class GraphEntry
	{
		internal GraphProgram? Latest { get; set; }

		internal GraphProgram? Effective { get; set; }
	}
}
