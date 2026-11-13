#include "Editor/EditorShortcuts.h"

#include "Editor/EditorEditingKeys.h"

#include <imgui_internal.h>

#include <array>

namespace Strada
{
	namespace
	{
		// A global shortcut: its chord and the action it runs.
		struct GlobalShortcut
		{
			ImGuiKeyChord Chord = ImGuiKey_None;
			std::function<void()> EditorShortcutActions::* Action = nullptr;
		};

		// In the order the editor has always tested them: Ctrl+Shift+S before Ctrl+S (ImGui matches chords exactly, so the
		// order only says which runs first when both are pressed in one frame).
		constexpr std::array<GlobalShortcut, 8> GlobalShortcuts = {{
			{ImGuiMod_Ctrl | ImGuiKey_N, &EditorShortcutActions::NewScene},
			{ImGuiMod_Ctrl | ImGuiKey_O, &EditorShortcutActions::OpenScene},
			{ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, &EditorShortcutActions::SaveSceneAs},
			{ImGuiMod_Ctrl | ImGuiKey_S, &EditorShortcutActions::SaveScene},
			{ImGuiMod_Ctrl | ImGuiKey_B, &EditorShortcutActions::BuildScripts},
			{ImGuiMod_Ctrl | ImGuiKey_P, &EditorShortcutActions::TogglePlay},
			{ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P, &EditorShortcutActions::TogglePause},
			{ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_P, &EditorShortcutActions::StepFrame},
		}};
	}

	bool AreEditorShortcutsHeldBack(bool startScreenVisible)
	{
		return startScreenVisible || ImGui::GetTopMostPopupModal() != nullptr;
	}

	void HandleEditorShortcuts(EditorShortcutActions const& actions, EditorOperations& operations, VisualScriptEditors* visualScripts,
	                           std::span<ImGuiKeyChord const> claimed, bool startScreenVisible)
	{
		bool const heldBack = AreEditorShortcutsHeldBack(startScreenVisible);
		// Every route is registered every frame, also while held back (as the editing keys' are), so that routing does not
		// change with a dialog; a focused text field keeps its own keys (Ctrl+Z in a text field undoes typing).
		for (GlobalShortcut const& shortcut : GlobalShortcuts)
		{
			bool const pressed = ImGui::Shortcut(shortcut.Chord, ImGuiInputFlags_RouteGlobal);
			std::function<void()> const& action = actions.*shortcut.Action;
			if (pressed && !heldBack && action)
			{
				action();
			}
		}
		if (startScreenVisible)
		{
			return;
		}
		if (!heldBack && actions.DebugKeys)
		{
			actions.DebugKeys();
		}
		// Undo and Redo, then Cut, Copy, Paste, Duplicate, Delete and Select All: on the Edit menu's target, as its items, but
		// for the chords the window that has the keyboard claims. Each runs nothing while a modal is open.
		HandleEditorHistoryKeys(operations, visualScripts);
		HandleEditorEditingKeys(operations, visualScripts, claimed);
	}
}
