#include "Strada/Core/Events/ApplicationEvent.h"
#include "Strada/Core/Events/KeyEvent.h"
#include "Strada/Core/Events/MouseEvent.h"
#include "Strada/Core/FileSystem.h"

#include <doctest/doctest.h>

#include <array>
#include <string>

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

TEST_CASE("Events: file drop event carries paths")
{
	WindowFileDropEvent const event({std::filesystem::path("a.png"), std::filesystem::path("b.glb")});
	REQUIRE(event.GetPaths().size() == 2);
	CHECK(event.GetPaths()[1] == std::filesystem::path("b.glb"));
	CHECK(event.ToString() == "WindowFileDropEvent: 2 path(s)");
	CHECK(event.GetEventType() == EventType::WindowFileDrop);
	CHECK(std::string(event.GetName()) == "WindowFileDrop");
	CHECK(event.IsInCategory(EventCategoryApplication));
	CHECK_FALSE(event.IsInCategory(EventCategoryInput));
}

TEST_CASE("Events: file drop paths reported as UTF-8 keep every character, and empty entries are skipped")
{
	// Latin, Japanese and Greek names as the OS hands them over (UTF-8).
	std::array<char const*, 5> const reported = {"C:/Users/J\xC3\xBCrgen/Textur.png", "", nullptr,
	                                             "/home/\xE6\x97\xA5\xE6\x9C\xAC/\xE9\x9F\xB3.ogg",
	                                             "D:/\xCE\x95\xCE\xBB\xCE\xBB\xCE\xAC\xCE\xB4\xCE\xB1"};
	WindowFileDropEvent event = WindowFileDropEvent::FromUtf8(reported);
	REQUIRE(event.GetPaths().size() == 3);
	CHECK(FileSystem::PathToUtf8(event.GetPaths()[0]) == reported[0]);
	CHECK(FileSystem::PathToUtf8(event.GetPaths()[1]) == reported[3]);
	CHECK(FileSystem::PathToUtf8(event.GetPaths()[2]) == reported[4]);
	CHECK(FileSystem::PathToUtf8(event.GetPaths()[0].filename()) == "Textur.png");
	CHECK(FileSystem::PathToUtf8(event.GetPaths()[1].extension()) == ".ogg");
	CHECK(FileSystem::PathToUtf8(event.GetPaths()[1].stem()) == "\xE9\x9F\xB3");

	// Dispatched like any other event.
	bool received = false;
	EventDispatcher dispatcher(event);
	CHECK(dispatcher.Dispatch<WindowFileDropEvent>(
		[&received](WindowFileDropEvent& dropped)
		{
			received = dropped.GetPaths().size() == 3;
			return true;
		}));
	CHECK(received);
	CHECK(event.Handled);

	CHECK(WindowFileDropEvent::FromUtf8({}).GetPaths().empty());
}

TEST_CASE("Events: key typed event formats its code point")
{
	KeyTypedEvent const event(0x41);
	CHECK(event.ToString() == "KeyTypedEvent: U+0041");
}
