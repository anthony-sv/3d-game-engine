#include "Strada/Core/Events/ApplicationEvent.h"
#include "Strada/Core/Events/KeyEvent.h"
#include "Strada/Core/Events/MouseEvent.h"

#include <doctest/doctest.h>

using namespace Strada;

TEST_CASE("Events: type, name and categories")
{
	KeyPressedEvent const keyEvent(KeyCode::Space, false);
	CHECK(keyEvent.GetEventType() == EventType::KeyPressed);
	CHECK(std::string(keyEvent.GetName()) == "KeyPressed");
	CHECK(keyEvent.IsInCategory(EventCategoryKeyboard));
	CHECK(keyEvent.IsInCategory(EventCategoryInput));
	CHECK_FALSE(keyEvent.IsInCategory(EventCategoryMouse));

	MouseButtonPressedEvent const buttonEvent(MouseButton::Right);
	CHECK(buttonEvent.IsInCategory(EventCategoryMouseButton));
	CHECK(buttonEvent.GetMouseButton() == MouseButton::Right);

	WindowResizeEvent const resizeEvent(800, 600);
	CHECK(resizeEvent.IsInCategory(EventCategoryApplication));
	CHECK(resizeEvent.ToString() == "WindowResizeEvent: 800, 600");
}

TEST_CASE("Events: dispatcher calls only matching handlers and records handling")
{
	KeyReleasedEvent event(KeyCode::Escape);
	EventDispatcher dispatcher(event);

	bool resizeCalled = false;
	bool releaseCalled = false;
	CHECK_FALSE(dispatcher.Dispatch<WindowResizeEvent>(
		[&resizeCalled](WindowResizeEvent&)
		{
			resizeCalled = true;
			return true;
		}));
	CHECK(dispatcher.Dispatch<KeyReleasedEvent>(
		[&releaseCalled](KeyReleasedEvent& released)
		{
			releaseCalled = released.GetKeyCode() == KeyCode::Escape;
			return true;
		}));

	CHECK_FALSE(resizeCalled);
	CHECK(releaseCalled);
	CHECK(event.Handled);
}

TEST_CASE("Events: handled flag is sticky across dispatches")
{
	MouseMovedEvent event(10.0f, 20.0f);
	EventDispatcher dispatcher(event);
	dispatcher.Dispatch<MouseMovedEvent>(
		[](MouseMovedEvent&)
		{
			return true;
		});
	dispatcher.Dispatch<MouseMovedEvent>(
		[](MouseMovedEvent&)
		{
			return false;
		});
	CHECK(event.Handled);
}

TEST_CASE("Events: drop event carries paths")
{
	WindowDropEvent const event({std::filesystem::path("a.png"), std::filesystem::path("b.glb")});
	REQUIRE(event.GetPaths().size() == 2);
	CHECK(event.GetPaths()[1] == std::filesystem::path("b.glb"));
	CHECK(event.ToString() == "WindowDropEvent: 2 path(s)");
}

TEST_CASE("Events: key typed event formats its code point")
{
	KeyTypedEvent const event(0x41);
	CHECK(event.ToString() == "KeyTypedEvent: U+0041");
}
