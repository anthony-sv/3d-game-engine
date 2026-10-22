#pragma once

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/EditorOperations.h"

#include "Strada/Core/Image.h"
#include "Strada/Core/Result.h"

#include <functional>
#include <string>

namespace Strada
{
	class ContentBrowserPanel;
	class EditorCamera;
	struct SceneRendererStatistics;
	struct SceneViewOverlays;

	// What the automation commands need from the running editor besides the document state.
	struct EditorCommandEnvironment
	{
		using ScreenshotCallback = std::function<void(Result<Image>)>;

		// Captures the main window and calls back on the main thread. Unset when the editor has no window (headless);
		// viewport.screenshot then fails with AutomationErrorCode::Unavailable.
		std::function<void(ScreenshotCallback callback)> CaptureScreenshot;
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
	};

	// Registers the editor, project, scene, entity, component, asset, material, graph (visual script), prefab, inspector, log and
	// viewport commands. Entity parameters are UUIDs written as decimal strings. The registry, the operations (and their context) and
	// the environment callbacks must outlive every registered command. Documented in Docs/Automation.md.
	[[nodiscard]] Result<void> RegisterEditorCommands(CommandRegistry& registry, EditorOperations& operations,
	                                                  EditorCommandEnvironment environment);
}
