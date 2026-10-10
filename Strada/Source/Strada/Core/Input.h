#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/KeyCodes.h"

#include <glm/glm.hpp>

#include <array>
#include <functional>

namespace Strada
{
	class Event;

	// Frame-based input state. It is fed by window events (or by the automation API when injecting input) and is
	// independent of GLFW, so it also works headless. "Down" means held; "Pressed"/"Released" mean the state changed
	// during the current frame (a press and release within one frame reports both). Main thread only.
	class Input
	{
	public:
		struct GamepadState
		{
			bool Connected = false;
			std::array<float, GamepadAxisCount> Axes = {};
			std::array<bool, GamepadButtonCount> Buttons = {};
		};

		static bool IsKeyDown(KeyCode key);
		static bool IsKeyPressed(KeyCode key);
		static bool IsKeyReleased(KeyCode key);

		static bool IsMouseButtonDown(MouseButton button);
		static bool IsMouseButtonPressed(MouseButton button);
		static bool IsMouseButtonReleased(MouseButton button);

		// Cursor position in the active game view, in the units of the scene's viewport size: the window's framebuffer pixels in
		// the player, viewport-relative in the editor.
		static glm::vec2 GetMousePosition();
		// Cursor movement since the previous frame.
		static glm::vec2 GetMouseDelta();
		// Scroll accumulated during the current frame.
		static glm::vec2 GetScrollDelta();

		static bool IsGamepadConnected(uint32_t gamepad);
		static float GetGamepadAxis(uint32_t gamepad, GamepadAxis axis);
		static bool IsGamepadButtonDown(uint32_t gamepad, GamepadButton button);
		static bool IsGamepadButtonPressed(uint32_t gamepad, GamepadButton button);
		static bool IsGamepadButtonReleased(uint32_t gamepad, GamepadButton button);

		static CursorMode GetCursorMode();
		static void SetCursorMode(CursorMode mode);

		// --- Engine-facing API (application loop, editor, automation) ---

		// Updates the state from key, mouse button, mouse move and scroll events. Does not mark the event handled.
		static void ProcessEvent(Event& event);
		static void SetKeyState(KeyCode key, bool down);
		static void SetMouseButtonState(MouseButton button, bool down);
		static void SetMousePosition(glm::vec2 position);
		static void AddScrollDelta(glm::vec2 delta);
		static void SetGamepadState(uint32_t gamepad, GamepadState const& state);

		// Ends the frame: clears pressed/released flags and per-frame deltas. Called once per frame by Application.
		static void EndFrame();
		// Releases everything (e.g. when the window loses focus or play mode stops). Held keys report Released.
		static void ReleaseAll();
		// Clears all state including flags; used by tests and on shutdown.
		static void Reset();

		// The platform layer registers how cursor mode changes are applied to the OS window.
		static void SetCursorModeHandler(std::function<void(CursorMode)> handler);
	};
}
