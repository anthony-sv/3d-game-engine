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
	class WindowFrame;

	// The parts of a title bar an application draws itself (WindowSpecification::CustomTitleBar), as it names them to the
	// window.
	enum class TitleBarArea : uint8_t
	{
		// Not the title bar, or a part of it the application handles itself (menus, fields).
		Client,
		// Empty title bar: dragging it moves the window, a double click maximizes or restores it, a right click opens the
		// system's window menu.
		Caption,
		// The buttons. The window presses them itself: the application draws them (Window::GetHoveredTitleBarButton).
		Minimize,
		Maximize,
		Close
	};

	struct WindowSpecification
	{
		std::string Title = "Strada";
		// Initial size in screen coordinates.
		uint32_t Width = 1600;
		uint32_t Height = 900;
		bool Fullscreen = false;
		bool Resizable = true;
		bool Decorated = true;
		// The application draws the title bar: the client area covers the whole window, and the application tells the window
		// which part of it is what (Window::SetTitleBarHitTest). The window keeps the system's frame, with its resize borders,
		// shadow, snapping and animations. Windows only: elsewhere, and for a window that is not decorated, the window keeps
		// the system's title bar (Window::HasCustomTitleBar tells).
		bool CustomTitleBar = false;
		// The frame the system draws (the border, and the title bar where the window has the system's) in the system's dark
		// colors. Windows only.
		bool DarkFrame = false;
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
		// What is at a position of the client area (pixels from its top-left corner).
		using TitleBarHitTestFn = std::function<TitleBarArea(glm::ivec2 position)>;

		[[nodiscard]] static Result<Scope<Window>> Create(WindowSpecification const& specification);
		~Window();

		Window(Window const&) = delete;
		Window& operator=(Window const&) = delete;

		// Processes pending OS events for all windows and polls gamepads into Input.
		static void PollEvents();
		// Like PollEvents, but sleeps until an event arrives or the timeout elapses.
		static void WaitEvents(double timeoutSeconds);

		void SetEventCallback(EventCallbackFn callback);
		// Files dropped onto another GLFW window of the application (the platform window of an ImGui panel dragged out of
		// this one) reach this window's event callback as WindowFileDropEvents. Takes over the other window's user pointer.
		void ForwardFileDrops(GLFWwindow* window);

		// Framebuffer size in pixels (what the swapchain uses). Zero while minimized.
		uint32_t GetWidth() const { return m_Data.FramebufferWidth; }
		uint32_t GetHeight() const { return m_Data.FramebufferHeight; }
		// Window size in screen coordinates (differs from the framebuffer size on high-DPI displays).
		glm::uvec2 GetWindowSize() const;
		// The cursor's position from the window's top-left corner in screen coordinates (also outside the window).
		glm::vec2 GetCursorPosition() const;
		glm::vec2 GetContentScale() const;

		bool IsMinimized() const { return m_Data.Minimized; }
		bool IsMaximized() const;
		bool IsFocused() const;

		// Whether the application draws the title bar (WindowSpecification::CustomTitleBar, where the platform has it).
		bool HasCustomTitleBar() const { return m_Frame != nullptr; }
		// Where the title bar the application draws is. The window asks while events are processed, also between frames and
		// while the user moves or resizes it, so the answer comes from what the last frame drew. Without a hit test the
		// window has no title bar at all. Ignored without a custom title bar.
		void SetTitleBarHitTest(TitleBarHitTestFn hitTest);
		// The title bar button under the cursor, and the one held down with the left mouse button (released over it, the
		// window minimizes, maximizes, restores or asks to close): Minimize, Maximize or Close, else Client. The window
		// receives no other input over the buttons, so the application draws them from these.
		TitleBarArea GetHoveredTitleBarButton() const;
		TitleBarArea GetPressedTitleBarButton() const;

		std::string const& GetTitle() const { return m_Data.Title; }
		void SetTitle(std::string const& title);

		bool IsVSync() const { return m_Data.VSync; }
		void SetVSync(bool enabled) { m_Data.VSync = enabled; }

		bool IsFullscreen() const { return m_Data.Fullscreen; }
		// Borderless fullscreen on the window's current monitor, using the monitor's current video mode.
		void SetFullscreen(bool fullscreen);

		// Sets the window size in screen coordinates.
		void SetSize(uint32_t width, uint32_t height);
		// The size in screen coordinates a window created at this size has (WindowSpecification's Width and Height), on the
		// window's monitor now: GLFW scales a new window's size by its monitor's content scale where screen coordinates are
		// pixels (Windows and X11, GLFW_SCALE_TO_MONITOR), truncating as it does, and keeps it where they are not (macOS and
		// Wayland, whose framebuffer scales instead). SetSize of it sizes the window as one opened at that size.
		glm::uvec2 GetCreatedSize(glm::uvec2 specifiedSize) const;
		// Sets the window size so that its framebuffer is this many pixels, whatever the display's scale: the size in screen
		// coordinates is the framebuffer's scaled as the two compare now (one to one on Windows and X11, so the framebuffer
		// is exact there; on a display with points of several pixels, it is exact when the sizes divide evenly), as far as
		// the system lets the window grow: Windows keeps a window within about the desktop's size. The framebuffer's new
		// size arrives with the resize event, during the call on Windows and with the next events elsewhere; a caller that
		// needs the exact size compares GetWidth and GetHeight with it then (the player's --resolution does).
		void SetFramebufferSize(uint32_t width, uint32_t height);
		void Maximize();
		void Restore();
		void Show();
		void Focus();

		void SetCursorMode(CursorMode mode);
		// Applies a cursor mode to any GLFW window, such as the platform window ImGui creates for a panel dragged out of the
		// main window.
		static void ApplyCursorMode(GLFWwindow* window, CursorMode mode);
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
		// The frame of a window with a custom title bar; null for every other window.
		Scope<WindowFrame> m_Frame;
		// Windowed placement restored when leaving fullscreen.
		glm::ivec2 m_WindowedPosition = {0, 0};
		glm::ivec2 m_WindowedSize = {0, 0};

		static uint32_t s_WindowCount;
	};
}
