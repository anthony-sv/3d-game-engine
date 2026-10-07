#pragma once

#include "Strada/Core/Events/Event.h"
#include "Strada/Core/KeyCodes.h"

#include <spdlog/fmt/fmt.h>

#include <string>

namespace Strada
{
	class MouseMovedEvent : public Event
	{
	public:
		// Cursor position in window coordinates (screen units, origin top-left).
		MouseMovedEvent(float x, float y)
			: m_X(x),
			  m_Y(y)
		{
		}

		float GetX() const { return m_X; }
		float GetY() const { return m_Y; }

		std::string ToString() const override { return fmt::format("MouseMovedEvent: {}, {}", m_X, m_Y); }

		ST_EVENT_CLASS_TYPE(MouseMoved)
		ST_EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)

	private:
		float m_X;
		float m_Y;
	};

	class MouseScrolledEvent : public Event
	{
	public:
		MouseScrolledEvent(float xOffset, float yOffset)
			: m_XOffset(xOffset),
			  m_YOffset(yOffset)
		{
		}

		float GetXOffset() const { return m_XOffset; }
		float GetYOffset() const { return m_YOffset; }

		std::string ToString() const override { return fmt::format("MouseScrolledEvent: {}, {}", m_XOffset, m_YOffset); }

		ST_EVENT_CLASS_TYPE(MouseScrolled)
		ST_EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput)

	private:
		float m_XOffset;
		float m_YOffset;
	};

	class MouseButtonEvent : public Event
	{
	public:
		MouseButton GetMouseButton() const { return m_Button; }

		ST_EVENT_CLASS_CATEGORY(EventCategoryMouse | EventCategoryInput | EventCategoryMouseButton)

	protected:
		explicit MouseButtonEvent(MouseButton button)
			: m_Button(button)
		{
		}

		MouseButton m_Button;
	};

	class MouseButtonPressedEvent : public MouseButtonEvent
	{
	public:
		explicit MouseButtonPressedEvent(MouseButton button)
			: MouseButtonEvent(button)
		{
		}

		std::string ToString() const override { return fmt::format("MouseButtonPressedEvent: {}", MouseButtonToString(m_Button)); }

		ST_EVENT_CLASS_TYPE(MouseButtonPressed)
	};

	class MouseButtonReleasedEvent : public MouseButtonEvent
	{
	public:
		explicit MouseButtonReleasedEvent(MouseButton button)
			: MouseButtonEvent(button)
		{
		}

		std::string ToString() const override { return fmt::format("MouseButtonReleasedEvent: {}", MouseButtonToString(m_Button)); }

		ST_EVENT_CLASS_TYPE(MouseButtonReleased)
	};
}
