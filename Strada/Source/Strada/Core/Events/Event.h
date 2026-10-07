#pragma once

#include "Strada/Core/Base.h"

#include <string>

namespace Strada
{
	enum class EventType : uint8_t
	{
		None = 0,
		WindowClose,
		WindowResize,
		WindowFocus,
		WindowLostFocus,
		WindowMoved,
		WindowMinimize,
		WindowDrop,
		KeyPressed,
		KeyReleased,
		KeyTyped,
		MouseButtonPressed,
		MouseButtonReleased,
		MouseMoved,
		MouseScrolled
	};

	enum EventCategory : uint32_t
	{
		EventCategoryNone = 0,
		EventCategoryApplication = ST_BIT(0),
		EventCategoryInput = ST_BIT(1),
		EventCategoryKeyboard = ST_BIT(2),
		EventCategoryMouse = ST_BIT(3),
		EventCategoryMouseButton = ST_BIT(4)
	};

#define ST_EVENT_CLASS_TYPE(type)           \
	static EventType GetStaticType()        \
	{                                       \
		return EventType::type;             \
	}                                       \
	EventType GetEventType() const override \
	{                                       \
		return GetStaticType();             \
	}                                       \
	char const* GetName() const override    \
	{                                       \
		return #type;                       \
	}

#define ST_EVENT_CLASS_CATEGORY(category)      \
	uint32_t GetCategoryFlags() const override \
	{                                          \
		return category;                       \
	}

	class Event
	{
	public:
		virtual ~Event() = default;

		virtual EventType GetEventType() const = 0;
		virtual char const* GetName() const = 0;
		virtual uint32_t GetCategoryFlags() const = 0;
		virtual std::string ToString() const { return GetName(); }

		bool IsInCategory(EventCategory category) const { return (GetCategoryFlags() & category) != 0; }

		// Set by a handler to stop propagation to lower layers.
		bool Handled = false;
	};

	class EventDispatcher
	{
	public:
		explicit EventDispatcher(Event& event)
			: m_Event(event)
		{
		}

		// Calls func(T&) if the event is of type T. Returns true if the event matched.
		template<typename T, typename F>
		bool Dispatch(F const& func)
		{
			if (m_Event.GetEventType() == T::GetStaticType())
			{
				m_Event.Handled |= func(static_cast<T&>(m_Event));
				return true;
			}
			return false;
		}

	private:
		Event& m_Event;
	};
}
