#pragma once

#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Serialization/JsonSerialization.h"

#include <cstddef>
#include <span>
#include <vector>

namespace Strada
{
	// An entity subtree serialized with the scene serializer together with where it is attached, so commands can destroy
	// it and later recreate it with the same UUIDs, components, hierarchy and sibling position.
	struct EntitySnapshot
	{
		// SceneSerializer::SerializeEntity of the root followed by its descendants in hierarchy order.
		Json Entities;
		UUID Parent = UUID::Invalid();
		size_t SiblingIndex = 0;

		[[nodiscard]] static EntitySnapshot Capture(Scene& scene, Entity root);
		// Fails (creating nothing) when the parent no longer exists or an entity UUID is in use.
		[[nodiscard]] Result<Entity> Restore(Scene& scene, DeserializationContext const& context) const;

		// Restores several snapshots of the same scene state in ascending sibling order, so siblings land exactly where
		// they were captured. All or nothing: on failure the subtrees restored so far are destroyed again.
		[[nodiscard]] static Result<void> RestoreAll(Scene& scene, std::span<EntitySnapshot const> snapshots,
		                                             DeserializationContext const& context);
	};
}
