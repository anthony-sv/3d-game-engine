#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Core/UUID.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Renderer/SceneRendererSettings.h"
#include "Strada/Scene/Components.h"

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Strada
{
	class AudioScene;
	class Entity;
	class PhysicsScene;

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

	// Project settings the scene runtime uses (ProjectSettings provides them when a project is open).
	struct SceneRuntimeSettings
	{
		// Seconds per physics step.
		float FixedTimestep = 1.0f / 60.0f;
		// Physics layers in use (1 to MaxPhysicsLayers) and the layer pairs that do not collide.
		uint32_t PhysicsLayerCount = 1;
		std::vector<glm::uvec2> IgnoredCollisions;
		// Physics steps per frame at most: slower frames slow the simulation down instead of falling further behind.
		uint32_t MaxStepsPerFrame = 8;
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

		// Starts simulating: a physics body for every entity with colliders (static without a RigidBody), a sound for every
		// audio source with a clip (playing when PlayOnStart is set) and a script instance for every Script component whose
		// class the loaded game assembly has (all are created, then OnCreate runs for each). Bodies, sounds and scripts
		// follow component changes while running. Requires PhysicsSystem, AudioEngine and ScriptEngine (without them the
		// scene runs without physics, audio or scripts, logged); stop the runtime or destroy the scene before shutting them
		// down. One scene runs scripts at a time.
		void OnRuntimeStart(SceneRuntimeSettings const& settings = {});
		// Calls OnDestroy on every script, then stops the audio and physics.
		void OnRuntimeStop();
		// Advances the runtime: scripts' OnUpdate, fixed steps (scripts' OnFixedUpdate, then physics: kinematic bodies follow
		// their transforms, moved static and dynamic bodies teleport, dynamic bodies write their transforms back), contact
		// events to scripts, audio (sources and the listener follow their entities), then deferred entity destruction
		// (destroyed scripts get OnDestroy).
		void OnUpdateRuntime(Timestep timestep);
		bool IsRunning() const { return m_IsRunning; }
		bool IsPaused() const { return m_IsPaused; }
		// Pausing holds the simulation and every sound.
		void SetPaused(bool paused);
		// While paused, advances the given number of frames.
		void Step(uint32_t frames = 1) { m_StepFrames += frames; }
		uint64_t GetRuntimeFrame() const { return m_RuntimeFrame; }
		double GetRuntimeTime() const { return m_RuntimeTime; }
		// The physics world while running; null otherwise or when physics is unavailable.
		PhysicsScene* GetPhysicsScene() { return m_Physics.get(); }
		// The sounds while running; null otherwise or when audio is unavailable.
		AudioScene* GetAudioScene() { return m_Audio.get(); }
		// Collisions and trigger overlaps that began or ended during the last runtime update.
		std::vector<ContactEvent> const& GetContactEvents() const { return m_ContactEvents; }

		// How fast the runtime's time passes for scripts and physics (sounds are not affected); 0 freezes the simulation.
		float GetTimeScale() const { return m_TimeScale; }
		// Negative and non-finite scales are ignored.
		void SetTimeScale(float scale);
		// Creates and rebuilds the bodies of physics components changed since the last step. The runtime does it before
		// each update; script calls on bodies do it first, so a body added this frame takes forces at once.
		void ApplyPhysicsChanges();
		// Control the sound of an entity's AudioSource while running, bringing it up to date with the component first (a
		// clip set this frame plays). Without a running audio scene or a playable clip they do nothing.
		void PlayAudioSource(Entity entity);
		void PauseAudioSource(Entity entity);
		void StopAudioSource(Entity entity);
		bool IsAudioSourcePlaying(Entity entity);

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

		// Physics runtime (ScenePhysics.cpp).
		void StartPhysics();
		void StopPhysics();
		void UpdatePhysics(float deltaTime);
		void RebuildChangedBodies();
		void UpdateBody(entt::entity handle);
		void SyncBodiesFromTransforms(float stepDelta);
		void WriteBodyTransforms();
		void OnPhysicsComponentChanged(entt::registry& registry, entt::entity handle);
		void OnMeshComponentChanged(entt::registry& registry, entt::entity handle);
		void ConnectPhysicsSignals(bool connect);

		// Audio runtime (SceneAudio.cpp).
		struct AudioSourceState;
		void StartAudio();
		void StopAudio();
		void UpdateAudio();
		// Brings an entity's sound up to date with its component; false when it has no sound.
		bool SyncAudioSource(entt::entity handle, AudioSourceComponent const& component);
		void CreateAudioSource(entt::entity handle, AudioSourceState& state, AudioSourceComponent const& component, bool playOnStart);
		void UpdateAudioSource(AudioSourceState& state, AudioSourceComponent const& component);
		// The ID of the entity's sound after syncing it, if it has one.
		std::optional<UUID> PrepareAudioSource(Entity entity);
		Entity FindAudioListener();

		// Script runtime (SceneScripting.cpp).
		void StartScripts();
		void StopScripts();
		// Creates instances for new Script components and replaces those whose class changed; OnCreate runs for each new
		// instance once all exist.
		void SyncScriptInstances();
		void UpdateScripts(float deltaTime);
		void FixedUpdateScripts(float fixedDeltaTime);
		void DispatchContactEventsToScripts();
		void DestroyScriptInstance(entt::entity handle);
		// Instances whose entity or Script component is gone.
		void DestroyRemovedScriptInstances();
		std::vector<UUID> GetScriptInstanceIDs() const;

		std::string m_Name;
		SceneSettings m_Settings;
		entt::registry m_Registry;
		std::unordered_map<UUID, entt::entity> m_EntityMap;
		std::vector<UUID> m_RootEntities;
		std::vector<UUID> m_PendingDestruction;

		bool m_IsRunning = false;
		bool m_IsPaused = false;
		float m_TimeScale = 1.0f;
		uint32_t m_StepFrames = 0;
		uint64_t m_RuntimeFrame = 0;
		double m_RuntimeTime = 0.0;

		// The world pose last exchanged with an entity's body (to tell transform edits from simulation).
		struct BodyState
		{
			UUID Entity = UUID::Invalid();
			glm::vec3 Position = glm::vec3(0.0f);
			glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		};

		SceneRuntimeSettings m_RuntimeSettings;
		Scope<PhysicsScene> m_Physics;
		float m_PhysicsAccumulator = 0.0f;
		std::unordered_map<entt::entity, BodyState> m_Bodies;
		// Entities whose physics components changed while running; their bodies are rebuilt before the next step.
		std::unordered_set<entt::entity> m_ChangedBodies;
		std::vector<ContactEvent> m_ContactEvents;

		// The component settings an entity's sound was last updated with (to apply changes made while running).
		struct AudioSourceState
		{
			UUID Entity = UUID::Invalid();
			AudioSourceComponent Applied;
			// False when there is no clip or it cannot be played; retried when the clip changes.
			bool HasSound = false;
		};

		Scope<AudioScene> m_Audio;
		std::unordered_map<entt::entity, AudioSourceState> m_AudioSources;

		// The class an entity's script instance was made from; HasInstance is false when it could not be made (unknown
		// class, throwing constructor) until the class name changes.
		struct ScriptInstanceState
		{
			UUID Entity = UUID::Invalid();
			std::string ClassName;
			bool HasInstance = false;
		};

		// Whether this scene runs scripts (it is the ScriptEngine's scene context).
		bool m_RunsScripts = false;
		std::unordered_map<entt::entity, ScriptInstanceState> m_ScriptInstances;

		uint32_t m_ViewportWidth = 0;
		uint32_t m_ViewportHeight = 0;
	};
}
