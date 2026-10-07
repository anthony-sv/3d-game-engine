#include "stpch.h"
#include "Strada/Core/Window.h"

#include "Strada/Core/Events/ApplicationEvent.h"
#include "Strada/Core/Events/KeyEvent.h"
#include "Strada/Core/Events/MouseEvent.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Input.h"
#include "Strada/Core/Platform.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <vector>

namespace Strada
{
	uint32_t Window::s_WindowCount = 0;

	namespace
	{
		void GLFWErrorCallback(int error, char const* description)
		{
			ST_CORE_ERROR("GLFW error {}: {}", error, description);
		}

		int SelectPlatformHint()
		{
			// STRADA_GLFW_PLATFORM=x11|wayland forces a backend on Linux; the default lets GLFW choose.
			std::optional<std::string> const requested = Platform::ReadEnvironmentVariable("STRADA_GLFW_PLATFORM");
			if (requested)
			{
				if (*requested == "x11")
				{
					return GLFW_PLATFORM_X11;
				}
				if (*requested == "wayland")
				{
					return GLFW_PLATFORM_WAYLAND;
				}
				ST_CORE_WARN("Ignoring unknown STRADA_GLFW_PLATFORM value '{}' (expected 'x11' or 'wayland').", *requested);
			}
			return GLFW_ANY_PLATFORM;
		}

		Result<void> InitializeGLFW()
		{
			glfwSetErrorCallback(GLFWErrorCallback);
			glfwInitHint(GLFW_PLATFORM, SelectPlatformHint());
			if (glfwInit() != GLFW_TRUE)
			{
				return Error{"Failed to initialize GLFW"};
			}
			if (glfwVulkanSupported() != GLFW_TRUE)
			{
				glfwTerminate();
				return Error{"GLFW reports that Vulkan is not supported on this system (no Vulkan loader or driver found)"};
			}
			return {};
		}

		Window::WindowData& GetData(GLFWwindow* window)
		{
			return *static_cast<Window::WindowData*>(glfwGetWindowUserPointer(window));
		}

		void Dispatch(GLFWwindow* window, Event& event)
		{
			Window::WindowData const& data = GetData(window);
			if (data.EventCallback)
			{
				data.EventCallback(event);
			}
		}

		void PollGamepads()
		{
			for (int joystick = GLFW_JOYSTICK_1; joystick <= GLFW_JOYSTICK_LAST && joystick < MaxGamepads; joystick++)
			{
				Input::GamepadState state;
				GLFWgamepadstate glfwState;
				if (glfwJoystickIsGamepad(joystick) == GLFW_TRUE && glfwGetGamepadState(joystick, &glfwState) == GLFW_TRUE)
				{
					state.Connected = true;
					for (size_t axis = 0; axis < state.Axes.size(); axis++)
					{
						state.Axes[axis] = glfwState.axes[axis];
					}
					for (size_t button = 0; button < state.Buttons.size(); button++)
					{
						state.Buttons[button] = glfwState.buttons[button] == GLFW_PRESS;
					}
				}
				Input::SetGamepadState(static_cast<uint32_t>(joystick), state);
			}
		}
	}

	Result<Scope<Window>> Window::Create(WindowSpecification const& specification)
	{
		Scope<Window> window(new Window(specification));
		if (Result<void> result = window->Init(); !result)
		{
			return Error{result.GetError()};
		}
		return window;
	}

	Window::Window(WindowSpecification const& specification)
		: m_Specification(specification)
	{
		m_Data.Title = specification.Title;
		m_Data.VSync = specification.VSync;
	}

