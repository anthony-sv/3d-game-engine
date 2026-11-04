#pragma once

#include "Strada/Asset/AssetHandle.h"

#include <imgui.h>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Strada
{
	class GameInputRouter;
	class PlayMode;
	class VisualScriptDebugSession;
	struct VisualScriptStop;

	// The toolbar's debugger buttons, in their order.
	enum class VisualScriptDebugButton : uint8_t
	{
		Continue = 0,
		StepOver,
		StepInto,
		StepOut,
		Break,
		Count
	};

	// What the debug controls need to know of the editor's windows: whether the Game View shows without a tab change (in a dock
	// node of its own, or floating), and the graph whose window is the Edit menu's target (which Break stops in). The editor
	// reads ImGui's windows; tests fake them.
	struct VisualScriptDebugControlsHost
	{
		std::function<bool()> IsGameViewVisible;
		// Invalid while no graph window is the edit target.
		std::function<AssetHandle()> GetBreakGraph;
		// The name the Break button's tooltip gives a graph ("Player.sgraph").
		std::function<std::string(AssetHandle)> GetGraphName;
	};

	// The editor's debug controls (spec-slice4 §14.2-§14.4), drawn by the main toolbar and pressed through the Play menu and the
	// keys: the toolbar's debugger buttons, the Pause toggle and frame Step while an execution is stopped, the global debug keys
	// (F5, F10, F11, Ctrl+F11, Shift+F11), and the hand-off of the game's input at stops. Resumes are queued with the session
	// (RequestResume), never run while panels draw. The editor only calls these, so tests drive the real behaviour. Main thread
	// only; ImGui is needed by the Draw and Handle members and GetPauseTooltip (whose key follows the platform) alone.
	class VisualScriptDebugControls
	{
	public:
		// The fixed texts, listed in All for the glyph test.
		struct Texts
		{
			// "Continue (F5): runs ...": the key from FormatVisualScriptChord between the two parts.
			static constexpr std::string_view PauseWhileStopped = "Continue (";
			static constexpr std::string_view PauseWhileStoppedEnd = "): runs the stopped executions on, then the game";
			static constexpr std::string_view Pause = "Pause (Ctrl+Shift+P)";
			static constexpr std::string_view Resume = "Resume (Ctrl+Shift+P)";
			static constexpr std::string_view Step = "Advance the paused scene by one frame (Ctrl+Alt+P)";
			static constexpr std::string_view Continue = "Continue";
			static constexpr std::string_view StepOver = "Step Over";
			static constexpr std::string_view StepInto = "Step Into";
			static constexpr std::string_view StepOut = "Step Out";
			static constexpr std::string_view BreakIn = "Break: stop at the next instruction of ";
			static constexpr std::string_view BreakAny = "Break: stop at the next instruction of any graph";

			static constexpr std::array<std::string_view, 11> All = {
				PauseWhileStopped, PauseWhileStoppedEnd, Pause, Resume, Step, Continue, StepOver, StepInto, StepOut, BreakIn, BreakAny};
		};

		// The session, play mode and router must outlive the controls.
		VisualScriptDebugControls(VisualScriptDebugSession& session, PlayMode& playMode, GameInputRouter& input);

		void SetHost(VisualScriptDebugControlsHost host) { m_Host = std::move(host); }

		// The toolbar's Continue, Step Over, Step Into, Step Out and Break, while playing (not simulating), each enabled by the
		// session's state: Continue and the steps while stopped or after a step ended (Continue also while the pause is held with
		// nothing stopped, Step Out not after a step ended), Break while running or stepping. Inside the toolbar's window.
		void DrawToolbarButtons();
		// The width DrawToolbarButtons takes, for centring the toolbar; 0 while it draws nothing.
		float GetToolbarButtonsWidth() const;
		// The toolbar's Pause toggle, Ctrl+Shift+P and the Play menu's Pause: while an execution is stopped, Continue of every
		// stop then the unpause, queued for the next frame; otherwise play mode's pause as before.
		void TogglePause();
		// The Pause toggle's tooltip (with an ImGui context: it names the key as the platform does).
		std::string GetPauseTooltip() const;
		// The toolbar's Step, Ctrl+Alt+P and the Play menu's Step: one frame of a paused scene; nothing while refused.
		void StepFrame();
		bool CanStepFrame() const;
		// Why frames cannot be stepped now (the UI's words), as the Step button's and menu item's tooltip; none when they can.
		std::optional<std::string> GetStepRefusal() const;
		// The global debug keys while an execution is stopped (and F5 while the pause is held with nothing stopped), each queued
		// for the next frame; none behind a modal popup. Once per frame, from the editor's shortcuts; it also takes the game's
		// input back whenever the session holds the pause (UpdateInput).
		void HandleKeys();
		// Once per frame (HandleKeys calls it): while the session holds the pause (stopped, a step ended, the pause held with
		// nothing stopped) the game gives its input back, so the debug keys reach the editor; once play stops, nothing is owed
		// back.
		void UpdateInput();

		// The session's stop listener: a stop that paused the scene (it is paused when the listener is told) takes the input
		// back from the game.
		void OnStop(VisualScriptStop const& stop);
		// The session's resume listener: the input taken at a stop goes back to the game where the user can play (the Game View
		// shows without a tab change); otherwise the graph stays in front and the game waits for a click.
		void OnResume();
		// Where the last DrawToolbarButtons drew a button (its screen rectangle), for tests; none when it drew none.
		struct ButtonRect
		{
			ImVec2 Min;
			ImVec2 Max;
		};
		std::optional<ButtonRect> GetButtonRect(VisualScriptDebugButton button) const;

		// The controls took the game's input at a stop (or while the pause was held) and have not given it back; false once
		// play stopped.
		bool HasTakenInput() const { return m_TookInput; }

		// Whether an ImGui window shows without a tab change: it was drawn in the last frame, and it is floating or the tab in
		// front of its dock node (the Game View beside a graph window, not behind it). Inside an ImGui frame.
		static bool IsWindowShownWithoutTabChange(char const* name);

	private:
		bool IsPlayingWithDebugger() const;
		bool CanContinue() const;
		bool CanStep() const;
		bool CanBreak() const;
		std::string GetBreakTooltip() const;

		VisualScriptDebugSession& m_Session;
		PlayMode& m_PlayMode;
		GameInputRouter& m_Input;
		VisualScriptDebugControlsHost m_Host;
		bool m_TookInput = false;
		std::array<std::optional<ButtonRect>, static_cast<size_t>(VisualScriptDebugButton::Count)> m_ButtonRects;
	};
}
