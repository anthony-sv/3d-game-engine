#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace Strada
{
	// Key codes. Values match GLFW's key tokens, which are based on the US keyboard layout.
	enum class KeyCode : uint16_t
	{
		Unknown = 0,

		Space = 32,
		Apostrophe = 39,
		Comma = 44,
		Minus = 45,
		Period = 46,
		Slash = 47,

		D0 = 48,
		D1 = 49,
		D2 = 50,
		D3 = 51,
		D4 = 52,
		D5 = 53,
		D6 = 54,
		D7 = 55,
		D8 = 56,
		D9 = 57,

		Semicolon = 59,
		Equal = 61,

		A = 65,
		B = 66,
		C = 67,
		D = 68,
		E = 69,
		F = 70,
		G = 71,
		H = 72,
		I = 73,
		J = 74,
		K = 75,
		L = 76,
		M = 77,
		N = 78,
		O = 79,
		P = 80,
		Q = 81,
		R = 82,
		S = 83,
		T = 84,
		U = 85,
		V = 86,
		W = 87,
		X = 88,
		Y = 89,
		Z = 90,

		LeftBracket = 91,
		Backslash = 92,
		RightBracket = 93,
		GraveAccent = 96,

		World1 = 161,
		World2 = 162,

		Escape = 256,
		Enter = 257,
		Tab = 258,
		Backspace = 259,
		Insert = 260,
		Delete = 261,
		Right = 262,
		Left = 263,
		Down = 264,
		Up = 265,
		PageUp = 266,
		PageDown = 267,
		Home = 268,
		End = 269,
		CapsLock = 280,
		ScrollLock = 281,
		NumLock = 282,
		PrintScreen = 283,
		Pause = 284,
		F1 = 290,
		F2 = 291,
		F3 = 292,
		F4 = 293,
		F5 = 294,
		F6 = 295,
		F7 = 296,
		F8 = 297,
		F9 = 298,
		F10 = 299,
		F11 = 300,
		F12 = 301,
		F13 = 302,
		F14 = 303,
		F15 = 304,
		F16 = 305,
		F17 = 306,
		F18 = 307,
		F19 = 308,
		F20 = 309,
		F21 = 310,
		F22 = 311,
		F23 = 312,
		F24 = 313,
		F25 = 314,

		KP0 = 320,
		KP1 = 321,
		KP2 = 322,
		KP3 = 323,
		KP4 = 324,
		KP5 = 325,
		KP6 = 326,
		KP7 = 327,
		KP8 = 328,
		KP9 = 329,
		KPDecimal = 330,
		KPDivide = 331,
		KPMultiply = 332,
		KPSubtract = 333,
		KPAdd = 334,
		KPEnter = 335,
		KPEqual = 336,

		LeftShift = 340,
		LeftControl = 341,
		LeftAlt = 342,
		LeftSuper = 343,
		RightShift = 344,
		RightControl = 345,
		RightAlt = 346,
		RightSuper = 347,
		Menu = 348
	};

	// One past the largest key code value; sizes key state tables.
	inline constexpr uint16_t KeyCodeCount = 349;

	enum class MouseButton : uint8_t
	{
		Left = 0,
		Right = 1,
		Middle = 2,
		Button3 = 3,
		Button4 = 4,
		Button5 = 5,
		Button6 = 6,
		Button7 = 7
	};

	inline constexpr uint8_t MouseButtonCount = 8;

	// Gamepad buttons and axes follow GLFW's standard (SDL_GameControllerDB) mapping.
	enum class GamepadButton : uint8_t
	{
		A = 0,
		B = 1,
		X = 2,
		Y = 3,
		LeftBumper = 4,
		RightBumper = 5,
		Back = 6,
		Start = 7,
		Guide = 8,
		LeftThumb = 9,
		RightThumb = 10,
		DPadUp = 11,
		DPadRight = 12,
		DPadDown = 13,
		DPadLeft = 14
	};

	inline constexpr uint8_t GamepadButtonCount = 15;

	enum class GamepadAxis : uint8_t
	{
		LeftX = 0,
		LeftY = 1,
		RightX = 2,
		RightY = 3,
		LeftTrigger = 4,
		RightTrigger = 5
	};

	inline constexpr uint8_t GamepadAxisCount = 6;
	inline constexpr uint8_t MaxGamepads = 16;

	enum class CursorMode : uint8_t
	{
		Normal = 0,
		Hidden = 1,
		Locked = 2
	};

	// Enumerator name of the key ("A", "Space", "LeftShift", ...), or "Unknown".
	char const* KeyCodeToString(KeyCode key);
	// Parses an enumerator name; case-insensitive.
	std::optional<KeyCode> KeyCodeFromString(std::string_view name);

	char const* MouseButtonToString(MouseButton button);
	std::optional<MouseButton> MouseButtonFromString(std::string_view name);
}
