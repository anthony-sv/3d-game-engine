#pragma once

#include "Editor/Agents/AgentKit.h"
#include "Editor/Automation/CommandRegistry.h"
#include "Editor/EditorLocations.h"
#include "Editor/EditorOperations.h"
#include "Editor/UI/WindowCapture.h"

#include "Strada/Asset/AssetHandle.h"

#include "Strada/Core/Image.h"
#include "Strada/Core/Result.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Strada
{
	class ContentBrowserPanel;
	class EditorCamera;
	struct SceneRendererStatistics;
	struct SceneViewOverlays;
	class VisualScriptDebuggerPanel;
	class VisualScriptEditors;
	class VisualScriptLibrary;
	struct VisualScriptDocument;

	// The windows viewport.screenshot cuts out of the editor's image (Editor: the whole window).
	enum class ScreenshotWindow : uint8_t
	{
		Editor = 0,
		SceneView,
		GameView,
		Hierarchy,
		Inspector,
		ContentBrowser,
		Console,
		Debugger,
		// A graph window: the one of the screenshot's graph.
		Graph
	};

	// viewport.screenshot's name of a window ("SceneView"), and the window of a name; nothing for other names.
	[[nodiscard]] std::string_view GetScreenshotWindowName(ScreenshotWindow window);
	[[nodiscard]] std::optional<ScreenshotWindow> ParseScreenshotWindow(std::string_view name);

	// What the automation commands need from the running editor besides the document state.
	struct EditorCommandEnvironment
	{
		using ScreenshotCallback = std::function<void(Result<Image>)>;

		// Captures the main window and calls back on the main thread. Unset when the editor has no window (headless);
		// viewport.screenshot then fails with AutomationErrorCode::Unavailable.
		std::function<void(ScreenshotCallback callback)> CaptureScreenshot;
		// Where an editor window lies in the image CaptureScreenshot gives (FindEditorWindowRect), asked in the capture's
		// callback, after ImGui drew the frame captured; graph names the graph window's visual script (Graph only). Fails with a
		// message naming the window when it cannot be cut out (not open, behind another tab, in a window of its own, no area).
		// Unset when the editor has no UI (headless); viewport.screenshot of a window other than Editor is then Unavailable.
		std::function<Result<ScreenshotRect>(ScreenshotWindow window, AssetHandle graph)> FindWindowRect;
		// Asks the editor to exit after the current frame. Unset when the host cannot quit (tests).
		std::function<void()> RequestQuit;
		// Called after a project was created or opened (the editor lists it under its recent projects). May be unset.
		std::function<void()> ProjectOpened;
		// The camera of the editor's viewport. Unset when the editor has no viewport (headless); viewport.camera and
		// viewport.frame then fail with AutomationErrorCode::Unavailable.
		std::function<EditorCamera&()> ViewportCamera;
		// What the viewport draws over the scene. Unset when the editor has no viewport (headless); viewport.overlays then
		// fails with AutomationErrorCode::Unavailable.
		std::function<SceneViewOverlays&()> ViewportOverlays;
		// The statistics of the last frame the viewport rendered, null when it has no renderer (no GPU). Unset when the
		// editor has no viewport (headless). viewport.statistics fails with AutomationErrorCode::Unavailable in both cases.
		std::function<SceneRendererStatistics const*()> ViewportStatistics;
		// The content browser panel. Unset when the editor has no UI (headless); asset.browse then fails with
		// AutomationErrorCode::Unavailable.
		std::function<ContentBrowserPanel&()> ContentBrowser;
		// Whether a dialog (a modal popup such as the unsaved changes prompt) is open: menus do not open over it, as users
		// cannot open them then either. May be unset (the editor shows no dialogs).
		std::function<bool()> IsDialogOpen;
		// Shows the inspector with its Add Component menu open and the search text typed in or, without search text, the
		// submenu of a component category open (none when empty), or closes the menu (open false). Unset when the editor has
		// no UI (headless); inspector.add-component-menu then fails with AutomationErrorCode::Unavailable.
		std::function<void(bool open, std::string const& search, std::string const& category)> ShowAddComponentMenu;
		// Opens the menu bar's Entity menu with the submenu of an entity preset category open (none when empty), or closes
		// it (open false). Unset when the editor has no UI (headless); editor.entity-menu then fails with
		// AutomationErrorCode::Unavailable.
		std::function<void(bool open, std::string const& submenu)> ShowEntityMenu;
		// The visual script editor windows. Unset when the editor has no UI (headless); graph.open, graph.close, graph.editors
		// and graph.palette then fail with AutomationErrorCode::Unavailable.
		std::function<VisualScriptEditors&()> GraphEditors;
		// The visual-script Debugger panel. Unset when the editor has no UI (headless); graph.debug-panel then fails with
		// AutomationErrorCode::Unavailable, and graph.editors reports debugPanel as null.
		std::function<VisualScriptDebuggerPanel&()> DebuggerPanel;
		// The folders the commands work with besides the open project: the engine content, the editor's user data and the
		// default location of new projects (the editor's are the process's, EditorLocations::FromProcess). Default-constructed
		// it names no folder (tests pass folders of their own, Testing::MakeEditorLocations).
		EditorLocations Locations;
		// Finds the strada tool on PATH (the editor's: Process::FindExecutable("strada")), which decides whether the .mcp.json
		// that project.setup-agents writes names strada or its path (AgentKit::ResolveTool). Unset finds nothing, so that
		// tests never depend on what the machine has.
		AgentToolLookup FindToolOnPath;
	};

	// Registers the editor, project (with the agent kit's, RegisterAgentCommands), scene, entity, component, asset, material, graph
	// (visual script), prefab, inspector, log and viewport commands. Entity parameters are UUIDs written as decimal strings. The registry,
	// the operations (and their context) and the environment callbacks must outlive every registered command. Documented in
	// Docs/Automation.md.
	[[nodiscard]] Result<void> RegisterEditorCommands(CommandRegistry& registry, EditorOperations& operations,
	                                                  EditorCommandEnvironment environment);

	// The open project as the project commands describe it ({ name, file, assetDirectory }; null when none is open), and the
	// edited scene ({ name, path, dirty, entityCount }).
	[[nodiscard]] Json DescribeEditorProject(EditorContext const& context);
	[[nodiscard]] Json DescribeEditorScene(EditorContext const& context);

	// graph.library's notes: the library's notes (the game's [VisualNode] members without a node) whose category is the given
	// one or lies below it (any for an empty one) and whose member or message contains the query (any for an empty one), both
	// ignoring the case of ASCII letters, in library order: [{ member, category, message }].
	[[nodiscard]] Json ListVisualScriptLibraryNotes(VisualScriptLibrary const& library, std::string_view query, std::string_view category);

	// The programRevision the graph commands give a document: 16 lowercase hex digits of the FNV-1a hash of what
	// HaveSameVisualScriptProgram compares, so moves, comments, arranging and collapsing leave it unchanged.
	[[nodiscard]] std::string GetVisualScriptProgramRevision(VisualScriptDocument const& document);
}