	Result<void> Window::Init()
	{
		if (s_WindowCount == 0)
		{
			if (Result<void> result = InitializeGLFW(); !result)
			{
				return result;
			}
		}

		glfwDefaultWindowHints();
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_RESIZABLE, m_Specification.Resizable ? GLFW_TRUE : GLFW_FALSE);
		glfwWindowHint(GLFW_DECORATED, m_Specification.Decorated ? GLFW_TRUE : GLFW_FALSE);
		glfwWindowHint(GLFW_MAXIMIZED, m_Specification.Maximized ? GLFW_TRUE : GLFW_FALSE);
		glfwWindowHint(GLFW_VISIBLE, m_Specification.Visible ? GLFW_TRUE : GLFW_FALSE);
		glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

		int const width = static_cast<int>(std::max(1u, m_Specification.Width));
		int const height = static_cast<int>(std::max(1u, m_Specification.Height));
		m_Window = glfwCreateWindow(width, height, m_Data.Title.c_str(), nullptr, nullptr);
		if (m_Window == nullptr)
		{
			if (s_WindowCount == 0)
			{
				glfwTerminate();
			}
			return MakeError("Failed to create a {}x{} window", width, height);
		}
		s_WindowCount++;

		int framebufferWidth = 0;
		int framebufferHeight = 0;
		glfwGetFramebufferSize(m_Window, &framebufferWidth, &framebufferHeight);
		m_Data.FramebufferWidth = static_cast<uint32_t>(std::max(0, framebufferWidth));
		m_Data.FramebufferHeight = static_cast<uint32_t>(std::max(0, framebufferHeight));

		glfwSetWindowUserPointer(m_Window, &m_Data);

		glfwSetFramebufferSizeCallback(m_Window,
		                               [](GLFWwindow* window, int newWidth, int newHeight)
		                               {
										   WindowData& data = GetData(window);
										   data.FramebufferWidth = static_cast<uint32_t>(std::max(0, newWidth));
										   data.FramebufferHeight = static_cast<uint32_t>(std::max(0, newHeight));
										   WindowResizeEvent event(data.FramebufferWidth, data.FramebufferHeight);
										   Dispatch(window, event);
									   });

		glfwSetWindowCloseCallback(m_Window,
		                           [](GLFWwindow* window)
		                           {
									   WindowCloseEvent event;
									   Dispatch(window, event);
								   });

		glfwSetWindowFocusCallback(m_Window,
		                           [](GLFWwindow* window, int focused)
		                           {
									   if (focused == GLFW_TRUE)
									   {
										   WindowFocusEvent event;
										   Dispatch(window, event);
									   }
									   else
									   {
										   WindowLostFocusEvent event;
										   Dispatch(window, event);
									   }
								   });

		glfwSetWindowPosCallback(m_Window,
		                         [](GLFWwindow* window, int x, int y)
		                         {
									 WindowMovedEvent event(x, y);
									 Dispatch(window, event);
								 });

		glfwSetWindowIconifyCallback(m_Window,
		                             [](GLFWwindow* window, int iconified)
		                             {
										 WindowData& data = GetData(window);
										 data.Minimized = iconified == GLFW_TRUE;
										 WindowMinimizeEvent event(data.Minimized);
										 Dispatch(window, event);
									 });

		glfwSetDropCallback(m_Window,
		                    [](GLFWwindow* window, int count, char const** paths)
		                    {
								std::vector<std::filesystem::path> droppedPaths;
								droppedPaths.reserve(static_cast<size_t>(std::max(0, count)));
								for (int i = 0; i < count; i++)
								{
									droppedPaths.push_back(FileSystem::PathFromUtf8(paths[i]));
								}
								WindowDropEvent event(std::move(droppedPaths));
								Dispatch(window, event);
							});

		glfwSetKeyCallback(m_Window,
		                   [](GLFWwindow* window, int key, int scancode, int action, int mods)
		                   {
							   (void)scancode;
							   (void)mods;
							   if (key < 0 || key >= KeyCodeCount)
							   {
								   return;
							   }
							   KeyCode const keyCode = static_cast<KeyCode>(key);
							   if (action == GLFW_PRESS || action == GLFW_REPEAT)
							   {
								   KeyPressedEvent event(keyCode, action == GLFW_REPEAT);
								   Dispatch(window, event);
							   }
							   else if (action == GLFW_RELEASE)
							   {
								   KeyReleasedEvent event(keyCode);
								   Dispatch(window, event);
							   }
						   });

