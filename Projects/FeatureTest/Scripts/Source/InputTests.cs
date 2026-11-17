using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>Input queries. Test runs have no keyboard, mouse or gamepad: nothing is held, moved or connected, and the
/// cursor mode is what the scripts set. The project's input actions (Input/FeatureTest.sinput) are driven by overrides: Jump
/// and Move here, Throttle by the Features graph. Rebinding starts and ends with every binding the asset's, so a run in the
/// editor leaves its profile (.strada/PlayData/Input/FeatureTest.sinput.json) without entries.</summary>
public sealed class InputTests : FeatureTestScript
{
	private const int MaxFrames = 600;

	// What the action events and the fixed steps saw of Jump.
	private int m_JumpPresses;
	private int m_JumpReleases;
	private int m_StepPresses;
	private int m_StepReleases;

	protected override void OnActionPressed(string action) => m_JumpPresses += action == "Jump" ? 1 : 0;

	protected override void OnActionReleased(string action) => m_JumpReleases += action == "Jump" ? 1 : 0;

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		m_StepPresses += Input.IsActionPressed("Jump") ? 1 : 0;
		m_StepReleases += Input.IsActionReleased("Jump") ? 1 : 0;
	}

	// Frame 1 puts overrides on Jump and Move, which frame 2's evaluation takes (its press events come before its update),
	// and clears them; frame 3 sees the releases. The fixed steps see each edge once, in the first step after it.
	protected override bool Step(int frame, float deltaTime)
	{
		switch (frame)
		{
			case 1:
				Check("actions read nothing before their overrides", () =>
				{
					Assert.AreEqual(0.0f, Input.GetAction("Jump"));
					Assert.IsFalse(Input.IsActionDown("Jump"));
					Assert.IsFalse(Input.IsActionPressed("Jump"));
					Assert.IsFalse(Input.IsActionReleased("Jump"));
					Assert.AreEqual(Vector2.Zero, Input.GetActionVector("Move"));
				});
				Input.SetActionOverride("Jump", 1.0f);
				Input.SetActionVectorOverride("Move", new Vector2(0.0f, 1.0f));
				return false;
			case 2:
				Check("an override presses an action from the next frame, with its event", () =>
				{
					Assert.AreEqual(1.0f, Input.GetAction("Jump"));
					Assert.IsTrue(Input.IsActionDown("Jump"));
					Assert.IsTrue(Input.IsActionPressed("Jump"));
					Assert.AreEqual(1, m_JumpPresses);
					Assert.AreEqual(new Vector2(0.0f, 1.0f), Input.GetActionVector("Move"));
					Assert.AreEqual(1.0f, Input.GetAction("Move"));
					Assert.IsTrue(Input.IsActionDown("Move"));
				});
				Input.ClearActionOverride("Jump");
				Input.ClearActionOverride("Move");
				return false;
			case 3:
				Check("clearing an override releases the action, with its event", () =>
				{
					Assert.AreEqual(0.0f, Input.GetAction("Jump"));
					Assert.IsFalse(Input.IsActionDown("Jump"));
					Assert.IsTrue(Input.IsActionReleased("Jump"));
					Assert.AreEqual(1, m_JumpReleases);
					Assert.AreEqual(Vector2.Zero, Input.GetActionVector("Move"));
				});
				return false;
			default:
				if (m_StepReleases == 0 && frame < MaxFrames)
				{
					return false;
				}
				Check("each edge of an action reaches one fixed step", () =>
				{
					Assert.AreEqual(1, m_StepPresses);
					Assert.AreEqual(1, m_StepReleases);
					Assert.AreEqual(1, m_JumpPresses);
				});
				return true;
		}
	}

	protected override void Start()
	{
		Check("the project's input actions are listed by name", () =>
		{
			InputActions? actions = Assets.Load<InputActions>("Input/FeatureTest.sinput");
			Assert.IsNotNull(actions);
			Assert.AreEqual("Jump,Throttle,Move", string.Join(",", actions!.ActionNames));
		});
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
		Check("a binding changes while the game runs, answering its conflicts, and goes back to the asset's", () =>
		{
			Assert.AreEqual(RebindStatus.Done, Input.ResetBindings().Status);
			Assert.AreEqual("Key/Space", Input.GetBindingControls("Jump", 0));
			RebindResult jump = Input.Rebind("Jump", 0, "Key/J");
			Assert.AreEqual(RebindStatus.Done, jump.Status);
			Assert.AreEqual("Jump", jump.Action);
			Assert.AreEqual(0, jump.Binding);
			Assert.AreEqual(-1, jump.Part);
			Assert.AreEqual("Key/J", jump.Controls);
			Assert.AreEqual(0, jump.Conflicts.Length);
			Assert.AreEqual("Key/J", Input.GetBindingControls("Jump", 0));
			// The W of Move's WASD becomes Up; its S still reads what Throttle's composite reads.
			RebindResult move = Input.Rebind("Move", 0, "Key/Up", 0);
			Assert.AreEqual("Key/Up Key/S Key/A Key/D", move.Controls);
			Assert.AreEqual("Throttle", string.Join(",", move.Conflicts));
			RebindResult unbound = Input.Rebind("Jump", 1, "");
			Assert.AreEqual(RebindStatus.Done, unbound.Status);
			Assert.AreEqual(string.Empty, Input.GetBindingControls("Jump", 1));
			RebindResult refused = Input.Rebind("Jump", 5, "Key/K");
			Assert.AreEqual(RebindStatus.Refused, refused.Status);
			Assert.IsTrue(refused.Message.Length > 0);
			Assert.AreEqual(RebindStatus.Done, Input.ResetBindings().Status);
			Assert.AreEqual("Key/Space", Input.GetBindingControls("Jump", 0));
			Assert.AreEqual("Key/W Key/S Key/A Key/D", Input.GetBindingControls("Move", 0));
		});
		Check("a capture listens for the player's next control until it is cancelled", () =>
		{
			Assert.AreEqual(RebindStatus.None, Input.CurrentRebind.Status);
			RebindResult listening = Input.StartRebind("Jump", 1);
			Assert.AreEqual(RebindStatus.Listening, listening.Status);
			Assert.AreEqual("Gamepad/A", listening.Controls);
			Assert.AreEqual(RebindStatus.Listening, Input.CurrentRebind.Status);
			Input.CancelRebind();
			Assert.AreEqual(RebindStatus.Cancelled, Input.CurrentRebind.Status);
			Assert.AreEqual("Gamepad/A", Input.GetBindingControls("Jump", 1));
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
