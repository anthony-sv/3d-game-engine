#include "GpuTestUtilities.h"

#include "Strada/Core/Events/ApplicationEvent.h"
#include "Strada/Core/FileSystem.h"
#include "Strada/Core/Window.h"

#include <doctest/doctest.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <array>
#include <cmath>
#include <vector>

using namespace Strada;

namespace
{
	// The drop callback installed on a window, called the way GLFW calls it when files are dropped onto the window.
	GLFWdropfun GetDropCallback(GLFWwindow* window)
	{
		GLFWdropfun const callback = glfwSetDropCallback(window, nullptr);
		glfwSetDropCallback(window, callback);
		return callback;
	}
}

TEST_CASE("Window: reports its size in screen coordinates and pixels, and the cursor's position")
{
	ST_REQUIRE_DISPLAY();

	WindowSpecification specification;
	specification.Title = "Strada Window Test";
	specification.Width = 200;
	specification.Height = 150;
	specification.Visible = false;
	Result<Scope<Window>> created = Window::Create(specification);
	REQUIRE_MESSAGE(created.IsOk(), (created ? std::string() : created.GetError()));
	Window const& window = *created.GetValue();
	Window::PollEvents();

	glm::uvec2 const size = window.GetWindowSize();
	CHECK(size.x > 0);
	CHECK(size.y > 0);
	CHECK(window.GetWidth() > 0);
	CHECK(window.GetHeight() > 0);
	// Wherever the cursor is, also outside the window, its position is a number of screen coordinates.
	glm::vec2 const cursor = window.GetCursorPosition();
	CHECK(std::isfinite(cursor.x));
	CHECK(std::isfinite(cursor.y));
}

TEST_CASE("Window: files dropped onto it, or onto windows that forward their drops to it, arrive as file drop events")
{
	ST_REQUIRE_DISPLAY();

	WindowSpecification specification;
	specification.Title = "Strada Drop Test";
	specification.Width = 200;
	specification.Height = 150;
	specification.Visible = false;
	Result<Scope<Window>> created = Window::Create(specification);
	REQUIRE_MESSAGE(created.IsOk(), (created ? std::string() : created.GetError()));
	Window& window = *created.GetValue();
	std::vector<std::vector<std::filesystem::path>> drops;
	window.SetEventCallback(
		[&drops](Event& event)
		{
			EventDispatcher dispatcher(event);
			dispatcher.Dispatch<WindowFileDropEvent>(
				[&drops](WindowFileDropEvent& dropped)
				{
					drops.push_back(dropped.GetPaths());
					return true;
				});
		});

	// The OS reports UTF-8 paths, in any script.
	GLFWwindow* const native = window.GetNativeWindow();
	GLFWdropfun const drop = GetDropCallback(native);
	REQUIRE(drop != nullptr);
	std::array<char const*, 2> paths = {"C:/Users/J\xC3\xBCrgen/Textur.png", "/home/\xE6\x97\xA5\xE6\x9C\xAC/\xE9\x9F\xB3.ogg"};
	drop(native, static_cast<int>(paths.size()), paths.data());
	REQUIRE(drops.size() == 1);
	REQUIRE(drops[0].size() == 2);
	CHECK(FileSystem::PathToUtf8(drops[0][0]) == paths[0]);
	CHECK(FileSystem::PathToUtf8(drops[0][1].filename()) == "\xE9\x9F\xB3.ogg");

	// Another window of the application (the platform window of a panel dragged out of the main one) forwards its drops.
	glfwDefaultWindowHints();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	GLFWwindow* const panel = glfwCreateWindow(120, 80, "Strada Drop Test Panel", nullptr, nullptr);
	glfwDefaultWindowHints();
	REQUIRE(panel != nullptr);
	CHECK(GetDropCallback(panel) == nullptr);
	window.ForwardFileDrops(panel);
	GLFWdropfun const forwarded = GetDropCallback(panel);
	REQUIRE(forwarded != nullptr);
	std::array<char const*, 1> panelPaths = {"D:/Pack/Crate.glb"};
	forwarded(panel, static_cast<int>(panelPaths.size()), panelPaths.data());
	REQUIRE(drops.size() == 2);
	REQUIRE(drops[1].size() == 1);
	CHECK(FileSystem::PathToUtf8(drops[1][0]) == "D:/Pack/Crate.glb");
	glfwDestroyWindow(panel);

	// Nothing to forward from the window itself or from no window: its own drops still arrive once.
	window.ForwardFileDrops(nullptr);
	window.ForwardFileDrops(native);
	drop(native, static_cast<int>(panelPaths.size()), panelPaths.data());
	CHECK(drops.size() == 3);
}