		glfwSetCharCallback(m_Window,
		                    [](GLFWwindow* window, unsigned int codepoint)
		                    {
								KeyTypedEvent event(codepoint);
								Dispatch(window, event);
							});

		glfwSetMouseButtonCallback(m_Window,
		                           [](GLFWwindow* window, int button, int action, int mods)
		                           {
									   (void)mods;
									   if (button < 0 || button >= MouseButtonCount)
									   {
										   return;
									   }
									   MouseButton const mouseButton = static_cast<MouseButton>(button);
									   if (action == GLFW_PRESS)
									   {
										   MouseButtonPressedEvent event(mouseButton);
										   Dispatch(window, event);
									   }
									   else if (action == GLFW_RELEASE)
									   {
										   MouseButtonReleasedEvent event(mouseButton);
										   Dispatch(window, event);
									   }
								   });

		glfwSetScrollCallback(m_Window,
		                      [](GLFWwindow* window, double xOffset, double yOffset)
		                      {
								  MouseScrolledEvent event(static_cast<float>(xOffset), static_cast<float>(yOffset));
								  Dispatch(window, event);
							  });

		glfwSetCursorPosCallback(m_Window,
		                         [](GLFWwindow* window, double x, double y)
		                         {
									 MouseMovedEvent event(static_cast<float>(x), static_cast<float>(y));
									 Dispatch(window, event);
								 });

		if (m_Specification.Fullscreen)
		{
			SetFullscreen(true);
		}

