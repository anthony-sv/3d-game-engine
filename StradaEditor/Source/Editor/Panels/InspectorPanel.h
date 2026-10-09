#pragma once

#include "Editor/EditorOperations.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/UI/FieldEditor.h"

#include "Strada/Core/UUID.h"
#include "Strada/Scene/ComponentRegistry.h"

#include <functional>
#include <string>
#include <vector>

namespace Strada
{
	// Edits the selected entities: name, the components they all have (fields generated from the component registry, one
	// undo step per widget interaction), and adding, resetting, copying, pasting and removing components. With several
	// entities selected, the primary entity's values are shown, fields whose values differ are flagged, and an edit
	// changes only the edited value (or vector component) on every selected entity. Main thread only.
	class InspectorPanel
	{
	public:
		InspectorPanel();

		void OnImGuiRender(EditorOperations& operations, bool& open);

	private:
		void DrawHeader(EditorOperations& operations, std::vector<UUID> const& entities);
		void DrawComponent(EditorOperations& operations, ComponentInfo const& component, std::vector<UUID> const& entities);
		void DrawComponentMenu(EditorOperations& operations, ComponentInfo const& component, std::vector<UUID> const& entities,
		                       Json const& primaryValues);
		void DrawAddComponent(EditorOperations& operations, std::vector<UUID> const& entities);
		// Applies the same partial patch to the component of every entity as one undo step.
		void ApplyPatch(EditorOperations& operations, ComponentInfo const& component, std::vector<UUID> const& entities, Json const& patch,
		                uint64_t mergeKey);

		FieldEditor m_FieldEditor;
		EditSession m_EditSession;
		std::string m_AddComponentFilter;
		// Component removals requested while the components are drawn.
		std::vector<std::function<void()>> m_Deferred;
	};
}
