#pragma once

#include <imgui.h>

#include <functional>
#include <span>

// The editor's global shortcuts as one unit, which the editor and its tests both run, with the one rule of when they act:
// never while a modal popup is open (the dialog acts on what they would change, and ImGui's global routes ignore modals:
// imgui.cpp CalcRoutingScore gives ImGuiInputFlags_RouteGlobal a score whatever popup is open), and never while the start
// screen shows (what they act on is behind it). Main thread only, inside the ImGui frame.
namespace Strada
{
	class EditorOperations;
	class VisualScriptEditors;

	// What the global shortcuts run besides Undo, Redo and the editing keys (EditorEditingKeys): the editor's actions, each
	// checking its own conditions (Build Scripts needs a script project, Pause needs a game playing). An unset one does
	// nothing.
	struct EditorShortcutActions
	{
		// Ctrl+N
		std::function<void()> NewScene;
		// Ctrl+O
		std::function<void()> OpenScene;
		// Ctrl+S
		std::function<void()> SaveScene;
		// Ctrl+Shift+S
		std::function<void()> SaveSceneAs;
		// Ctrl+B
		std::function<void()> BuildScripts;
		// Ctrl+P: plays, or stops playing.
		std::function<void()> TogglePlay;
		// Ctrl+Shift+P
		std::function<void()> TogglePause;
		// Ctrl+Alt+P
		std::function<void()> StepFrame;
		// The debugger's keys (F5, F10, F11, Ctrl+F11, Shift+F11: VisualScriptDebugControls::HandleKeys), which register their
		// own routes; run only while the shortcuts act.
		std::function<void()> DebugKeys;
	};

	// Whether the global shortcuts are held back this frame: while a modal popup is open (ImGui::GetTopMostPopupModal) or the
	// start screen shows.
	[[nodiscard]] bool AreEditorShortcutsHeldBack(bool startScreenVisible);

	// Registers the global shortcuts as global routes (ImGuiInputFlags_RouteGlobal) and runs what was pressed, then the
	// debugger's keys, Undo and Redo (HandleEditorHistoryKeys) and the editing keys (HandleEditorEditingKeys, with the chords
	// the window that has the keyboard claims). Runs nothing while held back (AreEditorShortcutsHeldBack); while the start
	// screen shows no route of the editing keys is registered either, as no window they act on is drawn. A focused text field
	// keeps the editing keys and Undo and Redo (they do nothing while io.WantTextInput). visualScripts is null without the
	// graph windows. Once per frame inside the ImGui frame.
	void HandleEditorShortcuts(EditorShortcutActions const& actions, EditorOperations& operations, VisualScriptEditors* visualScripts,
	                           std::span<ImGuiKeyChord const> claimed, bool startScreenVisible);
}
