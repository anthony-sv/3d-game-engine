#include "Editor/EditorEditingKeys.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorOperations.h"
#include "Editor/Panels/ContentBrowserPanel.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/VisualScripting/VisualScriptEditors.h"
#include "Editor/VisualScripting/VisualScriptShortcuts.h"

#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <imgui_internal.h>

#include <algorithm>
#include <optional>
#include <utility>

namespace Strada
{
	namespace
	{
		// The editing chords and what they ask of the edit target. Delete has two: Cmd+Backspace on macOS keyboards, which
		// have no forward-delete key.
		constexpr ImGuiKeyChord CutChord = ImGuiMod_Ctrl | ImGuiKey_X;
		constexpr ImGuiKeyChord CopyChord = ImGuiMod_Ctrl | ImGuiKey_C;
		constexpr ImGuiKeyChord PasteChord = ImGuiMod_Ctrl | ImGuiKey_V;
		constexpr ImGuiKeyChord DuplicateChord = ImGuiMod_Ctrl | ImGuiKey_D;
		constexpr ImGuiKeyChord DeleteChord = ImGuiKey_Delete;
		constexpr ImGuiKeyChord DeleteBackspaceChord = ImGuiMod_Ctrl | ImGuiKey_Backspace;
		constexpr ImGuiKeyChord SelectAllChord = ImGuiMod_Ctrl | ImGuiKey_A;

		// The main viewport's side bars: BeginMainMenuBar's (imgui_widgets.cpp:9351) and the editor's toolbar
		// (EditorLayer::DrawToolbar), both BeginViewportSideBar windows (imgui_widgets.cpp:9301) named as given.
		constexpr char const* MainMenuBarName = "##MainMenuBar";
		constexpr char const* ToolbarName = "##Toolbar";

		// Asks the graph window that is the Edit menu's target; false while the scene is the target.
		bool ToGraphTarget(VisualScriptEditors* visualScripts, VisualScriptEditorAction action)
		{
			return visualScripts != nullptr && visualScripts->HandleGlobalEditingKey(action);
		}
	}

	void DuplicateSelectedEntities(EditorOperations& operations)
	{
		EditorContext const& context = operations.GetContext();
		if (context.GetSelection().IsEmpty())
		{
			return;
		}
		std::vector<UUID> const selection = context.GetSelection().GetEntities();
		Result<std::vector<UUID>> copies = operations.DuplicateEntities(selection);
		UI::ReportFailure(copies, "Duplicating entities");
		if (copies)
		{
			UI::ReportFailure(operations.Select(copies.GetValue()), "Selecting the copies");
		}
	}

	void DeleteSelectedEntities(EditorOperations& operations)
	{
		EditorContext const& context = operations.GetContext();
		if (context.GetSelection().IsEmpty())
		{
			return;
		}
		std::vector<UUID> const selection = context.GetSelection().GetEntities();
		UI::ReportFailure(operations.DeleteEntities(selection), "Deleting entities");
	}

	void SelectAllEntities(EditorOperations& operations)
	{
		std::vector<UUID> entities;
		operations.GetContext().GetScene().ForEachEntityInHierarchyOrder(
			[&entities](Entity entity)
			{
				entities.push_back(entity.GetUUID());
			});
		UI::ReportFailure(operations.Select(entities), "Selecting all entities");
	}

