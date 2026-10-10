#include "GpuTestUtilities.h"

#include "Strada/Core/Window.h"

#include <doctest/doctest.h>

#include <cmath>

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
