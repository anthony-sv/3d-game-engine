#pragma once

#include "Editor/Commands/ComponentCommands.h"

#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Scene/Components.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

namespace Strada
{
	class Scene;

	namespace TransformEditing
	{
		// Selected entities without a selected ancestor, in selection order (moving an entity already moves its descendants).
		// Entities that do not exist are skipped.
		std::vector<UUID> GetSelectionRoots(Scene& scene, std::span<UUID const> selection);

		// The local transform that gives an entity the world transform `world` under a parent whose world transform is
		// `parentWorld` (identity for root entities). Fails for degenerate matrices.
		[[nodiscard]] Result<TransformComponent> ComputeLocalTransform(glm::mat4 const& world, glm::mat4 const& parentWorld);

		// Transform patches that apply a world-space delta to entities (each new world transform is delta * old world
		// transform), for an undoable SetComponentFields call.
		[[nodiscard]] Result<std::vector<ComponentEdit>> ApplyWorldDelta(Scene& scene, std::span<UUID const> entities,
		                                                                 glm::mat4 const& delta);
	}
}
