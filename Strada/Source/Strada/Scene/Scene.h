#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Core/UUID.h"
#include "Strada/Renderer/SceneRendererSettings.h"

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strada
{
	class Entity;

	struct ScenePhysicsSettings
	{
		glm::vec3 Gravity = {0.0f, -9.81f, 0.0f};

		bool operator==(ScenePhysicsSettings const& other) const = default;
	};

	template<>
	struct StructTraits<ScenePhysicsSettings>
	{
		static constexpr std::string_view Name = "Physics";
		static constexpr auto Fields = std::make_tuple(
			Field("Gravity", &ScenePhysicsSettings::Gravity).Doc("Acceleration of dynamic bodies in meters per second squared."));
	};

	// Per-scene settings, stored in scene files under "Scene": { "Settings": { ... } }.
	struct SceneSettings
	{
		ScenePhysicsSettings Physics;
		SceneRendererSettings Renderer;

		bool operator==(SceneSettings const& other) const = default;
	};

	template<>
	struct StructTraits<SceneSettings>
	{
		static constexpr std::string_view Name = "Settings";
		static constexpr auto Fields =
			std::make_tuple(Field("Physics", &SceneSettings::Physics), Field("Renderer", &SceneSettings::Renderer));
	};

	// An ECS world: entities with components, an ordered hierarchy, settings and runtime state. Main thread only.
	class Scene
	{
	public:
		explicit Scene(std::string name = "Untitled");
		~Scene();

		Scene(Scene const&) = delete;
		Scene& operator=(Scene const&) = delete;

		// Deep copy with the same entity UUIDs, components, hierarchy and settings (used to enter play mode). Runtime
		// state is not copied.
		[[nodiscard]] static Ref<Scene> Copy(Scene const& source);

		std::string const& GetName() const { return m_Name; }
		void SetName(std::string name) { m_Name = std::move(name); }
		SceneSettings& GetSettings() { return m_Settings; }
		SceneSettings const& GetSettings() const { return m_Settings; }

		// --- Entities ---

		// Creates a root entity with ID, Tag, Transform and Relationship components.
		Entity CreateEntity(std::string const& name = "Entity");
		Entity CreateEntity(std::string const& name, Entity parent);
		// The UUID must be valid and unused (check with HasEntity).
		Entity CreateEntityWithUUID(UUID id, std::string const& name = "Entity");
		// Destroys the entity and its descendants. While the scene is running, destruction is deferred to the end of
		// the frame (the entity stays valid until then); otherwise it is immediate.
		void DestroyEntity(Entity entity);
		// Copies the entity and its descendants with new UUIDs; the copy is placed right after the original.
		Entity DuplicateEntity(Entity entity);

		bool HasEntity(UUID id) const { return m_EntityMap.contains(id); }
		// A random UUID not used by any entity of this scene.
		UUID GenerateUniqueID() const;
		// Invalid Entity when not found.
		Entity GetEntityByUUID(UUID id);
		// First match in hierarchy order; invalid Entity when not found.
		Entity FindEntityByName(std::string_view name);
		size_t GetEntityCount() const { return m_EntityMap.size(); }
		bool IsPendingDestruction(Entity entity) const;

		// --- Hierarchy ---

		std::vector<Entity> GetRootEntities();
		std::vector<Entity> GetChildren(Entity entity);
		// Invalid Entity for root entities.
		Entity GetParent(Entity entity);
		// Reparents (an invalid parent makes it a root). Fails for cycles and entities of other scenes. The child is
		// appended to the new parent's children.
		[[nodiscard]] Result<void> SetParent(Entity child, Entity parent, bool keepWorldTransform = true);
		// Moves the entity within its siblings; indices past the end move it last.
		void SetSiblingIndex(Entity entity, size_t index);
		size_t GetSiblingIndex(Entity entity);
		bool IsDescendantOf(Entity entity, Entity ancestor);

		glm::mat4 GetWorldTransform(Entity entity);
		// Sets the local transform so the entity ends up with the given world transform.
		void SetWorldTransform(Entity entity, glm::mat4 const& worldTransform);

		// Depth-first, parents before children, siblings in order.
		void ForEachEntityInHierarchyOrder(std::function<void(Entity)> const& function);

		// First entity (in hierarchy order) with a primary camera; invalid Entity when none.
		Entity GetPrimaryCameraEntity();

		// --- Runtime ---

		void OnRuntimeStart();
		void OnRuntimeStop();
		void OnUpdateRuntime(Timestep timestep);
		bool IsRunning() const { return m_IsRunning; }
		bool IsPaused() const { return m_IsPaused; }
		void SetPaused(bool paused) { m_IsPaused = paused; }
		// While paused, advances the given number of frames.
		void Step(uint32_t frames = 1) { m_StepFrames += frames; }
		uint64_t GetRuntimeFrame() const { return m_RuntimeFrame; }
		double GetRuntimeTime() const { return m_RuntimeTime; }

		void OnViewportResize(uint32_t width, uint32_t height);
		uint32_t GetViewportWidth() const { return m_ViewportWidth; }
		uint32_t GetViewportHeight() const { return m_ViewportHeight; }

		template<typename... T>
		auto GetAllEntitiesWith()
		{
			return m_Registry.view<T...>();
		}

		entt::registry& GetRegistry() { return m_Registry; }
		entt::registry const& GetRegistry() const { return m_Registry; }

		// Root order, used by serialization.
		std::vector<UUID> const& GetRootEntityIDs() const { return m_RootEntities; }

	private:
		friend class Entity;
		friend class SceneSerializer;
		friend class PrefabSerializer;

		entt::entity CreateHandle(UUID id);
		void DestroyEntityImmediate(entt::entity handle);
		void DetachFromParent(entt::entity handle);
		std::vector<UUID>& GetSiblingList(entt::entity handle);
		void FlushPendingDestruction();
		void RemapEntityReferences(entt::entity handle, std::unordered_map<UUID, UUID> const& remap);
		entt::entity FindHandle(UUID id) const;

		std::string m_Name;
		SceneSettings m_Settings;
		entt::registry m_Registry;
		std::unordered_map<UUID, entt::entity> m_EntityMap;
		std::vector<UUID> m_RootEntities;
		std::vector<UUID> m_PendingDestruction;

		bool m_IsRunning = false;
		bool m_IsPaused = false;
		uint32_t m_StepFrames = 0;
		uint64_t m_RuntimeFrame = 0;
		double m_RuntimeTime = 0.0;

		uint32_t m_ViewportWidth = 0;
		uint32_t m_ViewportHeight = 0;
	};
}
