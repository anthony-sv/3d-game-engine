#include "Strada/Core/Events/KeyEvent.h"
#include "Strada/Core/Events/MouseEvent.h"
#include "Strada/Core/Input.h"

#include <doctest/doctest.h>

using namespace Strada;

namespace
{
	struct InputFixture
	{
		InputFixture() { Input::Reset(); }
		~InputFixture() { Input::Reset(); }
	};
}

TEST_CASE_FIXTURE(InputFixture, "Input: key press lifecycle across frames")
{
	CHECK_FALSE(Input::IsKeyDown(KeyCode::A));

	Input::SetKeyState(KeyCode::A, true);
	CHECK(Input::IsKeyDown(KeyCode::A));
	CHECK(Input::IsKeyPressed(KeyCode::A));
	CHECK_FALSE(Input::IsKeyReleased(KeyCode::A));

	Input::EndFrame();
	CHECK(Input::IsKeyDown(KeyCode::A));
	CHECK_FALSE(Input::IsKeyPressed(KeyCode::A));

	Input::SetKeyState(KeyCode::A, false);
	CHECK_FALSE(Input::IsKeyDown(KeyCode::A));
	CHECK(Input::IsKeyReleased(KeyCode::A));

	Input::EndFrame();
	CHECK_FALSE(Input::IsKeyReleased(KeyCode::A));
}

TEST_CASE_FIXTURE(InputFixture, "Input: a tap within one frame reports both press and release")
{
	Input::SetKeyState(KeyCode::Space, true);
	Input::SetKeyState(KeyCode::Space, false);
	CHECK_FALSE(Input::IsKeyDown(KeyCode::Space));
	CHECK(Input::IsKeyPressed(KeyCode::Space));
	CHECK(Input::IsKeyReleased(KeyCode::Space));
}

TEST_CASE_FIXTURE(InputFixture, "Input: key repeat does not re-trigger pressed")
{
	KeyPressedEvent first(KeyCode::W, false);
	Input::ProcessEvent(first);
	Input::EndFrame();

	KeyPressedEvent repeat(KeyCode::W, true);
	Input::ProcessEvent(repeat);
	CHECK(Input::IsKeyDown(KeyCode::W));
	CHECK_FALSE(Input::IsKeyPressed(KeyCode::W));
}

TEST_CASE_FIXTURE(InputFixture, "Input: out-of-range and unknown keys are ignored")
{
	Input::SetKeyState(KeyCode::Unknown, true);
	CHECK_FALSE(Input::IsKeyDown(KeyCode::Unknown));
	Input::SetKeyState(static_cast<KeyCode>(5000), true);
	CHECK_FALSE(Input::IsKeyDown(static_cast<KeyCode>(5000)));
}

TEST_CASE_FIXTURE(InputFixture, "Input: mouse buttons, position, delta and scroll")
{
	MouseButtonPressedEvent press(MouseButton::Left);
	Input::ProcessEvent(press);
	CHECK(Input::IsMouseButtonDown(MouseButton::Left));
	CHECK(Input::IsMouseButtonPressed(MouseButton::Left));

	MouseMovedEvent firstMove(100.0f, 50.0f);
	Input::ProcessEvent(firstMove);
	// The first known position does not produce a delta.
	CHECK(Input::GetMouseDelta() == glm::vec2(0.0f, 0.0f));
	Input::EndFrame();

	MouseMovedEvent secondMove(110.0f, 45.0f);
	Input::ProcessEvent(secondMove);
	CHECK(Input::GetMousePosition() == glm::vec2(110.0f, 45.0f));
	CHECK(Input::GetMouseDelta() == glm::vec2(10.0f, -5.0f));

	MouseScrolledEvent scroll(0.0f, 1.0f);
	Input::ProcessEvent(scroll);
	Input::ProcessEvent(scroll);
	CHECK(Input::GetScrollDelta() == glm::vec2(0.0f, 2.0f));

	Input::EndFrame();
	CHECK(Input::GetMouseDelta() == glm::vec2(0.0f, 0.0f));
	CHECK(Input::GetScrollDelta() == glm::vec2(0.0f, 0.0f));
	CHECK_FALSE(Input::IsMouseButtonPressed(MouseButton::Left));
	CHECK(Input::IsMouseButtonDown(MouseButton::Left));

	MouseButtonReleasedEvent release(MouseButton::Left);
	Input::ProcessEvent(release);
	CHECK(Input::IsMouseButtonReleased(MouseButton::Left));
}

TEST_CASE_FIXTURE(InputFixture, "Input: release all reports releases for held input")
{
	Input::SetKeyState(KeyCode::LeftShift, true);
	Input::SetMouseButtonState(MouseButton::Right, true);
	Input::EndFrame();

	Input::ReleaseAll();
	CHECK_FALSE(Input::IsKeyDown(KeyCode::LeftShift));
	CHECK(Input::IsKeyReleased(KeyCode::LeftShift));
	CHECK(Input::IsMouseButtonReleased(MouseButton::Right));
}

TEST_CASE_FIXTURE(InputFixture, "Input: gamepad state, buttons and disconnection")
{
	CHECK_FALSE(Input::IsGamepadConnected(0));
	CHECK(Input::GetGamepadAxis(0, GamepadAxis::LeftX) == 0.0f);

	Input::GamepadState state;
	state.Connected = true;
	state.Axes[static_cast<size_t>(GamepadAxis::LeftX)] = 0.75f;
	state.Buttons[static_cast<size_t>(GamepadButton::A)] = true;
	Input::SetGamepadState(0, state);

	CHECK(Input::IsGamepadConnected(0));
	CHECK(Input::GetGamepadAxis(0, GamepadAxis::LeftX) == doctest::Approx(0.75f));
	CHECK(Input::IsGamepadButtonDown(0, GamepadButton::A));
	CHECK(Input::IsGamepadButtonPressed(0, GamepadButton::A));

	Input::EndFrame();
	Input::SetGamepadState(0, state);
	CHECK_FALSE(Input::IsGamepadButtonPressed(0, GamepadButton::A));

	state.Buttons[static_cast<size_t>(GamepadButton::A)] = false;
	Input::SetGamepadState(0, state);
	CHECK(Input::IsGamepadButtonReleased(0, GamepadButton::A));

	Input::SetGamepadState(0, Input::GamepadState{});
	CHECK_FALSE(Input::IsGamepadConnected(0));
	CHECK_FALSE(Input::IsGamepadButtonDown(0, GamepadButton::A));
	CHECK_FALSE(Input::IsGamepadConnected(MaxGamepads + 3));
}

TEST_CASE_FIXTURE(InputFixture, "Input: cursor mode changes are forwarded to the handler")
{
	CursorMode received = CursorMode::Normal;
	Input::SetCursorModeHandler(
		[&received](CursorMode mode)
		{
			received = mode;
		});

	Input::SetCursorMode(CursorMode::Locked);
	CHECK(Input::GetCursorMode() == CursorMode::Locked);
	CHECK(received == CursorMode::Locked);

	Input::SetCursorModeHandler(nullptr);
	Input::SetCursorMode(CursorMode::Hidden);
	CHECK(Input::GetCursorMode() == CursorMode::Hidden);
	CHECK(received == CursorMode::Locked);
}
