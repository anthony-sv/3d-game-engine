#include "stpch.h"
#include "Strada/Scene/Scene.h"

#include "Strada/Core/Profiler.h"
#include "Strada/Math/Math.h"
#include "Strada/Scene/Entity.h"

#include <cstring>

// The world-transform cache (Docs/Architecture.md §7.1, "The world-transform cache"): world matrices kept per entity, made
// again by the refresh when their inputs changed, and read through checked chains between refreshes.

namespace Strada
{
	namespace
	{
		// Bit for bit, so that -0 and +0 differ and a NaN equals itself: the cache is exact, never merely close.
		template<typename T>
		bool HasSameBits(T const& a, T const& b)
		{
			return std::memcmp(&a, &b, sizeof(T)) == 0;
		}

		bool HasSameInputs(TransformComponent const& transform, glm::vec3 const& translation, glm::quat const& rotation,
		                   glm::vec3 const& scale)
		{
			return HasSameBits(transform.Translation, translation) && HasSameBits(transform.Rotation, rotation) &&
			       HasSameBits(transform.Scale, scale);
		}
	}

	struct Scene::TransformStorages
	{
		explicit TransformStorages(entt::registry& registry)
			: Transforms(registry.storage<TransformComponent>()),
			  Relationships(registry.storage<RelationshipComponent>()),
			  Ids(registry.storage<IDComponent>()),
			  Entries(registry.storage<WorldTransformEntry>())
		{
		}

		UUID GetParentId(entt::entity handle) const
		{
			return Relationships.contains(handle) ? Relationships.get(handle).Parent : UUID::Invalid();
		}

		// The entry of a live entity: an entry is destroyed with its entity.
		WorldTransformEntry* FindEntry(entt::entity handle) const { return Entries.contains(handle) ? &Entries.get(handle) : nullptr; }

		entt::storage_for_t<TransformComponent>& Transforms;
		entt::storage_for_t<RelationshipComponent>& Relationships;
		entt::storage_for_t<IDComponent>& Ids;
		entt::storage_for_t<WorldTransformEntry>& Entries;
	};

	entt::entity Scene::ResolveTransformParent(TransformStorages const& storages, entt::entity handle, UUID parentId) const
	{
		if (!parentId.IsValid())
		{
			return entt::null;
		}
		// The handle the entry resolved last, while an entry of that UUID lives there: entries die with their entities, and
		// a handle's version has 12 bits (entt/entity/entity.hpp, entt_traits<uint32_t>), so after 4,096 reuses of its slot
		// a handle alone would not prove it.
		entt::entity parent = entt::null;
		if (WorldTransformEntry const* entry = storages.FindEntry(handle); entry != nullptr && entry->Parent == parentId)
		{
			WorldTransformEntry const* parentEntry = entry->ParentHandle != entt::null ? storages.FindEntry(entry->ParentHandle) : nullptr;
			if (parentEntry != nullptr && parentEntry->Id == parentId)
			{
				parent = entry->ParentHandle;
			}
		}
		if (parent == entt::null)
		{
			parent = FindHandle(parentId);
		}
		if (parent != entt::null && !storages.Transforms.contains(parent))
		{
			return entt::null;
		}
		return parent;
	}

	Scene::TransformWalk Scene::WalkTransformChain(TransformStorages const& storages, entt::entity handle, bool stopAtChecked)
	{
		m_TransformChain.clear();
		TransformWalk walk;
		// Brent's cycle detection: the walk follows one parent per entity, so it either ends or comes back to an entity it
		// passed. The tortoise waits at the link of each power of two; the walk meeting it again closes a cycle.
		entt::entity tortoise = handle;
		size_t tortoiseIndex = 0;
		size_t power = 1;
		size_t length = 1;
		entt::entity current = handle;
		TransformComponent const* transform = &storages.Transforms.get(handle);
		while (true)
		{
			UUID const parentId = storages.GetParentId(current);
			entt::entity const parent = ResolveTransformParent(storages, current, parentId);
			m_TransformChain.push_back({current, transform, parentId, parent});
			if (parent == entt::null)
			{
				return walk;
			}
			if (stopAtChecked)
			{
				if (WorldTransformEntry const* parentEntry = storages.FindEntry(parent);
				    parentEntry != nullptr && parentEntry->CheckedRefresh == m_TransformRefresh)
				{
					walk.ReachedChecked = true;
					return walk;
				}
			}
			if (parent == tortoise)
			{
				// The links from the tortoise on are the cycle, once each; the first link whose entity comes back a cycle's
				// length later is where the walk entered it. Links after one pass of the cycle repeat it and are dropped.
				size_t const cycleLength = m_TransformChain.size() - tortoiseIndex;
				size_t start = 0;
				while (start < tortoiseIndex && m_TransformChain[start].Handle != m_TransformChain[start + cycleLength].Handle)
				{
					start++;
				}
				m_TransformChain.resize(start + cycleLength);
				walk.CycleStart = start;
				return walk;
			}
			if (length == power)
			{
				tortoise = parent;
				tortoiseIndex = m_TransformChain.size();
				power *= 2;
				length = 0;
			}
			length++;
			current = parent;
			transform = &storages.Transforms.get(parent);
		}
	}