	ImGuiWindow const* FindKeyboardWindow(ImGuiWindow const* navWindow, ImGuiWindow const* previous)
	{
		if (navWindow == nullptr)
		{
			return previous;
		}
		// A popup's root is the popup itself, a menu's submenu has its menu as its root (imgui.cpp:7847-7856); modals are
		// popups too (ImGuiWindowFlags_Popup, _ChildMenu: imgui.h:1247, :1249). A side bar's ID is its name's hash.
		ImGuiWindow const* root = navWindow->RootWindow != nullptr ? navWindow->RootWindow : navWindow;
		bool const popup = (root->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_ChildMenu)) != 0;
		bool const sideBar = root->ID == ImHashStr(MainMenuBarName) || root->ID == ImHashStr(ToolbarName);
		return popup || sideBar ? previous : root;
	}

	std::span<ImGuiKeyChord const> FindClaimedEditingChords(ImGuiWindow const* keyboardWindow)
	{
		// A top-level window's ID is its name's hash: the content browser's, docked or floating.
		if (keyboardWindow != nullptr && keyboardWindow->ID == ImHashStr(ContentBrowserPanel::WindowName))
		{
			return ContentBrowserPanel::FocusedChords;
		}
		return {};
	}

	std::string GetEditShortcutText(ImGuiKeyChord chord, std::span<ImGuiKeyChord const> claimed)
	{
		if (std::find(claimed.begin(), claimed.end(), chord) != claimed.end())
		{
			return {};
		}
		// As the editor's other menu items write their shortcuts (Undo's "Ctrl+Z"), on every platform.
		return FormatVisualScriptChord(chord, false);
	}

	void HandleEditorEditingKeys(EditorOperations& operations, VisualScriptEditors* visualScripts)
	{
		if (ImGui::GetIO().WantTextInput)
		{
			return;
		}
		// Every route is registered every frame, pressed or not: routes decide the next frame (imgui.cpp:10460-10540).
		ImGuiInputFlags const route = ImGuiInputFlags_RouteGlobal;
		bool const cut = ImGui::Shortcut(CutChord, route);
		bool const copy = ImGui::Shortcut(CopyChord, route);
		bool const paste = ImGui::Shortcut(PasteChord, route);
		bool const duplicate = ImGui::Shortcut(DuplicateChord, route);
		bool const deleteKey = ImGui::Shortcut(DeleteChord, route);
		bool const deleteBackspace = ImGui::Shortcut(DeleteBackspaceChord, route);
		bool const selectAll = ImGui::Shortcut(SelectAllChord, route);

		// Cut, Copy and Paste mean nothing to the scene.
		if (cut)
		{
			ToGraphTarget(visualScripts, VisualScriptEditorAction::Cut);
		}
		if (copy)
		{
			ToGraphTarget(visualScripts, VisualScriptEditorAction::Copy);
		}
		if (paste)
		{
			ToGraphTarget(visualScripts, VisualScriptEditorAction::Paste);
		}
		if (duplicate && !ToGraphTarget(visualScripts, VisualScriptEditorAction::Duplicate))
		{
			DuplicateSelectedEntities(operations);
		}
		if ((deleteKey || deleteBackspace) && !ToGraphTarget(visualScripts, VisualScriptEditorAction::Delete))
		{
			DeleteSelectedEntities(operations);
		}
		if (selectAll && !ToGraphTarget(visualScripts, VisualScriptEditorAction::SelectAll))
		{
			SelectAllEntities(operations);
		}
	}

	std::vector<EditorMenuItemRecord> DrawEditorEditingMenuItems(EditorOperations& operations, VisualScriptEditors* visualScripts,
	                                                             std::span<ImGuiKeyChord const> claimed)
	{
		if (visualScripts != nullptr)
		{
			if (std::optional<std::vector<EditorMenuItemRecord>> drawn = visualScripts->DrawEditMenuItems(claimed))
			{
				return std::move(*drawn);
			}
		}

		std::vector<EditorMenuItemRecord> drawn;
		bool const hasSelection = !operations.GetContext().GetSelection().IsEmpty();
		auto const item = [&drawn](char const* label, std::string shortcut, bool enabled)
		{
			bool const chosen = ImGui::MenuItem(label, shortcut.c_str(), false, enabled);
			drawn.push_back({label, std::move(shortcut), enabled});
			return chosen;
		};
		if (item("Duplicate", GetEditShortcutText(DuplicateChord, claimed), hasSelection))
		{
			DuplicateSelectedEntities(operations);
		}
		if (item("Delete", GetEditShortcutText(DeleteChord, claimed), hasSelection))
		{
			DeleteSelectedEntities(operations);
		}
		ImGui::Separator();
		if (item("Select All", GetEditShortcutText(SelectAllChord, claimed), true))
		{
			SelectAllEntities(operations);
		}
		if (item("Deselect All", std::string(), hasSelection))
		{
			operations.GetContext().ClearSelection();
		}
		return drawn;
	}
}
