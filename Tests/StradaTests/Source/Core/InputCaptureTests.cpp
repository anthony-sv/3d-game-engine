#include "Strada/Core/Input.h"
#include "Strada/Core/InputActions.h"
#include "Strada/Core/InputCapture.h"
#include "Strada/Core/KeyCodes.h"

#include <doctest/doctest.h>

#include <cstdint>

using namespace Strada;

// The capture that listens for a player's control while a game rebinds (Docs/Architecture.md §7.4, "Rebinding"): each
// source kind, what it ignores (controls held as it starts, triggers at rest, gamepads the binding does not read, controls
// that do not fit), Escape and the time limit. The binding table and the scene's use are SceneInputRebindingTests.cpp's.
namespace
{
	struct InputScope
	{
		InputScope() { Input::Reset(); }
		~InputScope() { Input::Reset(); }
		InputScope(InputScope const&) = delete;
		InputScope& operator=(InputScope const&) = delete;
	};

	constexpr float Frame = 1.0f / 60.0f;

	// Injects a pad at rest with the axis at the value given (GLFW's convention: triggers rest at -1, sticks' Y up is -1).
	void InjectAxis(uint32_t slot, GamepadAxis axis, float value)
	{
		Input::GamepadState state = Input::GetInjectedGamepadState(slot).value_or(Input::GetRestingGamepadState());
		state.Axes[static_cast<size_t>(axis)] = value;
		Input::InjectGamepadState(slot, state);
	}

	void InjectButton(uint32_t slot, GamepadButton button, bool down)
	{
		Input::GamepadState state = Input::GetInjectedGamepadState(slot).value_or(Input::GetRestingGamepadState());
		state.Buttons[static_cast<size_t>(button)] = down;
		Input::InjectGamepadState(slot, state);
	}

	void Tap(KeyCode key)
	{
		Input::SetKeyState(key, true);
		Input::SetKeyState(key, false);
	}
}

TEST_CASE("InputCapture: a key, a mouse button and a gamepad button pressed after the start are captured, a held one once pressed again")
{
	InputScope const input;
	InputControlCapture capture;

	SUBCASE("a key, also when it was tapped and released before the update")
	{
		capture.Start(InputCaptureTarget::Control, AnyGamepad);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Waiting);
		Tap(KeyCode::J);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Captured);
		CHECK(capture.GetControl() == InputControl{InputControlKind::Key, static_cast<uint16_t>(KeyCode::J)});
		CHECK_FALSE(capture.IsListening());
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Waiting);
	}
	SUBCASE("a key held when it starts counts only once released and pressed again")
	{
		Input::SetKeyState(KeyCode::Space, true);
		capture.Start(InputCaptureTarget::Control, AnyGamepad);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Waiting);
		Input::SetKeyState(KeyCode::Space, false);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Waiting);
		Input::SetKeyState(KeyCode::Space, true);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Captured);
		CHECK(FormatInputControl(capture.GetControl()) == "Key/Space");
	}
	SUBCASE("a mouse button")
	{
		capture.Start(InputCaptureTarget::Part, AnyGamepad);
		Input::SetMouseButtonState(MouseButton::Right, true);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Captured);
		CHECK(FormatInputControl(capture.GetControl()) == "Mouse/Right");
	}
	SUBCASE("a gamepad button of any pad, or only of the binding's pad")
	{
		capture.Start(InputCaptureTarget::Control, 2);
		InjectButton(1, GamepadButton::Y, true);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Waiting);
		InjectButton(2, GamepadButton::X, true);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Captured);
		CHECK(FormatInputControl(capture.GetControl()) == "Gamepad/X");

		capture.Start(InputCaptureTarget::Control, AnyGamepad);
		InjectButton(1, GamepadButton::Y, false);
		InjectButton(1, GamepadButton::Y, true);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Captured);
		CHECK(FormatInputControl(capture.GetControl()) == "Gamepad/Y");
	}
	SUBCASE("a mouse press while the pointer is over the game's UI is no control, then or later")
	{
		capture.Start(InputCaptureTarget::Control, AnyGamepad);
		Input::SetMouseButtonState(MouseButton::Left, true);
		CHECK(capture.Update(Frame, false) == InputCaptureOutcome::Waiting);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Waiting);
		Input::SetMouseButtonState(MouseButton::Left, false);
		Input::SetMouseButtonState(MouseButton::Left, true);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Captured);
		CHECK(FormatInputControl(capture.GetControl()) == "Mouse/Left");
	}
	SUBCASE("keys come before mouse buttons and gamepad buttons in one update, keys by code")
	{
		capture.Start(InputCaptureTarget::Control, AnyGamepad);
		InjectButton(0, GamepadButton::A, true);
		Input::SetMouseButtonState(MouseButton::Left, true);
		Input::SetKeyState(KeyCode::Z, true);
		Input::SetKeyState(KeyCode::B, true);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Captured);
		CHECK(FormatInputControl(capture.GetControl()) == "Key/B");
	}
}