	void Scene::StoreWorldTransform(WorldTransformEntry& entry, TransformComponent const& transform, UUID parentId,
	                                entt::entity parentHandle, entt::entity worldParent, WorldTransformEntry const* worldParentEntry)
	{
		glm::mat4 const local = transform.GetTransform();
		glm::mat4 const world = worldParentEntry != nullptr ? Math::ComposeWorldTransform(worldParentEntry->World, local) : local;
		// The matrix the frame began with is kept the first time the entry changes in the frame; a new entry has none.
		if (entry.Version == 0)
		{
			entry.Previous = world;
			entry.ChangedFrame = m_TransformFrame;
		}
		else if (entry.ChangedFrame != m_TransformFrame)
		{
			entry.Previous = entry.World;
			entry.ChangedFrame = m_TransformFrame;
		}
		entry.World = world;
		entry.Translation = transform.Translation;
		entry.Rotation = transform.Rotation;
		entry.Scale = transform.Scale;
		entry.Parent = parentId;
		entry.ParentHandle = parentHandle;
		entry.WorldParent = worldParent;
		entry.ParentVersion = worldParentEntry != nullptr ? worldParentEntry->Version : 0;
		entry.Version = ++m_TransformVersion;
	}

	bool Scene::UpdateTransformEntry(TransformStorages const& storages, entt::entity handle, TransformComponent const& transform,
	                                 WorldTransformEntry& entry)
	{
		// Without a walk: an entry computed before, whose parent link is still the one it was computed under and whose parent
		// (if any) this refresh checked already. Parents are mostly made before their children, so the refresh's order
		// (insertion) reaches most entities this way.
		if (entry.Version == 0)
		{
			return false;
		}
		UUID const parentId = storages.GetParentId(handle);
		if (parentId != entry.Parent)
		{
			return false;
		}
		WorldTransformEntry const* parentEntry = nullptr;
		if (parentId.IsValid())
		{
			// An unresolved parent and the entities of a cycle (computed as roots) take the walk.
			if (entry.WorldParent == entt::null || entry.WorldParent != entry.ParentHandle)
			{
				return false;
			}
			parentEntry = storages.FindEntry(entry.WorldParent);
			if (parentEntry == nullptr || parentEntry->Id != parentId || parentEntry->CheckedRefresh != m_TransformRefresh)
			{
				return false;
			}
		}
		entry.CheckedRefresh = m_TransformRefresh;
		if (entry.ParentVersion == (parentEntry != nullptr ? parentEntry->Version : 0) &&
		    HasSameInputs(transform, entry.Translation, entry.Rotation, entry.Scale))
		{
			return true;
		}
		StoreWorldTransform(entry, transform, parentId, entry.ParentHandle, entry.WorldParent, parentEntry);
		return true;
	}

	void Scene::UpdateTransformChain(TransformStorages const& storages, TransformWalk const& walk)
	{
		for (size_t index = m_TransformChain.size(); index-- > 0;)
		{
			TransformLink const& link = m_TransformChain[index];
			entt::entity const worldParent = index >= walk.CycleStart ? entt::null : link.Parent;
			// Processed earlier in this loop or checked by this refresh: an entry the refresh has brought up to date.
			WorldTransformEntry const* parentEntry = worldParent != entt::null ? &storages.Entries.get(worldParent) : nullptr;
			WorldTransformEntry* entry = storages.FindEntry(link.Handle);
			if (entry == nullptr)
			{
				entry = &storages.Entries.emplace(link.Handle);
				entry->Id = storages.Ids.contains(link.Handle) ? storages.Ids.get(link.Handle).ID : UUID::Invalid();
			}
			bool const current = entry->Version != 0 && HasSameInputs(*link.Transform, entry->Translation, entry->Rotation, entry->Scale) &&
			                     entry->Parent == link.ParentId && entry->ParentHandle == link.Parent &&
			                     entry->WorldParent == worldParent &&
			                     entry->ParentVersion == (parentEntry != nullptr ? parentEntry->Version : 0);
			entry->CheckedRefresh = m_TransformRefresh;
			if (!current)
			{
				StoreWorldTransform(*entry, *link.Transform, link.ParentId, link.Parent, worldParent, parentEntry);
			}
		}
	}

