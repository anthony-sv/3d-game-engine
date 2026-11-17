#include "Strada/Core/Events/KeyEvent.h"
#include "Strada/Core/Events/MouseEvent.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/Log.h"

#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strada;

namespace
{
	struct InputFixture
	{
		InputFixture() { Input::Reset(); }
		~InputFixture()
		{
			Input::SetFrameOwned(false);
			Input::Reset();
		}
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

TEST_CASE_FIXTURE(InputFixture, "Input: the application's frames end the input frame unless it is owned, whose owner ends it")
{
	// Unowned (edit mode, the player, exported games), every application frame ends the input frame.
	CHECK_FALSE(Input::IsFrameOwned());
	Input::SetMousePosition({10.0f, 10.0f});
	Input::SetKeyState(KeyCode::Space, true);
	Input::SetMousePosition({14.0f, 13.0f});
	Input::AddScrollDelta({0.0f, 2.0f});
	Input::EndApplicationFrame();
	CHECK_FALSE(Input::IsKeyPressed(KeyCode::Space));
	CHECK(Input::GetMouseDelta() == glm::vec2(0.0f));
	CHECK(Input::GetScrollDelta() == glm::vec2(0.0f));

	// Owned, the application's frames keep the press, the release, the movement and the scroll for the owner's next frame.
	Input::SetFrameOwned(true);
	CHECK(Input::IsFrameOwned());
	Input::SetKeyState(KeyCode::Space, false);
	Input::SetMouseButtonState(MouseButton::Left, true);
	Input::SetMousePosition({20.0f, 18.0f});
	Input::AddScrollDelta({0.0f, -1.0f});
	Input::EndApplicationFrame();
	Input::EndApplicationFrame();
	CHECK(Input::IsKeyReleased(KeyCode::Space));
	CHECK(Input::IsMouseButtonPressed(MouseButton::Left));
	CHECK(Input::GetMouseDelta() == glm::vec2(6.0f, 5.0f));
	CHECK(Input::GetScrollDelta() == glm::vec2(0.0f, -1.0f));
	// The owner's end of frame ends it; held buttons stay down.
	Input::EndFrame();
	CHECK_FALSE(Input::IsKeyReleased(KeyCode::Space));
	CHECK_FALSE(Input::IsMouseButtonPressed(MouseButton::Left));
	CHECK(Input::IsMouseButtonDown(MouseButton::Left));
	CHECK(Input::GetMouseDelta() == glm::vec2(0.0f));
	CHECK(Input::GetScrollDelta() == glm::vec2(0.0f));

	// Reset clears the state, not the owner; giving the frame back makes the application's frames end it again.
	Input::Reset();
	CHECK(Input::IsFrameOwned());
	Input::SetFrameOwned(false);
	Input::SetKeyState(KeyCode::A, true);
	Input::EndApplicationFrame();
	CHECK_FALSE(Input::IsKeyPressed(KeyCode::A));
	CHECK(Input::IsKeyDown(KeyCode::A));
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

TEST_CASE_FIXTURE(InputFixture, "Input: warping the cursor moves it without reporting the jump as movement")
{
	// The first known position.
	Input::WarpMousePosition({40.0f, 30.0f});
	CHECK(Input::GetMousePosition() == glm::vec2(40.0f, 30.0f));
	CHECK(Input::GetMouseDelta() == glm::vec2(0.0f, 0.0f));
	Input::EndFrame();

	// Movement before the jump stays, the jump adds nothing, and movement after it counts from the new place.
	Input::SetMousePosition({45.0f, 30.0f});
	Input::WarpMousePosition({500.0f, 400.0f});
	CHECK(Input::GetMousePosition() == glm::vec2(500.0f, 400.0f));
	CHECK(Input::GetMouseDelta() == glm::vec2(5.0f, 0.0f));
	Input::SetMousePosition({498.0f, 401.0f});
	CHECK(Input::GetMouseDelta() == glm::vec2(3.0f, 1.0f));

	Input::EndFrame();
	CHECK(Input::GetMouseDelta() == glm::vec2(0.0f, 0.0f));
	CHECK(Input::GetMousePosition() == glm::vec2(498.0f, 401.0f));
}

TEST_CASE_FIXTURE(InputFixture, "Input: releasing the keys and mouse buttons reports their releases and leaves gamepads alone")
{
	Input::SetKeyState(KeyCode::LeftShift, true);
	Input::SetMouseButtonState(MouseButton::Right, true);
	Input::GamepadState gamepad;
	gamepad.Connected = true;
	gamepad.Buttons[static_cast<size_t>(GamepadButton::A)] = true;
	gamepad.Axes[static_cast<size_t>(GamepadAxis::LeftY)] = -0.5f;
	Input::SetGamepadState(0, gamepad);
	Input::EndFrame();

	Input::ReleaseKeysAndMouseButtons();
	CHECK_FALSE(Input::IsKeyDown(KeyCode::LeftShift));
	CHECK(Input::IsKeyReleased(KeyCode::LeftShift));
	CHECK_FALSE(Input::IsMouseButtonDown(MouseButton::Right));
	CHECK(Input::IsMouseButtonReleased(MouseButton::Right));
	// A gamepad is polled every frame: a button held on it stays held, without a release and a second press.
	CHECK(Input::IsGamepadButtonDown(0, GamepadButton::A));
	CHECK_FALSE(Input::IsGamepadButtonReleased(0, GamepadButton::A));
	CHECK(Input::GetGamepadAxis(0, GamepadAxis::LeftY) == doctest::Approx(-0.5f));
	Input::SetGamepadState(0, gamepad);
	CHECK_FALSE(Input::IsGamepadButtonPressed(0, GamepadButton::A));

	// Nothing held: nothing to report.
	Input::EndFrame();
	Input::ReleaseKeysAndMouseButtons();
	CHECK_FALSE(Input::IsKeyReleased(KeyCode::LeftShift));
	CHECK_FALSE(Input::IsMouseButtonReleased(MouseButton::Right));
}

TEST_CASE_FIXTURE(InputFixture, "Input: mouse buttons tell whether their latest release came from a reset")
{
	CHECK_FALSE(Input::WasMouseButtonReleasedByReset(MouseButton::Left));
	Input::SetMouseButtonState(MouseButton::Left, true);
	Input::ReleaseKeysAndMouseButtons();
	CHECK(Input::IsMouseButtonReleased(MouseButton::Left));
	CHECK(Input::WasMouseButtonReleasedByReset(MouseButton::Left));
	// It lasts past the frame, for interfaces that look less often, until the button is pressed again.
	Input::EndFrame();
	CHECK(Input::WasMouseButtonReleasedByReset(MouseButton::Left));
	Input::ReleaseKeysAndMouseButtons();
	CHECK(Input::WasMouseButtonReleasedByReset(MouseButton::Left));
	Input::SetMouseButtonState(MouseButton::Left, true);
	CHECK_FALSE(Input::WasMouseButtonReleasedByReset(MouseButton::Left));
	// Releases from the device are not resets, and a reset with nothing held changes nothing.
	Input::SetMouseButtonState(MouseButton::Left, false);
	CHECK_FALSE(Input::WasMouseButtonReleasedByReset(MouseButton::Left));
	Input::ReleaseKeysAndMouseButtons();
	CHECK_FALSE(Input::WasMouseButtonReleasedByReset(MouseButton::Left));
	// Other buttons keep their own, and a full reset forgets them.
	Input::SetMouseButtonState(MouseButton::Right, true);
	Input::ReleaseKeysAndMouseButtons();
	CHECK(Input::WasMouseButtonReleasedByReset(MouseButton::Right));
	CHECK_FALSE(Input::WasMouseButtonReleasedByReset(MouseButton::Left));
	CHECK_FALSE(Input::WasMouseButtonReleasedByReset(static_cast<MouseButton>(MouseButtonCount)));
	Input::Reset();
	CHECK_FALSE(Input::WasMouseButtonReleasedByReset(MouseButton::Right));
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

TEST_CASE_FIXTURE(InputFixture, "Input: press and release counts outlive the input frame, and Reset moves the generation")
{
	CHECK(Input::GetKeyCounts(KeyCode::A) == Input::ButtonCounts{});
	Input::SetKeyState(KeyCode::A, true);
	Input::SetKeyState(KeyCode::A, true);
	Input::EndFrame();
	Input::SetKeyState(KeyCode::A, false);
	Input::EndFrame();
	CHECK(Input::GetKeyCounts(KeyCode::A) == Input::ButtonCounts{1, 1});
	Input::SetMouseButtonState(MouseButton::Left, true);
	Input::ReleaseKeysAndMouseButtons();
	CHECK(Input::GetMouseButtonCounts(MouseButton::Left) == Input::ButtonCounts{1, 1});
	CHECK(Input::GetKeyCounts(static_cast<KeyCode>(KeyCodeCount + 5)) == Input::ButtonCounts{});

	uint64_t const generation = Input::GetResetGeneration();
	Input::Reset();
	CHECK(Input::GetResetGeneration() != generation);
	CHECK(Input::GetKeyCounts(KeyCode::A) == Input::ButtonCounts{});
}

TEST_CASE_FIXTURE(InputFixture, "Input: an injected gamepad holds across polls, and returns to its device with the edges that makes")
{
	Input::GamepadState device;
	device.Connected = true;
	device.Buttons[static_cast<size_t>(GamepadButton::B)] = true;
	Input::SetGamepadState(3, device);
	CHECK(Input::IsGamepadButtonDown(3, GamepadButton::B));
	CHECK_FALSE(Input::IsGamepadInjected(3));

	Input::GamepadState injected = Input::GetRestingGamepadState();
	CHECK(injected.Connected);
	CHECK(injected.Axes[static_cast<size_t>(GamepadAxis::LeftTrigger)] == -1.0f);
	injected.Buttons[static_cast<size_t>(GamepadButton::A)] = true;
	injected.Axes[static_cast<size_t>(GamepadAxis::LeftX)] = 0.5f;
	Input::InjectGamepadState(3, injected);
	CHECK(Input::IsGamepadInjected(3));
	CHECK(Input::GetInjectedGamepadState(3).has_value());
	CHECK(Input::IsGamepadButtonPressed(3, GamepadButton::A));
	CHECK(Input::IsGamepadButtonReleased(3, GamepadButton::B));
	CHECK(Input::GetGamepadButtonCounts(3, GamepadButton::B) == Input::ButtonCounts{1, 1});

	// Polls write every slot every frame; the injection holds.
	Input::EndFrame();
	for (int poll = 0; poll < 3; poll++)
	{
		Input::SetGamepadState(3, Input::GamepadState{});
	}
	CHECK(Input::IsGamepadConnected(3));
	CHECK(Input::IsGamepadButtonDown(3, GamepadButton::A));
	CHECK(Input::GetGamepadAxis(3, GamepadAxis::LeftX) == 0.5f);
	CHECK_FALSE(Input::IsGamepadButtonReleased(3, GamepadButton::A));

	// The window losing the focus leaves it; releasing it returns the slot to what the device last gave (disconnected).
	Input::ReleaseKeysAndMouseButtons();
	CHECK(Input::IsGamepadButtonDown(3, GamepadButton::A));
	Input::ReleaseInjectedGamepad(3);
	CHECK_FALSE(Input::IsGamepadInjected(3));
	CHECK_FALSE(Input::IsGamepadConnected(3));
	CHECK(Input::GetGamepadButtonCounts(3, GamepadButton::A) == Input::ButtonCounts{1, 1});
	CHECK_FALSE(Input::GetInjectedGamepadState(3).has_value());
	CHECK(Input::GetGamepadButtonCounts(16, GamepadButton::A) == Input::ButtonCounts{});
	Input::InjectGamepadState(16, injected);
	CHECK_FALSE(Input::IsGamepadInjected(16));

	// ReleaseInjectedInput ends injections and overrides alike; Reset clears everything.
	Input::InjectGamepadState(1, injected);
	REQUIRE(Input::SetActionOverride("Jump", glm::vec2(1.0f, 0.0f)));
	Input::ReleaseInjectedInput();
	CHECK_FALSE(Input::IsGamepadInjected(1));
	CHECK(Input::GetActionOverrides().empty());
	Input::InjectGamepadState(1, injected);
	Input::Reset();
	CHECK_FALSE(Input::IsGamepadInjected(1));
	CHECK_FALSE(Input::IsGamepadConnected(1));
}

TEST_CASE_FIXTURE(InputFixture, "Input: an axis has moved once it read anything but 0 since its pad connected")
{
	Input::GamepadState pad;
	pad.Connected = true;
	Input::SetGamepadState(0, pad);
	CHECK_FALSE(Input::HasGamepadAxisMoved(0, GamepadAxis::RightTrigger));
	pad.Axes[static_cast<size_t>(GamepadAxis::RightTrigger)] = -1.0f;
	Input::SetGamepadState(0, pad);
	CHECK(Input::HasGamepadAxisMoved(0, GamepadAxis::RightTrigger));
	pad.Axes[static_cast<size_t>(GamepadAxis::RightTrigger)] = 0.0f;
	Input::SetGamepadState(0, pad);
	CHECK(Input::HasGamepadAxisMoved(0, GamepadAxis::RightTrigger));
	CHECK_FALSE(Input::HasGamepadAxisMoved(0, GamepadAxis::LeftTrigger));
	// Unplugged and plugged in again: not moved until it reads something else.
	Input::SetGamepadState(0, Input::GamepadState{});
	CHECK_FALSE(Input::HasGamepadAxisMoved(0, GamepadAxis::RightTrigger));
	Input::SetGamepadState(0, pad);
	CHECK_FALSE(Input::HasGamepadAxisMoved(0, GamepadAxis::RightTrigger));
}

TEST_CASE_FIXTURE(InputFixture, "Input: action overrides count presses and releases as a button, and keep their record when cleared")
{
	CHECK_FALSE(Input::HasActionOverrides());
	CHECK(Input::FindActionOverride("Jump") == nullptr);
	uint64_t const version = Input::GetActionOverridesVersion();
	REQUIRE(Input::SetActionOverride("Jump", glm::vec2(0.5f, 0.0f)));
	CHECK(Input::GetActionOverridesVersion() != version);
	REQUIRE(Input::SetActionOverride("Jump", glm::vec2(-0.75f, 0.25f)));
	Input::ActionOverride const* record = Input::FindActionOverride("Jump");
	REQUIRE(record != nullptr);
	CHECK(record->Held);
	CHECK(record->Value == glm::vec2(-0.75f, 0.25f));
	CHECK(record->Presses == 1);
	CHECK(record->Releases == 0);
	CHECK(record->PeakX == 0.75f);
	CHECK(record->PeakLength == doctest::Approx(std::sqrt(0.625f)));

	Input::ClearActionOverride("Jump");
	record = Input::FindActionOverride("Jump");
	REQUIRE(record != nullptr);
	CHECK_FALSE(record->Held);
	CHECK(record->Value == glm::vec2(0.0f));
	CHECK(record->Releases == 1);
	CHECK(Input::GetActionOverrides().empty());
	// Setting (0, 0) holds the override at rest: no press.
	REQUIRE(Input::SetActionOverride("Jump", glm::vec2(0.0f)));
	CHECK(Input::FindActionOverride("Jump")->Presses == 1);
	CHECK(Input::GetActionOverrides() == std::vector<std::pair<std::string, glm::vec2>>{{"Jump", glm::vec2(0.0f)}});

	CHECK_FALSE(Input::SetActionOverride("", glm::vec2(1.0f)));
	CHECK_FALSE(Input::SetActionOverride("Jump", glm::vec2(std::numeric_limits<float>::infinity(), 0.0f)));
	CHECK_FALSE(Input::SetActionOverride("Jump", glm::vec2(0.0f, std::nanf(""))));
	for (size_t index = 1; index < Input::MaxActionOverrides; index++)
	{
		REQUIRE(Input::SetActionOverride(fmt::format("Action{}", index), glm::vec2(1.0f)));
	}
	CHECK_FALSE(Input::SetActionOverride("OneTooMany", glm::vec2(1.0f)));
	CHECK(Input::SetActionOverride("Jump", glm::vec2(1.0f)));
	size_t records = 0;
	Input::ForEachActionOverride(
		[&records](std::string_view, Input::ActionOverride const&)
		{
			records++;
		});
	CHECK(records == Input::MaxActionOverrides);

	uint64_t const before = Input::GetActionOverridesVersion();
	Input::Reset();
	CHECK(Input::GetActionOverridesVersion() > before);
	CHECK_FALSE(Input::HasActionOverrides());
}
