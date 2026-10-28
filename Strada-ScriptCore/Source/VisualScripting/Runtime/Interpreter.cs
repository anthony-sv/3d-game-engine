using System;
using System.Collections.Generic;
using System.Globalization;
using System.Runtime.CompilerServices;

namespace Strada.VisualScripting;

// The interpreter (spec section 5.3): one loop over the nodes an execution enters, with its flow stack and call stack on
// the heap. Function calls push frames instead of recursing on the host stack, so every execution can stop at any node and
// continue later exactly where it was; only Event.Send nests a run of another execution, which the depth limit bounds.
// Evaluating a node costs one step and allocates nothing unless the node produces a new object.
internal sealed partial class VisualScriptRuntime
{
	private const string BudgetMessage = "execution exceeded 1,000,000 steps (endless loop?)";
	private const string DepthMessage = "call depth exceeded 64";
	private const string NoTarget = "Target is None";
	private const string NoArray = Intrinsics.NoArray;

	// Runs an execution until it ends, suspends or aborts. freshBudget starts the steps of an outermost execution; nested
	// executions share the steps of the one they run in. Active executions and their function frames count towards the
	// depth while they run.
	private ExecutionResult Execute(Execution execution, bool freshBudget)
	{
		int depth = m_Depth;
		int steps = m_StepsRemaining;
		if (freshBudget)
		{
			m_StepsRemaining = MaxSteps;
		}
		execution.IsRunning = true;
		try
		{
			if (m_Depth + 1 + execution.FunctionDepth > MaxDepth)
			{
				// A new execution fails at its event node, a resumed one at the latent node it waited in.
				CompiledNode node = execution.Current ?? execution.TopFrame.Graph.Nodes[execution.Node];
				execution.Error = new ExecutionError(Describe(execution, node, DepthMessage));
				return ExecutionResult.Aborted;
			}
			m_Depth += 1 + execution.FunctionDepth;
			return Run(execution);
		}
		finally
		{
			execution.IsRunning = false;
			m_Depth = depth;
			if (freshBudget)
			{
				m_StepsRemaining = steps;
			}
		}
	}

