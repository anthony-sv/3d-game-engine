#include "stpch.h"
#include "Strada/Scene/Scene.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/PrefabAsset.h"
#include "Strada/Audio/AudioScene.h"
#include "Strada/Core/Hash.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Math/Math.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Scene/ComponentRegistry.h"
#include "Strada/Scene/Entity.h"
#include "Strada/Scene/SceneSerializer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace Strada
{
	namespace
	{
		// The next value of a SplitMix64 sequence: well mixed, the same on every platform.
		uint64_t NextSequenceValue(uint64_t& state)
		{
			state += 0x9e3779b97f4a7c15ull;
			uint64_t value = state;
			value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
			value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
			return value ^ (value >> 31);
		}

		// Atomic, so that serials stay unique whichever thread makes a scene.
		std::atomic<uint64_t> s_NextSceneSerial{1};

		// The entity a remapped entity's reference refers to: its copy; other references are kept.
		UUID RemapEntity(UUID entity, std::unordered_map<UUID, UUID> const& remap)
		{
			auto const it = remap.find(entity);
			return it != remap.end() ? it->second : entity;
		}

		// Entity-typed values, and the elements of lists of entities, that refer to remapped entities refer to their copies;
		// the others are kept.
		void RemapEntityValues(ScriptFieldMap& values, std::unordered_map<UUID, UUID> const& remap)
		{
			for (auto& [name, value] : values)
			{
				if (value.GetType() == ScriptFieldType::Entity)
				{
					value = ScriptFieldValue::FromEntity(RemapEntity(value.GetEntity(), remap));
				}
				else if (value.GetType() == MakeScriptFieldListType(ScriptFieldType::Entity))
				{
					std::vector<ScriptFieldValue> elements;
					elements.reserve(value.GetElements().size());
					for (ScriptFieldValue const& element : value.GetElements())
					{
						elements.push_back(ScriptFieldValue::FromEntity(RemapEntity(element.GetEntity(), remap)));
					}
					value = ScriptFieldValue::FromList(ScriptFieldType::Entity, std::move(elements));
				}
			}
		}
	}

	Scene::Scene(std::string name)
		: m_Serial(s_NextSceneSerial.fetch_add(1, std::memory_order_relaxed)),
		  m_Name(std::move(name))
	{
	}

	Scene::~Scene()
	{
		// Scripts get OnDestroy, sounds stop and the physics registry signals disconnect before the registry goes away.
		StopScripts();
		StopAudio();
		StopPhysics();
		ReleaseRuntimeAssets();
	}

	Ref<Scene> Scene::Copy(Scene const& source)
	{
		Ref<Scene> copy = CreateRef<Scene>(source.m_Name);
		copy->m_Settings = source.m_Settings;
		copy->m_ViewportWidth = source.m_ViewportWidth;
		copy->m_ViewportHeight = source.m_ViewportHeight;

		std::span<ComponentInfo const> const components = ComponentRegistry::GetComponents();
		for (auto const& [id, sourceHandle] : source.m_EntityMap)
		{
			entt::entity const handle = copy->CreateHandle(id);
			for (ComponentInfo const& component : components)
			{
				component.Copy(source.m_Registry, sourceHandle, copy->m_Registry, handle);
			}
		}
		copy->m_RootEntities = source.m_RootEntities;
		return copy;
	}

	entt::entity Scene::CreateHandle(UUID id)
	{
		ST_CORE_ASSERT(id.IsValid(), "Entities need a valid UUID");
		ST_CORE_ASSERT(!m_EntityMap.contains(id), "An entity with UUID {} already exists", id);

		entt::entity const handle = m_Registry.create();
		m_Registry.emplace<IDComponent>(handle, id);
		m_EntityMap[id] = handle;
		return handle;
	}

	UUID Scene::GenerateUniqueID()
	{
		// Physics is called in the order of the entities' UUIDs: entities made while running take them from the run's own
		// sequence, so every run of the scene makes the same ones.
		auto const next = [this]
		{
			return m_IsRunning ? UUID(NextSequenceValue(m_RuntimeIDSequence)) : UUID();
		};
		// Collisions of 64-bit IDs are astronomically unlikely, but a duplicate would corrupt the entity map.
		UUID id = next();
		while (!id.IsValid() || m_EntityMap.contains(id))
		{
			id = next();
		}
		return id;
	}

	Entity Scene::CreateEntity(std::string const& name)
	{
		return CreateEntityWithUUID(GenerateUniqueID(), name);
	}

	Entity Scene::CreateEntity(std::string const& name, Entity parent)
	{
		Entity entity = CreateEntityWithUUID(GenerateUniqueID(), name);
		if (parent)
		{
			Result<void> result = SetParent(entity, parent, false);
			ST_CORE_ASSERT(result.IsOk(), "Failed to parent a new entity: {}", result.GetError());
		}
		return entity;
	}

	Entity Scene::CreateEntityWithUUID(UUID id, std::string const& name)
	{
		entt::entity const handle = CreateHandle(id);
		m_Registry.emplace<TagComponent>(handle, name.empty() ? std::string("Entity") : name);
		m_Registry.emplace<TransformComponent>(handle);
		m_Registry.emplace<RelationshipComponent>(handle);
		m_RootEntities.push_back(id);
		return Entity(handle, this);
	}

	void Scene::DestroyEntity(Entity entity)
	{
		if (!entity || entity.GetScene() != this)
		{
			return;
		}

		if (m_IsRunning)
		{
			UUID const id = entity.GetUUID();
			if (std::find(m_PendingDestruction.begin(), m_PendingDestruction.end(), id) == m_PendingDestruction.end())
			{
				m_PendingDestruction.push_back(id);
			}
			return;
		}
		DestroyEntityImmediate(entity.GetHandle());
	}

	void Scene::DestroyEntityImmediate(entt::entity handle)
	{
		// The scripts (the C# script, then the visual script) see their entity and the children intact in OnDestroy.
		DestroyScriptInstance(handle);
		DestroyVisualScriptInstance(handle);

		// Copy: destroying a child modifies this entity's child list.
		std::vector<UUID> const children = m_Registry.get<RelationshipComponent>(handle).Children;
		for (UUID const child : children)
		{
			if (entt::entity const childHandle = FindHandle(child); childHandle != entt::null)
			{
				DestroyEntityImmediate(childHandle);
			}
		}

		DetachFromParent(handle);
		m_EntityMap.erase(m_Registry.get<IDComponent>(handle).ID);
		m_Registry.destroy(handle);
	}

	void Scene::FlushPendingDestruction()
	{
		while (!m_PendingDestruction.empty())
		{
			std::vector<UUID> const pending = std::move(m_PendingDestruction);
			m_PendingDestruction.clear();
			for (UUID const id : pending)
			{
				if (entt::entity const handle = FindHandle(id); handle != entt::null)
				{
					DestroyEntityImmediate(handle);
				}
			}
		}
	}

	bool Scene::IsPendingDestruction(Entity entity) const
	{
		if (!entity || entity.GetScene() != this)
		{
			return false;
		}
		UUID const id = entity.GetUUID();
		return std::find(m_PendingDestruction.begin(), m_PendingDestruction.end(), id) != m_PendingDestruction.end();
	}

	Result<Entity> Scene::InstantiatePrefab(AssetHandle prefab, glm::vec3 const& position, glm::quat const& rotation, Entity parent)
	{
		Result<Ref<PrefabAsset>> asset = AssetManager::TryGetAsset<PrefabAsset>(prefab);
		if (!asset)
		{
			return Error{asset.GetError()};
		}
		Result<Entity> instance = PrefabSerializer::Instantiate(*this, asset.GetValue()->GetDocument(), prefab, parent,
		                                                        AssetManager::CreateDeserializationContext(UnknownFieldPolicy::Warn));
		if (!instance)
		{
			return instance;
		}

		Entity const root = instance.GetValue();
		glm::vec3 currentPosition(0.0f);
		glm::quat currentRotation(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec3 scale = root.GetComponent<TransformComponent>().Scale;
		Math::DecomposeTransform(GetWorldTransform(root), currentPosition, currentRotation, scale);
		SetWorldTransform(root, Math::ComposeTransform(position, rotation, scale));

		StartScriptsOf(SceneSerializer::CollectHierarchy(*this, {root.GetUUID()}));
		return root;
	}

	Entity Scene::DuplicateEntity(Entity entity)
	{
		if (!entity || entity.GetScene() != this)
		{
			return {};
		}

		// Assign new UUIDs to the whole subtree first so internal references can be remapped.
		std::vector<entt::entity> subtree;
		std::unordered_map<UUID, UUID> remap;
		std::function<void(entt::entity)> const collect = [&](entt::entity handle)
		{
			subtree.push_back(handle);
			UUID newId = GenerateUniqueID();
			while (std::any_of(remap.begin(), remap.end(),
			                   [newId](auto const& entry)
			                   {
								   return entry.second == newId;
							   }))
			{
				newId = GenerateUniqueID();
			}
			remap[m_Registry.get<IDComponent>(handle).ID] = newId;
			for (UUID const child : m_Registry.get<RelationshipComponent>(handle).Children)
			{
				if (entt::entity const childHandle = FindHandle(child); childHandle != entt::null)
				{
					collect(childHandle);
				}
			}
		};
		collect(entity.GetHandle());

		std::span<ComponentInfo const> const components = ComponentRegistry::GetComponents();
		std::vector<entt::entity> copies;
		copies.reserve(subtree.size());
		for (entt::entity const sourceHandle : subtree)
		{
			UUID const newId = remap.at(m_Registry.get<IDComponent>(sourceHandle).ID);
			entt::entity const handle = CreateHandle(newId);
			for (ComponentInfo const& component : components)
			{
				if (component.Name != ComponentTraits<IDComponent>::Name)
				{
					component.Copy(m_Registry, sourceHandle, m_Registry, handle);
				}
			}
			copies.push_back(handle);
		}

		for (entt::entity const handle : copies)
		{
			RelationshipComponent& relationship = m_Registry.get<RelationshipComponent>(handle);
			if (auto const it = remap.find(relationship.Parent); it != remap.end())
			{
				relationship.Parent = it->second;
			}
			// Children that did not resolve to an entity were not copied; drop them instead of keeping dangling links.
			std::vector<UUID> children;
			children.reserve(relationship.Children.size());
			for (UUID const child : relationship.Children)
			{
				if (auto const it = remap.find(child); it != remap.end())
				{
					children.push_back(it->second);
				}
			}
			relationship.Children = std::move(children);
			RemapEntityReferences(handle, remap);
		}

		// Place the copy right after the original among its siblings.
		entt::entity const copyRoot = copies.front();
		std::vector<UUID>& siblings = GetSiblingList(entity.GetHandle());
		auto const original = std::find(siblings.begin(), siblings.end(), entity.GetUUID());
		siblings.insert(original == siblings.end() ? siblings.end() : original + 1, m_Registry.get<IDComponent>(copyRoot).ID);
		return Entity(copyRoot, this);
	}

	void Scene::RemapEntityReferences(entt::entity handle, std::unordered_map<UUID, UUID> const& remap)
	{
		if (JointComponent* joint = m_Registry.try_get<JointComponent>(handle))
		{
			if (auto const it = remap.find(joint->ConnectedEntity); it != remap.end())
			{
				joint->ConnectedEntity = it->second;
			}
		}
		if (ScriptComponent* script = m_Registry.try_get<ScriptComponent>(handle))
		{
			RemapEntityValues(script->Fields, remap);
		}
		if (VisualScriptComponent* visualScript = m_Registry.try_get<VisualScriptComponent>(handle))
		{
			RemapEntityValues(visualScript->Variables, remap);
		}
	}

	entt::entity Scene::FindHandle(UUID id) const
	{
		auto const it = m_EntityMap.find(id);
		return it != m_EntityMap.end() ? it->second : entt::null;
	}

	Entity Scene::GetEntityByUUID(UUID id)
	{
		entt::entity const handle = FindHandle(id);
		return handle != entt::null ? Entity(handle, this) : Entity();
	}

	Entity Scene::FindEntityByName(std::string_view name)
	{
		Entity result;
		ForEachEntityInHierarchyOrder(
			[&result, name](Entity entity)
			{
				if (!result && entity.GetName() == name)
				{
					result = entity;
				}
			});
		return result;
	}

	std::vector<Entity> Scene::GetRootEntities()
	{
		std::vector<Entity> roots;
		roots.reserve(m_RootEntities.size());
		for (UUID const id : m_RootEntities)
		{
			if (entt::entity const handle = FindHandle(id); handle != entt::null)
			{
				roots.emplace_back(handle, this);
			}
		}
		return roots;
	}

	std::vector<Entity> Scene::GetChildren(Entity entity)
	{
		std::vector<Entity> children;
		if (!entity || entity.GetScene() != this)
		{
			return children;
		}
		for (UUID const child : entity.GetComponent<RelationshipComponent>().Children)
		{
			if (entt::entity const handle = FindHandle(child); handle != entt::null)
			{
				children.emplace_back(handle, this);
			}
		}
		return children;
	}

	Entity Scene::GetParent(Entity entity)
	{
		if (!entity || entity.GetScene() != this)
		{
			return {};
		}
		return GetEntityByUUID(entity.GetComponent<RelationshipComponent>().Parent);
	}

	std::vector<UUID>& Scene::GetSiblingList(entt::entity handle)
	{
		UUID const parent = m_Registry.get<RelationshipComponent>(handle).Parent;
		if (entt::entity const parentHandle = FindHandle(parent); parentHandle != entt::null)
		{
			return m_Registry.get<RelationshipComponent>(parentHandle).Children;
		}
		return m_RootEntities;
	}

	void Scene::DetachFromParent(entt::entity handle)
	{
		UUID const id = m_Registry.get<IDComponent>(handle).ID;
		std::vector<UUID>& siblings = GetSiblingList(handle);
		siblings.erase(std::remove(siblings.begin(), siblings.end(), id), siblings.end());
		m_Registry.get<RelationshipComponent>(handle).Parent = UUID::Invalid();
	}

	bool Scene::IsDescendantOf(Entity entity, Entity ancestor)
	{
		if (!entity || !ancestor || entity.GetScene() != this || ancestor.GetScene() != this)
		{
			return false;
		}

		UUID const ancestorId = ancestor.GetUUID();
		UUID current = entity.GetComponent<RelationshipComponent>().Parent;
		// Bounded by the entity count so a corrupted hierarchy cannot loop forever.
		for (size_t depth = 0; current.IsValid() && depth <= m_EntityMap.size(); depth++)
		{
			if (current == ancestorId)
			{
				return true;
			}
			entt::entity const handle = FindHandle(current);
			if (handle == entt::null)
			{
				return false;
			}
			current = m_Registry.get<RelationshipComponent>(handle).Parent;
		}
		return false;
	}

	Result<void> Scene::SetParent(Entity child, Entity parent, bool keepWorldTransform)
	{
		if (!child || child.GetScene() != this)
		{
			return Error{"the entity does not belong to this scene"};
		}
		if (parent)
		{
			if (parent.GetScene() != this)
			{
				return Error{"the new parent belongs to another scene"};
			}
			if (parent == child)
			{
				return Error{"an entity cannot be its own parent"};
			}
			if (IsDescendantOf(parent, child))
			{
				return MakeError("cannot parent '{}' to its descendant '{}'", child.GetName(), parent.GetName());
			}
		}

		glm::mat4 const worldTransform = keepWorldTransform ? GetWorldTransform(child) : glm::mat4(1.0f);

		DetachFromParent(child.GetHandle());
		RelationshipComponent& relationship = child.GetComponent<RelationshipComponent>();
		if (parent)
		{
			relationship.Parent = parent.GetUUID();
			parent.GetComponent<RelationshipComponent>().Children.push_back(child.GetUUID());
		}
		else
		{
			m_RootEntities.push_back(child.GetUUID());
		}

		if (keepWorldTransform)
		{
			SetWorldTransform(child, worldTransform);
		}
		return {};
	}

	void Scene::SetSiblingIndex(Entity entity, size_t index)
	{
		if (!entity || entity.GetScene() != this)
		{
			return;
		}
		std::vector<UUID>& siblings = GetSiblingList(entity.GetHandle());
		UUID const id = entity.GetUUID();
		siblings.erase(std::remove(siblings.begin(), siblings.end(), id), siblings.end());
		siblings.insert(siblings.begin() + static_cast<std::ptrdiff_t>(std::min(index, siblings.size())), id);
	}

	size_t Scene::GetSiblingIndex(Entity entity)
	{
		if (!entity || entity.GetScene() != this)
		{
			return 0;
		}
		std::vector<UUID> const& siblings = GetSiblingList(entity.GetHandle());
		auto const it = std::find(siblings.begin(), siblings.end(), entity.GetUUID());
		return static_cast<size_t>(it - siblings.begin());
	}

	void Scene::ForEachEntityInHierarchyOrder(std::function<void(Entity)> const& function)
	{
		// Explicit stack: deep hierarchies must not overflow the call stack.
		std::vector<UUID> stack(m_RootEntities.rbegin(), m_RootEntities.rend());
		while (!stack.empty())
		{
			UUID const id = stack.back();
			stack.pop_back();
			entt::entity const handle = FindHandle(id);
			if (handle == entt::null)
			{
				continue;
			}

			function(Entity(handle, this));
			// The callback may have destroyed the entity.
			if (!m_Registry.valid(handle))
			{
				continue;
			}
			std::vector<UUID> const& children = m_Registry.get<RelationshipComponent>(handle).Children;
			stack.insert(stack.end(), children.rbegin(), children.rend());
		}
	}

	Entity Scene::GetPrimaryCameraEntity()
	{
		// Scenes usually have one primary camera; only several need the hierarchy walk to pick the first.
		entt::entity primary = entt::null;
		size_t primaryCount = 0;
		for (auto const [handle, camera] : m_Registry.view<CameraComponent>().each())
		{
			if (camera.Primary)
			{
				primary = handle;
				primaryCount++;
			}
		}
		if (primaryCount <= 1)
		{
			return primaryCount == 1 ? Entity(primary, this) : Entity();
		}

		Entity result;
		ForEachEntityInHierarchyOrder(
			[&result](Entity entity)
			{
				if (!result)
				{
					if (CameraComponent const* camera = entity.TryGetComponent<CameraComponent>(); camera != nullptr && camera->Primary)
					{
						result = entity;
					}
				}
			});
		return result;
	}

	void Scene::OnRuntimeStart(SceneRuntimeSettings const& settings)
	{
		ST_CORE_ASSERT(!m_IsRunning, "The scene is already running");
		m_IsRunning = true;
		m_IsPaused = false;
		m_StepFrames = 0;
		m_RuntimeFrame = 0;
		m_RuntimeTime = 0.0;
		m_FixedStepCount = 0;
		m_RecordedPhysicsHash.reset();
		m_FoldedPhysicsHash = HashBuilder();
		m_RuntimeSettings = settings;
		// The sequence of the UUIDs of entities made while running starts from the scene's own entities, so that two scenes
		// make different ones and every run of a scene the same.
		std::vector<uint64_t> ids;
		ids.reserve(m_EntityMap.size());
		for (auto const& [id, handle] : m_EntityMap)
		{
			ids.push_back(id.GetValue());
		}
		std::sort(ids.begin(), ids.end());
		HashBuilder seed;
		for (uint64_t const id : ids)
		{
			seed.AddUInt64(id);
		}
		m_RuntimeIDSequence = seed.GetHash();
		// Systems start over with the run (their bursts come with its first update).
		ResetParticles();
		ResetUI();
		StartInputActions();
		StartPhysics();
		if (settings.PlayAudio)
		{
			StartAudio();
		}
		if (settings.RunScripts)
		{
			StartScripts();
		}
	}

	void Scene::OnRuntimeStop()
	{
		if (!m_IsRunning)
		{
			return;
		}
		// While still running, so destruction scripts request in OnDestroy is deferred like any other.
		StopScripts();
		m_IsRunning = false;
		StopAudio();
		StopPhysics();
		FlushPendingDestruction();
		m_DebugLines.clear();
		m_DebugLinesDropped = false;
		ResetParticles();
		ResetUI();
		StopInputActions();
		ReleaseRuntimeAssets();
	}

	void Scene::OnUpdateRuntime(Timestep timestep)
	{
		if (!m_IsRunning)
		{
			return;
		}
		if (m_IsPaused)
		{
			if (m_StepFrames == 0)
			{
				// Nothing runs, and the views still draw a frame of the scene (edits made while paused move what they show).
				BeginTransformFrame();
				return;
			}
			m_StepFrames--;
		}
		// Before anything of the frame writes: the matrices the cache holds become the frame's previous ones.
		BeginTransformFrame();

		ST_PROFILE_SCOPE("Scene.Update");
		float const deltaTime = timestep.GetSeconds() * m_TimeScale;
		m_RuntimeFrame++;
		m_RuntimeTime += deltaTime;
		// Lines drawn last frame were rendered; this frame's scripts draw new ones.
		AgeDebugLines(deltaTime);
		// Actions are evaluated before anything of the frame reads them: the UI's clicks, the scripts, the fixed steps.
		UpdateInputActions(timestep.GetSeconds());
		// Clicks reach the scripts before their update, which sees WasClicked.
		{
			ST_PROFILE_SCOPE("Scene.UpdateUI");
			UpdateUI();
		}
		{
			ST_PROFILE_SCOPE("Scene.UpdateScripts");
			UpdateScripts(deltaTime);
		}
		{
			ST_PROFILE_SCOPE("Scene.UpdatePhysics");
			UpdatePhysics(deltaTime);
		}
		{
			ST_PROFILE_SCOPE("Scene.ContactEvents");
			DispatchContactEventsToScripts();
		}
		{
			ST_PROFILE_SCOPE("Scene.UpdateAudio");
			UpdateAudio(deltaTime);
		}
		{
			ST_PROFILE_SCOPE("Scene.UpdateParticles");
			UpdateParticles(deltaTime);
		}
		{
			ST_PROFILE_SCOPE("Scene.Destroy");
			FlushPendingDestruction();
		}
	}

	void Scene::SetTimeScale(float scale)
	{
		if (std::isfinite(scale) && scale >= 0.0f)
		{
			m_TimeScale = scale;
		}
	}

	void Scene::SetPaused(bool paused)
	{
		if (m_IsPaused && !paused)
		{
			// What moved while the game stood still (an edit, a debugger's change) did not move in the game's time.
			ForgetAudioMotion();
		}
		m_IsPaused = paused;
		if (m_Audio)
		{
			m_Audio->SetPaused(paused);
		}
	}

	void Scene::DrawDebugLine(glm::vec3 const& from, glm::vec3 const& to, glm::vec4 const& color, float duration)
	{
		if (!m_IsRunning)
		{
			return;
		}
		if (m_DebugLines.size() >= MaxDebugLines)
		{
			if (!m_DebugLinesDropped)
			{
				ST_CORE_WARN("Scene '{}' has {} debug lines; more are dropped until some expire", m_Name, MaxDebugLines);
				m_DebugLinesDropped = true;
			}
			return;
		}
		m_DebugLines.push_back({from, to, color, std::max(duration, 0.0f)});
	}

	void Scene::AgeDebugLines(float deltaTime)
	{
		// Every line is rendered at least once (it was drawn during the last update); it stays until its duration has passed.
		std::erase_if(m_DebugLines,
		              [deltaTime](SceneDebugLine& line)
		              {
						  line.RemainingTime -= deltaTime;
						  return line.RemainingTime <= 0.0f;
					  });
		if (m_DebugLines.size() < MaxDebugLines)
		{
			m_DebugLinesDropped = false;
		}
	}

	void Scene::AddRuntimeAsset(AssetHandle handle)
	{
		m_RuntimeAssets.insert(handle);
	}

	void Scene::ReleaseRuntimeAssets()
	{
		if (AssetManager::IsInitialized())
		{
			for (AssetHandle const handle : m_RuntimeAssets)
			{
				AssetManager::RemoveMemoryAsset(handle);
			}
		}
		m_RuntimeAssets.clear();
	}

	void Scene::OnViewportResize(uint32_t width, uint32_t height)
	{
		m_ViewportWidth = width;
		m_ViewportHeight = height;
	}
}
