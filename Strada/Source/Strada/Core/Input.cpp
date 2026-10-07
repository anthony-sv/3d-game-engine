#include "stpch.h"
#include "Strada/Core/Input.h"

#include "Strada/Core/Events/Event.h"
#include "Strada/Core/Events/KeyEvent.h"
#include "Strada/Core/Events/MouseEvent.h"

namespace Strada
{
	namespace
	{
		struct ButtonState
		{
			bool Down = false;
			bool Pressed = false;
			bool Released = false;
		};

		struct GamepadData
		{
			Input::GamepadState Current;
			std::array<ButtonState, GamepadButtonCount> Buttons = {};
		};

		struct InputData
		{
			std::array<ButtonState, KeyCodeCount> Keys = {};
			std::array<ButtonState, MouseButtonCount> MouseButtons = {};
			std::array<GamepadData, MaxGamepads> Gamepads = {};

			glm::vec2 MousePosition = {0.0f, 0.0f};
			glm::vec2 MousePositionAtFrameStart = {0.0f, 0.0f};
			bool HasMousePosition = false;
			glm::vec2 ScrollDelta = {0.0f, 0.0f};

			CursorMode Mode = CursorMode::Normal;
			std::function<void(CursorMode)> CursorModeHandler;
		};

		InputData s_Data;

		void ApplyButtonState(ButtonState& state, bool down)
		{
			if (down && !state.Down)
			{
				state.Pressed = true;
			}
			else if (!down && state.Down)
			{
				state.Released = true;
			}
			state.Down = down;
		}

		ButtonState const* FindKey(KeyCode key)
		{
			size_t const index = static_cast<size_t>(key);
			return index < s_Data.Keys.size() ? &s_Data.Keys[index] : nullptr;
		}

		ButtonState const* FindMouseButton(MouseButton button)
		{
			size_t const index = static_cast<size_t>(button);
			return index < s_Data.MouseButtons.size() ? &s_Data.MouseButtons[index] : nullptr;
		}

		GamepadData const* FindGamepad(uint32_t gamepad)
		{
			return gamepad < s_Data.Gamepads.size() ? &s_Data.Gamepads[gamepad] : nullptr;
		}

		ButtonState const* FindGamepadButton(uint32_t gamepad, GamepadButton button)
		{
			GamepadData const* data = FindGamepad(gamepad);
			size_t const index = static_cast<size_t>(button);
			if (data == nullptr || !data->Current.Connected || index >= data->Buttons.size())
			{
				return nullptr;
			}
			return &data->Buttons[index];
		}
	}

	bool Input::IsKeyDown(KeyCode key)
	{
		ButtonState const* state = FindKey(key);
		return state != nullptr && state->Down;
	}

	bool Input::IsKeyPressed(KeyCode key)
	{
		ButtonState const* state = FindKey(key);
		return state != nullptr && state->Pressed;
	}

	bool Input::IsKeyReleased(KeyCode key)
	{
		ButtonState const* state = FindKey(key);
		return state != nullptr && state->Released;
	}

	bool Input::IsMouseButtonDown(MouseButton button)
	{
		ButtonState const* state = FindMouseButton(button);
		return state != nullptr && state->Down;
	}

	bool Input::IsMouseButtonPressed(MouseButton button)
	{
		ButtonState const* state = FindMouseButton(button);
		return state != nullptr && state->Pressed;
	}

	bool Input::IsMouseButtonReleased(MouseButton button)
	{
		ButtonState const* state = FindMouseButton(button);
		return state != nullptr && state->Released;
	}

	glm::vec2 Input::GetMousePosition()
	{
		return s_Data.MousePosition;
	}

	glm::vec2 Input::GetMouseDelta()
	{
		return s_Data.MousePosition - s_Data.MousePositionAtFrameStart;
	}

	glm::vec2 Input::GetScrollDelta()
	{
		return s_Data.ScrollDelta;
	}

	bool Input::IsGamepadConnected(uint32_t gamepad)
	{
		GamepadData const* data = FindGamepad(gamepad);
		return data != nullptr && data->Current.Connected;
	}

	float Input::GetGamepadAxis(uint32_t gamepad, GamepadAxis axis)
	{
		GamepadData const* data = FindGamepad(gamepad);
		size_t const index = static_cast<size_t>(axis);
		if (data == nullptr || !data->Current.Connected || index >= data->Current.Axes.size())
		{
			return 0.0f;
		}
		return data->Current.Axes[index];
	}

	bool Input::IsGamepadButtonDown(uint32_t gamepad, GamepadButton button)
	{
		ButtonState const* state = FindGamepadButton(gamepad, button);
		return state != nullptr && state->Down;
	}

	bool Input::IsGamepadButtonPressed(uint32_t gamepad, GamepadButton button)
	{
		ButtonState const* state = FindGamepadButton(gamepad, button);
		return state != nullptr && state->Pressed;
	}

