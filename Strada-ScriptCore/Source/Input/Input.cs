using System;
using Strada.Interop;
using Strada.VisualScripting;

namespace Strada;

/// <summary>Keyboard, mouse and gamepad state of the current frame, and the project's input actions (named values such as
/// Jump or Steer that bindings give from keys and gamepads alike). "Pressed" and "released" are true only during the frame
/// the button changed.</summary>
public static unsafe class Input
{
	/// <summary>Whether the key is held down.</summary>
	[PureNode]
	public static bool IsKeyDown(KeyCode key) => InternalCalls.Input_IsKeyDown((int)key) != 0;

	/// <summary>Whether the key went down this frame.</summary>
	[PureNode]
	public static bool IsKeyPressed(KeyCode key) => InternalCalls.Input_IsKeyPressed((int)key) != 0;

	/// <summary>Whether the key went up this frame.</summary>
	[PureNode]
	public static bool IsKeyReleased(KeyCode key) => InternalCalls.Input_IsKeyReleased((int)key) != 0;

	/// <summary>Whether the mouse button is held down.</summary>
	[PureNode]
	public static bool IsMouseButtonDown(MouseButton button) => InternalCalls.Input_IsMouseButtonDown((int)button) != 0;

	/// <summary>Whether the mouse button went down this frame.</summary>
	[PureNode]
	public static bool IsMouseButtonPressed(MouseButton button) => InternalCalls.Input_IsMouseButtonPressed((int)button) != 0;

	/// <summary>Whether the mouse button went up this frame.</summary>
	[PureNode]
	public static bool IsMouseButtonReleased(MouseButton button) => InternalCalls.Input_IsMouseButtonReleased((int)button) != 0;

	/// <summary>The cursor position in pixels from the window's top-left corner.</summary>
	public static Vector2 MousePosition
	{
		get
		{
			Vector2 position;
			InternalCalls.Input_GetMousePosition(&position);
			return position;
		}
	}

	/// <summary>How far the mouse moved this frame, in pixels.</summary>
	public static Vector2 MouseDelta
	{
		get
		{
			Vector2 delta;
			InternalCalls.Input_GetMouseDelta(&delta);
			return delta;
		}
	}

	/// <summary>How far the mouse wheel scrolled this frame (Y is the usual wheel).</summary>
	public static Vector2 MouseScrollDelta
	{
		get
		{
			Vector2 delta;
			InternalCalls.Input_GetMouseScrollDelta(&delta);
			return delta;
		}
	}

	/// <summary>Whether the mouse is over the screen-space UI: an image or a button of a canvas is under the cursor (as of this
	/// frame's UI update, before <c>OnUpdate</c>), so a click there goes to the UI. Scripts that act on mouse clicks in the
	/// world skip those, so clicking a HUD or menu button does not also shoot, place or select in the game. False while the
	/// cursor is locked.</summary>
	public static bool IsPointerOverUI => InternalCalls.Input_IsPointerOverUI() != 0;

	/// <summary>Whether the cursor is visible, hidden or locked.</summary>
	public static CursorMode CursorMode
	{
		get => (CursorMode)InternalCalls.Input_GetCursorMode();
		set => InternalCalls.Input_SetCursorMode((int)value);
	}

	/// <summary>Whether a gamepad is connected at the index (0-15).</summary>
	[PureNode]
	public static bool IsGamepadConnected(int gamepad = 0) => gamepad >= 0 && InternalCalls.Input_IsGamepadConnected((uint)gamepad) != 0;

	/// <summary>The position of a gamepad axis (0 when the gamepad is not connected).</summary>
	[PureNode]
	public static float GetGamepadAxis(GamepadAxis axis, int gamepad = 0) =>
		gamepad >= 0 ? InternalCalls.Input_GetGamepadAxis((uint)gamepad, (int)axis) : 0.0f;

	/// <summary>Whether a gamepad button is held down.</summary>
	[PureNode]
	public static bool IsGamepadButtonDown(GamepadButton button, int gamepad = 0) =>
		gamepad >= 0 && InternalCalls.Input_IsGamepadButtonDown((uint)gamepad, (int)button) != 0;

	/// <summary>Whether a gamepad button went down this frame.</summary>
	[PureNode]
	public static bool IsGamepadButtonPressed(GamepadButton button, int gamepad = 0) =>
		gamepad >= 0 && InternalCalls.Input_IsGamepadButtonPressed((uint)gamepad, (int)button) != 0;

	/// <summary>Whether a gamepad button went up this frame.</summary>
	[PureNode]
	public static bool IsGamepadButtonReleased(GamepadButton button, int gamepad = 0) =>
		gamepad >= 0 && InternalCalls.Input_IsGamepadButtonReleased((uint)gamepad, (int)button) != 0;

