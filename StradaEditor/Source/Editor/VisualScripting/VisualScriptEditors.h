#pragma once

#include "Editor/EditorEditingKeys.h"
#include "Editor/Panels/VisualScriptEditorPanel.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/VisualScripting/VisualScriptEditorModel.h"
#include "Editor/VisualScripting/VisualScriptEditorTypes.h"

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Strada
{
	class EditorOperations;
	class Project;
	class VisualScriptSemantics;
	class VisualScriptViewSettings;

	// The open graph windows (spec §12): one VisualScriptEditorPanel per open visual script, in the order they were opened,
	// named "<title>###VisualScript<handle>" so renames and moves keep each window and its dock node. The windows share the
	// operations, the semantics, the per-user view settings, the services and one edit session (one widget interaction is
	// one undo step, whichever window it is in).
	//
	// Each frame, before any window draws, it follows the project (a change flushes the windows' pending edits, saves the
	// old project's open windows and views, closes every window and opens the new project's windows again), closes the
	// windows of visual scripts that were deleted, ends the edit session's step once the active item changed (spec §4.4
	// rule 2), opens the windows other windows asked for, and keeps the edit target: the graph window focused last while it
	// is shown, until "Scene View", "Scene Hierarchy" or "Inspector" is focused (spec §8.4). A window that opens for the first
	// time in a session docks beside the graph window focused last, else into the default dock node (the scene view's),
	// else floats in the middle of the main viewport.
	//
	// Only OnImGuiRender, DrawEditMenuItems, DrawViewMenuItems and ResetLayout call ImGui: everything else records or reads
	// state, so automation opens, closes and describes windows between frames and without an ImGui context. Those calls
	// first close the windows of graphs no longer registered, so a graph deleted between frames is never reported or
	// reached. The views file is written when windows close, when the project changes and at exit (SaveViews), never per
	// frame. Main thread only.
	class VisualScriptEditors
	{
	public:
		// The session key prefix every graph window commits under ("VSED").
		static constexpr uint64_t SessionKeyPrefix = 0x5653454400000000ull;
		// A window that has nowhere to dock floats in the middle of the main viewport, this share of its work size.
		static constexpr float FloatingShare = 0.7f;

		VisualScriptEditors(EditorOperations& operations, VisualScriptSemantics& semantics, VisualScriptViewSettings& settings);
		~VisualScriptEditors();
		VisualScriptEditors(VisualScriptEditors const&) = delete;
		VisualScriptEditors& operator=(VisualScriptEditors const&) = delete;

		// The services every window and pane uses (the monospace font, the console callback); the editor sets them once it has
		// its fonts, tests keep the defaults. The windows keep a reference to the shell's copy, so they see the new ones.
		void SetServices(VisualScriptEditorServices services);
		VisualScriptEditorServices const& GetServices() const { return m_Services; }

		// Opens the graph's window, or applies the request to the window already open (with Focus, it comes to the front at
		// its next draw). Checked now, applied at the window's next draw: the asset must be a registered visual script; when its
		// document loads, the function shown and the selected nodes and comments must exist in it (the sub-graph shown is the
		// request's, else the window's, else the one the views file keeps); the context entity must be in the scene. A file
		// that is missing or does not load opens too, showing its card: the request's function, selection, framing and find
		// cannot be checked then and are left out. Returns the window's state as its last draw saw it (a new window's: not
		// visible, not focused), but for Function: the sub-graph the window shows once its next draw applied the request.
		[[nodiscard]] Result<VisualScriptEditorState> Open(VisualScriptOpenRequest const& request);
		// Closes the graph's window, or every window when none is given; closing a graph that is not open is no error. Each
		// window commits its pending widget edits first, and the views file is written (as a window's X does). Returns the
		// graphs closed, in tab order.
		std::vector<AssetHandle> Close(std::optional<AssetHandle> graph = std::nullopt);
		// The open windows in the order they were opened, each as its last draw saw it.
		std::vector<VisualScriptEditorState> Describe();
		// The sub-graph the graph's window shows, or shows when it opens without asking for one: the open window's, else the one
		// the views file keeps, else 0 (the event graph). A function the document no longer has shows the event graph.
		uint32_t GetShownFunction(AssetHandle graph);
		// Whether the windows have a node library (the scripting runtime's): the node palette needs it.
		bool HasNodeLibrary();
		// Calls back once the next OnImGuiRender has drawn every window, in the order given (graph.palette reads what a window's
		// palette showed in that draw). A callback given while they run waits for the draw after. Callbacks still waiting when
		// the windows are destroyed are dropped uncalled.
		void AfterNextDraw(std::function<void()> callback);
		// The graph's window; null when it is not open.
		VisualScriptEditorPanel* FindPanel(AssetHandle graph);
		// The window the Edit menu acts on (spec §8.4): the graph window focused last while its tab is in front; null while the
		// scene is the target, a scene window ("Scene View", "Scene Hierarchy", "Inspector") was focused after it, or its
		// window is hidden.
		VisualScriptEditorPanel* GetEditTarget();
		// Where windows dock when no graph window is docked: the editor gives the scene view's dock node.
		void SetDefaultDockProvider(std::function<std::optional<ImGuiID>()> provider) { m_DefaultDock = std::move(provider); }
		// An editing key the editor's global routes received (EditorEditingKeys: Cut, Copy, Paste, Duplicate, Delete, Select
		// All), pressed while no graph window had the keyboard: the edit target takes it as its Edit menu item's action
		// (RequestAction), returning true even when it has nothing to act on, so the key never reaches the entities. False,
		// doing nothing, while the scene is the target.
		bool HandleGlobalEditingKey(VisualScriptEditorAction action);
		// After an undo or redo, with the step's description and edited asset as they were before it ran: the edit target
		// shows the history toast (spec §10.4); nothing without one.
		void NotifyHistoryStep(bool undone, std::string const& description, AssetHandle editedAsset);

		// Draws every window (spec §0.4 step 1). Called by the editor after its fixed panels, before its modal popups.
		void OnImGuiRender();
		// The Edit menu's items for the edit target's selection (Cut, Copy, Paste, Duplicate, Delete, Select All, Deselect
		// All), enabled by it, each asking the window for the action at its next draw. Each shows its key's text but for the
		// claimed chords, which the window that has the keyboard keeps for itself (GetEditShortcutText). Returns what it drew,
		// in order; nothing, drawing nothing, while the scene is the target. Inside a menu.
		std::optional<std::vector<EditorMenuItemRecord>> DrawEditMenuItems(std::span<ImGuiKeyChord const> claimed);
		// The View menu's "Visual Scripts" submenu: the open graphs, checked (unchecking closes one), and Close All; disabled
		// while none is open. Inside a menu.
		void DrawViewMenuItems();
		// Commits every window's pending widget edits (the editor's exit, a project change).
		void Flush();
		// Writes the views file with the current project's open windows, in tab order. Failures are logged.
		void SaveViews();
		// View > Reset Layout: docks every open window into the node (the layout's centre). Inside the dock builder's edit,
		// before DockBuilderFinish.
		void ResetLayout(ImGuiID centerNode);

	private:
		struct GraphWindow
		{
			Scope<VisualScriptEditorPanel> Panel;
			// The asset's path as last seen (a memory asset's name), which says what was closed once the asset is gone.
			std::string Path;
			bool HasFile = false;
			// The title the window shows: the file's stem, with its folder when another open graph has the same stem.
			std::string Title;
		};

		// What every call does first: follow the project, then close the windows of deleted visual scripts (spec §12.4).
		void Maintain();
		void FollowProject();
		void CloseDeletedGraphs();
		// The edit target and the order graph windows were focused in, from the window that has the keyboard.
		void UpdateFocus();
		void UpdateTitles();
		// Ends the edit session's step once no item is active, or another item is, than in the previous frame (spec §4.4 rule
		// 2), committing the pending edits of widgets that are no longer active first.
		void EndInteractions();
		void OpenOtherGraphRequests();
		// Where a window that does not exist yet docks: beside the graph window focused last, else the default node; none
		// floats.
		std::optional<ImGuiID> FindDockTarget() const;
		GraphWindow* FindGraphWindow(AssetHandle graph);
		// The window's path from the asset's metadata, while the asset is registered.
		static void RefreshPath(GraphWindow& window);
		// Removes the windows at the indices (ascending), committing their pending edits first.
		void RemoveWindows(std::vector<size_t> const& indices);

		EditorOperations& m_Operations;
		VisualScriptSemantics& m_Semantics;
		VisualScriptViewSettings& m_Settings;
		VisualScriptEditorServices m_Services;
		std::function<std::optional<ImGuiID>()> m_DefaultDock;
		// In tab-open order.
		std::vector<GraphWindow> m_Windows;
		// The session every window shares, and the item that was active in the previous frame (spec §4.4 rule 2).
		EditSession m_EditSession{SessionKeyPrefix};
		ImGuiID m_LastActiveItem = 0;
		// The project the windows belong to, as the last check saw it.
		Project const* m_Project = nullptr;
		std::filesystem::path m_ProjectFile;
		// The graph window focused last (none: the scene), and the graphs whose windows were focused, the most recent first.
		std::optional<AssetHandle> m_EditTarget;
		std::vector<AssetHandle> m_FocusOrder;
		// What AfterNextDraw was given, called at the end of the next OnImGuiRender.
		std::vector<std::function<void()>> m_AfterDraw;
	};
}
