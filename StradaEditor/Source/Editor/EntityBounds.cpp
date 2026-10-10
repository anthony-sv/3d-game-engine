#include "Editor/EntityBounds.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/Scene.h"

namespace Strada
{
	AABB ComputeEntityBounds(Scene& scene, std::span<UUID const> entities)
	{
		AABB bounds;
		for (UUID const id : entities)
		{
			Entity entity = scene.GetEntityByUUID(id);
			if (!entity)
			{
				continue;
			}
			glm::mat4 const world = scene.GetWorldTransform(entity);
			MeshComponent const* mesh = entity.TryGetComponent<MeshComponent>();
			Ref<MeshSource> const source = mesh != nullptr ? AssetManager::GetAsset<MeshSource>(mesh->Mesh) : nullptr;
			if (source)
			{
				bounds.Expand(source->GetBounds().Transform(world));
			}
			else
			{
				bounds.Expand(glm::vec3(world[3]));
			}
		}
		return bounds;
	}
}
