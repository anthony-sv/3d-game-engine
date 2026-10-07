#pragma once

#include "Strada/Core/Events/Event.h"

#include <spdlog/fmt/fmt.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace Strada
{
	class WindowResizeEvent : public Event
	{
	public:
		// Framebuffer size in pixels (zero while minimized).
		WindowResizeEvent(uint32_t width, uint32_t height)
			: m_Width(width),
			  m_Height(height)
		{
		}

		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }

		std::string ToString() const override { return fmt::format("WindowResizeEvent: {}, {}", m_Width, m_Height); }

		ST_EVENT_CLASS_TYPE(WindowResize)
		ST_EVENT_CLASS_CATEGORY(EventCategoryApplication)

	private:
		uint32_t m_Width;
		uint32_t m_Height;
	};

	class WindowCloseEvent : public Event
	{
	public:
		ST_EVENT_CLASS_TYPE(WindowClose)
		ST_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class WindowFocusEvent : public Event
	{
	public:
		ST_EVENT_CLASS_TYPE(WindowFocus)
		ST_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class WindowLostFocusEvent : public Event
	{
	public:
		ST_EVENT_CLASS_TYPE(WindowLostFocus)
		ST_EVENT_CLASS_CATEGORY(EventCategoryApplication)
	};

	class WindowMovedEvent : public Event
	{
	public:
		WindowMovedEvent(int32_t x, int32_t y)
			: m_X(x),
			  m_Y(y)
		{
		}

		int32_t GetX() const { return m_X; }
		int32_t GetY() const { return m_Y; }

		std::string ToString() const override { return fmt::format("WindowMovedEvent: {}, {}", m_X, m_Y); }

		ST_EVENT_CLASS_TYPE(WindowMoved)
		ST_EVENT_CLASS_CATEGORY(EventCategoryApplication)

	private:
		int32_t m_X;
		int32_t m_Y;
	};

	class WindowMinimizeEvent : public Event
	{
	public:
		explicit WindowMinimizeEvent(bool minimized)
			: m_Minimized(minimized)
		{
		}

		bool IsMinimized() const { return m_Minimized; }

		std::string ToString() const override { return fmt::format("WindowMinimizeEvent: {}", m_Minimized); }

		ST_EVENT_CLASS_TYPE(WindowMinimize)
		ST_EVENT_CLASS_CATEGORY(EventCategoryApplication)

	private:
		bool m_Minimized;
	};

	// Files dropped onto the window from the OS.
	class WindowDropEvent : public Event
	{
	public:
		explicit WindowDropEvent(std::vector<std::filesystem::path> paths)
			: m_Paths(std::move(paths))
		{
		}

		std::vector<std::filesystem::path> const& GetPaths() const { return m_Paths; }

		std::string ToString() const override { return fmt::format("WindowDropEvent: {} path(s)", m_Paths.size()); }

		ST_EVENT_CLASS_TYPE(WindowDrop)
		ST_EVENT_CLASS_CATEGORY(EventCategoryApplication)

	private:
		std::vector<std::filesystem::path> m_Paths;
	};
}
