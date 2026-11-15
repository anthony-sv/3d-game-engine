#include "stpch.h"
#include "Strada/Core/WindowFrame.h"

#include <algorithm>
#include <utility>

#if defined(ST_PLATFORM_WINDOWS)
// windowsx.h stays out: its IsMinimized and IsMaximized macros would rename Window's functions.
#include <Windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_NATIVE_INCLUDE_NONE
#include <GLFW/glfw3native.h>
#endif

namespace Strada
{
	struct WindowFrame::State
	{
		Window::TitleBarHitTestFn HitTest;
		TitleBarArea Hovered = TitleBarArea::Client;
		TitleBarArea Pressed = TitleBarArea::Client;
#if defined(ST_PLATFORM_WINDOWS)
		HWND Handle = nullptr;
		// Whether the window is to be told when the cursor leaves its frame (asked for again after every time).
		bool TracksLeave = false;
#endif
	};

	WindowFrame::WindowFrame()
		: m_State(CreateScope<State>())
	{
	}

	void WindowFrame::SetHitTest(Window::TitleBarHitTestFn hitTest)
	{
		m_State->HitTest = std::move(hitTest);
	}

	TitleBarArea WindowFrame::GetHoveredButton() const
	{
		return m_State->Hovered;
	}

	TitleBarArea WindowFrame::GetPressedButton() const
	{
		return m_State->Pressed;
	}

#if defined(ST_PLATFORM_WINDOWS)
	namespace
	{
		// The window properties that lead the window procedure to the frame's state and to the procedure it replaced. The
		// second outlives the frame when the procedure cannot be taken out of the chain.
		constexpr wchar_t const* StateProperty = L"Strada.WindowFrame";
		constexpr wchar_t const* PreviousProperty = L"Strada.WindowFrame.Previous";
		// What a maximized window leaves free at a screen edge with a taskbar that hides itself, so that the cursor still
		// reaches the taskbar there.
		constexpr LONG AutoHideTaskbarEdge = 2;

		DWORD GetStyle(HWND handle)
		{
			return static_cast<DWORD>(GetWindowLongW(handle, GWL_STYLE));
		}

		// Borderless fullscreen takes the title bar's style away (GLFW makes the window a popup): the frame then leaves
		// every message alone.
		bool HasCaption(HWND handle)
		{
			return (GetStyle(handle) & WS_CAPTION) == WS_CAPTION;
		}

		// The width of the resize border on the window's left, which the top border in the client area matches.
		LONG GetBorderWidth(HWND handle)
		{
			RECT window = {};
			POINT origin = {};
			if (GetWindowRect(handle, &window) == 0 || ClientToScreen(handle, &origin) == 0)
			{
				return 0;
			}
			return std::max<LONG>(origin.x - window.left, 0);
		}

		// The rectangle of the window whose client area is the given one (screen coordinates): the system's frame on the
		// left, the right and the bottom, nothing above.
		RECT GetWindowRectForClient(HWND handle, RECT client)
		{
			LONG const top = client.top;
			DWORD const exStyle = static_cast<DWORD>(GetWindowLongW(handle, GWL_EXSTYLE));
			AdjustWindowRectExForDpi(&client, GetStyle(handle), FALSE, exStyle, GetDpiForWindow(handle));
			if (HasCaption(handle))
			{
				client.top = top;
			}
			return client;
		}

		// WM_NCCALCSIZE: the client area is the window without the frame's left, right and bottom borders. Its top is the
		// window's, where the title bar and the top border were.
		LRESULT CalculateClientArea(HWND handle, WNDPROC previous, WPARAM wParam, LPARAM lParam)
		{
			// The window's rectangle comes in and the client area's goes out: alone, or first in the parameters' structure.
			RECT* const area = wParam != 0 ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam)->rgrc[0] : reinterpret_cast<RECT*>(lParam);
			RECT const window = *area;
			LRESULT const result = CallWindowProcW(previous, handle, WM_NCCALCSIZE, wParam, lParam);
			if (result != 0)
			{
				return result;
			}
			area->top = window.top;
			if (IsZoomed(handle) == 0)
			{
				return 0;
			}

