#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <glm/glm.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace Strada
{
	class EditorOperations;

	// A kind of entity offered by the editor's create menus (empty entity, primitives, lights, camera, ...).
	struct EntityPreset
	{
		// One of EntityPresets::GetCategories().
		std::string_view Category;
		// Menu label.
		std::string_view Label;
		// Name of the created entity.
		std::string_view EntityName;
		// The components of the new entity in the scene-file format.
		Json (*BuildComponents)();
		// A UI element, which only shows inside a canvas (see EntityPresets::Create).
		bool NeedsCanvas = false;
	};

	// A part of the create menus: the presets of one category, in a submenu or (Core) directly in the menu.
	struct EntityPresetMenuSection
	{
		std::string_view Category;
		bool Submenu = true;
		std::vector<EntityPreset const*> Presets;
	};

	namespace EntityPresets
	{
		// The category whose presets (Create Empty) come first, directly in the menu.
		inline constexpr std::string_view CoreCategory = "Core";

		// In menu order; entries of a category are contiguous.
		std::span<EntityPreset const> GetAll();
		// Core, 3D Object, 2D Object, Rendering, Effects, Physics, Audio and UI.
		std::span<std::string_view const> GetCategories();
		// The create menus (the Entity menu, the hierarchy's create button and Create Child): a section per category in
		// GetCategories() order, each with its presets in GetAll() order.
		std::vector<EntityPresetMenuSection> GetMenu();
		// Creates a preset entity under parent (invalid = root) as one undo step. Root entities are placed at `position`
		// (for example the viewport's focal point); children start at their parent's origin. UI elements outside a canvas
		// go into one: under a parent, a new canvas there; at the root, the scene's first canvas, or a new one at `position`.
		[[nodiscard]] Result<UUID> Create(EditorOperations& operations, EntityPreset const& preset, UUID parent, glm::vec3 const& position);
		// Creates an entity that draws a mesh asset, named after it, placed like preset entities.
		[[nodiscard]] Result<UUID> CreateFromMesh(EditorOperations& operations, AssetHandle mesh, UUID parent, glm::vec3 const& position);
	}
}
