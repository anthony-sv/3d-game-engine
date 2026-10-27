#pragma once

#include <imgui.h>

#include <span>
#include <string>
#include <vector>

struct ImGuiWindow;

// The editor's editing keys (Cut, Copy, Paste, Duplicate, Delete, Select All) and the Edit menu's editing items as one unit,
// which the editor and its tests both run. Keys and items act on the Edit menu's target: the graph window VisualScriptEditors
// keeps as its edit target, else the selected entities. Windows that do not change the target (the console, the content
// browser, the settings panels) leave the keys acting on it, as the menu does; the only keys they keep are the ones the
// window that has the keyboard claims for itself (the content browser's Delete and F2), whose texts the menu leaves out.
// Main thread only.
namespace Strada
{
	class EditorOperations;
	class VisualScriptEditors;

	// An item an Edit menu drew: its label, the shortcut text it showed (empty: none) and whether it could be chosen.
	struct EditorMenuItemRecord
	{
		std::string Label;
		std::string ShortcutText;
		bool Enabled = false;

		bool operator==(EditorMenuItemRecord const& other) const = default;
	};

	// The scene's editing actions on the selected entities, each reporting a failure (UI::ReportFailure). Duplicate selects
	// the copies; Select All selects every entity in hierarchy order. Duplicate and Delete do nothing without a selection.
	void DuplicateSelectedEntities(EditorOperations& operations);
	void DeleteSelectedEntities(EditorOperations& operations);
	void SelectAllEntities(EditorOperations& operations);

	// The window that has the keyboard for the editing keys' purposes, from ImGui's navigation window and the one this
	// returned in the previous frame: the navigation window's root, unless that root is a popup or a menu (a menu bar's menu,
	// a context menu, a modal) or one of the main viewport's side bars ("##MainMenuBar", "##Toolbar"), which keep the window
	// that had the keyboard before them; previous too while no window has the navigation. Pure.
	[[nodiscard]] ImGuiWindow const* FindKeyboardWindow(ImGuiWindow const* navWindow, ImGuiWindow const* previous);
	// The editing chords the window that has the keyboard (FindKeyboardWindow) claims for itself while it has it: the content
	// browser's (ContentBrowserPanel::FocusedChords); none for the other windows, whose editing keys reach the Edit menu's
	// target (a graph window that has the keyboard is that target). Pure.
	[[nodiscard]] std::span<ImGuiKeyChord const> FindClaimedEditingChords(ImGuiWindow const* keyboardWindow);
	// The shortcut text an Edit item shows for its chord ("Ctrl+X", "Delete"): an item shows its shortcut exactly when
	// pressing it would run the item, so the text is empty while the window that has the keyboard claims the chord. Pure.
	[[nodiscard]] std::string GetEditShortcutText(ImGuiKeyChord chord, std::span<ImGuiKeyChord const> claimed);

	// Registers the editing keys as global routes (ImGuiInputFlags_RouteGlobal, the lowest score: a focused window's own
	// routes for the same chords win while it has the keyboard) and runs what was pressed. Delete and Ctrl+Backspace (Delete),
	// Ctrl+D (Duplicate) and Ctrl+A (Select All) go to the graph window that is the Edit menu's target, and to the entities
	// only without one; Ctrl+X, Ctrl+C and Ctrl+V only to that graph window. Does nothing while a text field has the keyboard.
	// Called once per frame inside the ImGui frame; visualScripts is null without the graph windows.
	void HandleEditorEditingKeys(EditorOperations& operations, VisualScriptEditors* visualScripts);
	// The Edit menu's editing items: the graph target's (VisualScriptEditors::DrawEditMenuItems), else the scene's
	// (Duplicate, Delete, Select All, Deselect All), with the shortcut texts GetEditShortcutText gives for the claimed chords.
	// Returns what it drew, in order. Inside a menu.
	std::vector<EditorMenuItemRecord> DrawEditorEditingMenuItems(EditorOperations& operations, VisualScriptEditors* visualScripts,
	                                                             std::span<ImGuiKeyChord const> claimed);
}
