using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>Input queries. Test runs have no keyboard, mouse or gamepad: nothing is held, moved or connected, and the
/// cursor mode is what the scripts set.</summary>
public sealed class InputTests : FeatureTestScript
{
	protected override void Start()
	{
		Check("no key or button is held", () =>
		{
			foreach (KeyCode key in new[] { KeyCode.Space, KeyCode.A, KeyCode.Escape, KeyCode.LeftShift })
			{
				Assert.IsFalse(Input.IsKeyDown(key), key.ToString());
				Assert.IsFalse(Input.IsKeyPressed(key), key.ToString());
				Assert.IsFalse(Input.IsKeyReleased(key), key.ToString());
			}
			foreach (MouseButton button in new[] { MouseButton.Left, MouseButton.Right, MouseButton.Middle })
			{
				Assert.IsFalse(Input.IsMouseButtonDown(button), button.ToString());
				Assert.IsFalse(Input.IsMouseButtonPressed(button), button.ToString());
				Assert.IsFalse(Input.IsMouseButtonReleased(button), button.ToString());
			}
		});
		Check("the mouse is still", () =>
		{
			Assert.AreEqual(Vector2.Zero, Input.MouseDelta);
			Assert.AreEqual(Vector2.Zero, Input.MouseScrollDelta);
			Vector2 position = Input.MousePosition;
			Assert.IsTrue(float.IsFinite(position.X) && float.IsFinite(position.Y));
		});
		Check("no gamepad is connected", () =>
		{
			Assert.IsFalse(Input.IsGamepadConnected());
			Assert.IsFalse(Input.IsGamepadConnected(3));
			Assert.AreEqual(0.0f, Input.GetGamepadAxis(GamepadAxis.LeftX));
			Assert.AreEqual(0.0f, Input.GetGamepadAxis(GamepadAxis.RightTrigger, 1));
			Assert.IsFalse(Input.IsGamepadButtonDown(GamepadButton.A));
			Assert.IsFalse(Input.IsGamepadButtonPressed(GamepadButton.Start, 0));
			Assert.IsFalse(Input.IsGamepadButtonReleased(GamepadButton.DPadUp));
		});
		Check("the cursor mode is kept", () =>
		{
			CursorMode original = Input.CursorMode;
			Input.CursorMode = CursorMode.Hidden;
			Assert.AreEqual(CursorMode.Hidden, Input.CursorMode);
			Input.CursorMode = original;
			Assert.AreEqual(original, Input.CursorMode);
		});
	}
}
