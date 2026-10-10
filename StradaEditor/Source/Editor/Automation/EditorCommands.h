#pragma once

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/EditorOperations.h"

#include "Strada/Core/Image.h"
#include "Strada/Core/Result.h"

#include <functional>

namespace Strada
{
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
	};

	// Registers the editor, project, scene, entity, component, asset, material, prefab, log and viewport commands. Entity parameters are
	// UUIDs written as decimal strings. The registry, the operations (and their context) and the environment callbacks must outlive every
	// registered command. Documented in Docs/Automation.md.
	[[nodiscard]] Result<void> RegisterEditorCommands(CommandRegistry& registry, EditorOperations& operations,
	                                                  EditorCommandEnvironment environment);
}
