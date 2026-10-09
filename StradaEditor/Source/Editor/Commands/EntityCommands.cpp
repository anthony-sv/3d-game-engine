#include "Editor/Commands/EntityCommands.h"

#include "Editor/Commands/ComponentCommands.h"
#include "Editor/EditorContext.h"

#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Scene/Entity.h"

#include <limits>
#include <unordered_set>
#include <utility>

namespace Strada
{
	namespace
	{
		// Places the entity under parent (keeping its local transform) at the sibling index; staying under the same parent
		// only reorders.
		Result<void> MoveEntity(Scene& scene, Entity entity, Entity parent, size_t siblingIndex)
		{
			if (scene.GetParent(entity) != parent)
			{
				if (Result<void> result = scene.SetParent(entity, parent, false); !result)
				{
					return result;
				}
			}
			scene.SetSiblingIndex(entity, siblingIndex);
			return {};
		}

		// Where an entity is in the hierarchy, and its local transform.
		struct Placement
		{
			Entity Parent;
			size_t SiblingIndex = 0;
			Json Transform;

			bool operator==(Placement const& other) const = default;
		};

		// Resolves the IDs (all must exist) and keeps, in request order and without duplicates, the entities none of whose
		// ancestors is part of the request: their subtrees contain everything requested.
		Result<std::vector<Entity>> ResolveSubtreeRoots(Scene& scene, std::vector<UUID> const& ids)
		{
			std::vector<Entity> entities;
			std::unordered_set<UUID> requested;
			for (UUID const id : ids)
			{
				Entity const entity = scene.GetEntityByUUID(id);
				if (!entity)
				{
					return MakeError("entity {} does not exist", id);
				}
				if (requested.insert(id).second)
				{
					entities.push_back(entity);
				}
			}
			if (entities.empty())
			{
				return Error{"no entities were given"};
			}

			std::vector<Entity> roots;
			for (Entity const entity : entities)
			{
				bool hasRequestedAncestor = false;
				Entity ancestor = scene.GetParent(entity);
				// Bounded by the entity count so a corrupted hierarchy cannot loop forever.
				for (size_t depth = 0; ancestor && depth <= scene.GetEntityCount() && !hasRequestedAncestor; depth++)
				{
					hasRequestedAncestor = requested.contains(ancestor.GetUUID());
					ancestor = scene.GetParent(ancestor);
				}
				if (!hasRequestedAncestor)
				{
					roots.push_back(entity);
				}
			}
			return roots;
		}
	}

	CreateEntityCommand::CreateEntityCommand(UUID id, EntityCreateInfo info)
		: m_ID(id),
		  m_Info(std::move(info))
	{
	}

	Result<void> CreateEntityCommand::Execute(EditorContext& context)
	{
		if (!m_Snapshot)
		{
			return Create(context);
		}
		Result<Entity> restored = m_Snapshot->Restore(context.GetScene(), context.CreateDeserializationContext());
		if (!restored)
		{
			return Error{restored.GetError()};
		}
		return {};
	}

	Result<void> CreateEntityCommand::Create(EditorContext& context)
	{
		Scene& scene = context.GetScene();
		if (!m_ID.IsValid() || scene.HasEntity(m_ID))
		{
			return MakeError("entity ID {} is invalid or already in use", m_ID);
		}

		Entity parent;
		if (m_Info.Parent.IsValid())
		{
			parent = scene.GetEntityByUUID(m_Info.Parent);
			if (!parent)
			{
				return MakeError("parent entity {} does not exist", m_Info.Parent);
			}
		}

		if (!m_Info.Components.is_object())
		{
			return Error{"components must be an object mapping component names to fields"};
		}
		std::vector<ComponentInfo const*> components;
		components.reserve(m_Info.Components.size());
		for (auto const& item : m_Info.Components.items())
		{
			Result<ComponentInfo const*> component = FindEditableComponent(item.key(), ComponentAccess::Modify);
			if (!component)
			{
				return Error{component.GetError()};
			}
			components.push_back(component.GetValue());
		}

		Entity entity = scene.CreateEntityWithUUID(m_ID, m_Info.Name);
		if (parent)
		{
			if (Result<void> result = scene.SetParent(entity, parent, false); !result)
			{
				scene.DestroyEntity(entity);
				return result;
			}
		}
		if (m_Info.SiblingIndex)
		{
			scene.SetSiblingIndex(entity, *m_Info.SiblingIndex);
		}

		DeserializationContext const deserialization = context.CreateDeserializationContext();
		size_t index = 0;
		for (auto const& item : m_Info.Components.items())
		{
			if (Result<void> result =
			        components[index++]->Deserialize(scene.GetRegistry(), entity.GetHandle(), item.value(), deserialization);
			    !result)
			{
				scene.DestroyEntity(entity);
				return result;
			}
		}

		m_Snapshot = EntitySnapshot::Capture(scene, entity);
		return {};
	}

