#pragma once

#include "Strada/Core/UUID.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Strada
{
	class Scene;

	// Where entities dropped onto a row of the scene hierarchy go.
	enum class DropPosition : uint8_t
	{
		// Siblings right before the target.
		Before = 0,
		// Last children of the target.
		Inside,
		// Siblings right after the target.
		After
	};

	// Arguments of EditorOperations::MoveEntities.
	struct MoveTarget
	{
		// Invalid for root entities.
		UUID Parent = UUID::Invalid();
		// Invalid to append after the last child.
		UUID InsertBefore = UUID::Invalid();

		bool operator==(MoveTarget const& other) const = default;
	};

	// Scene hierarchy logic independent of the UI.
	namespace HierarchyEditing
	{
		// Where dropping the dragged entities relative to target puts them; empty when the drop is not allowed (an entity
		// onto itself or into its own subtree, or entities that do not exist).
		std::optional<MoveTarget> ResolveDrop(Scene& scene, std::span<UUID const> dragged, UUID target, DropPosition position);
		// The displayed entities from anchor to clicked (inclusive, in display order); only clicked when the anchor is not
		// displayed.
		std::vector<UUID> GetRange(std::span<UUID const> displayed, UUID anchor, UUID clicked);
		// Entities whose name contains the filter text (ignoring case), in hierarchy order.
		std::vector<UUID> FindByName(Scene& scene, std::string_view filter);
		// The ancestors of an entity, the root first; empty for root or unknown entities.
		std::vector<UUID> GetAncestors(Scene& scene, UUID entity);
	}
}
