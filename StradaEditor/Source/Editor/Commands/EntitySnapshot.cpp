#include "Editor/Commands/EntitySnapshot.h"

#include "Strada/Scene/SceneSerializer.h"

#include <algorithm>
#include <numeric>

namespace Strada
{
	EntitySnapshot EntitySnapshot::Capture(Scene& scene, Entity root)
	{
		EntitySnapshot snapshot;
		snapshot.Entities = Json::array();
		for (entt::entity const handle : SceneSerializer::CollectHierarchy(scene, {root.GetUUID()}))
		{
			snapshot.Entities.push_back(SceneSerializer::SerializeEntity(scene, handle));
		}
		Entity const parent = scene.GetParent(root);
		snapshot.Parent = parent ? parent.GetUUID() : UUID::Invalid();
		snapshot.SiblingIndex = scene.GetSiblingIndex(root);
		return snapshot;
	}

	Result<Entity> EntitySnapshot::Restore(Scene& scene, DeserializationContext const& context) const
	{
		Entity parent;
		if (Parent.IsValid())
		{
			parent = scene.GetEntityByUUID(Parent);
			if (!parent)
			{
				return MakeError("the parent entity {} no longer exists", Parent);
			}
		}
		return SceneSerializer::DeserializeEntityHierarchy(scene, Entities, parent, SiblingIndex, context);
	}

	Result<void> EntitySnapshot::RestoreAll(Scene& scene, std::span<EntitySnapshot const> snapshots, DeserializationContext const& context)
	{
		std::vector<size_t> order(snapshots.size());
		std::iota(order.begin(), order.end(), size_t(0));
		std::stable_sort(order.begin(), order.end(),
		                 [snapshots](size_t a, size_t b)
		                 {
							 return snapshots[a].SiblingIndex < snapshots[b].SiblingIndex;
						 });

		std::vector<Entity> restored;
		restored.reserve(snapshots.size());
		for (size_t const index : order)
		{
			Result<Entity> entity = snapshots[index].Restore(scene, context);
			if (!entity)
			{
				for (auto it = restored.rbegin(); it != restored.rend(); ++it)
				{
					scene.DestroyEntity(*it);
				}
				return Error{entity.GetError()};
			}
			restored.push_back(entity.GetValue());
		}
		return {};
	}
}
