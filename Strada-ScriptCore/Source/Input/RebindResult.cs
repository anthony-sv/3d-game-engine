using System;
using Strada.Interop;

namespace Strada;

/// <summary>How a rebind of an input action's binding ended (<see cref="Input.Rebind"/>, <see cref="Input.ResetBindings"/>),
/// or that a capture listens for a control (<see cref="Input.StartRebind"/>).</summary>
public enum RebindStatus
{
	/// <summary>Nothing was asked yet.</summary>
	None = 0,
	/// <summary>A capture listens for the next key, button or axis the player gives.</summary>
	Listening = 1,
	/// <summary>The binding reads the new controls from the next frame on, and the player's profile keeps them.</summary>
	Done = 2,
	/// <summary>A capture ended without a control: Escape was pressed, its 10 seconds ran out, it was cancelled or another
	/// replaced it.</summary>
	Cancelled = 3,
	/// <summary>Nothing changed: the action, binding or part does not exist, the controls do not fit the action, or the project
	/// names no input actions; <see cref="RebindResult.Message"/> says which.</summary>
	Refused = 4,
}

/// <summary>The answer of a rebind of an input action's binding: how it ended, the binding's controls and the other actions
/// that now read one of them. The engine applies a binding whatever it conflicts with and reports the conflicts; the game
/// decides what to do about them (warn, swap, or undo with another <see cref="Input.Rebind"/>).</summary>
public sealed class RebindResult
{
	private RebindResult(RebindStatus status, string action, int binding, int part, string controls, string[] conflicts, string message)
	{
		Status = status;
		Action = action;
		Binding = binding;
		Part = part;
		Controls = controls;
		Conflicts = conflicts;
		Message = message;
	}

	/// <summary>How the rebind ended, or <see cref="RebindStatus.Listening"/> while a capture waits.</summary>
	public RebindStatus Status { get; }

	/// <summary>The action asked for (empty for a reset).</summary>
	public string Action { get; }

	/// <summary>The binding's index in its action (-1 for a reset).</summary>
	public int Binding { get; }

	/// <summary>The control of a composite binding that was asked for (0 to 3), or -1 for the whole binding.</summary>
	public int Part { get; }

	/// <summary>The binding's controls as text ("Key/J", "Key/A Key/D", "Gamepad/LeftStick"): the new ones when
	/// <see cref="RebindStatus.Done"/>, those it reads while a capture listens or after it was cancelled; empty when
	/// refused.</summary>
	public string Controls { get; }

	/// <summary>When <see cref="RebindStatus.Done"/>, the other actions with a binding that reads one of the binding's
	/// controls (on a gamepad both read), in the order of the actions in the asset; empty otherwise.</summary>
	public string[] Conflicts { get; }

	/// <summary>Why the rebind was refused or cancelled, or that a binding that was done could not be saved; empty
	/// otherwise.</summary>
	public string Message { get; }

	// Source 0 is the scene's latest answer, 1 its capture (Input_ReadRebindText).
	internal static unsafe RebindResult Read(NativeRebindAnswer answer, int source)
	{
		if (answer.Status == NativeRebindAnswer.NoScene)
		{
			return new RebindResult(RebindStatus.Refused, string.Empty, -1, -1, string.Empty, Array.Empty<string>(), "no scene runs scripts");
		}
		string[] conflicts = answer.ConflictCount > 0 ? new string[answer.ConflictCount] : Array.Empty<string>();
		for (int index = 0; index < conflicts.Length; index++)
		{
			conflicts[index] = ReadText(source, 3, index);
		}
		return new RebindResult((RebindStatus)answer.Status, ReadText(source, 0, 0), answer.Binding, answer.Part, ReadText(source, 1, 0),
			conflicts, ReadText(source, 2, 0));
	}

	private static unsafe string ReadText(int source, int field, int index)
	{
		int length;
		byte* text = InternalCalls.Input_ReadRebindText(source, field, index, &length);
		return NativeString.FromUtf8(text, length);
	}
}
