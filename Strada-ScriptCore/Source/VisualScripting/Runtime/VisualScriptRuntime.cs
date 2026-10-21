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
	// The graph handles whose instances still run an older program than the handle's effective one: it was running when
	// the new program loaded (spec section 5.6).
	private readonly HashSet<ulong> m_PendingSwaps = [];
	private IVisualScriptDebugger? m_Debugger;
	// Active executions plus the function frames on their call stacks.
	private int m_Depth;
	// Steps left to the outermost active execution, which its nested executions share.
	private int m_StepsRemaining;
	// The last execution serial.
	private ulong m_LastSerial;

	// The engine's runtime: documents are analyzed and compiled with the node library of the loaded assemblies.
	internal VisualScriptRuntime(IVisualScriptEnvironment environment)
		: this(environment, EngineLibrary.Get)
	{
	}

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

	// Attaches the debugger that traces executions from now on (spec section 5.5); null detaches it.
	internal void AttachDebugger(IVisualScriptDebugger? debugger)
	{
		m_Debugger = debugger;
	}

	// Analyzes and compiles a document for a graph handle and returns its diagnostics. A program is stored whatever the
	// document holds; a broken one keeps the handle's working program running (spec section 5.1), a working one swaps the
	// program of the handle's instances (spec section 5.6). name is the display name in messages (the asset path).
	internal IReadOnlyList<Diagnostic> Load(ulong graph, string name, ReadOnlySpan<byte> document)
	{
		AnalyzedDocument analyzed = GraphAnalyzer.Analyze(m_Library(), document);
		GraphProgram program = ProgramCompiler.Compile(analyzed, name);
		ProgramSource source = new(name, document.ToArray());
		bool stale = false;
		if (m_Graphs.TryGetValue(graph, out GraphEntry? entry))
		{
			stale = entry.IsStale;
			entry.LatestSource = source;
		}
		else
		{
			entry = new GraphEntry(source);
			m_Graphs[graph] = entry;
		}
		entry.Latest = program;
		entry.IsStale = false;
		if (!program.IsBroken)
		{
			entry.Effective = program;
			entry.EffectiveSource = source;
			m_PendingSwaps.Add(graph);
			ApplySwaps();
		}
		else if (stale)
		{
			// The game assembly changed since the working program compiled: it keeps running if it still compiles.
			entry.Effective = entry.EffectiveSource is { } working ? Recompile(working) : null;
			if (entry.Effective == null)
			{
				entry.EffectiveSource = null;
			}
		}
		return analyzed.Diagnostics;
	}

	// Forgets the handle and its programs; instances keep the program they run until they are destroyed. NotFound when
	// the handle was not loaded.
	internal VisualScriptStatus Unload(ulong graph)
	{
		m_PendingSwaps.Remove(graph);
		return m_Graphs.Remove(graph) ? VisualScriptStatus.Success : VisualScriptStatus.NotFound;
	}

	// The last program loaded for the handle (broken or not), and the last working one; null when the handle is unknown,
	// or when the game assembly changed and the programs did not compile again yet.
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
		Refresh(entry);
		if (entry.Effective is not { } program)
		{
			GraphProgram latest = entry.Latest!;
			m_Environment.Log(VisualScriptLogLevel.Error, $"{latest.Name} cannot run: {DescribeErrors(latest)}");
			return VisualScriptStatus.Failure;
		}
		GraphInstance instance = new(entity, graph, program);
		ApplyVariables(instance, variables);
		if (m_Instances.Remove(entity, out GraphInstance? replaced))
		{
			Kill(replaced);
		}
		m_Instances[entity] = instance;
		return VisualScriptStatus.Success;
	}

	// Removes the entity's instance without running Event.Destroy; NotFound when it has none.
	internal VisualScriptStatus DestroyInstance(ulong entity)
	{
		if (!m_Instances.Remove(entity, out GraphInstance? instance))
		{
			return VisualScriptStatus.NotFound;
		}
		Kill(instance);
		return VisualScriptStatus.Success;
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
	// ended in an aborting error (each logged once); NotFound without an instance. A live edit of the instance's graph
	// waits until the dispatch is over.
	internal VisualScriptStatus Invoke(ulong entity, ScriptEvent scriptEvent, float timeStep, ulong other)
	{
		if (!m_Instances.TryGetValue(entity, out GraphInstance? instance))
		{
			return VisualScriptStatus.NotFound;
		}
		if (scriptEvent is < ScriptEvent.Create or > ScriptEvent.Click)
		{
			return VisualScriptStatus.Failure;
		}
		GraphProgram program = instance.Program;
		Enter(program);
		try
		{
			return Dispatch(instance, program, scriptEvent, timeStep, other) ? VisualScriptStatus.Failure : VisualScriptStatus.Success;
		}
		finally
		{
			Leave(program);
		}
	}

	// Continues a fiber now at the continuation of the latent node it waits in, as an outermost execution with a budget of
	// its own (spec section 5.3), whether or not the node is due. NotFound when the fiber is not one of its instance's (it
	// finished, or its instance died).
	internal VisualScriptStatus Resume(Execution fiber)
	{
		if (fiber.SuspendedAt < 0 || fiber.IsRunning || !fiber.Instance.HasFiber(fiber))
		{
			return VisualScriptStatus.NotFound;
		}
		GraphProgram program = fiber.Program;
		Enter(program);
		try
		{
			return Continue(fiber, Execution.ResumeEntry) ? VisualScriptStatus.Failure : VisualScriptStatus.Success;
		}
		finally
		{
			Leave(program);
		}
	}

	// True when an execution the dispatch started ended in an aborting error.
	private bool Dispatch(GraphInstance instance, GraphProgram program, ScriptEvent scriptEvent, float timeStep, ulong other)
	{
		switch (scriptEvent)
		{
			case ScriptEvent.Create:
				return Start(instance, program.Events[(int)ScriptEvent.Create], default);
			case ScriptEvent.Update:
				return Update(instance, program, timeStep);
			case ScriptEvent.FixedUpdate:
				return Start(instance, program.Events[(int)ScriptEvent.FixedUpdate], Slot.FromFloat(timeStep));
			case ScriptEvent.Destroy:
				m_Instances.Remove(instance.Entity);
				Kill(instance);
				return Start(instance, program.Events[(int)ScriptEvent.Destroy], default);
			default:
				// Contacts and clicks look the other entity up only for a graph that has a node for the event.
				int eventNode = program.Events[(int)scriptEvent];
				return eventNode >= 0 && Start(instance, eventNode, Slot.FromObject(m_Environment.GetEntity(other)));
		}
	}

	// The Update dispatch (spec section 5.2): the frame time, the input events in Nodes order, Event.Update, then one pass
	// over the instance's pending latent nodes. An instance destroyed on the way receives nothing more.
	private bool Update(GraphInstance instance, GraphProgram program, float timeStep)
	{
		ulong dispatch = ++instance.Updates;
		m_Environment.SetDeltaTime(timeStep);
		bool failed = false;
		foreach (InputEvent input in program.InputEvents)
		{
			if (!instance.IsAlive)
			{
				return failed;
			}
			if (IsTriggered(input))
			{
				failed |= Start(instance, input.Node, default);
			}
		}
		if (instance.IsAlive)
		{
			failed |= Start(instance, program.Events[(int)ScriptEvent.Update], Slot.FromFloat(timeStep));
		}
		if (instance.IsAlive)
		{
			failed |= AdvancePending(instance, dispatch, timeStep);
		}
		return failed;
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
		Execution execution = Acquire(instance, eventNode, 0);
		return Finish(execution, Execute(execution, !IsExecuting));
	}

	// Runs a fiber again at its latent node, as an outermost execution: entry ResumeEntry continues at the node's
	// continuation, RecheckEntry checks a WaitUntil's condition first. True when it ended in an aborting error. A fiber whose
	// condition is checked stays in its place among the instance's fibers while the condition runs, and leaves only when it
	// holds (Wait); its instance's program is running then, so no live edit can move it.
	private bool Continue(Execution fiber, int entry)
	{
		if (entry != Execution.RecheckEntry)
		{
			fiber.Instance.RemoveFiber(fiber);
		}
		fiber.Node = fiber.SuspendedAt;
		fiber.Entry = entry;
		fiber.SuspendedAt = -1;
		fiber.Debugger = Trace(fiber.Instance);
		return Finish(fiber, Execute(fiber, true));
	}

	// What happens to an execution after it ran: one that ended or aborted is released (an abort logged once); a suspended
	// one stays with its instance as a fiber. True for an abort.
	private bool Finish(Execution execution, ExecutionResult result)
	{
		switch (result)
		{
			case ExecutionResult.Suspended:
				Park(execution);
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

	// A suspended execution becomes a fiber of its instance, last in the pending order; one already in its place (a WaitUntil
	// whose condition is still false, a RetriggerableDelay's latest trigger) stays there. One whose instance died while it
	// ran dies with it.
	private void Park(Execution fiber)
	{
		if (!fiber.Instance.IsAlive)
		{
			ClearPending(fiber);
			Release(fiber);
			return;
		}
		if (!fiber.Instance.HasFiber(fiber))
		{
			fiber.Instance.AddFiber(fiber);
		}
	}

	// A new execution of the instance that starts at a node of its event graph (and its exec input), traced when the
	// debugger targets the instance's entity.
	private Execution Acquire(GraphInstance instance, int node, int entry)
	{
		Execution execution = m_ExecutionPool.TryPop(out Execution? pooled) ? pooled : new Execution();
		execution.Serial = ++m_LastSerial;
		execution.Instance = instance;
		execution.Program = instance.Program;
		execution.PushCall(instance.Frame, -1, null, 0);
		execution.Node = node;
		execution.Entry = entry;
		execution.Debugger = Trace(instance);
		return execution;
	}

	private IVisualScriptDebugger? Trace(GraphInstance instance) => m_Debugger is { } debugger && debugger.IsTarget(instance.Entity) ? debugger : null;

	// Returns the frames of an execution's function calls to their pools and the execution to the runtime's pool; a fiber
	// leaves its instance's fibers first.
	private void Release(Execution execution)
	{
		execution.Instance.RemoveFiber(execution);
		for (int index = execution.CallCount - 1; index > 0; index--)
		{
			Frame frame = execution.Calls[index].Frame;
			frame.Graph.Function!.ReleaseFrame(frame);
		}
		execution.Reset();
		m_ExecutionPool.Push(execution);
	}

	// A fiber that never runs again: Stop cancelled its Tween or Interval, or a RetriggerableDelay's latest trigger replaced
	// it. Nothing of its stack runs.
	private void Discard(Execution fiber)
	{
		ClearPending(fiber);
		Release(fiber);
	}

	// The latent node a fiber waits in stops being pending.
	private static void ClearPending(Execution fiber)
	{
		Frame frame = fiber.TopFrame;
		CompiledNode node = frame.Graph.Nodes[fiber.SuspendedAt];
		if (ReferenceEquals(frame.Slots[node.StateSlot].Object, fiber))
		{
			frame.Slots[node.StateSlot] = default;
		}
	}

	// An instance that is destroyed or replaced: its fibers die and its latent nodes stop being pending (spec section 5.1).
	// A fiber that is running (its WaitUntil condition is checked) leaves too, and dies when its run is over (Park, Finish).
	private void Kill(GraphInstance instance)
	{
		instance.Kill();
		while (instance.FirstFiber is { Value: { } fiber })
		{
			instance.RemoveFiber(fiber);
			if (!fiber.IsRunning)
			{
				ClearPending(fiber);
				Release(fiber);
			}
		}
	}

	// A dispatch or execution of the program starts running, and one ends: when none runs any more, the live edits that
	// waited for it take effect.
	private static void Enter(GraphProgram program)
	{
		program.Activity++;
	}

	private void Leave(GraphProgram program)
	{
		if (--program.Activity == 0 && m_PendingSwaps.Count > 0)
		{
			ApplySwaps();
		}
	}

	// After the game assembly changed, a handle's programs compile again from their documents when the handle is next used
	// (spec section 5.6). The errors of documents that do not compile now are logged once, here.
	private void Refresh(GraphEntry entry)
	{
		if (!entry.IsStale)
		{
			return;
		}
		entry.IsStale = false;
		GraphProgram latest = Compile(entry.LatestSource);
		entry.Latest = latest;
		if (!latest.IsBroken)
		{
			entry.Effective = latest;
			entry.EffectiveSource = entry.LatestSource;
			return;
		}
		LogRecompiled(latest);
		entry.Effective = entry.EffectiveSource is { } working && !ReferenceEquals(working, entry.LatestSource) ? Recompile(working) : null;
		if (entry.Effective == null)
		{
			entry.EffectiveSource = null;
		}
	}

	// The program of a document that worked before the game assembly changed; null (its errors logged) when it does not
	// compile now.
	private GraphProgram? Recompile(ProgramSource source)
	{
		GraphProgram program = Compile(source);
		if (!program.IsBroken)
		{
			return program;
		}
		LogRecompiled(program);
		return null;
	}

	private GraphProgram Compile(ProgramSource source) => ProgramCompiler.Compile(GraphAnalyzer.Analyze(m_Library(), source.Document), source.Name);

	private void LogRecompiled(GraphProgram program)
	{
		m_Environment.Log(VisualScriptLogLevel.Error, $"{program.Name} was compiled again for the changed game assembly: {DescribeErrors(program)}");
	}

	// "<n> errors, first: <message> (node <id>)" of a broken program.
	private static string DescribeErrors(GraphProgram program)
	{
		Diagnostic first = program.FirstError!;
		string location = first.Node is { } node ? FormattableString.Invariant($" (node {node})")
			: first.Variable is { } variable ? FormattableString.Invariant($" (variable {variable})")
			: string.Empty;
		string errors = program.ErrorCount == 1 ? "1 error" : FormattableString.Invariant($"{program.ErrorCount} errors");
		return $"{errors}, first: {first.Message}{location}";
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

	// A document as it was loaded: what a program compiles from again after the game assembly changed.
	private sealed record ProgramSource(string Name, byte[] Document);

	// The programs of a graph handle.
	private sealed class GraphEntry
	{
		internal GraphEntry(ProgramSource source)
		{
			LatestSource = source;
		}

		// The document last loaded and its program, broken or not.
		internal ProgramSource LatestSource { get; set; }

		internal GraphProgram? Latest { get; set; }

		// The last document whose program worked, and that program.
		internal ProgramSource? EffectiveSource { get; set; }

		internal GraphProgram? Effective { get; set; }

		// The game assembly changed since the programs compiled: they were dropped and compile again when next used.
		internal bool IsStale { get; set; }
	}
}