	private ExecutionResult Run(Execution execution)
	{
		try
		{
			while (true)
			{
				if (execution.Node < 0)
				{
					if (!ContinueAfterChain(execution))
					{
						return ExecutionResult.Ended;
					}
					continue;
				}
				Frame frame = execution.TopFrame;
				CompiledNode node = frame.Graph.Nodes[execution.Node];
				int entry = execution.Entry;
				Slot[] slots = frame.Slots;
				Step(execution, node);
				switch (node.Op)
				{
					case Opcode.Event:
					case Opcode.FunctionEntry:
					case Opcode.ExecReroute:
						Follow(execution, node, 0);
						break;
					case Opcode.Branch:
						Evaluate(execution, node, frame);
						Follow(execution, node, slots[node.InputBase].Bool ? 0 : 1);
						break;
					case Opcode.Sequence:
						if (node.Count > 1)
						{
							execution.PushFlow(new FlowEntry { Kind = FlowKind.Sequence, Node = node.Index, Index = 1 });
						}
						Follow(execution, node, 0);
						break;
					case Opcode.Switch:
						Evaluate(execution, node, frame);
						Follow(execution, node, SwitchExit(node, slots[node.InputBase]));
						break;
					case Opcode.ForLoop:
						if (entry == 1)
						{
							BreakLoop(execution, node);
							break;
						}
						Evaluate(execution, node, frame);
						int first = slots[node.InputBase].Int;
						int last = slots[node.InputBase + 1].Int;
						if (first > last)
						{
							Follow(execution, node, 1);
							break;
						}
						execution.PushFlow(new FlowEntry { Kind = FlowKind.ForLoop, Node = node.Index, Index = first, Last = last });
						Follow(execution, node, 0);
						break;
					case Opcode.ForEach:
						if (entry == 1)
						{
							BreakLoop(execution, node);
							break;
						}
						Evaluate(execution, node, frame);
						Array array = slots[node.InputBase].Object as Array ?? throw new VisualScriptException(NoArray);
						if (array.Length == 0)
						{
							Follow(execution, node, 1);
							break;
						}
						execution.PushFlow(new FlowEntry { Kind = FlowKind.ForEach, Node = node.Index, Items = (Array)array.Clone() });
						Follow(execution, node, 0);
						break;
					case Opcode.While:
						Evaluate(execution, node, frame);
						if (slots[node.InputBase].Bool)
						{
							execution.PushFlow(new FlowEntry { Kind = FlowKind.While, Node = node.Index });
							Follow(execution, node, 0);
						}
						else
						{
							Follow(execution, node, 1);
						}
						break;
					case Opcode.DoOnce:
						if (entry == 1)
						{
							slots[node.StateSlot] = Slot.FromBool(true);
							EndChain(execution, node);
						}
						else if (slots[node.StateSlot].Bool)
						{
							slots[node.StateSlot] = Slot.FromBool(false);
							Follow(execution, node, 0);
						}
						else
						{
							EndChain(execution, node);
						}
						break;
					case Opcode.DoN:
						if (entry == 1)
						{
							slots[node.StateSlot] = Slot.FromInt(0);
							EndChain(execution, node);
							break;
						}
						Evaluate(execution, node, frame);
						int counter = slots[node.StateSlot].Int;
						if (counter < slots[node.InputBase].Int)
						{
							slots[node.StateSlot] = Slot.FromInt(counter + 1);
							Follow(execution, node, 0);
						}
						else
						{
							EndChain(execution, node);
						}
						break;
					case Opcode.FlipFlop:
						bool isA = !slots[node.StateSlot].Bool;
						slots[node.StateSlot] = Slot.FromBool(isA);
						Follow(execution, node, isA ? 0 : 1);
						break;
					case Opcode.Gate:
						bool open = slots[node.StateSlot].Bool;
						if (entry == 0 && open)
						{
							Follow(execution, node, 0);
							break;
						}
						if (entry > 0)
						{
							slots[node.StateSlot] = Slot.FromBool(entry == 1 || (entry == 3 && !open));
						}
						EndChain(execution, node);
						break;
					case Opcode.Latent:
						if (Wait(execution, node, frame, entry))
						{
							return ExecutionResult.Suspended;
						}
						break;
					case Opcode.VariableSet:
						Evaluate(execution, node, frame);
						Slot value = slots[node.InputBase];
						if (node.IsLocal)
						{
							slots[node.Count] = value;
						}
						else
						{
							execution.Instance.Variables[node.Count] = value;
						}
						slots[node.OutputBase] = value;
						Follow(execution, node, 0);
						break;
					case Opcode.FunctionCall:
						Evaluate(execution, node, frame);
						Call(execution, node, frame);
						break;
					case Opcode.FunctionReturn:
						Evaluate(execution, node, frame);
						Return(execution, node);
						break;
					case Opcode.Member:
						Evaluate(execution, node, frame);
						InvokeMember(execution, node, slots);
						Follow(execution, node, 0);
						break;
					case Opcode.Send:
						Evaluate(execution, node, frame);
						if (Send(execution, node, slots) == ExecutionResult.Aborted)
						{
							return ExecutionResult.Aborted;
						}
						Follow(execution, node, 0);
						break;
					case Opcode.PassThrough:
						Evaluate(execution, node, frame);
						PassThrough(node, frame);
						if (node.Exits.Length > 0)
						{
							Follow(execution, node, 0);
						}
						else
						{
							EndChain(execution, node);
						}
						break;
					default:
						throw new InvalidOperationException($"{node} is pure: the flow never enters it");
				}
			}
		}
		catch (Exception exception)
		{
			CompiledNode node = execution.Current ?? execution.TopFrame.Graph.Nodes[Math.Max(execution.Node, 0)];
			string message = exception is VisualScriptException ? exception.Message : $"{exception.GetType().Name}: {exception.Message}";
			execution.Error = new ExecutionError(Describe(execution, node, message));
			return ExecutionResult.Aborted;
		}
	}

	// One node evaluation: the budget of the outermost execution shrinks by one step.
	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	private void Step(Execution execution, CompiledNode node)
	{
		execution.Current = node;
		if (--m_StepsRemaining < 0)
		{
			throw new VisualScriptException(BudgetMessage);
		}
	}

	// The flow leaves the node through an exec output and goes where its link leads (nowhere: the chain ends). Every
	// instruction ends here or in EndChain, so a traced execution reports each one exactly once.
	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	private static void Follow(Execution execution, CompiledNode node, int exit)
	{
		ExecLink link = node.Exits[exit];
		execution.Node = link.Node;
		execution.Entry = link.Entry;
		if (execution.Debugger is { } debugger)
		{
			debugger.OnStep(execution.Instance, node.Id, node.ExitPins[exit]);
		}
	}

	// The node ends its chain without leaving through an output (or the execution suspends in it).
	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	private static void EndChain(Execution execution, CompiledNode node)
	{
		execution.Node = -1;
		if (execution.Debugger is { } debugger)
		{
			debugger.OnStep(execution.Instance, node.Id, null);
		}
	}

