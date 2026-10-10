using Strada.Interop;

namespace Strada;

/// <summary>Keyboard, mouse and gamepad state of the current frame. "Pressed" and "released" are true only during the
/// frame the button changed.</summary>
public static unsafe class Input
{
	/// <summary>Whether the key is held down.</summary>
	public static bool IsKeyDown(KeyCode key) => InternalCalls.Input_IsKeyDown((int)key) != 0;

	/// <summary>Whether the key went down this frame.</summary>
	public static bool IsKeyPressed(KeyCode key) => InternalCalls.Input_IsKeyPressed((int)key) != 0;

	/// <summary>Whether the key went up this frame.</summary>
	public static bool IsKeyReleased(KeyCode key) => InternalCalls.Input_IsKeyReleased((int)key) != 0;

	/// <summary>Whether the mouse button is held down.</summary>
	public static bool IsMouseButtonDown(MouseButton button) => InternalCalls.Input_IsMouseButtonDown((int)button) != 0;

	/// <summary>Whether the mouse button went down this frame.</summary>
	public static bool IsMouseButtonPressed(MouseButton button) => InternalCalls.Input_IsMouseButtonPressed((int)button) != 0;

	/// <summary>Whether the mouse button went up this frame.</summary>
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

	/// <summary>Whether the cursor is visible, hidden or locked.</summary>
	public static CursorMode CursorMode
	{
		get => (CursorMode)InternalCalls.Input_GetCursorMode();
		set => InternalCalls.Input_SetCursorMode((int)value);
	}

	/// <summary>Whether a gamepad is connected at the index (0-15).</summary>
	public static bool IsGamepadConnected(int gamepad = 0) => gamepad >= 0 && InternalCalls.Input_IsGamepadConnected((uint)gamepad) != 0;

	/// <summary>The position of a gamepad axis (0 when the gamepad is not connected).</summary>
	public static float GetGamepadAxis(GamepadAxis axis, int gamepad = 0) =>
		gamepad >= 0 ? InternalCalls.Input_GetGamepadAxis((uint)gamepad, (int)axis) : 0.0f;

	/// <summary>Whether a gamepad button is held down.</summary>
	public static bool IsGamepadButtonDown(GamepadButton button, int gamepad = 0) =>
		gamepad >= 0 && InternalCalls.Input_IsGamepadButtonDown((uint)gamepad, (int)button) != 0;

	/// <summary>Whether a gamepad button went down this frame.</summary>
	public static bool IsGamepadButtonPressed(GamepadButton button, int gamepad = 0) =>
		gamepad >= 0 && InternalCalls.Input_IsGamepadButtonPressed((uint)gamepad, (int)button) != 0;

	/// <summary>Whether a gamepad button went up this frame.</summary>
	public static bool IsGamepadButtonReleased(GamepadButton button, int gamepad = 0) =>
		gamepad >= 0 && InternalCalls.Input_IsGamepadButtonReleased((uint)gamepad, (int)button) != 0;
}
