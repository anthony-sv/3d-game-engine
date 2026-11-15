#include "GpuTestUtilities.h"

#include "Strada/Core/Events/ApplicationEvent.h"
#include "Strada/Core/Window.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

#if defined(ST_PLATFORM_WINDOWS)
#include <Windows.h>
#include <dwmapi.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_NATIVE_INCLUDE_NONE
#include <GLFW/glfw3native.h>
#endif

using namespace Strada;

namespace
{
	// A hidden window, with the title bar left to the application or the system's.
	Scope<Window> CreateTestWindow(bool customTitleBar)
	{
		WindowSpecification specification;
		specification.Title = "Strada Window Frame Test";
		specification.Width = 400;
		specification.Height = 300;
		specification.Visible = false;
		specification.CustomTitleBar = customTitleBar;
		Result<Scope<Window>> created = Window::Create(specification);
		REQUIRE_MESSAGE(created.IsOk(), (created ? std::string() : created.GetError()));
		return created.TakeValue();
	}
}

TEST_CASE("Window: a custom title bar is the window's where the platform has them, and no window has one unasked")
{
	ST_REQUIRE_DISPLAY();

	Scope<Window> const plain = CreateTestWindow(false);
	CHECK_FALSE(plain->HasCustomTitleBar());
	CHECK(plain->GetHoveredTitleBarButton() == TitleBarArea::Client);
	CHECK(plain->GetPressedTitleBarButton() == TitleBarArea::Client);
	// Nothing to tell a window with the system's title bar.
	plain->SetTitleBarHitTest(
		[](glm::ivec2)
		{
			return TitleBarArea::Caption;
		});
	CHECK_FALSE(plain->IsMaximized());

	Scope<Window> const custom = CreateTestWindow(true);
#if defined(ST_PLATFORM_WINDOWS)
	CHECK(custom->HasCustomTitleBar());
#else
	CHECK_FALSE(custom->HasCustomTitleBar());
#endif
	CHECK(custom->GetHoveredTitleBarButton() == TitleBarArea::Client);
	CHECK(custom->GetPressedTitleBarButton() == TitleBarArea::Client);
	// Either way the window has the size it was asked for: the plain window's.
	CHECK(custom->GetWindowSize() == plain->GetWindowSize());
	CHECK(custom->GetWidth() == plain->GetWidth());
	CHECK(custom->GetHeight() == plain->GetHeight());
}

TEST_CASE("Window: a window asked for a dark frame is made everywhere, and has one where the system draws dark frames")
{
	ST_REQUIRE_DISPLAY();

	for (bool const customTitleBar : {false, true})
	{
		WindowSpecification specification;
		specification.Title = "Strada Dark Frame Test";
		specification.Width = 200;
		specification.Height = 150;
		specification.Visible = false;
		specification.DarkFrame = true;
		specification.CustomTitleBar = customTitleBar;
		Result<Scope<Window>> created = Window::Create(specification);
		REQUIRE_MESSAGE(created.IsOk(), (created ? std::string() : created.GetError()));
		CHECK(created.GetValue()->GetWindowSize().x > 0);
#if defined(ST_PLATFORM_WINDOWS)
		// Versions of Windows that do not know the attribute answer with an error: nothing to check there.
		BOOL dark = FALSE;
		HWND const handle = glfwGetWin32Window(created.GetValue()->GetNativeWindow());
		if (SUCCEEDED(DwmGetWindowAttribute(handle, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark))))
		{
			CHECK(dark != FALSE);
		}
#endif
	}
}

#if defined(ST_PLATFORM_WINDOWS)
namespace
{
	// The areas a test names to its window: a bar with menus on its left, the three buttons at its right and the caption
	// between them. The bar is higher than the frame's border at any display scale.
	constexpr int BarHeight = 96;
	constexpr int MenusWidth = 100;
	constexpr int ButtonWidth = 30;

	Window::TitleBarHitTestFn MakeHitTest(Window const& window, std::vector<glm::ivec2>* asked = nullptr)
	{
		return [&window, asked](glm::ivec2 position)
		{
			if (asked != nullptr)
			{
				asked->push_back(position);
			}
			int const width = static_cast<int>(window.GetWindowSize().x);
			if (position.y >= BarHeight)
			{
				return TitleBarArea::Client;
			}
			if (position.x >= width - ButtonWidth)
			{
				return TitleBarArea::Close;
			}
			if (position.x >= width - ButtonWidth * 2)
			{
				return TitleBarArea::Maximize;
			}
			if (position.x >= width - ButtonWidth * 3)
			{
				return TitleBarArea::Minimize;
			}
			return position.x < MenusWidth ? TitleBarArea::Client : TitleBarArea::Caption;
		};
	}

