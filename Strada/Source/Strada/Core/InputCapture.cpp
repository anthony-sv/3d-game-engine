#include "stpch.h"
#include "Strada/Core/InputCapture.h"

#include "Strada/Core/Input.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Strada
{
	namespace
	{
		// Key codes with a name (KeyCode's enumerators): only these can be bound.
		bool IsNamedKey(uint16_t code)
		{
			return code != 0 && std::strcmp(KeyCodeToString(static_cast<KeyCode>(code)), "Unknown") != 0;
		}

		// Whether a button's press count moved past its baseline (counts wrap: compared by unsigned difference).
		bool WasPressed(uint32_t presses, uint32_t baseline)
		{
			return presses - baseline != 0;
		}
	}

	void InputControlCapture::Start(InputCaptureTarget target, int32_t gamepad)
	{
		m_Target = target;
		m_Gamepad = gamepad;
		m_Listening = true;
		m_Elapsed = 0.0f;
		m_Control = {};
		m_ResetGeneration = Input::GetResetGeneration();
		TakeBaselines();
		for (uint32_t slot = 0; slot < MaxGamepads; slot++)
		{
			for (uint32_t axis = 0; axis < GetAxisCount(); axis++)
			{
				m_Armed[slot][axis] = ReadAxis(slot, axis) < Threshold;
			}
		}
	}

	void InputControlCapture::TakeBaselines()
	{
		for (uint16_t key = 0; key < KeyCodeCount; key++)
		{
			m_KeyPresses[key] = Input::GetKeyCounts(static_cast<KeyCode>(key)).Presses;
		}
		for (uint8_t button = 0; button < MouseButtonCount; button++)
		{
			m_MousePresses[button] = Input::GetMouseButtonCounts(static_cast<MouseButton>(button)).Presses;
		}
		for (uint32_t slot = 0; slot < MaxGamepads; slot++)
		{
			for (uint8_t button = 0; button < GamepadButtonCount; button++)
			{
				m_PadPresses[slot][button] = Input::GetGamepadButtonCounts(slot, static_cast<GamepadButton>(button)).Presses;
			}
		}
	}

	InputCaptureOutcome InputControlCapture::Update(float seconds)
	{
		if (!m_Listening)
		{
			return InputCaptureOutcome::Waiting;
		}
		if (Input::GetResetGeneration() != m_ResetGeneration)
		{
			// Input::Reset cleared every count: what is counted from now on is new.
			m_ResetGeneration = Input::GetResetGeneration();
			TakeBaselines();
		}

		auto const capture = [this](InputControl control)
		{
			m_Control = control;
			m_Listening = false;
			return InputCaptureOutcome::Captured;
		};

		if (WasPressed(Input::GetKeyCounts(KeyCode::Escape).Presses, m_KeyPresses[static_cast<uint16_t>(KeyCode::Escape)]))
		{
			m_Listening = false;
			return InputCaptureOutcome::Escaped;
		}
		bool const takesButtons = m_Target != InputCaptureTarget::Stick;
		if (takesButtons)
		{
			for (uint16_t key = 0; key < KeyCodeCount; key++)
			{
				if (IsNamedKey(key) && WasPressed(Input::GetKeyCounts(static_cast<KeyCode>(key)).Presses, m_KeyPresses[key]))
				{
					return capture(InputControl{InputControlKind::Key, key});
				}
			}
			for (uint8_t button = 0; button < MouseButtonCount; button++)
			{
				if (WasPressed(Input::GetMouseButtonCounts(static_cast<MouseButton>(button)).Presses, m_MousePresses[button]))
				{
					return capture(InputControl{InputControlKind::Mouse, button});
				}
			}
			for (uint32_t slot = 0; slot < MaxGamepads; slot++)
			{
				if (!ReadsGamepad(slot))
				{
					continue;
				}
				for (uint8_t button = 0; button < GamepadButtonCount; button++)
				{
					if (WasPressed(Input::GetGamepadButtonCounts(slot, static_cast<GamepadButton>(button)).Presses,
					               m_PadPresses[slot][button]))
					{
						return capture(InputControl{InputControlKind::PadButton, button});
					}
				}
			}
		}
		for (uint32_t slot = 0; slot < MaxGamepads; slot++)
		{
			if (!ReadsGamepad(slot))
			{
				continue;
			}
			for (uint32_t axis = 0; axis < GetAxisCount(); axis++)
			{
				bool const reached = ReadAxis(slot, axis) >= Threshold;
				bool const armed = m_Armed[slot][axis];
				m_Armed[slot][axis] = !reached;
				if (reached && armed)
				{
					InputControlKind const kind =
						m_Target == InputCaptureTarget::Stick ? InputControlKind::PadStick : InputControlKind::PadAxis;
					return capture(InputControl{kind, static_cast<uint16_t>(axis)});
				}
			}
		}

		m_Elapsed += std::isfinite(seconds) ? std::max(seconds, 0.0f) : 0.0f;
		if (m_Elapsed >= Timeout)
		{
			m_Listening = false;
			return InputCaptureOutcome::TimedOut;
		}
		return InputCaptureOutcome::Waiting;
	}

	bool InputControlCapture::ReadsGamepad(uint32_t slot) const
	{
		return m_Gamepad == AnyGamepad || static_cast<int32_t>(slot) == m_Gamepad;
	}

	uint32_t InputControlCapture::GetAxisCount() const
	{
		// A Vector2 binding takes a stick: two of them, left (0) and right (1), as InputControlKind::PadStick names them.
		return m_Target == InputCaptureTarget::Stick ? 2u : static_cast<uint32_t>(GamepadAxisCount);
	}

	float InputControlCapture::ReadAxis(uint32_t slot, uint32_t axis) const
	{
		int32_t const gamepad = static_cast<int32_t>(slot);
		switch (m_Target)
		{
			case InputCaptureTarget::Control:
				return std::abs(
					InputProcessing::ReadControl(InputControl{InputControlKind::PadAxis, static_cast<uint16_t>(axis)}, gamepad).x);
			case InputCaptureTarget::Stick:
				return glm::length(
					InputProcessing::ReadControl(InputControl{InputControlKind::PadStick, static_cast<uint16_t>(axis)}, gamepad));
			case InputCaptureTarget::Part:
				return InputProcessing::ReadControl(InputControl{InputControlKind::PadAxis, static_cast<uint16_t>(axis)}, gamepad).x;
		}
		return 0.0f;
	}
}