	Result<void> CreateEntityCommand::Undo(EditorContext& context)
	{
		Scene& scene = context.GetScene();
		Entity const entity = scene.GetEntityByUUID(m_ID);
		if (!entity)
		{
			return MakeError("entity {} no longer exists", m_ID);
		}
		scene.DestroyEntity(entity);
		return {};
	}

	DeleteEntitiesCommand::DeleteEntitiesCommand(std::vector<UUID> entities)
		: m_Entities(std::move(entities))
	{
	}

	Result<void> DeleteEntitiesCommand::Execute(EditorContext& context)
	{
		Scene& scene = context.GetScene();
		Result<std::vector<Entity>> roots = ResolveSubtreeRoots(scene, m_Entities);
		if (!roots)
		{
			return Error{roots.GetError()};
		}

		// Every snapshot is taken before anything is deleted, so their sibling indices describe one consistent state.
		std::vector<EntitySnapshot> snapshots;
		snapshots.reserve(roots.GetValue().size());
		for (Entity const root : roots.GetValue())
		{
			snapshots.push_back(EntitySnapshot::Capture(scene, root));
		}
		for (Entity const root : roots.GetValue())
		{
			scene.DestroyEntity(root);
		}
		m_Snapshots = std::move(snapshots);
		return {};
	}

	Result<void> DeleteEntitiesCommand::Undo(EditorContext& context)
	{
		return EntitySnapshot::RestoreAll(context.GetScene(), m_Snapshots, context.CreateDeserializationContext());
	}

	std::string DeleteEntitiesCommand::GetDescription() const
	{
		return m_Entities.size() == 1 ? "Delete Entity" : "Delete Entities";
	}

	DuplicateEntitiesCommand::DuplicateEntitiesCommand(std::vector<UUID> entities)
		: m_Entities(std::move(entities))
	{
	}

	Result<void> DuplicateEntitiesCommand::Execute(EditorContext& context)
	{
		Scene& scene = context.GetScene();
		if (!m_Snapshots.empty())
		{
			return EntitySnapshot::RestoreAll(scene, m_Snapshots, context.CreateDeserializationContext());
		}

		Result<std::vector<Entity>> roots = ResolveSubtreeRoots(scene, m_Entities);
		if (!roots)
		{
			return Error{roots.GetError()};
		}

		std::vector<Entity> copies;
		copies.reserve(roots.GetValue().size());
		for (Entity const root : roots.GetValue())
		{
			Entity const copy = scene.DuplicateEntity(root);
			if (!copy)
			{
				for (Entity const created : copies)
				{
					scene.DestroyEntity(created);
				}
				return MakeError("entity {} could not be duplicated", root.GetUUID());
			}
			copies.push_back(copy);
		}

		// Snapshots are taken once every copy is in place, so restoring them in sibling order reproduces this arrangement.
		for (Entity const copy : copies)
		{
			m_Snapshots.push_back(EntitySnapshot::Capture(scene, copy));
			m_Copies.push_back(copy.GetUUID());
		}
		return {};
	}

	Result<void> DuplicateEntitiesCommand::Undo(EditorContext& context)
	{
		Scene& scene = context.GetScene();
		std::vector<Entity> copies;
		copies.reserve(m_Copies.size());
		for (UUID const id : m_Copies)
		{
			Entity const copy = scene.GetEntityByUUID(id);
			if (!copy)
			{
				return MakeError("entity {} no longer exists", id);
			}
			copies.push_back(copy);
		}
		for (Entity const copy : copies)
		{
			scene.DestroyEntity(copy);
		}
		return {};
	}

	std::string DuplicateEntitiesCommand::GetDescription() const
	{
		return m_Entities.size() == 1 ? "Duplicate Entity" : "Duplicate Entities";
	}

	ReparentEntityCommand::ReparentEntityCommand(UUID entity, UUID newParent, std::optional<size_t> siblingIndex, bool keepWorldTransform)
		: m_Entity(entity),
		  m_NewParent(newParent),
		  m_SiblingIndex(siblingIndex),
		  m_KeepWorldTransform(keepWorldTransform)
	{
	}

