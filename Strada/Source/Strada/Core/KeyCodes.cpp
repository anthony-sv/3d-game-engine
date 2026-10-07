#include "stpch.h"
#include "Strada/Core/KeyCodes.h"

#include <array>
#include <cctype>
#include <utility>

namespace Strada
{
	namespace
	{
		struct KeyName
		{
			KeyCode Key;
			char const* Name;
		};

		constexpr std::array s_KeyNames = {
			KeyName{KeyCode::Space, "Space"},
			KeyName{KeyCode::Apostrophe, "Apostrophe"},
			KeyName{KeyCode::Comma, "Comma"},
			KeyName{KeyCode::Minus, "Minus"},
			KeyName{KeyCode::Period, "Period"},
			KeyName{KeyCode::Slash, "Slash"},
			KeyName{KeyCode::D0, "D0"},
			KeyName{KeyCode::D1, "D1"},
			KeyName{KeyCode::D2, "D2"},
			KeyName{KeyCode::D3, "D3"},
			KeyName{KeyCode::D4, "D4"},
			KeyName{KeyCode::D5, "D5"},
			KeyName{KeyCode::D6, "D6"},
			KeyName{KeyCode::D7, "D7"},
			KeyName{KeyCode::D8, "D8"},
			KeyName{KeyCode::D9, "D9"},
			KeyName{KeyCode::Semicolon, "Semicolon"},
			KeyName{KeyCode::Equal, "Equal"},
			KeyName{KeyCode::A, "A"},
			KeyName{KeyCode::B, "B"},
			KeyName{KeyCode::C, "C"},
			KeyName{KeyCode::D, "D"},
			KeyName{KeyCode::E, "E"},
			KeyName{KeyCode::F, "F"},
			KeyName{KeyCode::G, "G"},
			KeyName{KeyCode::H, "H"},
			KeyName{KeyCode::I, "I"},
			KeyName{KeyCode::J, "J"},
			KeyName{KeyCode::K, "K"},
			KeyName{KeyCode::L, "L"},
			KeyName{KeyCode::M, "M"},
			KeyName{KeyCode::N, "N"},
			KeyName{KeyCode::O, "O"},
			KeyName{KeyCode::P, "P"},
			KeyName{KeyCode::Q, "Q"},
			KeyName{KeyCode::R, "R"},
			KeyName{KeyCode::S, "S"},
			KeyName{KeyCode::T, "T"},
			KeyName{KeyCode::U, "U"},
			KeyName{KeyCode::V, "V"},
			KeyName{KeyCode::W, "W"},
			KeyName{KeyCode::X, "X"},
			KeyName{KeyCode::Y, "Y"},
			KeyName{KeyCode::Z, "Z"},
			KeyName{KeyCode::LeftBracket, "LeftBracket"},
			KeyName{KeyCode::Backslash, "Backslash"},
			KeyName{KeyCode::RightBracket, "RightBracket"},
			KeyName{KeyCode::GraveAccent, "GraveAccent"},
			KeyName{KeyCode::World1, "World1"},
			KeyName{KeyCode::World2, "World2"},
			KeyName{KeyCode::Escape, "Escape"},
			KeyName{KeyCode::Enter, "Enter"},
			KeyName{KeyCode::Tab, "Tab"},
			KeyName{KeyCode::Backspace, "Backspace"},
			KeyName{KeyCode::Insert, "Insert"},
			KeyName{KeyCode::Delete, "Delete"},
			KeyName{KeyCode::Right, "Right"},
			KeyName{KeyCode::Left, "Left"},
			KeyName{KeyCode::Down, "Down"},
			KeyName{KeyCode::Up, "Up"},
			KeyName{KeyCode::PageUp, "PageUp"},
			KeyName{KeyCode::PageDown, "PageDown"},
			KeyName{KeyCode::Home, "Home"},
			KeyName{KeyCode::End, "End"},
			KeyName{KeyCode::CapsLock, "CapsLock"},
			KeyName{KeyCode::ScrollLock, "ScrollLock"},
			KeyName{KeyCode::NumLock, "NumLock"},
			KeyName{KeyCode::PrintScreen, "PrintScreen"},
			KeyName{KeyCode::Pause, "Pause"},
			KeyName{KeyCode::F1, "F1"},
			KeyName{KeyCode::F2, "F2"},
			KeyName{KeyCode::F3, "F3"},
			KeyName{KeyCode::F4, "F4"},
			KeyName{KeyCode::F5, "F5"},
			KeyName{KeyCode::F6, "F6"},
			KeyName{KeyCode::F7, "F7"},
			KeyName{KeyCode::F8, "F8"},
			KeyName{KeyCode::F9, "F9"},
			KeyName{KeyCode::F10, "F10"},
			KeyName{KeyCode::F11, "F11"},
			KeyName{KeyCode::F12, "F12"},
			KeyName{KeyCode::F13, "F13"},
			KeyName{KeyCode::F14, "F14"},
			KeyName{KeyCode::F15, "F15"},
			KeyName{KeyCode::F16, "F16"},
			KeyName{KeyCode::F17, "F17"},
			KeyName{KeyCode::F18, "F18"},
			KeyName{KeyCode::F19, "F19"},
			KeyName{KeyCode::F20, "F20"},
			KeyName{KeyCode::F21, "F21"},
			KeyName{KeyCode::F22, "F22"},
			KeyName{KeyCode::F23, "F23"},
			KeyName{KeyCode::F24, "F24"},
			KeyName{KeyCode::F25, "F25"},
			KeyName{KeyCode::KP0, "KP0"},
			KeyName{KeyCode::KP1, "KP1"},
			KeyName{KeyCode::KP2, "KP2"},
			KeyName{KeyCode::KP3, "KP3"},
			KeyName{KeyCode::KP4, "KP4"},
			KeyName{KeyCode::KP5, "KP5"},
			KeyName{KeyCode::KP6, "KP6"},
			KeyName{KeyCode::KP7, "KP7"},
			KeyName{KeyCode::KP8, "KP8"},
			KeyName{KeyCode::KP9, "KP9"},
			KeyName{KeyCode::KPDecimal, "KPDecimal"},
			KeyName{KeyCode::KPDivide, "KPDivide"},
			KeyName{KeyCode::KPMultiply, "KPMultiply"},
			KeyName{KeyCode::KPSubtract, "KPSubtract"},
			KeyName{KeyCode::KPAdd, "KPAdd"},
			KeyName{KeyCode::KPEnter, "KPEnter"},
			KeyName{KeyCode::KPEqual, "KPEqual"},
			KeyName{KeyCode::LeftShift, "LeftShift"},
			KeyName{KeyCode::LeftControl, "LeftControl"},
			KeyName{KeyCode::LeftAlt, "LeftAlt"},
			KeyName{KeyCode::LeftSuper, "LeftSuper"},
			KeyName{KeyCode::RightShift, "RightShift"},
			KeyName{KeyCode::RightControl, "RightControl"},
			KeyName{KeyCode::RightAlt, "RightAlt"},
			KeyName{KeyCode::RightSuper, "RightSuper"},
			KeyName{KeyCode::Menu, "Menu"},
		};

