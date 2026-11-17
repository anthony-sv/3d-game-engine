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
