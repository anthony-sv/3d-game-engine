using System;
using System.Text;
using Strada.Interop;

namespace Strada;

// Which edges an action query made outside every fixed step reads (the engine's InputActionView, Docs/Architecture.md
// section 7.4, "The fixed-step rule"); inside a step the engine reads that step's edges whatever the view.
internal enum InputActionView
{
	// The latest evaluation's edges: OnUpdate, events, clicks, contacts, OnDestroy and whatever a frame started.
	Frame = 0,
	// The edges of one fixed step, by its number: code a step started that a debugger resume runs between frames.
	FixedStep = 1,
	// None: code a step started that runs later for another reason (a latent node's continuation, a WaitUntil recheck, a
	// tick), where the frame's or a step's edges would show an edge twice.
	NoEdges = 2,
}

// The view of the code that asks, and for FixedStep the number of its step: what a graph execution records when it starts
// and runs under, and what C# callbacks run under (Frame).
internal readonly record struct InputActionOrigin(InputActionView View, ulong Step)
{
	internal static InputActionOrigin Frame => default;

	// What code a step started reads when it runs later outside a step for a reason other than a debugger resume: no edge.
	internal InputActionOrigin Continued() => View == InputActionView.FixedStep ? new InputActionOrigin(InputActionView.NoEdges, 0) : this;
}

// The view the action queries of the code running now read (Input.IsActionPressed and IsActionReleased): Frame while the
// engine runs a C# callback, the running execution's while a graph runs (VisualScriptRuntime sets and restores it). Main
// thread only.
internal static unsafe class InputActionContext
{
	// The bytes of the buffers names are encoded into on the stack; the engine's names have at most 64 bytes.
	internal const int StackNameBytes = 256;

	// The action name the last handled edge gave, and its UTF-8, so that handlers of the same action allocate nothing.
	private static string s_HandledName = string.Empty;
	private static byte[] s_HandledBytes = [];

	internal static InputActionOrigin Current { get; set; }

	// Makes the origin current until the returned value is given back to Restore.
	internal static InputActionOrigin Enter(InputActionOrigin origin)
	{
		InputActionOrigin previous = Current;
		Current = origin;
		return previous;
	}

	internal static void Restore(InputActionOrigin previous)
	{
		Current = previous;
	}

	// The name of the action whose edge the engine dispatches now (Input_GetHandledAction); "" outside an action event.
	internal static string GetHandledAction()
	{
		int length = 0;
		byte* name = InternalCalls.Input_GetHandledAction(&length);
		if (name == null || length <= 0)
		{
			return string.Empty;
		}
		ReadOnlySpan<byte> bytes = new(name, length);
		if (!bytes.SequenceEqual(s_HandledBytes))
		{
			s_HandledBytes = bytes.ToArray();
			s_HandledName = Encoding.UTF8.GetString(bytes);
		}
		return s_HandledName;
	}

	// The action name's UTF-8 in the buffer (StackNameBytes long, on the caller's stack: queries run every frame and must not
	// allocate), or on the heap for a name longer than the engine's.
	internal static ReadOnlySpan<byte> Encode(string action, Span<byte> buffer)
	{
		ArgumentNullException.ThrowIfNull(action);
		if (action.Length * 3 > buffer.Length)
		{
			return NativeString.ToUtf8(action);
		}
		int length = Encoding.UTF8.GetBytes(action, buffer);
		return buffer[..length];
	}
}