	/// <summary>The value of an input action of the project's <see cref="InputActions"/>: a Button action's from 0 (up) to 1
	/// (more with a binding's scale), an Axis action's signed value (typically -1 to 1), a Vector2 action's length. It is the
	/// reading of the binding with the largest magnitude (after its dead zone, response curve, inversion and scale) or the
	/// action's override (<see cref="SetActionOverride"/>), evaluated once per frame before <c>OnUpdate</c>. Names match with
	/// their letter case; an unknown name, and every name in a project that names no actions, reads 0 (logged once per
	/// name).</summary>
	[PureNode]
	public static float GetAction(string action)
	{
		Span<byte> buffer = stackalloc byte[InputActionContext.StackNameBytes];
		ReadOnlySpan<byte> name = InputActionContext.Encode(action, buffer);
		fixed (byte* text = name)
		{
			return InternalCalls.Input_GetAction(text, name.Length);
		}
	}

	/// <summary>The value of an input action as a vector: a Vector2 action's, +Y up (a stick pushed forward, the W of a WASD
	/// composite); (value, 0) for a Button or Axis action. Reads as <see cref="GetAction"/> does.</summary>
	[PureNode]
	public static Vector2 GetActionVector(string action)
	{
		Span<byte> buffer = stackalloc byte[InputActionContext.StackNameBytes];
		ReadOnlySpan<byte> name = InputActionContext.Encode(action, buffer);
		Vector2 value;
		fixed (byte* text = name)
		{
			InternalCalls.Input_GetActionVector(text, name.Length, &value);
		}
		return value;
	}

	/// <summary>Whether an input action is down: its value reached its press threshold and has not fallen below its release
	/// threshold since. Every action type has it, so an axis pushed far enough is down too.</summary>
	[PureNode]
	public static bool IsActionDown(string action)
	{
		Span<byte> buffer = stackalloc byte[InputActionContext.StackNameBytes];
		ReadOnlySpan<byte> name = InputActionContext.Encode(action, buffer);
		fixed (byte* text = name)
		{
			return InternalCalls.Input_IsActionDown(text, name.Length) != 0;
		}
	}

	/// <summary>Whether an input action went down: in <c>OnUpdate</c> and the frame's events, during the frame it went down; in
	/// <c>OnFixedUpdate</c>, during exactly one physics step, the first at or after that frame (carried across frames that run
	/// no step), so fixed steps never miss a press nor see one twice. A press and release within one frame reads pressed and
	/// released, and not down.</summary>
	[PureNode]
	public static bool IsActionPressed(string action)
	{
		Span<byte> buffer = stackalloc byte[InputActionContext.StackNameBytes];
		ReadOnlySpan<byte> name = InputActionContext.Encode(action, buffer);
		InputActionOrigin origin = InputActionContext.Current;
		fixed (byte* text = name)
		{
			return InternalCalls.Input_IsActionPressed(text, name.Length, (int)origin.View, origin.Step) != 0;
		}
	}

	/// <summary>Whether an input action went up again, by the same frame and fixed-step rule as
	/// <see cref="IsActionPressed"/>.</summary>
	[PureNode]
	public static bool IsActionReleased(string action)
	{
		Span<byte> buffer = stackalloc byte[InputActionContext.StackNameBytes];
		ReadOnlySpan<byte> name = InputActionContext.Encode(action, buffer);
		InputActionOrigin origin = InputActionContext.Current;
		fixed (byte* text = name)
		{
			return InternalCalls.Input_IsActionReleased(text, name.Length, (int)origin.View, origin.Step) != 0;
		}
	}

	/// <summary>Puts a value on an input action as one more device would (on a Vector2 action, the vector (value, 0)): the
	/// action takes it when it is larger than what its bindings read, and it presses and releases the action as a button does
	/// (a press when the value leaves 0, a release when it returns to 0 or is cleared). It is read from the next frame's
	/// evaluation on and held, across scene loads, until <see cref="ClearActionOverride"/>: how a graph drives a game (an
	/// autopilot, an attract mode) and how a test scene presses a button. A value set and cleared before the next frame
	/// still reaches it as a press and a release.</summary>
	public static void SetActionOverride(string action, float value) => SetOverride(action, new Vector2(value, 0.0f));

	/// <summary>Puts a vector on an input action as <see cref="SetActionOverride"/> does, for a Vector2 action (+Y up); a
	/// Button or Axis action reads its X.</summary>
	public static void SetActionVectorOverride(string action, Vector2 value) => SetOverride(action, value);

	/// <summary>Ends the override of an input action (<see cref="SetActionOverride"/>): from the next frame's evaluation on
	/// the action reads its bindings alone, released if only the override held it down. Nothing for an action without
	/// one.</summary>
	public static void ClearActionOverride(string action)
	{
		Span<byte> buffer = stackalloc byte[InputActionContext.StackNameBytes];
		ReadOnlySpan<byte> name = InputActionContext.Encode(action, buffer);
		fixed (byte* text = name)
		{
			InternalCalls.Input_ClearActionOverride(text, name.Length);
		}
	}

