#pragma once

#include "Editor/EditorOperations.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/UI/FieldEditor.h"

#include "Strada/Asset/Asset.h"
#include "Strada/Core/UUID.h"
#include "Strada/Scene/ComponentRegistry.h"

#include <functional>
#include <string>
#include <vector>

namespace Strada
{
	// Edits the selected entities: name, the components they all have (fields generated from the component registry, one
	// undo step per widget interaction; Script components get a class picker and the fields of their script class), and
	// adding, resetting, copying, pasting and removing components. With several
	// entities selected, the primary entity's values are shown, fields whose values differ are flagged, and an edit
	// changes only the edited value (or vector component) on every selected entity. Without selected entities it shows the
	// asset selected in the content browser: its details, the parameters of material files (editable, one undo step per
	// interaction; built-in and imported materials are read-only), a texture preview and mesh statistics. Main thread only.
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
		// The class picker and the fields of the script class (from the loaded game assembly).
		void DrawScriptComponent(EditorOperations& operations, ComponentInfo const& component, std::vector<UUID> const& entities,
		                         std::vector<Json> const& values);
		void DrawAddComponent(EditorOperations& operations, std::vector<UUID> const& entities);
		// Applies the same partial patch to the component of every entity as one undo step.
		void ApplyPatch(EditorOperations& operations, ComponentInfo const& component, std::vector<UUID> const& entities, Json const& patch,
		                uint64_t mergeKey);

		void DrawAsset(EditorOperations& operations, AssetHandle asset);
		void DrawMaterial(EditorOperations& operations, AssetMetadata const& metadata);
		void DrawMesh(EditorOperations& operations, AssetHandle asset);

		FieldEditor m_FieldEditor;
		std::vector<FieldDescriptor> m_MaterialFields;
		EditSession m_EditSession;
		std::string m_AddComponentFilter;
		// Component removals requested while the components are drawn.
		std::vector<std::function<void()>> m_Deferred;
	};
}
