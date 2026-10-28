using System;
using System.Buffers;

namespace Strada.VisualScripting;

// The state of a Flow.Timeline node in an instance (spec-slice3 section 3.8.4), made by its first input: where it is, the
// direction and rate it plays at, whether an event key at the current time fires at the next advance, and a counter of the
// inputs that changed something (an advance stops firing ticks once a tick handler changed the timeline).
internal sealed class TimelineState
{
	internal double Time { get; set; }

	internal TimelineDirection Direction { get; set; }

	internal double Rate { get; set; } = 1.0;

	internal bool ClosedStart { get; set; }

	internal uint Generation { get; set; }
}

// Flow.Timeline in the VM (spec-slice3 sections 3.8.4 and 3.8.5). The timeline plays while it has a run: an execution that
// waits in the node as a fiber of its instance (the node's state slot holds it), which is never resumed. The pending pass
// advances it: the event keys crossed, Update and, at the end, Finished fire as ticks, each a new execution starting at
// the node. Inputs end the chain that reached them; a timeline never suspends an execution.
internal sealed partial class VisualScriptRuntime
{
	// Spec-slice3 section 3.8.4: the event keys an advance fires at most.
	internal const int MaxTimelineFirings = 1000;

	private const int PlayInput = 0;
	private const int PlayFromStartInput = 1;
	private const int ReverseInput = 2;
	private const int ReverseFromEndInput = 3;
	private const int StopInput = 4;
	private const int SetTimeInput = 5;

	// The staged inputs, in DataInputs' order.
	private const int NewTimeInput = 0;
	private const int RateInput = 1;

	// An input of the timeline (entries 0 to 5), or a tick leaving it (TickEntry, through the tick execution's exit).
	private void TimelineInput(Execution execution, CompiledNode node, Frame frame, int entry)
	{
		if (entry == Execution.TickEntry)
		{
			Follow(execution, node, execution.TickExit);
			return;
		}
		CompiledTimeline timeline = node.Timeline!;
		TimelineDefinition definition = timeline.Definition;
		Slot[] slots = frame.Slots;
		TimelineState state = StateOf(slots, node);
		Execution? run = PendingAt(frame, node);
		bool changed = true;
		switch (entry)
		{
			case PlayInput:
			case ReverseInput:
				double rate = ReadRate(execution, node, frame);
				TimelineDirection direction = entry == PlayInput ? TimelineDirection.Forward : TimelineDirection.Backward;
				changed = state.Direction != direction || !state.Rate.Equals(rate);
				state.Direction = direction;
				state.Rate = rate;
				if (run == null && (run = StartRun(execution, node, frame)) != null)
				{
					state.ClosedStart = true;
					changed = true;
				}
				break;
			case PlayFromStartInput:
			case ReverseFromEndInput:
				state.Rate = ReadRate(execution, node, frame);
				bool forward = entry == PlayFromStartInput;
				state.Time = forward ? 0.0 : definition.Length;
				state.Direction = forward ? TimelineDirection.Forward : TimelineDirection.Backward;
				state.ClosedStart = true;
				run ??= StartRun(execution, node, frame);
				break;
			case StopInput:
				changed = run != null;
				if (run != null)
				{
					Discard(run);
					run = null;
				}
				break;
			case SetTimeInput:
				EvaluateInput(execution, node, frame, NewTimeInput);
				float time = slots[node.InputBase + NewTimeInput].Float;
				state.Time = float.IsNaN(time) ? 0.0 : Math.Clamp(time, 0.0, definition.Length);
				state.ClosedStart = false;
				break;
		}
		if (changed)
		{
			state.Generation++;
		}
		WriteTimeline(slots, timeline, state, run != null);
		EndChain(execution, node);
	}

	// Rate, read by the inputs that play: a negative or non-finite rate counts as 0 (the timeline holds still).
	private double ReadRate(Execution execution, CompiledNode node, Frame frame)
	{
		EvaluateInput(execution, node, frame, RateInput);
		double rate = frame.Slots[node.InputBase + RateInput].Float;
		return double.IsFinite(rate) && rate > 0.0 ? rate : 0.0;
	}

	// The instance's state of the timeline, made at its first input: at 0, forward, at rate 1.
	private static TimelineState StateOf(Slot[] slots, CompiledNode node)
	{
		if (slots[node.TimelineSlot].Object is TimelineState state)
		{
			return state;
		}
		TimelineState created = new();
		slots[node.TimelineSlot] = Slot.FromObject(created);
		return created;
	}

