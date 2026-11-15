#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Window.h"

#include <glm/glm.hpp>

struct GLFWwindow;

namespace Strada
{
	// Internal to Window: the frame of a window whose title bar the application draws (WindowSpecification::CustomTitleBar).
	//
	// On Windows the window keeps the style of a window with a title bar, so the system still gives it resize borders, a
	// shadow, snapping, the window menu and the animations of minimizing and maximizing, and the frame answers the messages
	// that say where the title bar is: the client area covers it, and the positions the application names (TitleBarArea)
	// are the caption and its buttons. Other platforms have no such frame (IsSupported). Main thread only.
	class WindowFrame
	{
	public:
		// Whether windows can have this frame on the platform.
		static bool IsSupported();
		// Gives a window (hidden, just created, with the system's title bar) the frame, keeping the size of its client area.
		// Null where unsupported or when the window has no native handle.
		[[nodiscard]] static Scope<WindowFrame> Install(GLFWwindow* window);
		// The window's default frame in the system's dark or light colors. Does nothing where unsupported.
		static void SetDarkColors(GLFWwindow* window, bool dark);

		// Before the window is destroyed.
		~WindowFrame();

		WindowFrame(WindowFrame const&) = delete;
		WindowFrame& operator=(WindowFrame const&) = delete;

		void SetHitTest(Window::TitleBarHitTestFn hitTest);
		TitleBarArea GetHoveredButton() const;
		TitleBarArea GetPressedButton() const;

		// Sizes, or moves and sizes, the window by its client area (screen coordinates), as GLFW does for windows with the
		// system's title bar: GLFW's own functions take the title bar this window lacks into account.
		void SetClientSize(glm::ivec2 size);
		void SetClientArea(glm::ivec2 position, glm::ivec2 size);

		struct State;

	private:
		WindowFrame();

		Scope<State> m_State;
	};
}
