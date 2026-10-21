using System;
using System.Buffers;
using System.Collections.Generic;

namespace Strada.VisualScripting;

// The pending latent nodes of instances (spec sections 4.2 and 5.3): each Update dispatch ends with one pass over the
// instance's fibers in the order their nodes became pending, which resumes the due ones and advances Tweens and Intervals.
// A fiber is advanced only by Update dispatches after the one running when it suspended, so it never resumes in the
// dispatch that created it. The pass costs time linear in the number of fibers: each fiber resumes, waits on, leaves
// or takes another's place among its instance's fibers in constant time (GraphInstance).
internal sealed partial class VisualScriptRuntime
{
	// Spec section 4.2: an Interval fires at most this many ticks per dispatch.
	internal const int MaxTicksPerUpdate = 1000;

	// A fiber as the pass found it: a fiber released and reused while the pass runs has another serial.
	private readonly record struct PendingFiber(Execution Fiber, ulong Serial);

	// One pass over the instance's pending latent nodes (spec section 5.2, step 4). True when an execution it started or
	// resumed ended in an aborting error. timeStep is the dispatch's (a non-finite one counts as no time).
	private bool AdvancePending(GraphInstance instance, ulong dispatch, float timeStep)
	{
		int count = instance.FiberCount;
		if (count == 0)
		{
			return false;
		}
		PendingFiber[] due = ArrayPool<PendingFiber>.Shared.Rent(count);
		int dueCount = 0;
		for (LinkedListNode<Execution>? node = instance.FirstFiber; node != null; node = node.Next)
		{
			Execution fiber = node.Value;
			if (fiber.Since < dispatch)
			{
				due[dueCount++] = new PendingFiber(fiber, fiber.Serial);
			}
		}
		double step = double.IsFinite(timeStep) ? timeStep : 0.0;
		bool failed = false;
		try
		{
			for (int index = 0; index < dueCount && instance.IsAlive; index++)
			{
				PendingFiber pending = due[index];
				if (IsWaiting(pending, instance))
				{
					failed |= Advance(pending, step);
				}
			}
		}
		finally
		{
			Array.Clear(due, 0, dueCount);
			ArrayPool<PendingFiber>.Shared.Return(due);
		}
		return failed;
	}

	// Whether the fiber the pass found still waits in its instance (an execution earlier in the pass may have stopped,
	// replaced or destroyed it).
	private static bool IsWaiting(PendingFiber pending, GraphInstance instance) => pending.Fiber.Serial == pending.Serial &&
		pending.Fiber.SuspendedAt >= 0 && !pending.Fiber.IsRunning && instance.HasFiber(pending.Fiber);

	private bool Advance(PendingFiber pending, double step)
	{
		Execution fiber = pending.Fiber;
		CompiledNode node = fiber.TopFrame.Graph.Nodes[fiber.SuspendedAt];
		switch (node.Builtin)
		{
			case BuiltinNode.FlowDelay:
			case BuiltinNode.FlowRetriggerableDelay:
				// The time steps sum in double; a Duration of 0 or less (or NaN) resumes in the first later update.
				fiber.WaitTime += step;
				return (fiber.WaitTime >= fiber.WaitLimit || !(fiber.WaitLimit > 0.0)) && Continue(fiber, Execution.ResumeEntry);
			case BuiltinNode.FlowWaitFrames:
				return --fiber.WaitCount <= 0 && Continue(fiber, Execution.ResumeEntry);
			case BuiltinNode.FlowWaitUntil:
				return Continue(fiber, Execution.RecheckEntry);
			case BuiltinNode.FlowTween:
				return AdvanceTween(pending, node, step);
			default:
				return AdvanceInterval(pending, node, step);
		}
	}

	// Tween (spec section 4.2): the elapsed time grows by the step, Alpha follows the easing and Update fires; once the
	// elapsed time reaches Duration (at once for a Duration of 0 or less) the fiber resumes at Completed.
	private bool AdvanceTween(PendingFiber pending, CompiledNode node, double step)
	{
		Execution fiber = pending.Fiber;
		GraphInstance instance = fiber.Instance;
		fiber.WaitTime += step;
		bool done = fiber.WaitTime >= fiber.WaitLimit || !(fiber.WaitLimit > 0.0);
		double t = done ? 1.0 : Math.Clamp(fiber.WaitTime / fiber.WaitLimit, 0.0, 1.0);
		fiber.TopFrame.Slots[node.OutputBase] = Slot.FromFloat((float)Easings.Apply(node.Easing, t));
		bool failed = Tick(fiber, node);
		if (done && IsWaiting(pending, instance))
		{
			failed |= Continue(fiber, Execution.ResumeEntry);
		}
		return failed;
	}

