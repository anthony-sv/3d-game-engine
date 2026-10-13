#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strada
{
	class EditorOperations;
	class Scene;

	// The material slots of Mesh components as the inspector edits them, independent of the UI. A mesh has one slot per
	// material of its own; a Mesh component's Materials override them slot by slot.
	namespace MaterialSlots
	{
		// The folder of the asset directory that editable copies of materials go to.
		inline constexpr std::string_view CopyFolder = "Materials";

		// The material one slot of an entity's mesh renders with: the Mesh component's override, else the mesh's own material,
		// else the engine's default material. Invalid when the entity has no Mesh component, its mesh cannot be loaded or has
		// no such slot.
		AssetHandle GetMaterial(Scene& scene, UUID entity, size_t slot);
		// The materials the slots of the entities' meshes render with: one per slot of the first entity's mesh (none when it
		// has no loadable mesh), invalid where another entity renders the slot with another material or has no such slot.
		std::vector<AssetHandle> GetMaterials(Scene& scene, std::span<UUID const> entities);
		// Whether the editor can change the material's parameters: material files (.smat) can; built-in materials and the
		// materials of model files are recreated from their source, so they are read-only.
		bool IsEditable(AssetHandle material);
		// The material's name, which the inspector shows and copies of it get: the material file's name without its extension,
		// the material's own name for materials of model files ("Visor" for "Helmet.glb/Materials/Visor") and the name of
		// built-in materials. Empty for unregistered handles.
		std::string GetMaterialName(AssetHandle material);

		// Whether the Mesh component of one of the entities overrides the slot's material.
		bool IsOverridden(Scene& scene, std::span<UUID const> entities, size_t slot);

		// Makes the material override the slot on every entity's Mesh component, as one undo step; the other slots keep their
		// materials.
		[[nodiscard]] Result<void> Assign(EditorOperations& operations, std::span<UUID const> entities, size_t slot, AssetHandle material);
		// Gives the slot of every entity the mesh's own material back: clears the slot's override of each Mesh component that
		// has one (empty overrides at the end of the list go too), as one undo step; with no override nothing is recorded.
		// Fails, changing nothing, for entities without a mesh that has the slot.
		[[nodiscard]] Result<void> Clear(EditorOperations& operations, std::span<UUID const> entities, size_t slot);
		// Writes a material file with the parameters of a material to CopyFolder, named `name` (numbered when the name is
		// taken; "Material" when nothing of it can be used in a file name). Parameters outside the material ranges are
		// brought into them first (FitMaterialToFieldRanges, logged). Not undoable, like every file operation. Returns the
		// new material.
		[[nodiscard]] Result<AssetHandle> CreateCopy(EditorOperations& operations, AssetHandle material, std::string_view name);
		// Gives a slot an editable material: a copy of the material the entities render it with (CreateCopy, named after that
		// material, or after the first entity for built-in materials such as the engine's default one), assigned to the slot of
		// every entity as one undo step. The file is deleted again when the assignment fails. Returns the new material.
		[[nodiscard]] Result<AssetHandle> CreateEditableCopy(EditorOperations& operations, std::span<UUID const> entities, size_t slot);
	}
}
