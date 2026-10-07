#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/KeyCodes.h"
#include "Strada/Core/Result.h"

#include <glm/glm.hpp>

#include <functional>
#include <string>

struct GLFWwindow;

namespace Strada
{
	class Event;

	struct WindowSpecification
	{
		std::string Title = "Strada";
		// Initial size in screen coordinates.
		uint32_t Width = 1600;
		uint32_t Height = 900;
		bool Fullscreen = false;
		bool Resizable = true;
		bool Decorated = true;
		bool Maximized = false;
		bool Visible = true;
		// Read by the swapchain; the window itself has no client API.
		bool VSync = true;
	};

	// OS window backed by GLFW, created without a client API (Vulkan surfaces are created by the RHI). Main thread only.
	class Window
	{
	public:
		using EventCallbackFn = std::function<void(Event&)>;

		[[nodiscard]] static Result<Scope<Window>> Create(WindowSpecification const& specification);
		~Window();

		Window(Window const&) = delete;
		Window& operator=(Window const&) = delete;

		// Processes pending OS events for all windows and polls gamepads into Input.
		static void PollEvents();
		// Like PollEvents, but sleeps until an event arrives or the timeout elapses.
		static void WaitEvents(double timeoutSeconds);

		void SetEventCallback(EventCallbackFn callback);

		// Framebuffer size in pixels (what the swapchain uses). Zero while minimized.
		uint32_t GetWidth() const { return m_Data.FramebufferWidth; }
		uint32_t GetHeight() const { return m_Data.FramebufferHeight; }
		// Window size in screen coordinates (differs from the framebuffer size on high-DPI displays).
		glm::uvec2 GetWindowSize() const;
		glm::vec2 GetContentScale() const;

		bool IsMinimized() const { return m_Data.Minimized; }
		bool IsFocused() const;

		std::string const& GetTitle() const { return m_Data.Title; }
		void SetTitle(std::string const& title);

		bool IsVSync() const { return m_Data.VSync; }
		void SetVSync(bool enabled) { m_Data.VSync = enabled; }

		bool IsFullscreen() const { return m_Data.Fullscreen; }
		// Borderless fullscreen on the window's current monitor, using the monitor's current video mode.
		void SetFullscreen(bool fullscreen);

		void Maximize();
		void Restore();
		void Show();
		void Focus();

		void SetCursorMode(CursorMode mode);
		// Sets the window icon from tightly packed RGBA8 pixels. Ignored where unsupported (macOS, Wayland).
		void SetIcon(uint32_t width, uint32_t height, uint8_t const* rgbaPixels);

		GLFWwindow* GetNativeWindow() const { return m_Window; }

		// True when GLFW runs on Wayland (ImGui multi-viewports are unavailable there).
		static bool IsWayland();

		// Internal: state shared with the GLFW callbacks through the window user pointer.
		struct WindowData
		{
			std::string Title;
			uint32_t FramebufferWidth = 0;
			uint32_t FramebufferHeight = 0;
			bool VSync = true;
			bool Fullscreen = false;
			bool Minimized = false;
			EventCallbackFn EventCallback;
		};

	private:
		explicit Window(WindowSpecification const& specification);
		Result<void> Init();

		WindowSpecification m_Specification;
		WindowData m_Data;
		GLFWwindow* m_Window = nullptr;
		// Windowed placement restored when leaving fullscreen.
		glm::ivec2 m_WindowedPosition = {0, 0};
		glm::ivec2 m_WindowedSize = {0, 0};

		static uint32_t s_WindowCount;
	};
}
