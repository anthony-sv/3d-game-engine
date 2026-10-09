#include "Editor/Viewport/TransformEditing.h"

#include "Strada/Math/Math.h"
#include "Strada/Scene/ComponentSerialization.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

#include <algorithm>

namespace Strada::TransformEditing
{
	std::vector<UUID> GetSelectionRoots(Scene& scene, std::span<UUID const> selection)
	{
		std::vector<UUID> roots;
		for (UUID const id : selection)
		{
			Entity const entity = scene.GetEntityByUUID(id);
			if (!entity || std::find(roots.begin(), roots.end(), id) != roots.end())
			{
				continue;
			}
			bool const hasSelectedAncestor =
				std::any_of(selection.begin(), selection.end(),
			                [&scene, entity](UUID const other)
			                {
								Entity const candidate = scene.GetEntityByUUID(other);
								return candidate && candidate != entity && scene.IsDescendantOf(entity, candidate);
							});
			if (!hasSelectedAncestor)
			{
				roots.push_back(id);
			}
		}
		return roots;
	}

	Result<TransformComponent> ComputeLocalTransform(glm::mat4 const& world, glm::mat4 const& parentWorld)
	{
		glm::mat4 const local = glm::inverse(parentWorld) * world;
		TransformComponent transform;
		if (!Math::DecomposeTransform(local, transform.Translation, transform.Rotation, transform.Scale))
		{
			return Error{"the transform is degenerate (zero scale)"};
		}
		return transform;
	}

	Result<std::vector<ComponentEdit>> ApplyWorldDelta(Scene& scene, std::span<UUID const> entities, glm::mat4 const& delta)
	{
		std::vector<ComponentEdit> edits;
		edits.reserve(entities.size());
		for (UUID const id : entities)
		{
			Entity const entity = scene.GetEntityByUUID(id);
			if (!entity)
			{
				return MakeError("entity {} does not exist", id);
			}
			Entity const parent = scene.GetParent(entity);
			glm::mat4 const parentWorld = parent ? scene.GetWorldTransform(parent) : glm::mat4(1.0f);
			Result<TransformComponent> local = ComputeLocalTransform(delta * scene.GetWorldTransform(entity), parentWorld);
			if (!local)
			{
				return MakeError("entity {}: {}", id, local.GetError());
			}
			edits.push_back({id, "Transform", SerializeComponent(local.GetValue())});
		}
		return edits;
	}
}
