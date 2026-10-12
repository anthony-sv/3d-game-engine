#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/KeyCodes.h"
#include "Strada/ImGui/ImGuiLayer.h"

#include <glm/glm.hpp>

#include <bitset>
#include <cstdint>
#include <functional>
#include <utility>

namespace Strada
{
	class Event;

	// Shares the keyboard and mouse of the editor's windows between the editor and the game playing in the game view, the
	// way Unity's game view does. While the game has the input every key and mouse event goes to the game (through Input)
	// and none to the editor, except Shift+Esc, which gives the input back, and the play shortcuts (Ctrl+P, Ctrl+Shift+P,
	// Ctrl+Alt+P; Cmd on macOS), which stay the editor's. Mouse buttons and the wheel reach the game over its view (anywhere
	// while the game locks the cursor); a click anywhere else gives the input back and goes to the editor. While playing, a
	// click on the game view gives the game the input, and the click reaches the game too. Without the input the game gets
	// nothing, and the editor works as usual.
	//
	// Decisions are made event by event as the window reports them (ImGuiLayer::InputFilter), so the game sees input in the
	// frame it happens, with the cursor in the pixels the game renders. Releases always reach both sides, which ignore
	// releases of presses they never saw, so nothing stays held anywhere. Gamepads are not the windows' input: the game reads
	// them whoever has the keyboard and mouse. Main thread only.
	//
	// The cursor mode the game asks for applies while it has the input: a locked cursor locks the window showing the game
	// view, moves only with that window and is hidden over the editor's other windows (GetWindowCursorMode); a hidden one is
	// hidden over the game view alone, which asks ImGui for no cursor there (GameViewPanel).
	class GameInputRouter
	{
	public:
		// The game view as its last frame drew it, in ImGui's coordinates.
		struct View
		{
			// The editor plays the scene (scripts run; not Simulate).
			bool Playing = false;
			// The game view was drawn and has ImGui's focus (its popups included).
			bool Focused = false;
			// The ImGui viewport the view is drawn in (ImGuiLayer::InputContext::Viewport): clicks from other windows, even
			// over its area, are not on the view.
			uint32_t Viewport = 0;
			// The area that takes the game's clicks: the game's image and its letterbox bars.
			glm::vec2 AreaMin = glm::vec2(0.0f);
			glm::vec2 AreaMax = glm::vec2(0.0f);
			// The game's image and the pixels it renders, for the cursor position the game reads.
			glm::vec2 ImageMin = glm::vec2(0.0f);
			glm::vec2 ImageSize = glm::vec2(1.0f);
			glm::uvec2 RenderSize = glm::uvec2(1);
		};

		// Whether something the editor shows covers the game view at a position (a window or popup over it, a modal dialog).
		using CoverageTest = std::function<bool(glm::vec2 position)>;

		// Decides where one window input event goes: passes the game's share to Input and returns whether the editor's
		// interface (ImGui) receives the event. Fits ImGuiLayer::InputFilter.
		bool Route(Event const& event, ImGuiLayer::InputContext const& context);

		// Takes the game view's last frame. The game gives the input back when the scene stopped playing, or the view is
		// hidden or lost the focus.
		void SetView(View const& view);
		void SetCoverageTest(CoverageTest test) { m_CoverageTest = std::move(test); }
		// Whether play shortcuts use Super (Cmd) rather than Control, as ImGui's do on macOS (io.ConfigMacOSXBehaviors).
		void SetShortcutsUseSuper(bool useSuper) { m_ShortcutsUseSuper = useSuper; }

		// Gives the game the input (playing starts) and asks for the game view's focus.
		void Grant();
		// Gives the input back to the editor: the keys and mouse buttons the game holds are released at the start of the next
		// frame (or before the game's next event), the cursor shows normally again.
		void Release();
		bool HasInput() const { return m_HasInput; }
		// Call at the start of each editor frame, before anything reads or injects the game's input.
		void BeginFrame();

		// The cursor mode the game asked for (Input::SetCursorMode); it applies while the game has the input.
		void SetGameCursorMode(CursorMode mode) { m_GameCursorMode = mode; }
		// The game's cursor mode while it has the input, Normal otherwise.
		CursorMode GetCursorMode() const { return m_HasInput ? m_GameCursorMode : CursorMode::Normal; }
		// The cursor mode the OS window of an ImGui viewport (ImGuiViewport::ID) takes: while the game has the input and locks
		// the cursor, the window showing the game view locks it and every other window hides it (on Windows another window can
		// lie over the locked window's middle, where the hidden cursor is kept). Normal otherwise: ImGui shows the cursors, and
		// the game view hides a hidden game cursor over itself.
		CursorMode GetWindowCursorMode(uint32_t viewport) const;
		// Whether the game view should take ImGui's focus because the game took the input; clears the request.
		bool TakeFocusRequest();

	private:
		bool RouteKeyPress(KeyCode key, bool repeat, ImGuiLayer::InputContext const& context);
		bool RouteButtonPress(MouseButton button, ImGuiLayer::InputContext const& context);
		bool RouteMove(glm::vec2 cursor, ImGuiLayer::InputContext const& context);
		bool RouteScroll(glm::vec2 offset, ImGuiLayer::InputContext const& context);
		// The game view is under the cursor of an event while playing: the event comes from the view's window, and the
		// cursor is in its area with nothing over it.
		bool IsViewAt(ImGuiLayer::InputContext const& context) const;
		glm::vec2 ToGamePixels(glm::vec2 position) const;
		void FlushRelease();

		View m_View;
		CoverageTest m_CoverageTest;
		bool m_ShortcutsUseSuper = false;
		bool m_HasInput = false;
		bool m_ReleasePending = false;
		bool m_FocusRequested = false;
		CursorMode m_GameCursorMode = CursorMode::Normal;
		// The last cursor position the windows reported.
		glm::vec2 m_Cursor = glm::vec2(0.0f);
		bool m_HasCursor = false;
		// Keys whose press went to the game: their repeats do too.
		std::bitset<KeyCodeCount> m_GameKeys;
	};
}