	// The chain ended: the top frame's continuations run first, then a function call returns (as if Function.Return set
	// nothing); false when the execution is over. The continuation on top of the flow stack advances in place and is
	// popped when it is done.
	private bool ContinueAfterChain(Execution execution)
	{
		int top = execution.FlowCount - 1;
		if (top >= execution.Top.FlowBase)
		{
			ref FlowEntry entry = ref execution.Flow[top];
			Frame frame = execution.TopFrame;
			CompiledNode node = frame.Graph.Nodes[entry.Node];
			Step(execution, node);
			switch (entry.Kind)
			{
				case FlowKind.Sequence:
					int output = entry.Index;
					if (output + 1 < node.Count)
					{
						entry.Index = output + 1;
					}
					else
					{
						execution.PopFlow();
					}
					Follow(execution, node, output);
					break;
				case FlowKind.ForLoop:
					// The index stops at Last, so a loop up to int.MaxValue does not overflow.
					if (entry.Break || entry.Index == entry.Last)
					{
						execution.PopFlow();
						Follow(execution, node, 1);
						break;
					}
					entry.Index++;
					Follow(execution, node, 0);
					break;
				case FlowKind.ForEach:
					if (entry.Break || entry.Index + 1 >= entry.Items!.Length)
					{
						execution.PopFlow();
						Follow(execution, node, 1);
						break;
					}
					entry.Index++;
					Follow(execution, node, 0);
					break;
				default:
					// While: plans push call frames, never flow entries, so the entry stays on top while the condition is
					// evaluated.
					Evaluate(execution, node, frame);
					if (frame.Slots[node.InputBase].Bool)
					{
						Follow(execution, node, 0);
					}
					else
					{
						execution.PopFlow();
						Follow(execution, node, 1);
					}
					break;
			}
			return true;
		}
		if (execution.CallCount > 1)
		{
			Return(execution, null);
			return true;
		}
		return false;
	}

	// Break: the innermost active loop of the node in the current frame exits after its current body; the chain that
	// reached Break ends.
	private static void BreakLoop(Execution execution, CompiledNode node)
	{
		int loop = execution.FindLoop(node.Index);
		if (loop >= 0)
		{
			execution.Flow[loop].Break = true;
		}
		EndChain(execution, node);
	}

	private static int SwitchExit(CompiledNode node, in Slot selection)
	{
		int exit;
		bool found = node.TextCases != null
			? node.TextCases.TryGetValue((string)selection.Object!, out exit)
			: node.NumberCases!.TryGetValue(node.Kind == SlotKind.Int ? selection.Int : unchecked((long)selection.ULong), out exit);
		return found ? exit : node.Count;
	}

	// A disabled node: whichever exec input the flow entered, its data outputs take the inputs the compiler chose for them
	// and the others their type defaults; it neither waits nor keeps state.
	private static void PassThrough(CompiledNode node, Frame frame)
	{
		Slot[] slots = frame.Slots;
		for (int output = 0; output < node.OutputCount; output++)
		{
			int source = node.PassThroughSources[output];
			int slot = node.OutputBase + output;
			slots[slot] = source >= 0 ? slots[node.InputBase + source] : frame.Graph.Template[slot];
		}
	}

	// A latent node (spec section 5.3, Suspension). Execute (Interval: Start) reaching it while it is pending ends the
	// chain there, except at a RetriggerableDelay, whose latest trigger replaces the waiting one; otherwise the node reads
	// its inputs and the whole execution (its position, flow stack and call frames) suspends in it as a fiber of its
	// instance, unless a WaitUntil's condition holds already. Stop cancels a pending Tween or Interval. A fiber runs the
	// node again when it resumes (Execution.ResumeEntry) or when its WaitUntil is checked (Execution.RecheckEntry). An
	// instance that was destroyed owns no fibers: the chain ends there with a Warning. True when the execution suspended.
	private bool Wait(Execution execution, CompiledNode node, Frame frame, int entry)
	{
		Slot[] slots = frame.Slots;
		switch (entry)
		{
			case Execution.ResumeEntry:
				StopPending(execution, node, frame);
				Follow(execution, node, node.ContinuationExit);
				return false;
			case Execution.RecheckEntry:
				// An error here is aborting and discards the fiber. The fiber is still in its place among the instance's fibers
				// (Continue): it leaves when the condition holds, and keeps the place while the condition is still false.
				StopPending(execution, node, frame);
				Evaluate(execution, node, frame);
				if (slots[node.InputBase].Bool)
				{
					execution.Instance.RemoveFiber(execution);
					Follow(execution, node, node.ContinuationExit);
					return false;
				}
				Suspend(execution, node, frame, execution.Since);
				return true;
			case Execution.TickEntry:
				Follow(execution, node, node.TickExit);
				return false;
			case 0:
				break;
			default:
				// Stop: the run is cancelled (no more ticks, Completed never runs); the chain that reached Stop ends.
				if (PendingAt(frame, node) is { } run)
				{
					Discard(run);
				}
				EndChain(execution, node);
				return false;
		}
		Execution? pending = PendingAt(frame, node);
		if (pending != null && node.Builtin != BuiltinNode.FlowRetriggerableDelay)
		{
			EndChain(execution, node);
			return false;
		}
		Evaluate(execution, node, frame);
		if (!execution.Instance.IsAlive)
		{
			Warn(execution, node, "the graph's instance was destroyed, so nothing can wait here: the chain ends");
			EndChain(execution, node);
			return false;
		}
		int input = node.InputBase;
		switch (node.Builtin)
		{
			case BuiltinNode.FlowWaitUntil:
				if (slots[input].Bool)
				{
					Follow(execution, node, node.ContinuationExit);
					return false;
				}
				break;
			case BuiltinNode.FlowWaitFrames:
				execution.WaitCount = Math.Max(slots[input].Int, 1);
				break;
			case BuiltinNode.FlowInterval:
				execution.WaitTime = 0.0;
				execution.WaitLimit = slots[input].Float;
				execution.WaitCount = slots[input + 1].Int;
				execution.WaitIndex = 0;
				break;
			default:
				// Delay, RetriggerableDelay and Tween: Duration, read once.
				execution.WaitTime = 0.0;
				execution.WaitLimit = slots[input].Float;
				break;
		}
		if (pending != null)
		{
			// The latest trigger waits in the earlier one's place in the pending order; nothing of the earlier one's stack
			// ever runs.
			execution.Instance.ReplaceFiber(pending, execution);
			Discard(pending);
		}
		Suspend(execution, node, frame, execution.Instance.Updates);
		return true;
	}

