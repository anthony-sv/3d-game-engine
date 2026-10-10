namespace Strada;

/// <summary>Keyboard keys by their position on a US keyboard layout (GLFW's key codes).</summary>
public enum KeyCode
{
	/// <summary>The Unknown key.</summary>
	Unknown = 0,
	/// <summary>The Space key.</summary>
	Space = 32,
	/// <summary>The Apostrophe key.</summary>
	Apostrophe = 39,
	/// <summary>The Comma key.</summary>
	Comma = 44,
	/// <summary>The Minus key.</summary>
	Minus = 45,
	/// <summary>The Period key.</summary>
	Period = 46,
	/// <summary>The Slash key.</summary>
	Slash = 47,
	/// <summary>The D0 key.</summary>
	D0 = 48,
	/// <summary>The D1 key.</summary>
	D1 = 49,
	/// <summary>The D2 key.</summary>
	D2 = 50,
	/// <summary>The D3 key.</summary>
	D3 = 51,
	/// <summary>The D4 key.</summary>
	D4 = 52,
	/// <summary>The D5 key.</summary>
	D5 = 53,
	/// <summary>The D6 key.</summary>
	D6 = 54,
	/// <summary>The D7 key.</summary>
	D7 = 55,
	/// <summary>The D8 key.</summary>
	D8 = 56,
	/// <summary>The D9 key.</summary>
	D9 = 57,
	/// <summary>The Semicolon key.</summary>
	Semicolon = 59,
	/// <summary>The Equal key.</summary>
	Equal = 61,
	/// <summary>The A key.</summary>
	A = 65,
	/// <summary>The B key.</summary>
	B = 66,
	/// <summary>The C key.</summary>
	C = 67,
	/// <summary>The D key.</summary>
	D = 68,
	/// <summary>The E key.</summary>
	E = 69,
	/// <summary>The F key.</summary>
	F = 70,
	/// <summary>The G key.</summary>
	G = 71,
	/// <summary>The H key.</summary>
	H = 72,
	/// <summary>The I key.</summary>
	I = 73,
	/// <summary>The J key.</summary>
	J = 74,
	/// <summary>The K key.</summary>
	K = 75,
	/// <summary>The L key.</summary>
	L = 76,
	/// <summary>The M key.</summary>
	M = 77,
	/// <summary>The N key.</summary>
	N = 78,
	/// <summary>The O key.</summary>
	O = 79,
	/// <summary>The P key.</summary>
	P = 80,
	/// <summary>The Q key.</summary>
	Q = 81,
	/// <summary>The R key.</summary>
	R = 82,
	/// <summary>The S key.</summary>
	S = 83,
	/// <summary>The T key.</summary>
	T = 84,
	/// <summary>The U key.</summary>
	U = 85,
	/// <summary>The V key.</summary>
	V = 86,
	/// <summary>The W key.</summary>
	W = 87,
	/// <summary>The X key.</summary>
	X = 88,
	/// <summary>The Y key.</summary>
	Y = 89,
	/// <summary>The Z key.</summary>
	Z = 90,
	/// <summary>The LeftBracket key.</summary>
	LeftBracket = 91,
	/// <summary>The Backslash key.</summary>
	Backslash = 92,
	/// <summary>The RightBracket key.</summary>
	RightBracket = 93,
	/// <summary>The GraveAccent key.</summary>
	GraveAccent = 96,
	/// <summary>The World1 key.</summary>
	World1 = 161,
	/// <summary>The World2 key.</summary>
	World2 = 162,
	/// <summary>The Escape key.</summary>
	Escape = 256,
	/// <summary>The Enter key.</summary>
	Enter = 257,
	/// <summary>The Tab key.</summary>
	Tab = 258,
	/// <summary>The Backspace key.</summary>
	Backspace = 259,
	/// <summary>The Insert key.</summary>
	Insert = 260,
	/// <summary>The Delete key.</summary>
	Delete = 261,
	/// <summary>The Right key.</summary>
	Right = 262,
	/// <summary>The Left key.</summary>
	Left = 263,
	/// <summary>The Down key.</summary>
	Down = 264,
	/// <summary>The Up key.</summary>
	Up = 265,
	/// <summary>The PageUp key.</summary>
	PageUp = 266,
	/// <summary>The PageDown key.</summary>
	PageDown = 267,
	/// <summary>The Home key.</summary>
	Home = 268,
	/// <summary>The End key.</summary>
	End = 269,
	/// <summary>The CapsLock key.</summary>
	CapsLock = 280,
	/// <summary>The ScrollLock key.</summary>
	ScrollLock = 281,
	/// <summary>The NumLock key.</summary>
	NumLock = 282,
	/// <summary>The PrintScreen key.</summary>
	PrintScreen = 283,
	/// <summary>The Pause key.</summary>
	Pause = 284,
	/// <summary>The F1 key.</summary>
	F1 = 290,
	/// <summary>The F2 key.</summary>
	F2 = 291,
	/// <summary>The F3 key.</summary>
	F3 = 292,
	/// <summary>The F4 key.</summary>
	F4 = 293,
	/// <summary>The F5 key.</summary>
	F5 = 294,
	/// <summary>The F6 key.</summary>
	F6 = 295,
	/// <summary>The F7 key.</summary>
	F7 = 296,
	/// <summary>The F8 key.</summary>
	F8 = 297,
	/// <summary>The F9 key.</summary>
	F9 = 298,
	/// <summary>The F10 key.</summary>
	F10 = 299,
	/// <summary>The F11 key.</summary>
	F11 = 300,
	/// <summary>The F12 key.</summary>
	F12 = 301,
	/// <summary>The F13 key.</summary>
	F13 = 302,
	/// <summary>The F14 key.</summary>
	F14 = 303,
	/// <summary>The F15 key.</summary>
	F15 = 304,
	/// <summary>The F16 key.</summary>
	F16 = 305,
	/// <summary>The F17 key.</summary>
	F17 = 306,
	/// <summary>The F18 key.</summary>
	F18 = 307,
	/// <summary>The F19 key.</summary>
	F19 = 308,
	/// <summary>The F20 key.</summary>
	F20 = 309,
	/// <summary>The F21 key.</summary>
	F21 = 310,
	/// <summary>The F22 key.</summary>
	F22 = 311,
	/// <summary>The F23 key.</summary>
	F23 = 312,
	/// <summary>The F24 key.</summary>
	F24 = 313,
	/// <summary>The F25 key.</summary>
	F25 = 314,
	/// <summary>The KP0 key.</summary>
	KP0 = 320,
	/// <summary>The KP1 key.</summary>
	KP1 = 321,
	/// <summary>The KP2 key.</summary>
	KP2 = 322,
	/// <summary>The KP3 key.</summary>
	KP3 = 323,
	/// <summary>The KP4 key.</summary>
	KP4 = 324,
	/// <summary>The KP5 key.</summary>
	KP5 = 325,
	/// <summary>The KP6 key.</summary>
	KP6 = 326,
	/// <summary>The KP7 key.</summary>
	KP7 = 327,
	/// <summary>The KP8 key.</summary>
	KP8 = 328,
	/// <summary>The KP9 key.</summary>
	KP9 = 329,
	/// <summary>The KPDecimal key.</summary>
	KPDecimal = 330,
	/// <summary>The KPDivide key.</summary>
	KPDivide = 331,
	/// <summary>The KPMultiply key.</summary>
	KPMultiply = 332,
	/// <summary>The KPSubtract key.</summary>
	KPSubtract = 333,
	/// <summary>The KPAdd key.</summary>
	KPAdd = 334,
	/// <summary>The KPEnter key.</summary>
	KPEnter = 335,
	/// <summary>The KPEqual key.</summary>
	KPEqual = 336,
	/// <summary>The LeftShift key.</summary>
	LeftShift = 340,
	/// <summary>The LeftControl key.</summary>
	LeftControl = 341,
	/// <summary>The LeftAlt key.</summary>
	LeftAlt = 342,
	/// <summary>The LeftSuper key.</summary>
	LeftSuper = 343,
	/// <summary>The RightShift key.</summary>
	RightShift = 344,
	/// <summary>The RightControl key.</summary>
	RightControl = 345,
	/// <summary>The RightAlt key.</summary>
	RightAlt = 346,
	/// <summary>The RightSuper key.</summary>
	RightSuper = 347,
	/// <summary>The Menu key.</summary>
	Menu = 348,
}