		constexpr std::array<char const*, MouseButtonCount> s_MouseButtonNames = {
			"Left", "Right", "Middle", "Button3", "Button4", "Button5", "Button6", "Button7",
		};

		bool EqualsIgnoreCase(std::string_view a, std::string_view b)
		{
			if (a.size() != b.size())
			{
				return false;
			}
			for (size_t i = 0; i < a.size(); i++)
			{
				if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
				{
					return false;
				}
			}
			return true;
		}
	}

	char const* KeyCodeToString(KeyCode key)
	{
		for (KeyName const& entry : s_KeyNames)
		{
			if (entry.Key == key)
			{
				return entry.Name;
			}
		}
		return "Unknown";
	}

	std::optional<KeyCode> KeyCodeFromString(std::string_view name)
	{
		for (KeyName const& entry : s_KeyNames)
		{
			if (EqualsIgnoreCase(entry.Name, name))
			{
				return entry.Key;
			}
		}
		return std::nullopt;
	}

	char const* MouseButtonToString(MouseButton button)
	{
		size_t const index = static_cast<size_t>(button);
		return index < s_MouseButtonNames.size() ? s_MouseButtonNames[index] : "Unknown";
	}

	std::optional<MouseButton> MouseButtonFromString(std::string_view name)
	{
		for (size_t i = 0; i < s_MouseButtonNames.size(); i++)
		{
			if (EqualsIgnoreCase(s_MouseButtonNames[i], name))
			{
				return static_cast<MouseButton>(i);
			}
		}
		return std::nullopt;
	}
}