	Result<void> ReparentEntityCommand::Execute(EditorContext& context)
	{
		Scene& scene = context.GetScene();
		Entity entity = scene.GetEntityByUUID(m_Entity);
		if (!entity)
		{
			return MakeError("entity {} does not exist", m_Entity);
		}
		Entity parent;
		if (m_NewParent.IsValid())
		{
			parent = scene.GetEntityByUUID(m_NewParent);
			if (!parent)
			{
				return MakeError("parent entity {} does not exist", m_NewParent);
			}
		}

		ComponentInfo const& transform = ComponentRegistry::Get<TransformComponent>();
		if (m_Executed)
		{
			if (Result<void> result = MoveEntity(scene, entity, parent, m_NewSiblingIndex); !result)
			{
				return result;
			}
			return transform.Deserialize(scene.GetRegistry(), entity.GetHandle(), m_NewTransform, context.CreateDeserializationContext());
		}

		Entity const oldParent = scene.GetParent(entity);
		size_t const oldSiblingIndex = scene.GetSiblingIndex(entity);
		Json oldTransform = transform.Serialize(scene.GetRegistry(), entity.GetHandle());

		// SetParent validates (cycles, foreign scenes) before it changes anything.
		if (parent != oldParent)
		{
			if (Result<void> result = scene.SetParent(entity, parent, m_KeepWorldTransform); !result)
			{
				return result;
			}
		}
		scene.SetSiblingIndex(entity, m_SiblingIndex.value_or(std::numeric_limits<size_t>::max()));

		m_OldParent = oldParent ? oldParent.GetUUID() : UUID::Invalid();
		m_OldSiblingIndex = oldSiblingIndex;
		m_OldTransform = std::move(oldTransform);
		m_NewSiblingIndex = scene.GetSiblingIndex(entity);
		m_NewTransform = transform.Serialize(scene.GetRegistry(), entity.GetHandle());
		m_Executed = true;
		return {};
	}

	Result<void> ReparentEntityCommand::Undo(EditorContext& context)
	{
		Scene& scene = context.GetScene();
		Entity entity = scene.GetEntityByUUID(m_Entity);
		if (!entity)
		{
			return MakeError("entity {} no longer exists", m_Entity);
		}
		Entity oldParent;
		if (m_OldParent.IsValid())
		{
			oldParent = scene.GetEntityByUUID(m_OldParent);
			if (!oldParent)
			{
				return MakeError("the original parent entity {} no longer exists", m_OldParent);
			}
		}

		if (Result<void> result = MoveEntity(scene, entity, oldParent, m_OldSiblingIndex); !result)
		{
			return result;
		}
		return ComponentRegistry::Get<TransformComponent>().Deserialize(scene.GetRegistry(), entity.GetHandle(), m_OldTransform,
		                                                                context.CreateDeserializationContext());
	}

	std::string ReparentEntityCommand::GetDescription() const
	{
		return m_Executed && m_OldParent == m_NewParent ? "Reorder Entity" : "Reparent Entity";
	}

	bool ReparentEntityCommand::HasEffect() const
	{
		return m_Executed && (m_OldParent != m_NewParent || m_OldSiblingIndex != m_NewSiblingIndex || m_OldTransform != m_NewTransform);
	}

	MoveEntitiesCommand::MoveEntitiesCommand(std::vector<UUID> entities, UUID newParent, UUID insertBefore, bool keepWorldTransform)
		: m_Entities(std::move(entities)),
		  m_NewParent(newParent),
		  m_InsertBefore(insertBefore),
		  m_KeepWorldTransform(keepWorldTransform)
	{
	}

	Result<void> MoveEntitiesCommand::Execute(EditorContext& context)
	{
		if (!m_Executed)
		{
			return ExecuteFirst(context);
		}

		// Replaying the recorded results in order reproduces every intermediate state, so the indices stay valid.
		Scene& scene = context.GetScene();
		Entity const parent = m_NewParent.IsValid() ? scene.GetEntityByUUID(m_NewParent) : Entity();
		if (m_NewParent.IsValid() && !parent)
		{
			return MakeError("parent entity {} no longer exists", m_NewParent);
		}
		for (Move const& move : m_Moves)
		{
			if (!scene.HasEntity(move.Entity))
			{
				return MakeError("entity {} no longer exists", move.Entity);
			}
		}
		ComponentInfo const& transform = ComponentRegistry::Get<TransformComponent>();
		DeserializationContext const deserialization = context.CreateDeserializationContext();
		for (Move const& move : m_Moves)
		{
			Entity const entity = scene.GetEntityByUUID(move.Entity);
			// Moves were validated by the first execution and the scene is in the same state again.
			(void)MoveEntity(scene, entity, parent, move.NewSiblingIndex);
			(void)transform.Deserialize(scene.GetRegistry(), entity.GetHandle(), move.NewTransform, deserialization);
		}
		return {};
	}