/// <summary>Mouse buttons.</summary>
public enum MouseButton
{
	/// <summary>The Left value.</summary>
	Left = 0,
	/// <summary>The Right value.</summary>
	Right = 1,
	/// <summary>The Middle value.</summary>
	Middle = 2,
	/// <summary>The Button3 value.</summary>
	Button3 = 3,
	/// <summary>The Button4 value.</summary>
	Button4 = 4,
	/// <summary>The Button5 value.</summary>
	Button5 = 5,
	/// <summary>The Button6 value.</summary>
	Button6 = 6,
	/// <summary>The Button7 value.</summary>
	Button7 = 7,
}

/// <summary>Gamepad buttons in the standard (SDL_GameControllerDB) layout.</summary>
public enum GamepadButton
{
	/// <summary>The A value.</summary>
	A = 0,
	/// <summary>The B value.</summary>
	B = 1,
	/// <summary>The X value.</summary>
	X = 2,
	/// <summary>The Y value.</summary>
	Y = 3,
	/// <summary>The LeftBumper value.</summary>
	LeftBumper = 4,
	/// <summary>The RightBumper value.</summary>
	RightBumper = 5,
	/// <summary>The Back value.</summary>
	Back = 6,
	/// <summary>The Start value.</summary>
	Start = 7,
	/// <summary>The Guide value.</summary>
	Guide = 8,
	/// <summary>The LeftThumb value.</summary>
	LeftThumb = 9,
	/// <summary>The RightThumb value.</summary>
	RightThumb = 10,
	/// <summary>The DPadUp value.</summary>
	DPadUp = 11,
	/// <summary>The DPadRight value.</summary>
	DPadRight = 12,
	/// <summary>The DPadDown value.</summary>
	DPadDown = 13,
	/// <summary>The DPadLeft value.</summary>
	DPadLeft = 14,
}

/// <summary>Gamepad axes in the standard layout: sticks from -1 to 1 (up is -1), triggers from -1 (released) to 1.</summary>
public enum GamepadAxis
{
	/// <summary>The LeftX value.</summary>
	LeftX = 0,
	/// <summary>The LeftY value.</summary>
	LeftY = 1,
	/// <summary>The RightX value.</summary>
	RightX = 2,
	/// <summary>The RightY value.</summary>
	RightY = 3,
	/// <summary>The LeftTrigger value.</summary>
	LeftTrigger = 4,
	/// <summary>The RightTrigger value.</summary>
	RightTrigger = 5,
}

/// <summary>How the mouse cursor behaves.</summary>
public enum CursorMode
{
	/// <summary>Visible and free to leave the window.</summary>
	Normal = 0,
	/// <summary>Hidden while over the window.</summary>
	Hidden = 1,
	/// <summary>Hidden and locked to the window; mouse movement still reports deltas (first-person cameras).</summary>
	Locked = 2,
}
