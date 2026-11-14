#pragma once

#include <imgui.h>

#include <span>

struct ImGuiWindow;

// Editing chords a widget takes for itself while it has the keyboard, told to the editor's editing keys and Edit menu
// (FindClaimedEditingChords, Editor/EditorEditingKeys.h). A widget that registers editing chords (Delete, Ctrl+A) as focused
// routes of its own child window wins them over the editor's global routes while its window has the keyboard; the editor
// learns of it through the root window that has the keyboard (FindKeyboardWindow), on which the widget leaves its claim. Main
// thread only, inside an ImGui frame.
namespace Strada::UI
{
	// Updates the claim of the current window's widget on its root window; called every frame the widget is drawn, inside its
	// child window. The widget claims its chords while that window or a popup it opened has the keyboard
	// (ImGuiFocusedFlags_ChildWindows: the popup hierarchy counts), and gives them up when another part of the root window or
	// of its popups takes the keyboard. While anything else has it (a menu bar's menu, a side bar, another window, none), the
	// claim stays as it was, as FindKeyboardWindow keeps the window that had the keyboard. The chords must outlive the ImGui
	// context (a static array): the root window keeps where they are.
	void UpdateEditingChordClaim(std::span<ImGuiKeyChord const> chords);
	// The chords a widget of the root window claims (UpdateEditingChordClaim), when it was drawn in this frame or the last;
	// none otherwise.
	[[nodiscard]] std::span<ImGuiKeyChord const> GetClaimedEditingChords(ImGuiWindow const& root);
}
