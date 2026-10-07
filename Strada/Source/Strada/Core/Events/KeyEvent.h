#pragma once

#include "Strada/Core/Events/Event.h"
#include "Strada/Core/KeyCodes.h"

#include <spdlog/fmt/fmt.h>

#include <string>

namespace Strada
{
	class KeyEvent : public Event
	{
	public:
		KeyCode GetKeyCode() const { return m_KeyCode; }

		ST_EVENT_CLASS_CATEGORY(EventCategoryKeyboard | EventCategoryInput)

	protected:
		explicit KeyEvent(KeyCode keyCode)
			: m_KeyCode(keyCode)
		{
		}

		KeyCode m_KeyCode;
	};

	class KeyPressedEvent : public KeyEvent
	{
	public:
		KeyPressedEvent(KeyCode keyCode, bool isRepeat)
			: KeyEvent(keyCode),
			  m_IsRepeat(isRepeat)
		{
		}

		bool IsRepeat() const { return m_IsRepeat; }

		std::string ToString() const override
		{
			return fmt::format("KeyPressedEvent: {} (repeat = {})", KeyCodeToString(m_KeyCode), m_IsRepeat);
		}

		ST_EVENT_CLASS_TYPE(KeyPressed)

	private:
		bool m_IsRepeat;
	};

	class KeyReleasedEvent : public KeyEvent
	{
	public:
		explicit KeyReleasedEvent(KeyCode keyCode)
			: KeyEvent(keyCode)
		{
		}

		std::string ToString() const override { return fmt::format("KeyReleasedEvent: {}", KeyCodeToString(m_KeyCode)); }

		ST_EVENT_CLASS_TYPE(KeyReleased)
	};

	// Text input: a Unicode code point produced by the keyboard layout.
	class KeyTypedEvent : public Event
	{
	public:
		explicit KeyTypedEvent(uint32_t codepoint)
			: m_Codepoint(codepoint)
		{
		}

		uint32_t GetCodepoint() const { return m_Codepoint; }

		std::string ToString() const override { return fmt::format("KeyTypedEvent: U+{:04X}", m_Codepoint); }

		ST_EVENT_CLASS_TYPE(KeyTyped)
		ST_EVENT_CLASS_CATEGORY(EventCategoryKeyboard | EventCategoryInput)

	private:
		uint32_t m_Codepoint;
	};
}