	/// <summary>Listens for the next key, mouse button, gamepad button or axis the player gives, and binds it to binding
	/// <paramref name="binding"/> (its index in the action) of the action: the whole binding with <paramref name="part"/> -1,
	/// or one control of a composite ("Key/A Key/D": part 0 is A, 1 is D). A key or button counts when it is pressed after
	/// this call, an axis when it moves past half way (a Vector2 action takes a stick; a part takes an axis in its positive
	/// direction); gamepads count only those the binding reads. Escape, or 10 seconds of frames (counted also while
	/// <see cref="Time.TimeScale"/> is 0), cancel it, as does <see cref="CancelRebind"/> or another StartRebind. Answers
	/// <see cref="RebindStatus.Listening"/>, or <see cref="RebindStatus.Refused"/> as <see cref="Rebind"/> refuses; read how
	/// it ends from <see cref="CurrentRebind"/>. The control that was pressed to bind does not press the action.</summary>
	public static RebindResult StartRebind(string action, int binding, int part = -1)
	{
		Span<byte> buffer = stackalloc byte[InputActionContext.StackNameBytes];
		ReadOnlySpan<byte> name = InputActionContext.Encode(action, buffer);
		NativeRebindAnswer answer;
		fixed (byte* text = name)
		{
			InternalCalls.Input_StartRebind(text, name.Length, binding, part, &answer);
		}
		return RebindResult.Read(answer, 0);
	}

	/// <summary>Binds controls to binding <paramref name="binding"/> of an input action while the game runs: with
	/// <paramref name="part"/> -1 the binding's controls as text ("Key/J", "Key/A Key/D", "Gamepad/LeftStick", or "" to leave
	/// it unbound), else the one control that replaces that part of a composite. The new binding is read from the next frame
	/// on, by every running scene and the scenes loaded later, and kept for the next run in the player's profile in the
	/// game's data folder; the actions asset is never changed. The answer lists the other actions that now read one of the
	/// same controls (the engine does not unbind them). Refused, changing nothing, for an unknown action, a binding or part
	/// it lacks, or controls that do not fit the action's type.</summary>
	public static RebindResult Rebind(string action, int binding, string controls, int part = -1)
	{
		ArgumentNullException.ThrowIfNull(controls);
		Span<byte> buffer = stackalloc byte[InputActionContext.StackNameBytes];
		ReadOnlySpan<byte> name = InputActionContext.Encode(action, buffer);
		byte[] controlsText = NativeString.ToUtf8(controls);
		NativeRebindAnswer answer;
		fixed (byte* text = name)
		fixed (byte* controlsBytes = controlsText)
		{
			InternalCalls.Input_Rebind(text, name.Length, binding, controlsBytes, controlsText.Length, part, &answer);
		}
		return RebindResult.Read(answer, 0);
	}

	/// <summary>Ends the capture <see cref="StartRebind"/> started, if one still listens: <see cref="CurrentRebind"/> then
	/// reads <see cref="RebindStatus.Cancelled"/>.</summary>
	public static void CancelRebind() => InternalCalls.Input_CancelRebind();

	/// <summary>Every binding back to the actions asset's, and the player's profile emptied: <see cref="RebindStatus.Done"/>,
	/// or <see cref="RebindStatus.Refused"/> when the project names no input actions.</summary>
	public static RebindResult ResetBindings()
	{
		NativeRebindAnswer answer;
		InternalCalls.Input_ResetBindings(&answer);
		return RebindResult.Read(answer, 0);
	}

	/// <summary>The latest capture of the running scene (<see cref="StartRebind"/>): <see cref="RebindStatus.Listening"/> while
	/// it waits, then how it ended, with the conflicts when <see cref="RebindStatus.Done"/>; <see cref="RebindStatus.None"/>
	/// before any.</summary>
	public static RebindResult CurrentRebind
	{
		get
		{
			NativeRebindAnswer answer;
			InternalCalls.Input_GetCurrentRebind(&answer);
			return RebindResult.Read(answer, 1);
		}
	}

	/// <summary>The controls binding <paramref name="binding"/> of an input action reads now, as text ("Key/Space", "Key/A
	/// Key/D"), the player's rebinding included: what a settings screen shows. Empty for an action or binding the project's
	/// actions lack.</summary>
	[PureNode]
	public static string GetBindingControls(string action, int binding)
	{
		Span<byte> buffer = stackalloc byte[InputActionContext.StackNameBytes];
		ReadOnlySpan<byte> name = InputActionContext.Encode(action, buffer);
		Span<byte> controls = stackalloc byte[BindingTextBytes];
		int length;
		fixed (byte* text = name)
		fixed (byte* controlsBytes = controls)
		{
			length = InternalCalls.Input_GetBindingControls(text, name.Length, binding, controlsBytes, controls.Length);
			return length > 0 ? NativeString.FromUtf8(controlsBytes, length) : string.Empty;
		}
	}

	// Room for a binding's text: four control names (the longest is about 20 bytes) and their spaces.
	private const int BindingTextBytes = 256;

	private static void SetOverride(string action, Vector2 value)
	{
		Span<byte> buffer = stackalloc byte[InputActionContext.StackNameBytes];
		ReadOnlySpan<byte> name = InputActionContext.Encode(action, buffer);
		fixed (byte* text = name)
		{
			InternalCalls.Input_SetActionOverride(text, name.Length, &value);
		}
	}
}
