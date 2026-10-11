#include "GpuTestUtilities.h"

#include "Strada/Core/Window.h"

#include <doctest/doctest.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cmath>
#include <string>

using namespace Strada;

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

TEST_CASE("Window: cursor modes apply to the window, and to any other GLFW window")
{
	ST_REQUIRE_DISPLAY();

	// Hidden windows never have the focus, so a locked cursor does not take the real pointer.
	WindowSpecification specification;
	specification.Title = "Strada Cursor Test";
	specification.Width = 200;
	specification.Height = 150;
	specification.Visible = false;
	Result<Scope<Window>> created = Window::Create(specification);
	REQUIRE_MESSAGE(created.IsOk(), (created ? std::string() : created.GetError()));
	Result<Scope<Window>> createdOther = Window::Create(specification);
	REQUIRE_MESSAGE(createdOther.IsOk(), (createdOther ? std::string() : createdOther.GetError()));
	Window& window = *created.GetValue();
	// Stands for the window of a panel dragged out of the editor, which ImGui creates.
	GLFWwindow* const other = createdOther.GetValue()->GetNativeWindow();
	CHECK(glfwGetInputMode(window.GetNativeWindow(), GLFW_CURSOR) == GLFW_CURSOR_NORMAL);

	window.SetCursorMode(CursorMode::Hidden);
	CHECK(glfwGetInputMode(window.GetNativeWindow(), GLFW_CURSOR) == GLFW_CURSOR_HIDDEN);
	CHECK(glfwGetInputMode(other, GLFW_CURSOR) == GLFW_CURSOR_NORMAL);

	Window::ApplyCursorMode(other, CursorMode::Locked);
	CHECK(glfwGetInputMode(other, GLFW_CURSOR) == GLFW_CURSOR_DISABLED);
	// A locked cursor moves by the mouse's own motion where the system reports it (no pointer acceleration in mouse look).
	CHECK(glfwGetInputMode(other, GLFW_RAW_MOUSE_MOTION) == glfwRawMouseMotionSupported());
	CHECK(glfwGetInputMode(window.GetNativeWindow(), GLFW_CURSOR) == GLFW_CURSOR_HIDDEN);

	Window::ApplyCursorMode(other, CursorMode::Normal);
	window.SetCursorMode(CursorMode::Normal);
	CHECK(glfwGetInputMode(other, GLFW_CURSOR) == GLFW_CURSOR_NORMAL);
	CHECK(glfwGetInputMode(window.GetNativeWindow(), GLFW_CURSOR) == GLFW_CURSOR_NORMAL);
}
