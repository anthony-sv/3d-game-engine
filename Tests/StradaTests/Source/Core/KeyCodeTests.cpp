#include "Strada/Core/KeyCodes.h"

#include <doctest/doctest.h>

#include <string>

using namespace Strada;

TEST_CASE("KeyCodes: names round trip")
{
	for (KeyCode const key : {KeyCode::A, KeyCode::Space, KeyCode::LeftShift, KeyCode::F12, KeyCode::D7, KeyCode::KPEnter, KeyCode::Escape})
	{
		std::optional<KeyCode> const parsed = KeyCodeFromString(KeyCodeToString(key));
		REQUIRE(parsed.has_value());
		CHECK(*parsed == key);
	}
}

TEST_CASE("KeyCodes: parsing is case-insensitive and rejects unknown names")
{
	CHECK(KeyCodeFromString("space") == std::optional<KeyCode>(KeyCode::Space));
	CHECK(KeyCodeFromString("LEFTSHIFT") == std::optional<KeyCode>(KeyCode::LeftShift));
	CHECK_FALSE(KeyCodeFromString("NotAKey").has_value());
	CHECK_FALSE(KeyCodeFromString("").has_value());
	CHECK(std::string(KeyCodeToString(KeyCode::Unknown)) == "Unknown");
}

TEST_CASE("KeyCodes: values match GLFW key tokens")
{
	// Spot checks against GLFW's documented values; Window relies on a direct cast.
	CHECK(static_cast<int>(KeyCode::Space) == 32);
	CHECK(static_cast<int>(KeyCode::A) == 65);
	CHECK(static_cast<int>(KeyCode::Escape) == 256);
	CHECK(static_cast<int>(KeyCode::F1) == 290);
	CHECK(static_cast<int>(KeyCode::Menu) == 348);
	CHECK(KeyCodeCount == 349);
}

TEST_CASE("KeyCodes: mouse button names round trip")
{
	for (uint8_t i = 0; i < MouseButtonCount; i++)
	{
		MouseButton const button = static_cast<MouseButton>(i);
		CHECK(MouseButtonFromString(MouseButtonToString(button)) == std::optional<MouseButton>(button));
	}
	CHECK(MouseButtonFromString("middle") == std::optional<MouseButton>(MouseButton::Middle));
	CHECK_FALSE(MouseButtonFromString("Button9").has_value());
}
