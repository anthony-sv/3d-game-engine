#include "stpch.h"
#include "Strada/Core/Input.h"

#include "Strada/Core/Events/Event.h"
#include "Strada/Core/Events/KeyEvent.h"
#include "Strada/Core/Events/MouseEvent.h"
#include "Strada/Core/Log.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace Strada
{
	namespace
	{
		struct ButtonState
		{
			bool Down = false;
			bool Pressed = false;
			bool Released = false;
			// The latest release came from ReleaseKeysAndMouseButtons (mouse buttons only).
			bool ReleasedByReset = false;
			// Every change since Reset; the input frame never clears them.
			uint32_t Presses = 0;
			uint32_t Releases = 0;
		};

		struct GamepadData
		{
			// The effective state, which every reader sees: the injected one while an injection holds the slot, otherwise
			// the device's.
			Input::GamepadState Current;
			std::array<ButtonState, GamepadButtonCount> Buttons = {};
			// Whether each axis of the effective state read anything but exactly 0 since the slot connected.
			std::array<bool, GamepadAxisCount> AxisMoved = {};
			// What the window polled last, kept aside while an injection holds the slot.
			Input::GamepadState Device;
			std::optional<Input::GamepadState> Injected;
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

			// Action overrides by name, ordinal order (lookups by string_view allocate nothing).
			std::map<std::string, Input::ActionOverride, std::less<>> ActionOverrides;

			CursorMode Mode = CursorMode::Normal;
			std::function<void(CursorMode)> CursorModeHandler;
			bool FrameOwned = false;
		};

		InputData s_Data;
		// Outside s_Data: Reset moves them, and they never go back.
		uint64_t s_ResetGeneration = 1;
		uint64_t s_ActionOverridesVersion = 1;

		void ApplyButtonState(ButtonState& state, bool down)
		{
			if (down && !state.Down)
			{
				state.Pressed = true;
				state.ReleasedByReset = false;
				state.Presses++;
			}
			else if (!down && state.Down)
			{
				state.Released = true;
				state.ReleasedByReset = false;
				state.Releases++;
			}
			state.Down = down;
		}

		Input::ButtonCounts GetCounts(ButtonState const* state)
		{
			return state != nullptr ? Input::ButtonCounts{state->Presses, state->Releases} : Input::ButtonCounts{};
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

		// Makes the state the slot's effective one: buttons report their edges and counts from the previous effective state.
		void ApplyGamepadState(GamepadData& data, Input::GamepadState const& state)
		{
			for (size_t i = 0; i < data.Buttons.size(); i++)
			{
				ApplyButtonState(data.Buttons[i], state.Connected && state.Buttons[i]);
			}
			if (!state.Connected || !data.Current.Connected)
			{
				data.AxisMoved = {};
			}
			if (state.Connected)
			{
				for (size_t i = 0; i < state.Axes.size(); i++)
				{
					data.AxisMoved[i] = data.AxisMoved[i] || state.Axes[i] != 0.0f;
				}
			}
			data.Current = state;
		}

		// An injected slot's state at rest: connected, buttons up, sticks centred and triggers released (-1, Input's
		// convention).
		Input::GamepadState MakeRestingGamepad()
		{
			Input::GamepadState state;
			state.Connected = true;
			state.Axes[static_cast<size_t>(GamepadAxis::LeftTrigger)] = -1.0f;
			state.Axes[static_cast<size_t>(GamepadAxis::RightTrigger)] = -1.0f;
			return state;
		}

		// Sets the record's value as a button would change: a press when it leaves (0, 0), a release when it returns.
		void ApplyOverrideValue(Input::ActionOverride& record, glm::vec2 value, bool held)
		{
			bool const wasActive = record.Held && record.Value != glm::vec2(0.0f);
			bool const active = held && value != glm::vec2(0.0f);
			if (active && !wasActive)
			{
				record.Presses++;
			}
			else if (!active && wasActive)
			{
				record.Releases++;
			}
			if (active)
			{
				record.PeaksX.Record(record.Presses, std::abs(value.x));
				record.PeaksLength.Record(record.Presses, glm::length(value));
			}
			record.Value = held ? value : glm::vec2(0.0f);
			record.Held = held;
			s_ActionOverridesVersion++;
		}
	}

	void Input::ActionOverridePeaks::Record(uint32_t press, float magnitude)
	{
		if (m_Count > 0 && m_Peaks[m_Count - 1].Press == press)
		{
			m_Peaks[m_Count - 1].Magnitude = std::max(m_Peaks[m_Count - 1].Magnitude, magnitude);
		}
		else
		{
			if (m_Count == m_Peaks.size())
			{
				std::move(m_Peaks.begin() + 1, m_Peaks.end(), m_Peaks.begin());
				m_Count--;
			}
			m_Peaks[m_Count++] = Peak{press, magnitude};
		}
		// A press that held as much as an earlier one answers for it: the earlier one is dropped, so magnitudes fall from the
		// oldest kept to the latest.
		while (m_Count >= 2 && m_Peaks[m_Count - 2].Magnitude <= m_Peaks[m_Count - 1].Magnitude)
		{
			m_Peaks[m_Count - 2] = m_Peaks[m_Count - 1];
			m_Count--;
		}
	}

	float Input::ActionOverridePeaks::GetSince(uint32_t press) const
	{
		// The first kept press after `press` held the most of every press after it. Counts wrap, so presses compare by
		// unsigned difference from the latest.
		if (m_Count == 0)
		{
			return 0.0f;
		}
		uint32_t const latest = m_Peaks[m_Count - 1].Press;
		uint32_t const since = latest - press;
		for (uint32_t index = 0; index < m_Count; index++)
		{
			uint32_t const age = latest - m_Peaks[index].Press;
			if (age < since)
			{
				return m_Peaks[index].Magnitude;
			}
		}
		return 0.0f;
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

	bool Input::WasMouseButtonReleasedByReset(MouseButton button)
	{
		ButtonState const* state = FindMouseButton(button);
		return state != nullptr && state->ReleasedByReset;
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

	Input::ButtonCounts Input::GetKeyCounts(KeyCode key)
	{
		return GetCounts(FindKey(key));
	}

	Input::ButtonCounts Input::GetMouseButtonCounts(MouseButton button)
	{
		return GetCounts(FindMouseButton(button));
	}

	Input::ButtonCounts Input::GetGamepadButtonCounts(uint32_t gamepad, GamepadButton button)
	{
		// Of a disconnected slot too: a release made by disconnecting is counted, and counts never go back.
		GamepadData const* data = FindGamepad(gamepad);
		size_t const index = static_cast<size_t>(button);
		return data != nullptr && index < data->Buttons.size() ? GetCounts(&data->Buttons[index]) : ButtonCounts{};
	}

	bool Input::HasGamepadAxisMoved(uint32_t gamepad, GamepadAxis axis)
	{
		GamepadData const* data = FindGamepad(gamepad);
		size_t const index = static_cast<size_t>(axis);
		return data != nullptr && data->Current.Connected && index < data->AxisMoved.size() && data->AxisMoved[index];
	}

	uint64_t Input::GetResetGeneration()
	{
		return s_ResetGeneration;
	}

	bool Input::SetActionOverride(std::string_view action, glm::vec2 value)
	{
		if (action.empty())
		{
			ST_CORE_WARN("Input: an action override needs an action name");
			return false;
		}
		if (!std::isfinite(value.x) || !std::isfinite(value.y))
		{
			ST_CORE_WARN("Input: the override of action '{}' is not a finite value", action);
			return false;
		}
		auto found = s_Data.ActionOverrides.find(action);
		if (found == s_Data.ActionOverrides.end())
		{
			if (s_Data.ActionOverrides.size() >= MaxActionOverrides)
			{
				ST_CORE_WARN("Input: action '{}' cannot be overridden: {} action names have overrides already", action, MaxActionOverrides);
				return false;
			}
			found = s_Data.ActionOverrides.emplace(std::string(action), ActionOverride{}).first;
		}
		ApplyOverrideValue(found->second, value, true);
		return true;
	}

	void Input::ClearActionOverride(std::string_view action)
	{
		auto const found = s_Data.ActionOverrides.find(action);
		if (found != s_Data.ActionOverrides.end() && found->second.Held)
		{
			ApplyOverrideValue(found->second, glm::vec2(0.0f), false);
		}
	}

	void Input::ClearActionOverrides()
	{
		for (auto& [name, record] : s_Data.ActionOverrides)
		{
			if (record.Held)
			{
				ApplyOverrideValue(record, glm::vec2(0.0f), false);
			}
		}
	}

	void Input::DropReleasedActionOverrides()
	{
		size_t const dropped = std::erase_if(s_Data.ActionOverrides,
		                                     [](auto const& entry)
		                                     {
												 return !entry.second.Held;
											 });
		if (dropped > 0)
		{
			s_ActionOverridesVersion++;
		}
	}

	Input::ActionOverride const* Input::FindActionOverride(std::string_view action)
	{
		auto const found = s_Data.ActionOverrides.find(action);
		return found != s_Data.ActionOverrides.end() ? &found->second : nullptr;
	}

	bool Input::HasActionOverrides()
	{
		return !s_Data.ActionOverrides.empty();
	}

	std::vector<std::pair<std::string, glm::vec2>> Input::GetActionOverrides()
	{
		std::vector<std::pair<std::string, glm::vec2>> overrides;
		for (auto const& [name, record] : s_Data.ActionOverrides)
		{
			if (record.Held)
			{
				overrides.emplace_back(name, record.Value);
			}
		}
		return overrides;
	}

	void Input::ForEachActionOverride(std::function<void(std::string_view name, ActionOverride const& record)> const& function)
	{
		for (auto const& [name, record] : s_Data.ActionOverrides)
		{
			function(name, record);
		}
	}

	uint64_t Input::GetActionOverridesVersion()
	{
		return s_ActionOverridesVersion;
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

	void Input::WarpMousePosition(glm::vec2 position)
	{
		// Shifting the frame's starting point with the cursor keeps the delta moved so far and adds nothing for the jump.
		s_Data.MousePositionAtFrameStart =
			s_Data.HasMousePosition ? s_Data.MousePositionAtFrameStart + (position - s_Data.MousePosition) : position;
		s_Data.MousePosition = position;
		s_Data.HasMousePosition = true;
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
		data.Device = state;
		if (!data.Injected)
		{
			ApplyGamepadState(data, state);
		}
	}

	void Input::InjectGamepadState(uint32_t gamepad, GamepadState const& state)
	{
		if (gamepad >= s_Data.Gamepads.size())
		{
			return;
		}
		GamepadData& data = s_Data.Gamepads[gamepad];
		if (!data.Injected)
		{
			// A change of source is a new connection for the axes: the device's axes that read exactly 0 (a trigger its
			// mapping lacks) say nothing of the injected ones.
			data.AxisMoved = {};
		}
		data.Injected = state;
		ApplyGamepadState(data, state);
	}

	void Input::ReleaseInjectedGamepad(uint32_t gamepad)
	{
		if (gamepad >= s_Data.Gamepads.size() || !s_Data.Gamepads[gamepad].Injected)
		{
			return;
		}
		GamepadData& data = s_Data.Gamepads[gamepad];
		data.Injected.reset();
		// Back to the device: the injected axes' movement says nothing of the device's (a trigger its mapping lacks reads 0).
		data.AxisMoved = {};
		ApplyGamepadState(data, data.Device);
	}

	void Input::ReleaseInjectedGamepads()
	{
		for (uint32_t gamepad = 0; gamepad < MaxGamepads; gamepad++)
		{
			ReleaseInjectedGamepad(gamepad);
		}
	}

	bool Input::IsGamepadInjected(uint32_t gamepad)
	{
		GamepadData const* data = FindGamepad(gamepad);
		return data != nullptr && data->Injected.has_value();
	}

	std::optional<Input::GamepadState> Input::GetInjectedGamepadState(uint32_t gamepad)
	{
		GamepadData const* data = FindGamepad(gamepad);
		return data != nullptr ? data->Injected : std::nullopt;
	}

	Input::GamepadState Input::GetRestingGamepadState()
	{
		return MakeRestingGamepad();
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

	void Input::SetFrameOwned(bool owned)
	{
		s_Data.FrameOwned = owned;
	}

	bool Input::IsFrameOwned()
	{
		return s_Data.FrameOwned;
	}

	void Input::EndApplicationFrame()
	{
		if (!s_Data.FrameOwned)
		{
			EndFrame();
		}
	}

	void Input::ReleaseKeysAndMouseButtons()
	{
		for (ButtonState& state : s_Data.Keys)
		{
			ApplyButtonState(state, false);
		}
		for (ButtonState& state : s_Data.MouseButtons)
		{
			if (state.Down)
			{
				ApplyButtonState(state, false);
				state.ReleasedByReset = true;
			}
		}
	}

	void Input::ReleaseInjectedInput()
	{
		ReleaseInjectedGamepads();
		ClearActionOverrides();
	}

	void Input::Reset()
	{
		std::function<void(CursorMode)> handler = std::move(s_Data.CursorModeHandler);
		bool const frameOwned = s_Data.FrameOwned;
		s_Data = InputData();
		s_Data.CursorModeHandler = std::move(handler);
		s_Data.FrameOwned = frameOwned;
		s_ResetGeneration++;
		s_ActionOverridesVersion++;
	}

	void Input::SetCursorModeHandler(std::function<void(CursorMode)> handler)
	{
		s_Data.CursorModeHandler = std::move(handler);
	}
}