	// The execution suspends in the latent node, which is pending while the node's state slot holds it.
	private static void Suspend(Execution execution, CompiledNode node, Frame frame, ulong since)
	{
		execution.SuspendedAt = node.Index;
		execution.Current = node;
		execution.Since = since;
		frame.Slots[node.StateSlot] = Slot.FromObject(execution);
		EndChain(execution, node);
	}

	// A resumed fiber's latent node stops being pending.
	private static void StopPending(Execution execution, CompiledNode node, Frame frame)
	{
		if (ReferenceEquals(frame.Slots[node.StateSlot].Object, execution))
		{
			frame.Slots[node.StateSlot] = default;
		}
	}

	// The fiber waiting in a latent node of the frame; null when the node is not pending. The state slot is checked against
	// the fiber, so a reference that outlived its fiber (resumed or released) never counts.
	private static Execution? PendingAt(Frame frame, CompiledNode node) => frame.Slots[node.StateSlot].Object is Execution fiber &&
		fiber.SuspendedAt == node.Index && ReferenceEquals(fiber.TopFrame, frame) ? fiber : null;

	// An impure function call: the call frame (the inputs, the locals and outputs of the function's nodes at their initial
	// values, fresh node states) goes on the call stack, and the flow enters the function at its Entry.
	private void Call(Execution execution, CompiledNode node, Frame frame)
	{
		PushFrame(execution, node, frame, null, 0);
		execution.Node = node.Function!.EntryNode;
		execution.Entry = 0;
		if (execution.Debugger is { } debugger)
		{
			debugger.OnStep(execution.Instance, node.Id, null);
		}
	}

	// Pushes the frame of a call of the node's function with the node's staged inputs as the function's inputs.
	private Frame PushFrame(Execution execution, CompiledNode call, Frame frame, int[]? resumePlan, int resumeIndex)
	{
		if (m_Depth >= MaxDepth)
		{
			throw new VisualScriptException(DepthMessage);
		}
		CompiledFunction function = call.Function!;
		Frame callee = function.AcquireFrame();
		int parameters = function.Graph.Nodes[function.EntryNode].OutputBase;
		Array.Copy(frame.Slots, call.InputBase, callee.Slots, parameters, call.InputCount);
		execution.PushCall(callee, call.Index, resumePlan, resumeIndex);
		m_Depth++;
		return callee;
	}

	// Ends the top call: its pending continuations are discarded, the call node's outputs take the Return node's inputs
	// (the type defaults when the chain ended without one), and the caller continues at the call's Then.
	private void Return(Execution execution, CompiledNode? returnNode)
	{
		CallRecord record = execution.PopCall();
		m_Depth--;
		execution.TruncateFlow(record.FlowBase);
		Frame callee = record.Frame;
		CompiledFunction function = callee.Graph.Function!;
		Frame caller = execution.TopFrame;
		CompiledNode call = caller.Graph.Nodes[record.CallNode];
		if (returnNode != null)
		{
			Array.Copy(callee.Slots, returnNode.InputBase, caller.Slots, call.OutputBase, call.OutputCount);
		}
		else
		{
			Array.Copy(function.OutputDefaults, 0, caller.Slots, call.OutputBase, call.OutputCount);
		}
		function.ReleaseFrame(callee);
		Follow(execution, call, 0);
	}

	// Evaluates the pure nodes the node's inputs depend on, then stages its inputs. Errors from then on are the node's own.
	private void Evaluate(Execution execution, CompiledNode node, Frame frame)
	{
		if (node.PureSources.Length > 0)
		{
			EvaluatePlan(execution, node.Plan ??= Plans.Of(frame.Graph, node), frame);
			execution.Current = node;
		}
		Load(execution, node, frame.Slots);
	}

