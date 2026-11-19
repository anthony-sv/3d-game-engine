using System;
using System.Text;
using Strada.Interop;

namespace Strada;

/// <summary>
/// The game's telemetry: named channels of one number per fixed step, and marks on steps, which the editor records while
/// its Telemetry panel is open or an agent's recording runs (the telemetry tools) and plots, reads and exports; the game
/// player records nothing. A channel's name and a mark's label are 1 to 64 bytes of UTF-8 without control characters; a
/// longer one is refused, never cut. A value recorded during a fixed step (<see cref="Script.OnFixedUpdate"/>) is that
/// step's, and the last of several wins; one recorded between steps (<see cref="Script.OnUpdate"/>, an event) is the next
/// step's; a step that records nothing for a channel has no value there. Laps run from a mark <c>Lap n</c> to the next
/// mark <c>Lap n+1</c> on a later step; <c>Lap end</c> ends a lap in progress without making one. What the recorder
/// refuses (a name too long, a value that is not finite, too many channels) is dropped, counted and warned about once per
/// recording session. Any thread may call these, and while nothing records each returns after one native call.
/// </summary>
public static unsafe class Telemetry
{
	// Names of up to this many UTF-8 bytes cross without allocating; longer ones still reach the recorder, which refuses them.
	private const int StackNameBytes = 256;

	/// <summary>Whether a recording runs, so that values costly to compute are computed only when they are kept.</summary>
	public static bool IsRecording => InternalCalls.Telemetry_IsRecording() != 0;

	/// <summary>Records a channel's value for the current fixed step: in <see cref="Script.OnFixedUpdate"/> that step, between
	/// steps the next one. Nothing happens while no recording runs.</summary>
	/// <param name="channel">The channel's name, such as <c>Ride.Distance</c>: equal names are one channel.</param>
	/// <param name="value">A finite number; one that is not finite is dropped and counted.</param>
	public static void Record(string channel, float value)
	{
		ArgumentNullException.ThrowIfNull(channel);
		if (!IsRecording)
		{
			return;
		}
		Span<byte> buffer = stackalloc byte[StackNameBytes];
		ReadOnlySpan<byte> name = Encode(channel, buffer);
		fixed (byte* bytes = name)
		{
			InternalCalls.Telemetry_Record(bytes, name.Length, value);
		}
	}

	/// <summary>Marks the current fixed step with a label (a lap boundary such as <c>Lap 2</c>, a sector, an event), placed as
	/// <see cref="Record"/> places a value. A step keeps every mark it gets, in order. Nothing happens while no recording
	/// runs.</summary>
	/// <param name="label">The mark's label.</param>
	public static void Mark(string label)
	{
		ArgumentNullException.ThrowIfNull(label);
		if (!IsRecording)
		{
			return;
		}
		Span<byte> buffer = stackalloc byte[StackNameBytes];
		ReadOnlySpan<byte> name = Encode(label, buffer);
		fixed (byte* bytes = name)
		{
			InternalCalls.Telemetry_Mark(bytes, name.Length);
		}
	}

	/// <summary>Marks a given fixed step, counted from 1 as <see cref="Collision.Step"/> counts it: a contact handler, which
	/// runs after the frame's steps, marks the step its contact happened in, whatever the frame rate. A step before the
	/// recording's session began, one it never reaches and step 0 drop the mark, counted. Nothing happens while no recording
	/// runs.</summary>
	/// <param name="label">The mark's label.</param>
	/// <param name="step">The fixed step to mark.</param>
	public static void MarkStep(string label, ulong step)
	{
		ArgumentNullException.ThrowIfNull(label);
		if (!IsRecording)
		{
			return;
		}
		Span<byte> buffer = stackalloc byte[StackNameBytes];
		ReadOnlySpan<byte> name = Encode(label, buffer);
		fixed (byte* bytes = name)
		{
			InternalCalls.Telemetry_MarkStep(bytes, name.Length, step);
		}
	}

	// The text's UTF-8 bytes: in the buffer when they fit, otherwise in an array of their own.
	private static ReadOnlySpan<byte> Encode(string text, Span<byte> buffer)
	{
		if (Encoding.UTF8.GetMaxByteCount(text.Length) <= buffer.Length)
		{
			return buffer[..Encoding.UTF8.GetBytes(text, buffer)];
		}
		return NativeString.ToUtf8(text);
	}
}