	glm::mat4 Scene::ComputeTransformChain(TransformStorages const& storages, TransformWalk const& walk) const
	{
		// The matrix of the link above, and that link's entry when it is current (null when its matrix was computed here).
		glm::mat4 world(1.0f);
		WorldTransformEntry const* above = nullptr;
		for (size_t index = m_TransformChain.size(); index-- > 0;)
		{
			TransformLink const& link = m_TransformChain[index];
			entt::entity const worldParent = index >= walk.CycleStart ? entt::null : link.Parent;
			bool const hasParent = worldParent != entt::null;
			WorldTransformEntry const* entry = storages.FindEntry(link.Handle);
			bool const current = entry != nullptr && entry->Version != 0 && (!hasParent || above != nullptr) &&
			                     HasSameInputs(*link.Transform, entry->Translation, entry->Rotation, entry->Scale) &&
			                     entry->Parent == link.ParentId && entry->ParentHandle == link.Parent &&
			                     entry->WorldParent == worldParent && entry->ParentVersion == (hasParent ? above->Version : 0);
			if (current)
			{
				world = entry->World;
				above = entry;
			}
			else
			{
				glm::mat4 const local = link.Transform->GetTransform();
				world = hasParent ? Math::ComposeWorldTransform(world, local) : local;
				above = nullptr;
			}
		}
		return world;
	}

	glm::mat4 Scene::GetWorldTransform(Entity entity)
	{
		if (!entity || entity.GetScene() != this || !m_Registry.all_of<TransformComponent>(entity.GetHandle()))
		{
			return glm::mat4(1.0f);
		}
		TransformStorages const storages(m_Registry);
		TransformWalk const walk = WalkTransformChain(storages, entity.GetHandle(), false);
		return ComputeTransformChain(storages, walk);
	}

	void Scene::SetWorldTransform(Entity entity, glm::mat4 const& worldTransform)
	{
		if (!entity || entity.GetScene() != this)
		{
			return;
		}

		glm::mat4 localTransform = worldTransform;
		if (Entity const parent = GetParent(entity))
		{
			localTransform = glm::inverse(GetWorldTransform(parent)) * worldTransform;
		}

		TransformComponent& transform = entity.GetComponent<TransformComponent>();
		glm::vec3 translation;
		glm::quat rotation;
		glm::vec3 scale;
		if (Math::DecomposeTransform(localTransform, translation, rotation, scale))
		{
			transform.Translation = translation;
			transform.Rotation = rotation;
			transform.Scale = scale;
		}
	}

	void Scene::BeginTransformFrame()
	{
		m_TransformFrame++;
	}

	void Scene::UpdateTransforms()
	{
		ST_PROFILE_SCOPE("Scene.UpdateTransforms");
		m_TransformRefresh++;
		TransformStorages const storages(m_Registry);
		// In the order the transforms were added, which puts most parents before their children. Entries are added while
		// the transforms are iterated: they live in a storage of their own, whose elements never move when it grows.
		for (auto const& [handle, transform] : storages.Transforms.reach())
		{
			WorldTransformEntry* entry = storages.FindEntry(handle);
			if (entry != nullptr &&
			    (entry->CheckedRefresh == m_TransformRefresh || UpdateTransformEntry(storages, handle, transform, *entry)))
			{
				continue;
			}
			TransformWalk const walk = WalkTransformChain(storages, handle, true);
			UpdateTransformChain(storages, walk);
		}
	}

	CachedWorldTransform Scene::GetCachedWorldTransform(entt::entity handle)
	{
		entt::storage_for_t<WorldTransformEntry> const& entries = m_Registry.storage<WorldTransformEntry>();
		if (m_Registry.valid(handle) && entries.contains(handle))
		{
			WorldTransformEntry const& entry = entries.get(handle);
			if (entry.Version != 0)
			{
				bool const changedThisFrame = m_TransformFrame != 0 && entry.ChangedFrame == m_TransformFrame;
				return {entry.World, changedThisFrame ? entry.Previous : entry.World};
			}
		}
		glm::mat4 const world = GetWorldTransform(Entity(handle, this));
		return {world, world};
	}
}