	// Interval (spec section 4.2): the timer grows by the step and fires Tick each time it reaches Interval (once per update
	// for an Interval of 0 or less), Index counting the ticks from 0; after Count ticks (when Count > 0) the fiber resumes
	// at Completed. Ticks beyond the per-dispatch cap are dropped with a Warning: the timer keeps only the part of an
	// interval it had begun.
	private bool AdvanceInterval(PendingFiber pending, CompiledNode node, double step)
	{
		Execution fiber = pending.Fiber;
		GraphInstance instance = fiber.Instance;
		bool everyUpdate = !(fiber.WaitLimit > 0.0);
		if (!everyUpdate)
		{
			fiber.WaitTime += step;
		}
		bool failed = false;
		for (int fired = 0; everyUpdate ? fired == 0 : fiber.WaitTime >= fiber.WaitLimit; fired++)
		{
			if (fired == MaxTicksPerUpdate)
			{
				fiber.WaitTime = double.IsFinite(fiber.WaitTime) ? fiber.WaitTime % fiber.WaitLimit : 0.0;
				Warn(fiber, node, $"more than {MaxTicksPerUpdate} ticks were due in one update: the time beyond them is dropped");
				break;
			}
			if (!everyUpdate)
			{
				fiber.WaitTime -= fiber.WaitLimit;
			}
			fiber.TopFrame.Slots[node.OutputBase] = Slot.FromInt((int)Math.Min(fiber.WaitIndex, int.MaxValue));
			fiber.WaitIndex++;
			failed |= Tick(fiber, node);
			if (!IsWaiting(pending, instance))
			{
				return failed;
			}
			if (fiber.WaitCount > 0 && fiber.WaitIndex >= fiber.WaitCount)
			{
				return failed | Continue(fiber, Execution.ResumeEntry);
			}
		}
		return failed;
	}

	// The Tick output of a pending Tween or Interval fires: a new execution with an empty flow stack and a budget of its
	// own leaves the node through it, in the event graph's frame (spec section 5.3). Nothing runs for an unlinked Tick.
	private bool Tick(Execution fiber, CompiledNode node)
	{
		if (node.Exits[node.TickExit].Node < 0)
		{
			return false;
		}
		Execution tick = Acquire(fiber.Instance, node.Index, Execution.TickEntry);
		return Finish(tick, Execute(tick, true));
	}
}

// The easing curves of Flow.Tween (spec section 4.2), for t in [0, 1], computed in double.
internal static class Easings
{
	private const double BounceScale = 7.5625;
	private const double BounceSpan = 2.75;
	private const double BackSquare = 1.70158;
	private const double BackCube = 2.70158;

	internal static double Apply(Easing easing, double t) => easing switch
	{
		Easing.Linear => t,
		Easing.EaseIn => t * t,
		Easing.EaseOut => 1.0 - Square(1.0 - t),
		Easing.EaseInOut => t < 0.5 ? 2.0 * t * t : 1.0 - Square(-2.0 * t + 2.0) / 2.0,
		Easing.EaseInCubic => t * t * t,
		Easing.EaseOutCubic => 1.0 - Cube(1.0 - t),
		Easing.EaseInOutCubic => t < 0.5 ? 4.0 * t * t * t : 1.0 - Cube(-2.0 * t + 2.0) / 2.0,
		Easing.EaseOutBack => 1.0 + BackCube * Cube(t - 1.0) + BackSquare * Square(t - 1.0),
		Easing.EaseOutBounce => Bounce(t),
		_ => Elastic(t),
	};

	private static double Square(double value) => value * value;

	private static double Cube(double value) => value * value * value;

	private static double Bounce(double t)
	{
		if (t < 1.0 / BounceSpan)
		{
			return BounceScale * t * t;
		}
		if (t < 2.0 / BounceSpan)
		{
			return BounceScale * Square(t - 1.5 / BounceSpan) + 0.75;
		}
		if (t < 2.5 / BounceSpan)
		{
			return BounceScale * Square(t - 2.25 / BounceSpan) + 0.9375;
		}
		return BounceScale * Square(t - 2.625 / BounceSpan) + 0.984375;
	}

	private static double Elastic(double t)
	{
		if (t == 0.0 || t == 1.0)
		{
			return t;
		}
		return Math.Pow(2.0, -10.0 * t) * Math.Sin((10.0 * t - 0.75) * (2.0 * Math.PI / 3.0)) + 1.0;
	}
}