	Result<void> MoveEntitiesCommand::ExecuteFirst(EditorContext& context)
	{
		Scene& scene = context.GetScene();
		Result<std::vector<Entity>> roots = ResolveSubtreeRoots(scene, m_Entities);
		if (!roots)
		{
			return Error{roots.GetError()};
		}
		Entity parent;
		if (m_NewParent.IsValid())
		{
			parent = scene.GetEntityByUUID(m_NewParent);
			if (!parent)
			{
				return MakeError("parent entity {} does not exist", m_NewParent);
			}
		}
		for (Entity const entity : roots.GetValue())
		{
			if (parent && (parent == entity || scene.IsDescendantOf(parent, entity)))
			{
				return MakeError("entity {} cannot become a child of itself or of its descendant {}", entity.GetUUID(), m_NewParent);
			}
		}
		Entity anchor;
		if (m_InsertBefore.IsValid())
		{
			anchor = scene.GetEntityByUUID(m_InsertBefore);
			if (!anchor || scene.GetParent(anchor) != parent)
			{
				return MakeError("entity {} is not a child of the new parent", m_InsertBefore);
			}
			for (Entity const entity : roots.GetValue())
			{
				if (entity == anchor)
				{
					return MakeError("entity {} cannot be moved before itself", m_InsertBefore);
				}
			}
		}

		ComponentInfo const& transform = ComponentRegistry::Get<TransformComponent>();
		// Where every moved entity starts. Siblings that are not moved keep their relative order, so the scene is unchanged
		// exactly when every moved entity ends where it started.
		auto const getPlacement = [&scene, &transform](Entity entity)
		{
			return Placement{scene.GetParent(entity), scene.GetSiblingIndex(entity),
			                 transform.Serialize(scene.GetRegistry(), entity.GetHandle())};
		};
		std::vector<Placement> initial;
		for (Entity const entity : roots.GetValue())
		{
			initial.push_back(getPlacement(entity));
		}

		bool reparents = false;
		m_Moves.clear();
		for (Entity entity : roots.GetValue())
		{
			Move move;
			move.Entity = entity.GetUUID();
			Entity const oldParent = scene.GetParent(entity);
			move.OldParent = oldParent ? oldParent.GetUUID() : UUID::Invalid();
			move.OldSiblingIndex = scene.GetSiblingIndex(entity);
			move.OldTransform = transform.Serialize(scene.GetRegistry(), entity.GetHandle());
			if (oldParent != parent)
			{
				reparents = true;
				// Validated above: neither cycles nor foreign entities can make this fail.
				(void)scene.SetParent(entity, parent, m_KeepWorldTransform);
			}
			size_t index = std::numeric_limits<size_t>::max();
			if (anchor)
			{
				// SetSiblingIndex removes the entity before inserting it, which shifts the anchor when the entity precedes it.
				index = scene.GetSiblingIndex(anchor);
				if (scene.GetSiblingIndex(entity) < index)
				{
					index--;
				}
			}
			scene.SetSiblingIndex(entity, index);
			move.NewSiblingIndex = scene.GetSiblingIndex(entity);
			move.NewTransform = transform.Serialize(scene.GetRegistry(), entity.GetHandle());
			m_Moves.push_back(std::move(move));
		}

		m_HasEffect = false;
		for (size_t i = 0; i < roots.GetValue().size(); i++)
		{
			m_HasEffect = m_HasEffect || getPlacement(roots.GetValue()[i]) != initial[i];
		}
		size_t const count = m_Moves.size();
		m_Description = fmt::format("{} {}", reparents ? "Reparent" : "Reorder", count == 1 ? "Entity" : "Entities");
		m_Executed = true;
		return {};
	}

	Result<void> MoveEntitiesCommand::Undo(EditorContext& context)
	{
		Scene& scene = context.GetScene();
		for (Move const& move : m_Moves)
		{
			if (!scene.HasEntity(move.Entity))
			{
				return MakeError("entity {} no longer exists", move.Entity);
			}
			if (move.OldParent.IsValid() && !scene.HasEntity(move.OldParent))
			{
				return MakeError("the original parent entity {} no longer exists", move.OldParent);
			}
		}

		// Reverse order: each move restores the state right before it.
		ComponentInfo const& transform = ComponentRegistry::Get<TransformComponent>();
		DeserializationContext const deserialization = context.CreateDeserializationContext();
		for (size_t i = m_Moves.size(); i-- > 0;)
		{
			Move const& move = m_Moves[i];
			Entity const entity = scene.GetEntityByUUID(move.Entity);
			Entity const oldParent = move.OldParent.IsValid() ? scene.GetEntityByUUID(move.OldParent) : Entity();
			(void)MoveEntity(scene, entity, oldParent, move.OldSiblingIndex);
			(void)transform.Deserialize(scene.GetRegistry(), entity.GetHandle(), move.OldTransform, deserialization);
		}
		return {};
	}

	bool MoveEntitiesCommand::HasEffect() const
	{
		return m_HasEffect;
	}
}
