#pragma once

#include "Editor/AssetThumbnails.h"
#include "Editor/EditorOperations.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/UI/FieldEditor.h"

#include "Strada/Asset/Asset.h"
#include "Strada/Core/UUID.h"
#include "Strada/Scene/ComponentRegistry.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Strada
{
	// Edits the selected entities: name, the components they all have (fields generated from the component registry, one
	// undo step per widget interaction; Script components get a class picker and the fields of their script class; Mesh
	// components a section per material slot with the material's parameters), and adding (a searchable menu of the
	// component categories), resetting, copying, pasting and removing components. With several entities selected, the
	// primary entity's values are shown, fields whose values differ are flagged, and an edit changes only the edited value
	// (or vector component) on every selected entity. Without selected entities it shows the asset selected in the content
	// browser: its details, the parameters of material files (editable, one undo step per interaction; built-in and
	// imported materials are read-only and can be copied to a material file), a texture preview and mesh statistics. Main
	// thread only.
	class InspectorPanel
	{
	public:
		InspectorPanel();

		void OnImGuiRender(EditorOperations& operations, bool& open);
		// In the next frame, brings the inspector to the front with its Add Component menu open and the search text typed in
		// (as if the user clicked Add Component and typed), or closes the menu (open false). Without selected entities in that
		// frame nothing opens.
		void ShowAddComponentMenu(bool open, std::string search);

	private:
		// The selected entities, or the selected asset.
		void DrawContents(EditorOperations& operations, std::optional<bool> menuRequest);
		void DrawHeader(EditorOperations& operations, std::vector<UUID> const& entities);
		void DrawComponent(EditorOperations& operations, ComponentInfo const& component, std::vector<UUID> const& entities);
		void DrawComponentMenu(EditorOperations& operations, ComponentInfo const& component, std::vector<UUID> const& entities,
		                       Json const& primaryValues);
		// The class picker and the fields of the script class (from the loaded game assembly).
		void DrawScriptComponent(EditorOperations& operations, ComponentInfo const& component, std::vector<UUID> const& entities,
		                         std::vector<Json> const& values);
		// The Add Component button and its menu: a search field above submenus per category; typing lists the matches, and
		// Enter adds the first one. `request` opens (true) or closes (false) the menu this frame.
		void DrawAddComponent(EditorOperations& operations, std::vector<UUID> const& entities, std::optional<bool> request);
		// Applies the same partial patch to the component of every entity as one undo step.
		void ApplyPatch(EditorOperations& operations, ComponentInfo const& component, std::vector<UUID> const& entities, Json const& patch,
		                uint64_t mergeKey);

		// One section per material slot of the entities' meshes, with the parameters of the material the slot renders with.
		void DrawMeshMaterials(EditorOperations& operations, std::vector<UUID> const& entities);
		// A material section's header: a preview of the slot's material (invalid: the entities use several) beside its name,
		// the slot and the material's path. Returns whether the section is open.
		bool DrawMaterialHeader(AssetHandle material, size_t slot, size_t slotCount);
		// A slot's material: an asset field choosing it ("x" gives the slot the mesh's own material back), then the editable
		// parameters of material files; other materials read-only with a Create Material button that gives the slot an
		// editable copy.
		void DrawSlotMaterial(EditorOperations& operations, std::vector<UUID> const& entities, size_t slot, AssetHandle material);
		// The asset field of a slot's material (invalid: the entities use several), lined up with the material's fields.
		void DrawSlotMaterialReference(EditorOperations& operations, std::vector<UUID> const& entities, size_t slot, AssetHandle material);

		void DrawAsset(EditorOperations& operations, AssetHandle asset);
		void DrawMaterial(EditorOperations& operations, AssetMetadata const& metadata);
		// The parameters of a material: editable for material files (one undo step per interaction), disabled otherwise.
		void DrawMaterialFields(EditorOperations& operations, AssetHandle material);
		void DrawMesh(EditorOperations& operations, AssetHandle asset);

		FieldEditor m_FieldEditor;
		std::vector<FieldDescriptor> m_MaterialFields;
		// Material previews, kept for the project they were made for.
		AssetThumbnails m_Previews;
		std::filesystem::path m_PreviewProject;
		EditSession m_EditSession;
		// Search text of the Add Component menu; while it is empty the menu shows the categories.
		std::string m_AddComponentFilter;
		// Gives the menu's search field the keyboard again next frame (Enter without a match ended the input).
		bool m_FocusComponentSearch = false;
		// ShowAddComponentMenu's request for the next frame: open (true) or close (false) the menu.
		std::optional<bool> m_AddComponentMenuRequest;
		// Component removals requested while the components are drawn.
		std::vector<std::function<void()>> m_Deferred;
	};
}