	HWND GetHandle(Window const& window)
	{
		HWND const handle = glfwGetWin32Window(window.GetNativeWindow());
		REQUIRE(handle != nullptr);
		return handle;
	}

	// The screen position of a point of the client area.
	POINT ToScreen(HWND handle, int x, int y)
	{
		POINT point = {x, y};
		REQUIRE(ClientToScreen(handle, &point) != 0);
		return point;
	}

	// What the window says is at a point of its client area, asked as the system asks.
	LRESULT HitTest(HWND handle, int x, int y)
	{
		POINT const point = ToScreen(handle, x, y);
		return SendMessageW(handle, WM_NCHITTEST, 0, MAKELPARAM(static_cast<WORD>(point.x), static_cast<WORD>(point.y)));
	}

	// The width of the frame's border on the window's left.
	int GetBorder(HWND handle)
	{
		RECT bounds = {};
		REQUIRE(GetWindowRect(handle, &bounds) != 0);
		return static_cast<int>(ToScreen(handle, 0, 0).x - bounds.left);
	}

	// The command a button posted to its window, taken out of the queue so that it never runs (minimizing or maximizing
	// would show the hidden window); 0 when there is none.
	WPARAM TakeSystemCommand(HWND handle)
	{
		MSG message = {};
		return PeekMessageW(&message, handle, WM_SYSCOMMAND, WM_SYSCOMMAND, PM_REMOVE) != 0 ? (message.wParam & 0xFFF0) : 0;
	}
}

TEST_CASE("Window: the client area of a window with a custom title bar starts at the window's top, inside the system's frame")
{
	ST_REQUIRE_DISPLAY();

	Scope<Window> const window = CreateTestWindow(true);
	REQUIRE(window->HasCustomTitleBar());
	HWND const handle = GetHandle(*window);

	// The window is still one with a title bar and resize borders for the system, which snaps and animates those.
	DWORD const style = static_cast<DWORD>(GetWindowLongW(handle, GWL_STYLE));
	CHECK((style & WS_CAPTION) == WS_CAPTION);
	CHECK((style & WS_THICKFRAME) != 0);
	CHECK((style & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) == (WS_MINIMIZEBOX | WS_MAXIMIZEBOX));

	RECT bounds = {};
	REQUIRE(GetWindowRect(handle, &bounds) != 0);
	RECT client = {};
	REQUIRE(GetClientRect(handle, &client) != 0);
	POINT const origin = ToScreen(handle, 0, 0);
	// Nothing above the client area; the frame's borders on the other three sides.
	CHECK(origin.y == bounds.top);
	int const border = GetBorder(handle);
	CHECK(border > 0);
	CHECK(origin.x + client.right + border == bounds.right);
	CHECK(origin.y + client.bottom + border == bounds.bottom);

	SUBCASE("sizes are the client area's")
	{
		window->SetSize(520, 340);
		CHECK(window->GetWindowSize() == glm::uvec2(520, 340));
		REQUIRE(GetWindowRect(handle, &bounds) != 0);
		CHECK(ToScreen(handle, 0, 0).y == bounds.top);
		CHECK(bounds.bottom - bounds.top == 340 + border);
	}

	SUBCASE("maximized, the client area stays on the monitor")
	{
		// Without showing the window: GLFW maximizes a hidden window by placing it.
		window->Maximize();
		CHECK(window->IsMaximized());
		MONITORINFO monitor = {};
		monitor.cbSize = sizeof(monitor);
		REQUIRE(GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor) != 0);
		POINT const top = ToScreen(handle, 0, 0);
		// The window reaches over the work area by its borders; the client area starts at the work area's corner (a few
		// pixels in at an edge with a taskbar that hides itself).
		CHECK(top.x >= monitor.rcWork.left);
		CHECK(top.x <= monitor.rcWork.left + 2);
		CHECK(top.y >= monitor.rcWork.top);
		CHECK(top.y <= monitor.rcWork.top + 2);
		// No border to resize a maximized window by: the bar reaches the top.
		window->SetTitleBarHitTest(MakeHitTest(*window));
		CHECK(HitTest(handle, MenusWidth + 10, 0) == HTCAPTION);
	}

	SUBCASE("without the title bar's style the frame is the system's")
	{
		// What borderless fullscreen does to the window.
		window->SetTitleBarHitTest(MakeHitTest(*window));
		SetWindowLongW(handle, GWL_STYLE, static_cast<LONG>((style & ~static_cast<DWORD>(WS_OVERLAPPEDWINDOW)) | WS_POPUP));
		SetWindowPos(handle, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
		REQUIRE(GetWindowRect(handle, &bounds) != 0);
		REQUIRE(GetClientRect(handle, &client) != 0);
		CHECK(client.right == bounds.right - bounds.left);
		CHECK(client.bottom == bounds.bottom - bounds.top);
		CHECK(HitTest(handle, MenusWidth + 10, BarHeight / 2) == HTCLIENT);
		CHECK(HitTest(handle, MenusWidth + 10, 0) == HTCLIENT);
	}
}

