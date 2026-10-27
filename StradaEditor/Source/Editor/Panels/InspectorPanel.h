#pragma once

#include "Editor/EditorOperations.h"
#include "Editor/UI/EditorUI.h"
#include "Editor/UI/FieldEditor.h"

#include "Strada/Asset/Asset.h"
#include "Strada/Core/UUID.h"
#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Script/VisualScriptVariables.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Strada
{
	class AssetThumbnails;

	// Edits the selected entities: name, the components they all have (fields generated from the component registry, one
	// undo step per widget interaction; Script components get a class picker and the fields of their script class; Mesh
	// components a section per material slot with the material's parameters; VisualScript components the values they give
	// the public variables of their graph), and adding (a searchable menu of the component categories), resetting, copying,
	// pasting and removing components. With several entities selected, the primary entity's values are shown, fields whose
	// values differ are flagged, and an edit changes only the edited value (or vector component) on every selected entity.
	// C# scripts and visual scripts dropped onto the panel run on the selected entities. Without selected entities it shows
	// the asset selected in the content browser: its details, the parameters of material files (editable, one undo step per
	// interaction; built-in and imported materials are read-only and can be copied to a material file), a texture preview,
	// mesh statistics and what a visual script holds (read-only). Images of assets (in asset fields, the asset picker's rows
	// and the material headers and previews) come from SetThumbnails. Visual scripts open in their graph windows from the
	// asset's Open and the component's Open Visual Script; entities whose VisualScript components run no graph get a Create
	// Visual Script button that writes one for them (SetOpenVisualScriptCallback). Main thread only.
	class InspectorPanel
	{
	public:
		InspectorPanel();

		// The images of assets: the editor's set, which the content browser shares and the editor updates once per frame
		// before the panels draw. Not owned; without them asset fields show paths only.
		void SetThumbnails(AssetThumbnails* thumbnails);
		// Tells the enum types of the scripting runtime, so public variables of enum types can be set; without it (the
		// default) they are listed read-only.
		void SetVisualScriptEnumLookup(VisualScriptEnumLookup lookup) { m_VisualScriptEnumLookup = std::move(lookup); }
		// Opens a visual script's graph window with the entity it runs on (invalid: none): the asset's Open, the component's
		// Open Visual Script, and the graph Create Visual Script makes. Without it no Open buttons are drawn and the graph made
		// is not opened.
		using OpenVisualScriptCallback = std::function<void(AssetHandle graph, UUID contextEntity)>;
		void SetOpenVisualScriptCallback(OpenVisualScriptCallback callback) { m_OpenVisualScript = std::move(callback); }

		void OnImGuiRender(EditorOperations& operations, bool& open);
		// In the next frame, brings the inspector to the front with its Add Component menu open and the search text typed in
		// (as if the user clicked Add Component and typed) or, without search text, the submenu of a component category open
		// (none when empty; as if the user pointed at it), or closes the menu (open false). Without selected entities in that
		// frame nothing opens.
		void ShowAddComponentMenu(bool open, std::string search, std::string category = {});

	private:
		// What ShowAddComponentMenu asks of the next frame.
		struct AddComponentMenuRequest
		{
			// Open the menu, or close it.
			bool Open = false;
			// The category whose submenu opens (none when empty).
			std::string Category;
		};

		// The selected entities, or the selected asset.
		void DrawContents(EditorOperations& operations, std::optional<AddComponentMenuRequest> const& menuRequest);
		void DrawHeader(EditorOperations& operations, std::vector<UUID> const& entities);
		void DrawComponent(EditorOperations& operations, ComponentInfo const& component, std::vector<UUID> const& entities);
		void DrawComponentMenu(EditorOperations& operations, ComponentInfo const& component, std::vector<UUID> const& entities,
		                       Json const& primaryValues);
		// The class picker and the fields of the script class (from the loaded game assembly).
		void DrawScriptComponent(EditorOperations& operations, ComponentInfo const& component, std::vector<UUID> const& entities,
		                         std::vector<Json> const& values);
		// The Add Component button and its menu: a search field above submenus per category; typing lists the matches, and
		// Enter adds the first one. `request` opens (with a category's submenu) or closes the menu this frame.
		void DrawAddComponent(EditorOperations& operations, std::vector<UUID> const& entities,
		                      std::optional<AddComponentMenuRequest> const& request);
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

		// The values the entities give the public variables of the visual script they all run (one undo step per interaction,
		// each writing an entity's whole map with the values under the variables' current names), under an Open Visual Script
		// button; variables that cannot be set here are listed with the reason. Nothing when the entities run different graphs
		// or theirs does not load; Create Visual Script when none of them runs a graph.
		void DrawVisualScriptVariables(EditorOperations& operations, std::vector<UUID> const& entities);
		// Writes Assets/Scripts/<the first entity's name>.sgraph from the Events template (the name made a portable file name
		// that no file has), makes every entity run it in one undo step ("Create Visual Script"), and opens it with the first
		// entity as its context entity. The file stays when that step is undone, as created materials do.
		void CreateVisualScript(EditorOperations& operations, std::vector<UUID> const& entities);

		void DrawAsset(EditorOperations& operations, AssetHandle asset);
		void DrawMaterial(EditorOperations& operations, AssetMetadata const& metadata);
		// The parameters of a material: editable for material files (one undo step per interaction), disabled otherwise.
		void DrawMaterialFields(EditorOperations& operations, AssetHandle material);
		void DrawMesh(EditorOperations& operations, AssetHandle asset);

		FieldEditor m_FieldEditor;
		std::vector<FieldDescriptor> m_MaterialFields;
		AssetThumbnails* m_Thumbnails = nullptr;
		VisualScriptEnumLookup m_VisualScriptEnumLookup;
		OpenVisualScriptCallback m_OpenVisualScript;
		EditSession m_EditSession;
		// Search text of the Add Component menu; while it is empty the menu shows the categories.
		std::string m_AddComponentFilter;
		// Gives the menu's search field the keyboard again next frame (Enter without a match ended the input).
		bool m_FocusComponentSearch = false;
		// ShowAddComponentMenu's request for the next frame.
		std::optional<AddComponentMenuRequest> m_AddComponentMenuRequest;
		// Component removals requested while the components are drawn.
		std::vector<std::function<void()>> m_Deferred;
	};
}
