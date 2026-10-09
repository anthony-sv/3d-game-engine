#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <glm/glm.hpp>

#include <span>
#include <string_view>

namespace Strada
{
	class EditorOperations;

	// A kind of entity offered by the editor's create menus (empty entity, primitives, lights, camera, ...).
	struct EntityPreset
	{
		// Submenu ("3D Object", "Light", ...); empty for top-level entries.
		std::string_view Category;
		// Menu label.
		std::string_view Label;
		// Name of the created entity.
		std::string_view EntityName;
		// The components of the new entity in the scene-file format.
		Json (*BuildComponents)();
	};

	namespace EntityPresets
	{
		// In menu order; entries of a category are contiguous.
		std::span<EntityPreset const> GetAll();
		// Creates a preset entity under parent (invalid = root) as one undo step. Root entities are placed at `position`
		// (for example the viewport's focal point); children start at their parent's origin.
		[[nodiscard]] Result<UUID> Create(EditorOperations& operations, EntityPreset const& preset, UUID parent, glm::vec3 const& position);
		// Creates an entity that draws a mesh asset, named after it, placed like preset entities.
		[[nodiscard]] Result<UUID> CreateFromMesh(EditorOperations& operations, AssetHandle mesh, UUID parent, glm::vec3 const& position);
	}
}