	// A run of the timeline: pending in the node from now on, first advanced in the Update dispatch after this one. An
	// instance that is being destroyed plays nothing (a Warning, once).
	private Execution? StartRun(Execution execution, CompiledNode node, Frame frame)
	{
		GraphInstance instance = execution.Instance;
		if (!instance.IsAlive)
		{
			Warn(execution, node, "the graph's instance was destroyed, so nothing can play");
			return null;
		}
		Execution run = Acquire(instance, node.Index, 0);
		run.Node = -1;
		run.SuspendedAt = node.Index;
		run.Current = node;
		run.Since = instance.Updates;
		frame.Slots[node.StateSlot] = Slot.FromObject(run);
		instance.AddFiber(run);
		return run;
	}

	// The outputs at the state's time: Time, Direction, Playing and every curve's value.
	private static void WriteTimeline(Slot[] slots, CompiledTimeline timeline, TimelineState state, bool playing)
	{
		slots[timeline.TimeSlot] = Slot.FromFloat((float)state.Time);
		slots[timeline.DirectionSlot] = state.Direction == TimelineDirection.Forward ? timeline.Forward : timeline.Backward;
		slots[timeline.PlayingSlot] = Slot.FromBool(playing);
		TimelineTrack[] curves = timeline.Definition.Curves;
		for (int curve = 0; curve < curves.Length; curve++)
		{
			slots[timeline.CurveBase + curve] = curves[curve].Evaluate(state.Time);
		}
	}

	// One advance of a playing timeline (spec-slice3 section 3.8.4): the time moves by the step times Rate, the outputs are
	// written at the new time, then the event keys crossed fire, Update fires and, when a timeline that does not loop
	// reached its end, the run ends and Finished fires. A tick whose handler changed the timeline (any input that changed
	// something) ends the advance's ticks. True when a tick ended in an aborting error.
	private bool AdvanceTimeline(PendingFiber pending, CompiledNode node, double step)
	{
		Execution run = pending.Fiber;
		GraphInstance instance = run.Instance;
		Slot[] slots = run.TopFrame.Slots;
		CompiledTimeline timeline = node.Timeline!;
		TimelineState state = (TimelineState)slots[node.TimelineSlot].Object!;
		double advance = step * state.Rate;
		uint generation = state.Generation;
		int[] firings = ArrayPool<int>.Shared.Rent(MaxTimelineFirings);
		try
		{
			TimelineSweep sweep = new(timeline.Definition, firings, state.ClosedStart);
			double to = state.Direction == TimelineDirection.Forward ? sweep.Forward(state.Time, double.IsFinite(advance) ? advance : 0.0)
				: sweep.Backward(state.Time, double.IsFinite(advance) ? advance : 0.0);
			if (sweep.Capped)
			{
				Warn(run, node, $"more than {MaxTimelineFirings} timeline events were due in one update: the time beyond them is dropped");
			}
			state.Time = to;
			state.ClosedStart = false;
			WriteTimeline(slots, timeline, state, true);
			bool failed = false;
			for (int index = 0; index < sweep.Count; index++)
			{
				if (!IsAdvancing(pending, instance, state, generation))
				{
					return failed;
				}
				failed |= TimelineTick(instance, node, timeline.EventExits[firings[index]]);
			}
			if (!IsAdvancing(pending, instance, state, generation))
			{
				return failed;
			}
			failed |= TimelineTick(instance, node, timeline.UpdateExit);
			if (!sweep.Finished || !IsAdvancing(pending, instance, state, generation))
			{
				return failed;
			}
			Discard(run);
			slots[timeline.PlayingSlot] = Slot.FromBool(false);
			return failed | TimelineTick(instance, node, timeline.FinishedExit);
		}
		finally
		{
			ArrayPool<int>.Shared.Return(firings);
		}
	}

	// Whether the advance still fires ticks: the instance lives, no input changed the timeline, and the run advanced is
	// still the one playing.
	private static bool IsAdvancing(PendingFiber pending, GraphInstance instance, TimelineState state, uint generation) =>
		instance.IsAlive && state.Generation == generation && IsWaiting(pending, instance);

	// An exec output of the timeline fires: a new execution with a budget of its own leaves the node through it, in the event
	// graph's frame. Nothing runs for an unlinked output.
	private bool TimelineTick(GraphInstance instance, CompiledNode node, int exit)
	{
		if (node.Exits[exit].Node < 0)
		{
			return false;
		}
		Execution tick = Acquire(instance, node.Index, Execution.TickEntry);
		tick.TickExit = exit;
		return Finish(tick, Execute(tick, true));
	}