			// A maximized window is larger than its monitor's work area by its borders, which the default took off at the
			// sides and the bottom.
			area->top += area->left - window.left;
			// A client area that covers its monitor makes a window count as fullscreen, and the taskbar never comes out
			// over one. That is the case where the taskbar hides itself (the work area is the monitor then).
			APPBARDATA taskbar = {};
			taskbar.cbSize = sizeof(taskbar);
			MONITORINFO monitor = {};
			monitor.cbSize = sizeof(monitor);
			if ((SHAppBarMessage(ABM_GETSTATE, &taskbar) & ABS_AUTOHIDE) == 0 ||
			    GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor) == 0)
			{
				return 0;
			}
			auto const hidesAt = [&monitor](UINT edge)
			{
				APPBARDATA query = {};
				query.cbSize = sizeof(query);
				query.uEdge = edge;
				query.rc = monitor.rcMonitor;
				return SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &query) != 0;
			};
			area->left += hidesAt(ABE_LEFT) ? AutoHideTaskbarEdge : 0;
			area->top += hidesAt(ABE_TOP) ? AutoHideTaskbarEdge : 0;
			area->right -= hidesAt(ABE_RIGHT) ? AutoHideTaskbarEdge : 0;
			area->bottom -= hidesAt(ABE_BOTTOM) ? AutoHideTaskbarEdge : 0;
			return 0;
		}

		// WM_NCHITTEST: the frame's borders as the system finds them, the top border in the client area, then what the
		// application says of its title bar.
		LRESULT FindArea(WindowFrame::State const& state, HWND handle, WNDPROC previous, WPARAM wParam, LPARAM lParam)
		{
			LRESULT const frame = CallWindowProcW(previous, handle, WM_NCHITTEST, wParam, lParam);
			if (frame != HTCLIENT)
			{
				return frame;
			}
			// Screen coordinates are signed: monitors lie left of and above the primary one.
			POINT point = {static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
			RECT client = {};
			if (ScreenToClient(handle, &point) == 0 || GetClientRect(handle, &client) == 0)
			{
				return frame;
			}
			DWORD const style = GetStyle(handle);
			LONG const border = GetBorderWidth(handle);
			// A maximized window has no borders to resize it by.
			if ((style & WS_THICKFRAME) != 0 && IsZoomed(handle) == 0 && point.y < border)
			{
				// The corners reach as far along the top as the system's do along the sides.
				LONG const corner = border * 2;
				return point.x < corner ? HTTOPLEFT : (point.x >= client.right - corner ? HTTOPRIGHT : HTTOP);
			}
			switch (state.HitTest ? state.HitTest(glm::ivec2(point.x, point.y)) : TitleBarArea::Client)
			{
				case TitleBarArea::Caption:
					return HTCAPTION;
				case TitleBarArea::Minimize:
					return (style & WS_MINIMIZEBOX) != 0 ? HTMINBUTTON : HTCAPTION;
				case TitleBarArea::Maximize:
					// The code Windows 11 shows its snap layouts for.
					return (style & WS_MAXIMIZEBOX) != 0 ? HTMAXBUTTON : HTCAPTION;
				case TitleBarArea::Close:
					return HTCLOSE;
				case TitleBarArea::Client:
					break;
			}
			return HTCLIENT;
		}

		TitleBarArea ToButton(WPARAM area)
		{
			switch (area)
			{
				case HTMINBUTTON:
					return TitleBarArea::Minimize;
				case HTMAXBUTTON:
					return TitleBarArea::Maximize;
				case HTCLOSE:
					return TitleBarArea::Close;
				default:
					return TitleBarArea::Client;
			}
		}

		void HoverButton(WindowFrame::State& state, TitleBarArea button)
		{
			if (button != state.Hovered)
			{
				// A press does not follow the cursor to another button.
				state.Hovered = button;
				state.Pressed = TitleBarArea::Client;
			}
			if (button != TitleBarArea::Client && !state.TracksLeave)
			{
				TRACKMOUSEEVENT track = {};
				track.cbSize = sizeof(track);
				track.dwFlags = TME_LEAVE | TME_NONCLIENT;
				track.hwndTrack = state.Handle;
				state.TracksLeave = TrackMouseEvent(&track) != 0;
			}
		}

		// What the system's buttons do, through the system: the window menu's commands, with their animations, and a close
		// request the application may refuse.
		void PressButton(HWND handle, TitleBarArea button)
		{
			WPARAM command = SC_CLOSE;
			if (button == TitleBarArea::Minimize)
			{
				command = SC_MINIMIZE;
			}
			else if (button == TitleBarArea::Maximize)
			{
				command = IsZoomed(handle) != 0 ? SC_RESTORE : SC_MAXIMIZE;
			}
			PostMessageW(handle, WM_SYSCOMMAND, command, 0);
		}

		LRESULT CALLBACK FrameProcedure(HWND handle, UINT message, WPARAM wParam, LPARAM lParam)
		{
			WNDPROC const previous = reinterpret_cast<WNDPROC>(GetPropW(handle, PreviousProperty));
			if (previous == nullptr)
			{
				return DefWindowProcW(handle, message, wParam, lParam);
			}
			WindowFrame::State* const state = static_cast<WindowFrame::State*>(GetPropW(handle, StateProperty));
			if (state == nullptr || !HasCaption(handle))
			{
				return CallWindowProcW(previous, handle, message, wParam, lParam);
			}

			switch (message)
			{
				case WM_NCCALCSIZE:
					return CalculateClientArea(handle, previous, wParam, lParam);
				case WM_NCHITTEST:
					return FindArea(*state, handle, previous, wParam, lParam);
				case WM_NCMOUSEMOVE:
					HoverButton(*state, ToButton(wParam));
					break;
				case WM_NCMOUSELEAVE:
					state->TracksLeave = false;
					state->Hovered = TitleBarArea::Client;
					state->Pressed = TitleBarArea::Client;
					break;
				case WM_MOUSEMOVE:
					// In the client area again, also when the leave was not tracked.
					state->Hovered = TitleBarArea::Client;
					state->Pressed = TitleBarArea::Client;
					break;
				// The buttons are the application's drawing: the system's own handling of a press on them would track the
				// mouse in a loop of its own and paint its classic buttons over the window.
				case WM_NCLBUTTONDOWN:
				case WM_NCLBUTTONDBLCLK:
					if (TitleBarArea const button = ToButton(wParam); button != TitleBarArea::Client)
					{
						HoverButton(*state, button);
						state->Pressed = button;
						return 0;
					}
					break;
				case WM_NCLBUTTONUP:
					if (TitleBarArea const button = ToButton(wParam); button != TitleBarArea::Client)
					{
						bool const pressed = state->Pressed == button;
						state->Pressed = TitleBarArea::Client;
						if (pressed)
						{
							PressButton(handle, button);
						}
						return 0;
					}
					state->Pressed = TitleBarArea::Client;
					break;
				default:
					break;
			}
			return CallWindowProcW(previous, handle, message, wParam, lParam);
		}

		constexpr UINT KeepPlacementFlags = SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE;
	}

	bool WindowFrame::IsSupported()
	{
		return true;
	}

	Scope<WindowFrame> WindowFrame::Install(GLFWwindow* window)
	{
		HWND const handle = window != nullptr ? glfwGetWin32Window(window) : nullptr;
		if (handle == nullptr)
		{
			return nullptr;
		}
		Scope<WindowFrame> frame(new WindowFrame());
		frame->m_State->Handle = handle;

		RECT before = {};
		GetClientRect(handle, &before);
		WNDPROC const previous = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(handle, GWLP_WNDPROC));
		if (previous == nullptr || SetPropW(handle, PreviousProperty, reinterpret_cast<HANDLE>(previous)) == 0 ||
		    SetPropW(handle, StateProperty, frame->m_State.get()) == 0)
		{
			RemovePropW(handle, PreviousProperty);
			frame->m_State->Handle = nullptr;
			return nullptr;
		}
		SetWindowLongPtrW(handle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&FrameProcedure));

		// The system calculates the frame again: the client area takes the title bar's place. The window then gives up what
		// its client area gained, and has the client area it was created with. A maximized window keeps its place; restored,
		// it is taller than asked for by the title bar it never had.
		SetWindowPos(handle, nullptr, 0, 0, 0, 0, KeepPlacementFlags | SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
		RECT after = {};
		RECT bounds = {};
		if (IsZoomed(handle) == 0 && GetClientRect(handle, &after) != 0 && GetWindowRect(handle, &bounds) != 0 &&
		    after.bottom > before.bottom)
		{
			LONG const height = bounds.bottom - bounds.top - (after.bottom - before.bottom);
			SetWindowPos(handle, nullptr, 0, 0, bounds.right - bounds.left, height, KeepPlacementFlags | SWP_NOMOVE);
		}
		return frame;
	}

	void WindowFrame::SetDarkColors(GLFWwindow* window, bool dark)
	{
		HWND const handle = window != nullptr ? glfwGetWin32Window(window) : nullptr;
		if (handle == nullptr)
		{
			return;
		}
		// Refused by the versions of Windows 10 that have no dark frames: the window keeps the light one there.
		BOOL const value = dark ? TRUE : FALSE;
		(void)DwmSetWindowAttribute(handle, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof(value));
	}

	WindowFrame::~WindowFrame()
	{
		HWND const handle = m_State->Handle;
		if (handle == nullptr)
		{
			return;
		}
		RemovePropW(handle, StateProperty);
		if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(handle, GWLP_WNDPROC)) == &FrameProcedure)
		{
			WNDPROC const previous = reinterpret_cast<WNDPROC>(GetPropW(handle, PreviousProperty));
			SetWindowLongPtrW(handle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
			RemovePropW(handle, PreviousProperty);
		}
		else
		{
			// A procedure installed after this one is still there (the ImGui layer's goes before the window does): this one
			// stays under it and passes every message on.
			ST_CORE_WARN("The window procedure of a window with a custom title bar was replaced and not put back");
		}
	}

	void WindowFrame::SetClientSize(glm::ivec2 size)
	{
		HWND const handle = m_State->Handle;
		RECT const bounds = GetWindowRectForClient(handle, RECT{0, 0, size.x, size.y});
		SetWindowPos(handle, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top, KeepPlacementFlags | SWP_NOMOVE);
	}

	void WindowFrame::SetClientArea(glm::ivec2 position, glm::ivec2 size)
	{
		HWND const handle = m_State->Handle;
		RECT const bounds = GetWindowRectForClient(handle, RECT{position.x, position.y, position.x + size.x, position.y + size.y});
		SetWindowPos(handle, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, KeepPlacementFlags);
	}
#else
	bool WindowFrame::IsSupported()
	{
		return false;
	}

	Scope<WindowFrame> WindowFrame::Install(GLFWwindow*)
	{
		return nullptr;
	}

	void WindowFrame::SetDarkColors(GLFWwindow*, bool)
	{
	}

	WindowFrame::~WindowFrame() = default;

	void WindowFrame::SetClientSize(glm::ivec2)
	{
	}

	void WindowFrame::SetClientArea(glm::ivec2, glm::ivec2)
	{
	}
#endif
}