	// Evaluates a plan's pure nodes in order, each once. A pure function call pushes its frame and evaluates the function's
	// Return plan in it, then the caller's plan continues where it was: no host recursion.
	private void EvaluatePlan(Execution execution, int[] plan, Frame frame)
	{
		int floor = execution.CallCount;
		int index = 0;
		while (true)
		{
			if (index < plan.Length)
			{
				CompiledNode node = frame.Graph.Nodes[plan[index++]];
				Step(execution, node);
				if (node.Op != Opcode.PureCall)
				{
					EvaluatePure(execution, node, frame);
					continue;
				}
				Load(execution, node, frame.Slots);
				frame = PushFrame(execution, node, frame, plan, index);
				CompiledNode returnNode = frame.Graph.Nodes[node.Function!.ReturnNode];
				plan = returnNode.PureSources.Length > 0 ? returnNode.Plan ??= Plans.Of(frame.Graph, returnNode) : [];
				index = 0;
				continue;
			}
			if (execution.CallCount == floor)
			{
				return;
			}
			// A pure call's plan is done: its Return node's inputs are the call's outputs.
			CompiledFunction function = frame.Graph.Function!;
			CompiledNode returned = frame.Graph.Nodes[function.ReturnNode];
			Step(execution, returned);
			Load(execution, returned, frame.Slots);
			CallRecord record = execution.PopCall();
			m_Depth--;
			Frame caller = execution.TopFrame;
			CompiledNode call = caller.Graph.Nodes[record.CallNode];
			Array.Copy(frame.Slots, returned.InputBase, caller.Slots, call.OutputBase, call.OutputCount);
			function.ReleaseFrame(frame);
			if (execution.Debugger is { } debugger)
			{
				ReportValues(debugger, execution, call, caller.Slots);
			}
			frame = caller;
			plan = record.ResumePlan!;
			index = record.ResumeIndex;
		}
	}

	// Stages the node's connected inputs: each value converted to its input's staging type.
	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	private static void Load(Execution execution, CompiledNode node, Slot[] slots)
	{
		foreach (ref readonly Operand operand in node.Operands.AsSpan())
		{
			Slot value;
			switch (operand.Source)
			{
				case OperandSource.Slot:
					value = slots[operand.Index];
					break;
				case OperandSource.LoopIndex:
					int loop = execution.FindLoop(operand.Index);
					value = Slot.FromInt(loop >= 0 ? execution.Flow[loop].Index : 0);
					break;
				default:
					// Outside an active iteration Element is its type default, which the ForEach node holds.
					int each = execution.FindLoop(operand.Index);
					value = each >= 0 ? ArrayElements.Read(execution.Flow[each].Items!, execution.Flow[each].Index, operand.From)
						: execution.TopFrame.Graph.Nodes[operand.Index].Constant;
					break;
			}
			if (operand.Conversion != ConversionOp.None)
			{
				Conversions.Apply(ref value, operand.Conversion, operand.From);
			}
			slots[operand.Target] = value;
		}
	}

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	private void EvaluatePure(Execution execution, CompiledNode node, Frame frame)
	{
		Slot[] slots = frame.Slots;
		Load(execution, node, slots);
		int input = node.InputBase;
		int output = node.OutputBase;
		switch (node.Op)
		{
			case Opcode.Self:
				slots[output] = Slot.FromObject(m_Environment.GetEntity(execution.Instance.Entity));
				break;
			case Opcode.Literal:
				slots[output] = node.Constant;
				break;
			case Opcode.Reroute:
				slots[output] = slots[input];
				break;
			case Opcode.Select:
				slots[output] = slots[input].Bool ? slots[input + 1] : slots[input + 2];
				break;
			case Opcode.IsValid:
				slots[output] = Slot.FromBool(IsValid(node, slots[input].Object));
				break;
			case Opcode.Convert:
				Convert(execution, node, slots);
				break;
			case Opcode.Make:
			case Opcode.Break:
				(node.Invoker ??= GetStructInvoker(node.ToType!, node.Op == Opcode.Make))(slots, input, output);
				break;
			case Opcode.Format:
				slots[output] = Slot.FromString(Format(node, slots));
				break;
			case Opcode.ArrayLength:
				Array lengthOf = slots[input].Object as Array ?? throw new VisualScriptException(NoArray);
				slots[output] = Slot.FromInt(lengthOf.Length);
				break;
			case Opcode.ArrayGet:
				Array elements = slots[input].Object as Array ?? throw new VisualScriptException(NoArray);
				int index = slots[input + 1].Int;
				if (index < 0 || index >= elements.Length)
				{
					throw new VisualScriptException(FormattableString.Invariant($"index {index} is outside the array (length {elements.Length})"));
				}
				slots[output] = ArrayElements.Read(elements, index, node.FromType!);
				break;
			case Opcode.VariableGet:
				slots[output] = node.IsLocal ? slots[node.Count] : execution.Instance.Variables[node.Count];
				break;
			case Opcode.PureMember:
				InvokeMember(execution, node, slots);
				break;
			case Opcode.Arithmetic:
				slots[output] = Operators.Arithmetic(node.Operator, node.Kind, slots[input], slots[input + 1]);
				break;
			case Opcode.Negate:
				slots[output] = Operators.Negate(node.Kind, slots[input]);
				break;
			case Opcode.Compare:
				slots[output] = Slot.FromBool(Operators.Compare(node.Operator, node.Kind, slots[input], slots[input + 1]));
				break;
			case Opcode.Equality:
				slots[output] = Slot.FromBool(Equal(node, slots) != node.Invert);
				break;
			case Opcode.Concatenate:
				slots[output] = Slot.FromString(string.Concat((string?)slots[input].Object, (string?)slots[input + 1].Object));
				break;
			case Opcode.Boolean:
				slots[output] = Slot.FromBool(Operators.Boolean(node.Operator, slots[input].Bool, node.InputCount > 1 && slots[input + 1].Bool));
				break;
			case Opcode.OperatorMethod:
				(node.Invoker ??= GetInvoker(node.Member!, MemberNodeKind.Call))(slots, input, output);
				break;
			case Opcode.Expression:
				ExpressionMachine.Run(node.Expression!, slots);
				break;
			case Opcode.Intrinsic:
				if (node.Member != null)
				{
					node.Invoker ??= GetInvoker(node.Member, MemberNodeKind.Call);
				}
				Intrinsics.Evaluate(node, slots);
				break;
			default:
				throw new InvalidOperationException($"{node} is impure: a plan never evaluates it");
		}
		if (execution.Debugger is { } debugger)
		{
			ReportValues(debugger, execution, node, slots);
		}
	}