	// Where an advance lands and the event keys it crosses, in the order they fire (spec-slice3 section 3.8.4): in the
	// direction of play, by time, then track, then key. A key at the time the advance starts from fires only when the start
	// is closed (the run just started, or the time was just set by PlayFromStart or ReverseFromEnd). In a loop the end and
	// the start are one instant: playing forward, a key at Length never fires and a key at 0 fires every cycle (backward the
	// other way round); landing exactly on the end wraps. At most MaxTimelineFirings keys fire: the time beyond the last of
	// them is dropped.
	private struct TimelineSweep
	{
		private readonly TimelineDefinition m_Timeline;
		private readonly int[] m_Firings;
		private readonly bool m_ClosedStart;
		private double m_LastTime;

		internal TimelineSweep(TimelineDefinition timeline, int[] firings, bool closedStart)
		{
			m_Timeline = timeline;
			m_Firings = firings;
			m_ClosedStart = closedStart;
		}

		// The event tracks (indices among the definition's Events) whose keys fire, firings[0..Count).
		internal int Count { get; private set; }

		internal bool Capped { get; private set; }

		// A timeline that does not loop reached its end in the direction of play.
		internal bool Finished { get; private set; }

		internal double Forward(double from, double advance)
		{
			double length = m_Timeline.Length;
			double end = from + advance;
			double to;
			if (!m_Timeline.Loop)
			{
				to = Math.Min(end, length);
				Finished = end >= length;
				CollectForward(from, to, m_ClosedStart, true);
			}
			else if (end < length)
			{
				to = end;
				CollectForward(from, end, m_ClosedStart, true);
			}
			else
			{
				CollectForward(from, length, m_ClosedStart, false);
				double remaining = end - length;
				if (remaining >= length && !HasKeys(0.0, length, true, false))
				{
					remaining %= length;
				}
				while (remaining >= length && !Capped)
				{
					CollectForward(0.0, length, true, false);
					remaining -= length;
				}
				CollectForward(0.0, remaining, true, true);
				to = remaining;
			}
			if (Capped)
			{
				Finished = false;
				return m_LastTime;
			}
			return to;
		}

		internal double Backward(double from, double advance)
		{
			double length = m_Timeline.Length;
			double end = from - advance;
			double to;
			if (!m_Timeline.Loop)
			{
				to = Math.Max(end, 0.0);
				Finished = end <= 0.0;
				CollectBackward(to, from, true, m_ClosedStart);
			}
			else if (end > 0.0)
			{
				to = end;
				CollectBackward(end, from, true, m_ClosedStart);
			}
			else
			{
				CollectBackward(0.0, from, false, m_ClosedStart);
				double remaining = -end;
				if (remaining >= length && !HasKeys(0.0, length, false, true))
				{
					remaining %= length;
				}
				while (remaining >= length && !Capped)
				{
					CollectBackward(0.0, length, false, true);
					remaining -= length;
				}
				to = length - remaining;
				CollectBackward(to, length, true, true);
			}
			if (Capped)
			{
				Finished = false;
				return m_LastTime;
			}
			return to;
		}

		// Whether a key lies in the range (its ends included as said).
		private readonly bool HasKeys(double low, double high, bool includeLow, bool includeHigh) =>
			Start(low, includeLow) < End(high, includeHigh);

		// The keys from low to high, by time, then track, then key.
		private void CollectForward(double low, double high, bool includeLow, bool includeHigh)
		{
			int end = End(high, includeHigh);
			for (int index = Start(low, includeLow); index < end && Add(index); index++)
			{
			}
		}

		// The keys from high down to low: by time descending, then track, then key.
		private void CollectBackward(double low, double high, bool includeLow, bool includeHigh)
		{
			double[] times = m_Timeline.EventTimes;
			int start = Start(low, includeLow);
			int group = End(high, includeHigh);
			while (group > start)
			{
				int first = group - 1;
				while (first > start && times[first - 1] == times[group - 1])
				{
					first--;
				}
				for (int index = first; index < group; index++)
				{
					if (!Add(index))
					{
						return;
					}
				}
				group = first;
			}
		}

		// The first key at or after time (after it when the time is excluded).
		private readonly int Start(double time, bool include)
		{
			double[] times = m_Timeline.EventTimes;
			int low = 0;
			int high = times.Length;
			while (low < high)
			{
				int middle = low + ((high - low) / 2);
				if (include ? times[middle] < time : times[middle] <= time)
				{
					low = middle + 1;
				}
				else
				{
					high = middle;
				}
			}
			return low;
		}

		// The first key after time (at or after it when the time is excluded).
		private readonly int End(double time, bool include) => Start(time, !include);

		private bool Add(int index)
		{
			if (Count == MaxTimelineFirings)
			{
				Capped = true;
				return false;
			}
			m_Firings[Count++] = m_Timeline.EventTracks[index];
			m_LastTime = m_Timeline.EventTimes[index];
			return true;
		}
	}
}
