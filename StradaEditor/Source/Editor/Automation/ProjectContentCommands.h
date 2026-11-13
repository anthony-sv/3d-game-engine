#pragma once

#include "Editor/Automation/CommandRegistry.h"
#include "Editor/EditorLocations.h"
#include "Editor/EditorOperations.h"
#include "Editor/ExampleCopyTask.h"

#include "Strada/Core/Result.h"

#include <functional>

namespace Strada
{
	// What the project content commands give a template and an example (project.templates, project.examples): { id, name,
	// description, order, tags, requires: { sdk }, thumbnail } and { id, name, description, order, requires: { sdk },
	// thumbnail, available, hint (when not available) }, thumbnails as absolute paths or null. The start screen lists the
	// same.
	[[nodiscard]] Json DescribeProjectTemplate(ProjectTemplate const& projectTemplate);
	[[nodiscard]] Json DescribeExample(ExampleCatalog const& catalog, ExampleEntry const& example);

	// Registers project.templates, project.examples (read-only) and project.copy-example (asynchronous: the copy runs on the
	// task, which the caller updates every frame, and the copy opens on the main thread). The engine content comes from the
	// locations' layout; projectOpened (may be unset) is called after a copy opened, as EditorCommandEnvironment::ProjectOpened
	// is. The registry, the operations and the task must outlive every registered command. Documented in Docs/Automation.md.
	[[nodiscard]] Result<void> RegisterProjectContentCommands(CommandRegistry& registry, EditorOperations& operations,
	                                                          ExampleCopyTask& copies, EditorLocations locations,
	                                                          std::function<void()> projectOpened);
}