	// The values a pure node of a traced execution computed (spec section 5.5).
	private static void ReportValues(IVisualScriptDebugger debugger, Execution execution, CompiledNode node, Slot[] slots)
	{
		for (int index = 0; index < node.OutputCount; index++)
		{
			object? value = SlotValues.ToValue(slots[node.OutputBase + index], node.OutputTypes[index]);
			debugger.OnValue(execution.Instance, node.Id, node.OutputPins[index], value);
		}
	}

	// Calls a reflected member. Component members fetch the Target entity's component through the environment, and members
	// of the game's script classes its script instance (Self when Target is unconnected); null Targets of reference types
	// are aborting errors.
	private void InvokeMember(Execution execution, CompiledNode node, Slot[] slots)
	{
		switch (node.Target)
		{
			case TargetKind.Entity:
				Entity entity = (node.TargetIsSelf ? m_Environment.GetEntity(execution.Instance.Entity) : (Entity?)slots[node.InputBase].Object) ??
					throw new VisualScriptException(NoTarget);
				slots[node.InputBase] = Slot.FromObject(entity);
				break;
			case TargetKind.Component:
				// The component is fetched by its entity's ID, so Self needs no entity object.
				ulong id = node.TargetIsSelf ? execution.Instance.Entity : ((Entity?)slots[node.InputBase].Object)?.ID ?? 0;
				if (id == 0)
				{
					throw new VisualScriptException(NoTarget);
				}
				Component component = m_Environment.GetComponent(id, node.DeclaringType!) ??
					throw new VisualScriptException($"{m_Environment.GetEntityName(id)} has no {Titles.OfType(node.DeclaringType!)}");
				slots[node.InputBase] = Slot.FromObject(component);
				break;
			case TargetKind.Script:
				// The script instance is looked up by its entity's ID, so Self and any entity value reach the same instance.
				ulong scripted = node.TargetIsSelf ? execution.Instance.Entity : ((Entity?)slots[node.InputBase].Object)?.ID ?? 0;
				if (scripted == 0)
				{
					throw new VisualScriptException(NoTarget);
				}
				Entity? script = m_Environment.GetEntity(scripted);
				if (!node.DeclaringType!.IsInstanceOfType(script))
				{
					throw new VisualScriptException($"{m_Environment.GetEntityName(scripted)} has no {Titles.OfType(node.DeclaringType!)} script");
				}
				slots[node.InputBase] = Slot.FromObject(script);
				break;
			case TargetKind.Value when !node.DeclaringType!.IsValueType && slots[node.InputBase].Object == null:
				throw new VisualScriptException(NoTarget);
		}
		(node.Invoker ??= GetInvoker(node.Member!, node.MemberKind))(slots, node.InputBase, node.OutputBase);
	}

