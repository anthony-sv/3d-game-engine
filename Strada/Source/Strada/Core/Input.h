#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/KeyCodes.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
		// Whether the mouse button's latest release came from ReleaseKeysAndMouseButtons rather than from the device (the window
		// lost the focus, the editor took the game's input back): interfaces cancel a press that ends this way instead of
		// clicking. Holds until the button is pressed again.
		static bool WasMouseButtonReleasedByReset(MouseButton button);

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

		// How often a button went down and up since Reset, counted at each change and never cleared by the input frame, so
		// that a consumer that does not read every input frame (input actions: a paused scene, one evaluation per game
		// frame) still sees a press and release that came and went between two of its reads. Counts wrap: consumers
		// compare them by unsigned difference.
		struct ButtonCounts
		{
			uint32_t Presses = 0;
			uint32_t Releases = 0;

			bool operator==(ButtonCounts const& other) const = default;
		};

		static ButtonCounts GetKeyCounts(KeyCode key);
		static ButtonCounts GetMouseButtonCounts(MouseButton button);
		// Of the gamepad's effective state (an injected one while an injection holds it); zero for a slot past MaxGamepads.
		static ButtonCounts GetGamepadButtonCounts(uint32_t gamepad, GamepadButton button);
		// Whether the axis of the gamepad's effective state has read a value other than exactly 0 since the slot connected or
		// changed source (an injection beginning or ending): input actions read a trigger that has not as at rest, since a
		// mapping that lacks it reads 0 forever.
		static bool HasGamepadAxisMoved(uint32_t gamepad, GamepadAxis axis);
		// Moved by every Reset and never repeated: consumers that keep counts take them as new baselines when it moved.
		static uint64_t GetResetGeneration();

		// --- Input actions' overrides (Docs/Architecture.md §7.4) ---

		// The largest magnitude an override held during its presses, kept per press so that each consumer can ask for the
		// largest since the press it last saw (consumers evaluate at different times, and none may consume it for another): a
		// press is kept while no later press held as much (the later one then answers for both), oldest first, at most
		// MaxPresses of them (beyond, the oldest kept is forgotten: only a consumer that missed more than MaxPresses presses of
		// falling magnitude can read less than was held).
		class ActionOverridePeaks final
		{
		public:
			static constexpr size_t MaxPresses = 16;

			// Records a magnitude held during the press numbered `press` (1 for the record's first; presses only grow).
			void Record(uint32_t press, float magnitude);
			// The largest magnitude held during a press numbered above `press`; 0 when there was none.
			[[nodiscard]] float GetSince(uint32_t press) const;

		private:
			struct Peak
			{
				uint32_t Press = 0;
				float Magnitude = 0.0f;
			};

			std::array<Peak, MaxPresses> m_Peaks = {};
			uint32_t m_Count = 0;
		};

		// One name's override, as a button's state: the value held, press and release counts (a press when the value leaves
		// (0, 0), a release when it returns to it or is cleared; never cleared while the record lives), and the largest
		// magnitudes held during its presses, of x (what Button and Axis actions read) and of the vector.
		struct ActionOverride
		{
			glm::vec2 Value = glm::vec2(0.0f);
			bool Held = false;
			uint32_t Presses = 0;
			uint32_t Releases = 0;
			ActionOverridePeaks PeaksX;
			ActionOverridePeaks PeaksLength;
		};
		// The names that can have an override record at once; a new name beyond is refused.
		static constexpr size_t MaxActionOverrides = 1024;

		// Puts a value on the named action as one more device would: the action's next evaluations read it beside its
		// bindings, under the largest-magnitude rule, until it is cleared, with edges as a button's (ActionOverride's
		// counts). Button and Axis actions read x; a name no evaluated set has is ignored there (and logged once). Refuses,
		// with a warning, an empty name, a value that is not finite, and a new name beyond MaxActionOverrides. Held across
		// scene loads; cleared by ClearActionOverride(s), ReleaseInjectedInput and Reset.
		static bool SetActionOverride(std::string_view action, glm::vec2 value);
		// Ends the override (a release when its value was not 0). Its record stays, not held, so that a consumer that has
		// not evaluated since still sees a value set and cleared in between, until DropReleasedActionOverrides or Reset.
		static void ClearActionOverride(std::string_view action);
		static void ClearActionOverrides();
		// Drops the record of every override that is not held, which frees its name's place under MaxActionOverrides: for
		// when no consumer that could still need their counts runs (the editor's play mode stopped, its project closed).
		static void DropReleasedActionOverrides();
		// The name's record (held or cleared), null for a name without one. Valid until the next change of the overrides
		// (any Set, Clear, DropReleasedActionOverrides, ReleaseInjectedInput or Reset).
		[[nodiscard]] static ActionOverride const* FindActionOverride(std::string_view action);
		// Whether a record exists for any name (consumers skip the lookups without one).
		static bool HasActionOverrides();
		// Every override held, by name in ordinal order (automation's answers).
		[[nodiscard]] static std::vector<std::pair<std::string, glm::vec2>> GetActionOverrides();
		// Calls the function with every record's name, held or not, in ordinal order.
		static void ForEachActionOverride(std::function<void(std::string_view name, ActionOverride const& record)> const& function);
		// Moved by every change of the overrides and never repeated, across Reset too.
		static uint64_t GetActionOverridesVersion();

		static CursorMode GetCursorMode();
		static void SetCursorMode(CursorMode mode);

		// --- Engine-facing API (application loop, editor, automation) ---

		// Updates the state from key, mouse button, mouse move and scroll events. Does not mark the event handled.
		static void ProcessEvent(Event& event);
		static void SetKeyState(KeyCode key, bool down);
		static void SetMouseButtonState(MouseButton button, bool down);
		static void SetMousePosition(glm::vec2 position);
		// Places the cursor without reporting the jump as movement: GetMouseDelta keeps only the movement before it this frame
		// (the editor's game takes the input back with the cursor somewhere else).
		static void WarpMousePosition(glm::vec2 position);
		static void AddScrollDelta(glm::vec2 delta);
		// The device's state of a gamepad slot, as the window polls it (every slot, every poll). While an injection holds the
		// slot, the state is kept aside and takes effect when the injection is released.
		static void SetGamepadState(uint32_t gamepad, GamepadState const& state);
		// Gives a slot a state of the caller's (automation's input.set), connected or not, which the device's state does not
		// replace until ReleaseInjectedGamepad: polling all 16 slots every frame (Window::PollGamepads) leaves it alone.
		// Buttons report their edges and counts from the slot's previous effective state.
		static void InjectGamepadState(uint32_t gamepad, GamepadState const& state);
		// Returns the slot to its device: the state SetGamepadState last gave it takes effect at once, with the edges that
		// make.
		static void ReleaseInjectedGamepad(uint32_t gamepad);
		static void ReleaseInjectedGamepads();
		static bool IsGamepadInjected(uint32_t gamepad);
		// The slot's injected state; nothing while no injection holds it.
		[[nodiscard]] static std::optional<GamepadState> GetInjectedGamepadState(uint32_t gamepad);
		// What a first injection of a slot starts from: connected, buttons up, sticks at 0, triggers released (-1).
		[[nodiscard]] static GamepadState GetRestingGamepadState();

		// Ends the input frame: clears pressed/released flags and per-frame deltas. Application ends it after each of its frames
		// (EndApplicationFrame); while the frame is owned, its owner ends it instead.
		static void EndFrame();
		// Whether something that runs frames of its own owns the input frame: the editor's play mode while it plays, whose game
		// frames are not the application's (a paused game runs none). While owned, the application's frames keep the input, and
		// the owner ends the frame after each frame it ran, so a press, release, movement or scroll given between two of them
		// (automation's input to a paused game) reaches the next frame once. Kept by Reset.
		static void SetFrameOwned(bool owned);
		static bool IsFrameOwned();
		// Ends the input frame at the end of an application frame (Application's loop), unless it is owned.
		static void EndApplicationFrame();
		// Releases the keys and mouse buttons held, which report Released (mouse buttons also WasMouseButtonReleasedByReset):
		// for when their release events will not come (the window lost the focus, the editor took the game's input back, play
		// mode stopped). Gamepads keep their state: it is polled every frame, so a button held on one is still held.
		static void ReleaseKeysAndMouseButtons();
		// Ends what a caller put in place of devices: every injected gamepad is released (ReleaseInjectedGamepads) and every
		// action override cleared (ClearActionOverrides). With ReleaseKeysAndMouseButtons, what automation's input.release
		// and the end of play mode do; the window losing the focus does not, since nothing of it was the window's.
		static void ReleaseInjectedInput();
		// Clears all state including flags, counts, injections and override records (not the cursor mode handler, nor
		// whether the frame is owned), and moves the reset generation and the overrides' version; used by tests and on
		// shutdown.
		static void Reset();

		// The platform layer registers how cursor mode changes are applied to the OS window.
		static void SetCursorModeHandler(std::function<void(CursorMode)> handler);
	};
}
