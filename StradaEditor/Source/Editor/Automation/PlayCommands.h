#pragma once

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/EditorContext.h"
#include "Editor/PlayMode.h"

#include "Strada/Core/Result.h"

#include <glm/glm.hpp>

#include <functional>

namespace Strada
{
	// The size in pixels of the view the game renders to: the editor's viewport, or the project's window when headless.
	using ViewportSizeProvider = std::function<glm::uvec2()>;

	// Registers the play mode, input and test commands: play.start (asynchronous), play.stop, play.pause, play.step,
	// play.advance, play.state, input.set, input.release and test.run (asynchronous). Everything passed in must outlive
	// every registered command. Documented in Docs/Automation.md.
	[[nodiscard]] Result<void> RegisterPlayCommands(CommandRegistry& registry, PlayMode& playMode, EditorContext& context,
	                                                ViewportSizeProvider viewportSize);
}