	bool Input::IsGamepadButtonReleased(uint32_t gamepad, GamepadButton button)
	{
		ButtonState const* state = FindGamepadButton(gamepad, button);
		return state != nullptr && state->Released;
	}

	CursorMode Input::GetCursorMode()
	{
		return s_Data.Mode;
	}

	void Input::SetCursorMode(CursorMode mode)
	{
		s_Data.Mode = mode;
		if (s_Data.CursorModeHandler)
		{
			s_Data.CursorModeHandler(mode);
		}
	}

	void Input::ProcessEvent(Event& event)
	{
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<KeyPressedEvent>(
			[](KeyPressedEvent& keyEvent)
			{
				SetKeyState(keyEvent.GetKeyCode(), true);
				return false;
			});
		dispatcher.Dispatch<KeyReleasedEvent>(
			[](KeyReleasedEvent& keyEvent)
			{
				SetKeyState(keyEvent.GetKeyCode(), false);
				return false;
			});
		dispatcher.Dispatch<MouseButtonPressedEvent>(
			[](MouseButtonPressedEvent& buttonEvent)
			{
				SetMouseButtonState(buttonEvent.GetMouseButton(), true);
				return false;
			});
		dispatcher.Dispatch<MouseButtonReleasedEvent>(
			[](MouseButtonReleasedEvent& buttonEvent)
			{
				SetMouseButtonState(buttonEvent.GetMouseButton(), false);
				return false;
			});
		dispatcher.Dispatch<MouseMovedEvent>(
			[](MouseMovedEvent& moveEvent)
			{
				SetMousePosition({moveEvent.GetX(), moveEvent.GetY()});
				return false;
			});
		dispatcher.Dispatch<MouseScrolledEvent>(
			[](MouseScrolledEvent& scrollEvent)
			{
				AddScrollDelta({scrollEvent.GetXOffset(), scrollEvent.GetYOffset()});
				return false;
			});
	}

	void Input::SetKeyState(KeyCode key, bool down)
	{
		size_t const index = static_cast<size_t>(key);
		if (index == 0 || index >= s_Data.Keys.size())
		{
			return;
		}
		ApplyButtonState(s_Data.Keys[index], down);
	}

	void Input::SetMouseButtonState(MouseButton button, bool down)
	{
		size_t const index = static_cast<size_t>(button);
		if (index >= s_Data.MouseButtons.size())
		{
			return;
		}
		ApplyButtonState(s_Data.MouseButtons[index], down);
	}

	void Input::SetMousePosition(glm::vec2 position)
	{
		if (!s_Data.HasMousePosition)
		{
			// The first known position must not produce a jump in the delta.
			s_Data.MousePositionAtFrameStart = position;
			s_Data.HasMousePosition = true;
		}
		s_Data.MousePosition = position;
	}

	void Input::AddScrollDelta(glm::vec2 delta)
	{
		s_Data.ScrollDelta += delta;
	}

	void Input::SetGamepadState(uint32_t gamepad, GamepadState const& state)
	{
		if (gamepad >= s_Data.Gamepads.size())
		{
			return;
		}

		GamepadData& data = s_Data.Gamepads[gamepad];
		for (size_t i = 0; i < data.Buttons.size(); i++)
		{
			ApplyButtonState(data.Buttons[i], state.Connected && state.Buttons[i]);
		}
		data.Current = state;
	}

	void Input::EndFrame()
	{
		auto const clearFlags = [](auto& states)
		{
			for (ButtonState& state : states)
			{
				state.Pressed = false;
				state.Released = false;
			}
		};

		clearFlags(s_Data.Keys);
		clearFlags(s_Data.MouseButtons);
		for (GamepadData& gamepad : s_Data.Gamepads)
		{
			clearFlags(gamepad.Buttons);
		}

		s_Data.MousePositionAtFrameStart = s_Data.MousePosition;
		s_Data.ScrollDelta = {0.0f, 0.0f};
	}

	void Input::ReleaseAll()
	{
		for (ButtonState& state : s_Data.Keys)
		{
			ApplyButtonState(state, false);
		}
		for (ButtonState& state : s_Data.MouseButtons)
		{
			ApplyButtonState(state, false);
		}
		for (GamepadData& gamepad : s_Data.Gamepads)
		{
			for (ButtonState& state : gamepad.Buttons)
			{
				ApplyButtonState(state, false);
			}
		}
	}

	void Input::Reset()
	{
		std::function<void(CursorMode)> handler = std::move(s_Data.CursorModeHandler);
		s_Data = InputData();
		s_Data.CursorModeHandler = std::move(handler);
	}

	void Input::SetCursorModeHandler(std::function<void(CursorMode)> handler)
	{
		s_Data.CursorModeHandler = std::move(handler);
	}
}