	// Event.Send: the matching Event.Custom of the Target's instance runs as a nested execution with the parameter inputs as
	// its outputs; it returns when that execution ends or suspends (the suspended part is a fiber of the Target's instance).
	// Without a receiver a Warning is logged, once per node per instance. An abort in the nested execution aborts this one.
	private ExecutionResult Send(Execution execution, CompiledNode node, Slot[] slots)
	{
		Entity? target = node.TargetIsSelf ? m_Environment.GetEntity(execution.Instance.Entity) : (Entity?)slots[node.InputBase].Object;
		if (target is null)
		{
			Warn(execution, node, $"Target is None, so no graph receives {node.EventName}");
			return ExecutionResult.Ended;
		}
		if (!m_Instances.TryGetValue(target.ID, out GraphInstance? receiver))
		{
			Warn(execution, node, $"{m_Environment.GetEntityName(target.ID)} ({target.ID}) runs no visual script to receive {node.EventName}");
			return ExecutionResult.Ended;
		}
		CompiledGraph graph = receiver.Program.EventGraph;
		int custom = receiver.Program.FindCustomEvent(node.EventName!);
		if (custom < 0 || !SameTypes(graph.Nodes[custom].EventParameters, node.EventParameters))
		{
			Warn(execution, node,
				$"the graph of {m_Environment.GetEntityName(target.ID)} ({target.ID}) has no custom event {node.EventName}({string.Join(", ", (object[])node.EventParameters)})");
			return ExecutionResult.Ended;
		}
		if (m_Depth >= MaxDepth)
		{
			throw new VisualScriptException(DepthMessage);
		}
		CompiledNode eventNode = graph.Nodes[custom];
		Array.Copy(slots, node.InputBase + 1, receiver.Frame.Slots, eventNode.OutputBase, eventNode.OutputCount);
		GraphProgram program = receiver.Program;
		Enter(program);
		try
		{
			Execution nested = Acquire(receiver, custom, 0);
			ExecutionResult result = Execute(nested, false);
			switch (result)
			{
				case ExecutionResult.Suspended:
					Park(nested);
					return ExecutionResult.Ended;
				case ExecutionResult.Aborted:
					execution.Error = nested.Error;
					Release(nested);
					return ExecutionResult.Aborted;
				default:
					Release(nested);
					return ExecutionResult.Ended;
			}
		}
		finally
		{
			Leave(program);
		}
	}

	private static bool SameTypes(VisualType[] a, VisualType[] b)
	{
		if (a.Length != b.Length)
		{
			return false;
		}
		for (int index = 0; index < a.Length; index++)
		{
			if (!ReferenceEquals(a[index], b[index]) && a[index].Id != b[index].Id)
			{
				return false;
			}
		}
		return true;
	}

	// IsValid (spec section 4.4), by the type of its value.
	private bool IsValid(CompiledNode node, object? value)
	{
		if (value == null)
		{
			return false;
		}
		switch (node.Validity)
		{
			case ValidityKind.Entity:
				return m_Environment.IsEntityValid(((Entity)value).ID);
			case ValidityKind.Component:
				Component component = (Component)value;
				ulong entity = component.Entity?.ID ?? 0;
				return m_Environment.IsEntityValid(entity) && m_Environment.HasComponent(entity, component.GetType());
			case ValidityKind.Asset:
				return ((Asset)value).Handle.IsValid;
			default:
				return true;
		}
	}

	// The equality table (spec section 4.4); arrays element by element (spec-slice3 section 3.4.2 rule 3). An equality
	// operator method of the operands writes its result to the node's output.
	private bool Equal(CompiledNode node, Slot[] slots)
	{
		if (node.Member != null)
		{
			node.Invoker ??= GetInvoker(node.Member, MemberNodeKind.Call);
		}
		int input = node.InputBase;
		if (node.Equality == EqualityKind.Array)
		{
			return ValueEquality.ArraysEqual(node, slots[input].Object as Array ?? throw new VisualScriptException(NoArray),
				slots[input + 1].Object as Array ?? throw new VisualScriptException(NoArray), slots);
		}
		return ValueEquality.Equal(node.Equality, node.Kind, node.Invoker, slots, input, node.OutputBase);
	}