		ST_CORE_INFO("Created window '{}' ({}x{} framebuffer)", m_Data.Title, m_Data.FramebufferWidth, m_Data.FramebufferHeight);
		return {};
	}

	Window::~Window()
	{
		if (m_Window == nullptr)
		{
			return;
		}

		glfwDestroyWindow(m_Window);
		m_Window = nullptr;
		s_WindowCount--;
		if (s_WindowCount == 0)
		{
			glfwTerminate();
		}
	}

	void Window::PollEvents()
	{
		glfwPollEvents();
		PollGamepads();
	}

	void Window::WaitEvents(double timeoutSeconds)
	{
		glfwWaitEventsTimeout(timeoutSeconds);
		PollGamepads();
	}

	void Window::SetEventCallback(EventCallbackFn callback)
	{
		m_Data.EventCallback = std::move(callback);
	}

	glm::uvec2 Window::GetWindowSize() const
	{
		int width = 0;
		int height = 0;
		glfwGetWindowSize(m_Window, &width, &height);
		return {static_cast<uint32_t>(std::max(0, width)), static_cast<uint32_t>(std::max(0, height))};
	}

	glm::vec2 Window::GetContentScale() const
	{
		float x = 1.0f;
		float y = 1.0f;
		glfwGetWindowContentScale(m_Window, &x, &y);
		return {x, y};
	}

	bool Window::IsFocused() const
	{
		return glfwGetWindowAttrib(m_Window, GLFW_FOCUSED) == GLFW_TRUE;
	}

	void Window::SetTitle(std::string const& title)
	{
		m_Data.Title = title;
		glfwSetWindowTitle(m_Window, title.c_str());
	}

	void Window::SetFullscreen(bool fullscreen)
	{
		if (fullscreen == m_Data.Fullscreen)
		{
			return;
		}

		if (fullscreen)
		{
			glfwGetWindowPos(m_Window, &m_WindowedPosition.x, &m_WindowedPosition.y);
			glfwGetWindowSize(m_Window, &m_WindowedSize.x, &m_WindowedSize.y);

			// Use the monitor that contains most of the window, falling back to the primary monitor.
			GLFWmonitor* targetMonitor = glfwGetPrimaryMonitor();
			int monitorCount = 0;
			GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
			int bestOverlap = 0;
			for (int i = 0; i < monitorCount; i++)
			{
				GLFWvidmode const* mode = glfwGetVideoMode(monitors[i]);
				if (mode == nullptr)
				{
					continue;
				}
				int monitorX = 0;
				int monitorY = 0;
				glfwGetMonitorPos(monitors[i], &monitorX, &monitorY);
				int const overlapWidth = std::max(0, std::min(m_WindowedPosition.x + m_WindowedSize.x, monitorX + mode->width) -
				                                         std::max(m_WindowedPosition.x, monitorX));
				int const overlapHeight = std::max(0, std::min(m_WindowedPosition.y + m_WindowedSize.y, monitorY + mode->height) -
				                                          std::max(m_WindowedPosition.y, monitorY));
				if (overlapWidth * overlapHeight > bestOverlap)
				{
					bestOverlap = overlapWidth * overlapHeight;
					targetMonitor = monitors[i];
				}
			}

			if (targetMonitor == nullptr)
			{
				ST_CORE_WARN("Cannot enter fullscreen: no monitor available");
				return;
			}
			GLFWvidmode const* mode = glfwGetVideoMode(targetMonitor);
			if (mode == nullptr)
			{
				ST_CORE_WARN("Cannot enter fullscreen: monitor has no video mode");
				return;
			}
			glfwSetWindowMonitor(m_Window, targetMonitor, 0, 0, mode->width, mode->height, mode->refreshRate);
		}
		else
		{
			glfwSetWindowMonitor(m_Window, nullptr, m_WindowedPosition.x, m_WindowedPosition.y, std::max(1, m_WindowedSize.x),
			                     std::max(1, m_WindowedSize.y), GLFW_DONT_CARE);
		}
		m_Data.Fullscreen = fullscreen;
	}

	void Window::SetSize(uint32_t width, uint32_t height)
	{
		glfwSetWindowSize(m_Window, static_cast<int>(std::max(1u, width)), static_cast<int>(std::max(1u, height)));
	}

	void Window::Maximize()
	{
		glfwMaximizeWindow(m_Window);
	}

	void Window::Restore()
	{
		glfwRestoreWindow(m_Window);
	}

	void Window::Show()
	{
		glfwShowWindow(m_Window);
	}

	void Window::Focus()
	{
		glfwFocusWindow(m_Window);
	}

	void Window::SetCursorMode(CursorMode mode)
	{
		int glfwMode = GLFW_CURSOR_NORMAL;
		switch (mode)
		{
			case CursorMode::Normal:
				glfwMode = GLFW_CURSOR_NORMAL;
				break;
			case CursorMode::Hidden:
				glfwMode = GLFW_CURSOR_HIDDEN;
				break;
			case CursorMode::Locked:
				glfwMode = GLFW_CURSOR_DISABLED;
				break;
		}
		glfwSetInputMode(m_Window, GLFW_CURSOR, glfwMode);

		if (mode == CursorMode::Locked && glfwRawMouseMotionSupported() == GLFW_TRUE)
		{
			glfwSetInputMode(m_Window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
		}
	}

	void Window::SetIcon(uint32_t width, uint32_t height, uint8_t const* rgbaPixels)
	{
		int const platform = glfwGetPlatform();
		if (platform == GLFW_PLATFORM_COCOA || platform == GLFW_PLATFORM_WAYLAND || rgbaPixels == nullptr)
		{
			return;
		}

		GLFWimage image;
		image.width = static_cast<int>(width);
		image.height = static_cast<int>(height);
		image.pixels = const_cast<unsigned char*>(rgbaPixels);
		glfwSetWindowIcon(m_Window, 1, &image);
	}

	bool Window::IsWayland()
	{
		return s_WindowCount > 0 && glfwGetPlatform() == GLFW_PLATFORM_WAYLAND;
	}
}