TEST_CASE("Window: a window with a custom title bar tells the system where the application's title bar is")
{
	ST_REQUIRE_DISPLAY();

	Scope<Window> const window = CreateTestWindow(true);
	REQUIRE(window->HasCustomTitleBar());
	HWND const handle = GetHandle(*window);
	int const width = static_cast<int>(window->GetWindowSize().x);
	int const height = static_cast<int>(window->GetWindowSize().y);
	int const border = GetBorder(handle);
	REQUIRE(border > 0);
	int const middle = BarHeight / 2;
	REQUIRE(border < middle);

	// No title bar at all until the application names one; the top border is there from the start.
	CHECK(HitTest(handle, MenusWidth + 10, middle) == HTCLIENT);
	CHECK(HitTest(handle, MenusWidth + 10, 0) == HTTOP);

	std::vector<glm::ivec2> asked;
	window->SetTitleBarHitTest(MakeHitTest(*window, &asked));
	auto const check = [&]()
	{
		CHECK(HitTest(handle, 10, middle) == HTCLIENT);
		CHECK(HitTest(handle, MenusWidth + 10, middle) == HTCAPTION);
		CHECK(HitTest(handle, width - ButtonWidth * 3 + 5, middle) == HTMINBUTTON);
		// The code Windows 11 shows its snap layouts at.
		CHECK(HitTest(handle, width - ButtonWidth * 2 + 5, middle) == HTMAXBUTTON);
		CHECK(HitTest(handle, width - ButtonWidth + 5, middle) == HTCLOSE);
		CHECK(HitTest(handle, MenusWidth + 10, BarHeight + 10) == HTCLIENT);
		CHECK(HitTest(handle, width / 2, height - 1) == HTCLIENT);
		// The top border lies in the client area, over the bar's upper edge, with corners at its ends.
		CHECK(HitTest(handle, MenusWidth + 10, 0) == HTTOP);
		CHECK(HitTest(handle, MenusWidth + 10, border - 1) == HTTOP);
		CHECK(HitTest(handle, MenusWidth + 10, border) == HTCAPTION);
		CHECK(HitTest(handle, 0, 0) == HTTOPLEFT);
		CHECK(HitTest(handle, width - 1, 0) == HTTOPRIGHT);
		CHECK(HitTest(handle, width - 1, border) == HTCLOSE);
		// The other borders are the system's, outside the client area.
		CHECK(HitTest(handle, -1, height / 2) == HTLEFT);
		CHECK(HitTest(handle, width, height / 2) == HTRIGHT);
		CHECK(HitTest(handle, width / 2, height) == HTBOTTOM);
	};
	check();
	// The application is asked in pixels of the client area.
	asked.clear();
	CHECK(HitTest(handle, 123, 17) == HTCAPTION);
	REQUIRE(asked.size() == 1);
	CHECK(asked[0] == glm::ivec2(123, 17));
	// Not for the frame's own borders.
	asked.clear();
	CHECK(HitTest(handle, 123, 0) == HTTOP);
	CHECK(asked.empty());

	// Wherever the window is: nothing was drawn or asked again in between.
	RECT bounds = {};
	REQUIRE(GetWindowRect(handle, &bounds) != 0);
	REQUIRE(SetWindowPos(handle, nullptr, bounds.left + 137, bounds.top + 61, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != 0);
	check();

	// A window that cannot be maximized has no such button: its place is caption.
	DWORD const style = static_cast<DWORD>(GetWindowLongW(handle, GWL_STYLE));
	SetWindowLongW(handle, GWL_STYLE, static_cast<LONG>(style & ~static_cast<DWORD>(WS_MAXIMIZEBOX)));
	CHECK(HitTest(handle, width - ButtonWidth * 2 + 5, middle) == HTCAPTION);
	SetWindowLongW(handle, GWL_STYLE, static_cast<LONG>(style));

	window->SetTitleBarHitTest({});
	CHECK(HitTest(handle, MenusWidth + 10, middle) == HTCLIENT);
}

TEST_CASE("Window: a window with a custom title bar tracks its buttons and presses them itself")
{
	ST_REQUIRE_DISPLAY();

	Scope<Window> const window = CreateTestWindow(true);
	REQUIRE(window->HasCustomTitleBar());
	HWND const handle = GetHandle(*window);
	window->SetTitleBarHitTest(MakeHitTest(*window));
	int closes = 0;
	window->SetEventCallback(
		[&closes](Event& event)
		{
			EventDispatcher dispatcher(event);
			dispatcher.Dispatch<WindowCloseEvent>(
				[&closes](WindowCloseEvent&)
				{
					closes++;
					return true;
				});
		});
	// The messages of the system for the mouse over the frame, with the area it found there.
	auto const send = [handle](UINT message, WPARAM area)
	{
		return SendMessageW(handle, message, area, 0);
	};

	// Under the cursor.
	send(WM_NCMOUSEMOVE, HTMAXBUTTON);
	CHECK(window->GetHoveredTitleBarButton() == TitleBarArea::Maximize);
	CHECK(window->GetPressedTitleBarButton() == TitleBarArea::Client);
	send(WM_NCMOUSEMOVE, HTCLOSE);
	CHECK(window->GetHoveredTitleBarButton() == TitleBarArea::Close);
	send(WM_NCMOUSEMOVE, HTCAPTION);
	CHECK(window->GetHoveredTitleBarButton() == TitleBarArea::Client);
	send(WM_NCMOUSEMOVE, HTMINBUTTON);
	CHECK(window->GetHoveredTitleBarButton() == TitleBarArea::Minimize);
	send(WM_NCMOUSELEAVE, 0);
	CHECK(window->GetHoveredTitleBarButton() == TitleBarArea::Client);
	send(WM_NCMOUSEMOVE, HTMINBUTTON);
	send(WM_MOUSEMOVE, 0);
	CHECK(window->GetHoveredTitleBarButton() == TitleBarArea::Client);

	// A press and a release on a button are the system's command for it, posted to the window.
	CHECK(send(WM_NCLBUTTONDOWN, HTMINBUTTON) == 0);
	CHECK(window->GetPressedTitleBarButton() == TitleBarArea::Minimize);
	CHECK(window->GetHoveredTitleBarButton() == TitleBarArea::Minimize);
	CHECK(TakeSystemCommand(handle) == 0);
	CHECK(send(WM_NCLBUTTONUP, HTMINBUTTON) == 0);
	CHECK(window->GetPressedTitleBarButton() == TitleBarArea::Client);
	CHECK(TakeSystemCommand(handle) == SC_MINIMIZE);

	send(WM_NCLBUTTONDOWN, HTMAXBUTTON);
	send(WM_NCLBUTTONUP, HTMAXBUTTON);
	CHECK(TakeSystemCommand(handle) == SC_MAXIMIZE);
	// The second press of a double click is a press too.
	send(WM_NCLBUTTONDBLCLK, HTMAXBUTTON);
	CHECK(window->GetPressedTitleBarButton() == TitleBarArea::Maximize);
	send(WM_NCLBUTTONUP, HTMAXBUTTON);
	CHECK(TakeSystemCommand(handle) == SC_MAXIMIZE);

	// A press that leaves its button presses nothing: released on another button, on the caption, or outside.
	send(WM_NCLBUTTONDOWN, HTMINBUTTON);
	send(WM_NCMOUSEMOVE, HTCLOSE);
	CHECK(window->GetPressedTitleBarButton() == TitleBarArea::Client);
	send(WM_NCLBUTTONUP, HTCLOSE);
	CHECK(TakeSystemCommand(handle) == 0);
	send(WM_NCLBUTTONDOWN, HTCLOSE);
	send(WM_NCMOUSELEAVE, 0);
	CHECK(window->GetPressedTitleBarButton() == TitleBarArea::Client);
	send(WM_NCLBUTTONUP, HTCLOSE);
	CHECK(TakeSystemCommand(handle) == 0);
	// A release without a press, such as a press that began elsewhere.
	send(WM_NCLBUTTONUP, HTMAXBUTTON);
	CHECK(TakeSystemCommand(handle) == 0);

	// Close asks the application, as the system's button does: the window's close event, once.
	send(WM_NCLBUTTONDOWN, HTCLOSE);
	CHECK(window->GetPressedTitleBarButton() == TitleBarArea::Close);
	send(WM_NCLBUTTONUP, HTCLOSE);
	CHECK(closes == 0);
	Window::PollEvents();
	CHECK(closes == 1);
	Window::PollEvents();
	CHECK(closes == 1);
}
#endif