TEST_CASE("InputCapture: axes count when they reach half way after reading below it, as actions read them, by what the binding takes")
{
	InputScope const input;
	InputControlCapture capture;

	SUBCASE("a whole binding takes a stick axis either way, and a trigger pressed")
	{
		capture.Start(InputCaptureTarget::Control, AnyGamepad);
		InjectAxis(0, GamepadAxis::LeftX, -0.4f);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Waiting);
		InjectAxis(0, GamepadAxis::LeftX, -0.6f);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Captured);
		CHECK(FormatInputControl(capture.GetControl()) == "Gamepad/LeftX");

		capture.Start(InputCaptureTarget::Control, AnyGamepad);
		// A trigger at rest reads 0 (GLFW's -1), so nothing; pressed past half way, it is the trigger.
		InjectAxis(3, GamepadAxis::RightTrigger, -1.0f);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Waiting);
		InjectAxis(3, GamepadAxis::RightTrigger, 0.2f);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Captured);
		CHECK(FormatInputControl(capture.GetControl()) == "Gamepad/RightTrigger");
	}
	SUBCASE("an axis held past half way when it starts counts only once it came back and went again")
	{
		InjectAxis(0, GamepadAxis::RightY, -0.9f);
		capture.Start(InputCaptureTarget::Control, AnyGamepad);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Waiting);
		InjectAxis(0, GamepadAxis::RightY, 0.0f);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Waiting);
		InjectAxis(0, GamepadAxis::RightY, 0.9f);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Captured);
		CHECK(FormatInputControl(capture.GetControl()) == "Gamepad/RightY");
	}
	SUBCASE("a trigger whose pad's mapping lacks it reads 0 and gives nothing")
	{
		Input::GamepadState state;
		state.Connected = true;
		Input::SetGamepadState(0, state);
		capture.Start(InputCaptureTarget::Control, AnyGamepad);
		for (int frame = 0; frame < 10; frame++)
		{
			Input::SetGamepadState(0, state);
			CHECK(capture.Update(Frame) == InputCaptureOutcome::Waiting);
		}
	}
	SUBCASE("a Vector2 binding takes a stick and nothing else")
	{
		capture.Start(InputCaptureTarget::Stick, AnyGamepad);
		Tap(KeyCode::W);
		InjectButton(0, GamepadButton::A, true);
		InjectAxis(0, GamepadAxis::LeftTrigger, 1.0f);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Waiting);
		InjectAxis(1, GamepadAxis::RightX, 0.4f);
		InjectAxis(1, GamepadAxis::RightY, 0.4f);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Captured);
		CHECK(FormatInputControl(capture.GetControl()) == "Gamepad/RightStick");
	}
	SUBCASE("a part takes an axis in its positive direction only")
	{
		capture.Start(InputCaptureTarget::Part, AnyGamepad);
		InjectAxis(0, GamepadAxis::LeftX, -1.0f);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Waiting);
		// Up is positive for actions (GLFW's -1).
		InjectAxis(0, GamepadAxis::LeftY, -0.8f);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Captured);
		CHECK(FormatInputControl(capture.GetControl()) == "Gamepad/LeftY");
	}
}

TEST_CASE("InputCapture: Escape cancels before anything else, and 10 seconds of updates end it")
{
	InputScope const input;
	InputControlCapture capture;

	SUBCASE("Escape, even with a key pressed in the same update")
	{
		capture.Start(InputCaptureTarget::Control, AnyGamepad);
		Tap(KeyCode::A);
		Tap(KeyCode::Escape);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Escaped);
		CHECK_FALSE(capture.IsListening());
	}
	SUBCASE("the time counts the updates' seconds, and a control given in the last update still counts")
	{
		capture.Start(InputCaptureTarget::Control, AnyGamepad);
		for (int update = 0; update < 9; update++)
		{
			CHECK(capture.Update(1.0f) == InputCaptureOutcome::Waiting);
		}
		CHECK(capture.Update(0.999f) == InputCaptureOutcome::Waiting);
		Tap(KeyCode::K);
		CHECK(capture.Update(1.0f) == InputCaptureOutcome::Captured);

		capture.Start(InputCaptureTarget::Control, AnyGamepad);
		CHECK(capture.Update(9.5f) == InputCaptureOutcome::Waiting);
		// Negative and non-finite seconds count as none.
		CHECK(capture.Update(-5.0f) == InputCaptureOutcome::Waiting);
		CHECK(capture.Update(0.5f) == InputCaptureOutcome::TimedOut);
		CHECK_FALSE(capture.IsListening());
	}
	SUBCASE("Stop ends it without a control, and an input reset is no press")
	{
		Input::SetKeyState(KeyCode::Q, true);
		Input::SetKeyState(KeyCode::Q, false);
		capture.Start(InputCaptureTarget::Control, AnyGamepad);
		Input::Reset();
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Waiting);
		Tap(KeyCode::Q);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Captured);

		capture.Start(InputCaptureTarget::Control, AnyGamepad);
		capture.Stop();
		Tap(KeyCode::Q);
		CHECK(capture.Update(Frame) == InputCaptureOutcome::Waiting);
	}
}
