#include "Editor/InputBindingCapture.h"

#include "Strada/Core/Events/Event.h"
#include "Strada/Core/Events/KeyEvent.h"
#include "Strada/Core/Events/MouseEvent.h"
#include "Strada/Core/KeyCodes.h"

#include <cstring>
#include <utility>

namespace Strada
{
	namespace
	{
		constexpr std::string_view StickNote = "A Vector2 binding taken whole is a stick: move one, or bind keys to its four parts.";

		// Whether ParseInputControl reads the key's name back (Unknown and codes without a name bind nothing).
		bool IsNamedKey(KeyCode key)
		{
			return key != KeyCode::Unknown && std::strcmp(KeyCodeToString(key), "Unknown") != 0;
		}
	}

	void InputBindingCapture::Start(InputBindingCaptureTarget target)
	{
		m_Target = std::move(target);
		m_Listening = true;
		m_Pending = InputBindingCaptureOutcome::Waiting;
		m_Note.clear();
		m_ClickedAway = false;
		m_HasPrompt = false;
		m_Devices.Start(m_Target.Kind, m_Target.Gamepad);
	}

	void InputBindingCapture::Cancel()
	{
		if (m_Listening)
		{
			Finish(InputBindingCaptureOutcome::Cancelled);
		}
	}

	void InputBindingCapture::SetPrompt(glm::vec2 min, glm::vec2 max, uint32_t viewport)
	{
		m_PromptMin = min;
		m_PromptMax = max;
		m_PromptViewport = viewport;
		m_HasPrompt = true;
	}

	bool InputBindingCapture::TakeEvent(Event const& event, ImGuiLayer::InputContext const& context)
	{
		if (!m_Listening)
		{
			return false;
		}
		bool const takesButtons = m_Target.Kind != InputCaptureTarget::Stick;
		switch (event.GetEventType())
		{
			case EventType::KeyPressed:
			{
				auto const& press = static_cast<KeyPressedEvent const&>(event);
				if (press.IsRepeat())
				{
					return true;
				}
				KeyCode const key = press.GetKeyCode();
				if (key == KeyCode::Escape)
				{
					Finish(InputBindingCaptureOutcome::Cancelled);
				}
				else if (!takesButtons)
				{
					m_Note = StickNote;
				}
				else if (IsNamedKey(key))
				{
					m_Control = InputControl{InputControlKind::Key, static_cast<uint16_t>(key)};
					Finish(InputBindingCaptureOutcome::Captured);
				}
				return true;
			}
			case EventType::KeyTyped:
				// Typing reaches no text field while a key is being bound.
				return true;
			case EventType::MouseButtonPressed:
			{
				bool const overPrompt = m_HasPrompt && context.Viewport == m_PromptViewport && context.Cursor.x >= m_PromptMin.x &&
				                        context.Cursor.y >= m_PromptMin.y && context.Cursor.x < m_PromptMax.x &&
				                        context.Cursor.y < m_PromptMax.y;
				if (!overPrompt)
				{
					// The click is for something else: the capture ends and the click goes on.
					Finish(InputBindingCaptureOutcome::Cancelled);
					m_ClickedAway = true;
					return false;
				}
				if (!takesButtons)
				{
					m_Note = StickNote;
					return true;
				}
				MouseButton const button = static_cast<MouseButtonPressedEvent const&>(event).GetMouseButton();
				m_Control = InputControl{InputControlKind::Mouse, static_cast<uint16_t>(button)};
				Finish(InputBindingCaptureOutcome::Captured);
				return true;
			}
			default:
				return false;
		}
	}

	InputBindingCaptureOutcome InputBindingCapture::Update(float seconds)
	{
		if (m_Pending != InputBindingCaptureOutcome::Waiting)
		{
			return std::exchange(m_Pending, InputBindingCaptureOutcome::Waiting);
		}
		if (!m_Listening)
		{
			return InputBindingCaptureOutcome::Waiting;
		}
		// Mouse buttons in Input are the game's (the windows' presses over the prompt come through TakeEvent).
		switch (m_Devices.Update(seconds, false))
		{
			case InputCaptureOutcome::Waiting:
				return InputBindingCaptureOutcome::Waiting;
			case InputCaptureOutcome::Captured:
				m_Control = m_Devices.GetControl();
				m_Listening = false;
				return InputBindingCaptureOutcome::Captured;
			case InputCaptureOutcome::Escaped:
				m_Listening = false;
				return InputBindingCaptureOutcome::Cancelled;
			case InputCaptureOutcome::TimedOut:
				m_Listening = false;
				return InputBindingCaptureOutcome::TimedOut;
		}
		return InputBindingCaptureOutcome::Waiting;
	}

	void InputBindingCapture::Finish(InputBindingCaptureOutcome outcome)
	{
		m_Listening = false;
		m_Devices.Stop();
		m_HasPrompt = false;
		m_Pending = outcome;
	}

	Result<std::string> ReplaceInputBindingControl(std::string_view controls, int32_t part, InputControl control)
	{
		if (part < 0)
		{
			return FormatInputControl(control);
		}
		Result<InputControlList> parsed = ParseInputControls(controls);
		if (!parsed)
		{
			return Error{parsed.GetError()};
		}
		InputControlList list = parsed.GetValue();
		if (list.Count < 2 || static_cast<size_t>(part) >= list.Count)
		{
			return MakeError("'{}' has no part {}", controls, part);
		}
		if (control.Kind == InputControlKind::PadStick)
		{
			return MakeError("a part takes one control that is not a stick, not '{}'", FormatInputControl(control));
		}
		list.Controls[static_cast<size_t>(part)] = control;
		return FormatInputControls(list);
	}

	InputCaptureTarget GetInputBindingCaptureKind(InputActionType type, int32_t part)
	{
		if (part >= 0)
		{
			return InputCaptureTarget::Part;
		}
		return type == InputActionType::Vector2 ? InputCaptureTarget::Stick : InputCaptureTarget::Control;
	}
}