	// Convert (spec section 4.4).
	private void Convert(Execution execution, CompiledNode node, Slot[] slots)
	{
		int input = node.InputBase;
		int output = node.OutputBase;
		VisualType from = node.FromType!;
		VisualType to = node.ToType!;
		Slot value = slots[input];
		switch (node.ConvertPlan)
		{
			case ConvertPlan.Identity:
				slots[output] = value;
				break;
			case ConvertPlan.NumericCast:
				slots[output] = Conversions.NumericCast(value, SlotValues.KindOf(from), SlotValues.KindOf(to));
				break;
			case ConvertPlan.EnumToInteger:
				// The bits are the underlying value: as a long for signed underlying types (node.Kind), else as a ulong.
				slots[output] = Conversions.NumericCast(value, node.Kind, SlotValues.KindOf(to));
				break;
			case ConvertPlan.IntegerToEnum:
				ulong integer = SlotValues.KindOf(from) switch
				{
					SlotKind.Int => unchecked((ulong)value.Int),
					SlotKind.UInt => value.UInt,
					SlotKind.Long => unchecked((ulong)value.Long),
					_ => value.ULong,
				};
				slots[output] = Slot.FromULong(SlotValues.ToEnumBits(integer, to.ClrType!));
				break;
			case ConvertPlan.ParseBool:
			case ConvertPlan.ParseNumber:
			case ConvertPlan.ParseEnum:
				string text = (string?)value.Object ?? string.Empty;
				if (Parsing.TryParse(node.ConvertPlan, text, to, out Slot parsed))
				{
					slots[output] = parsed;
					break;
				}
				slots[output] = node.Constant;
				Warn(execution, node, $"{JsonText.Quote(text)} cannot be read as {to.Title}: the result is {Conversions.ToText(slots[output], to)}");
				break;
			case ConvertPlan.ToText:
				slots[output] = Slot.FromString(Conversions.ToText(value, from));
				break;
			case ConvertPlan.ScriptInstance:
				Entity? entity = (Entity?)value.Object;
				Entity? instance = entity is null ? null : m_Environment.GetEntity(entity.ID);
				slots[output] = Slot.FromObject(to.ClrType!.IsInstanceOfType(instance) ? instance : null);
				break;
			case ConvertPlan.Link:
				Conversions.Apply(ref value, node.Conversion, from);
				slots[output] = value;
				break;
			default:
				(node.Invoker ??= GetInvoker(node.Member!, MemberNodeKind.Call))(slots, input, output);
				break;
		}
	}

	// FormatString: the literal text and the placeholders' values (staged as text).
	private static string Format(CompiledNode node, Slot[] slots)
	{
		DefaultInterpolatedStringHandler text = new(0, node.Segments.Length);
		foreach (FormatSegment segment in node.Segments)
		{
			if (segment.Text != null)
			{
				text.AppendLiteral(segment.Text);
			}
			else
			{
				text.AppendFormatted((string?)slots[node.InputBase + segment.Placeholder].Object);
			}
		}
		return text.ToStringAndClear();
	}

	// A non-aborting error: logged once per node per instance (spec section 5.3). Its text is built only when it is logged
	// (WarningText), so a Warning the node logged already costs one set lookup.
	private void Warn(Execution execution, CompiledNode node, [InterpolatedStringHandlerArgument(nameof(execution), nameof(node))] ref WarningText message)
	{
		if (message.IsLogged)
		{
			m_Environment.Log(VisualScriptLogLevel.Warn, Describe(execution, node, message.ToStringAndClear()));
		}
	}

	private void Warn(Execution execution, CompiledNode node, string message)
	{
		if (execution.Instance.ShouldWarn(node.Id))
		{
			m_Environment.Log(VisualScriptLogLevel.Warn, Describe(execution, node, message));
		}
	}

	// "<graph name>: <node title> (node <id>) on <entity name> (<entity id>): <message>".
	private string Describe(Execution execution, CompiledNode node, string message)
	{
		ulong entity = execution.Instance.Entity;
		return FormattableString.Invariant($"{execution.Program.Name}: {node.Title} (node {node.Id}) on {m_Environment.GetEntityName(entity)} ({entity}): {message}");
	}
}

// The plans of pure evaluation (spec section 5.3, Data evaluation): the pure nodes a node's inputs depend on, each once, in
// dependency order.
internal static class Plans
{
	internal static int[] Of(CompiledGraph graph, CompiledNode consumer)
	{
		List<int> order = [];
		HashSet<int> visited = [];
		Stack<(int Node, int Next)> pending = new();
		foreach (int source in consumer.PureSources)
		{
			if (!visited.Add(source))
			{
				continue;
			}
			pending.Push((source, 0));
			while (pending.TryPop(out (int Node, int Next) top))
			{
				int[] sources = graph.Nodes[top.Node].PureSources;
				if (top.Next < sources.Length)
				{
					pending.Push((top.Node, top.Next + 1));
					if (visited.Add(sources[top.Next]))
					{
						pending.Push((sources[top.Next], 0));
					}
					continue;
				}
				order.Add(top.Node);
			}
		}
		return [.. order];
	}
}

// The text of a node's Warning, formatted in the invariant culture only when the Warning is logged: once per node per
// instance (spec section 5.3), so a later one evaluates none of its parts and allocates nothing.
[InterpolatedStringHandler]
internal ref struct WarningText
{
	private DefaultInterpolatedStringHandler m_Text;

	internal WarningText(int literalLength, int formattedCount, Execution execution, CompiledNode node, out bool isLogged)
	{
		isLogged = execution.Instance.ShouldWarn(node.Id);
		IsLogged = isLogged;
		m_Text = isLogged ? new DefaultInterpolatedStringHandler(literalLength, formattedCount, CultureInfo.InvariantCulture) : default;
	}

	internal bool IsLogged { get; }

	internal void AppendLiteral(string text)
	{
		m_Text.AppendLiteral(text);
	}

	internal void AppendFormatted<T>(T value)
	{
		m_Text.AppendFormatted(value);
	}

	internal string ToStringAndClear() => m_Text.ToStringAndClear();
}
