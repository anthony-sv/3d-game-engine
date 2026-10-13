#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Core/UUID.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Renderer/SceneRendererSettings.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/ParticleEmitter.h"
#include "Strada/Scene/UILayout.h"

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
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
		// The scene's scripts and sounds run with it; the editor's Simulate mode runs physics only.
		bool RunScripts = true;
		bool PlayAudio = true;
	};

	// A line drawn over the running scene for debugging (Debug.DrawLine), in world space.
	struct SceneDebugLine
	{
		glm::vec3 From = glm::vec3(0.0f);
		glm::vec3 To = glm::vec3(0.0f);
		// Linear RGBA.
		glm::vec4 Color = glm::vec4(1.0f);
		// Seconds of runtime left to draw it.
		float RemainingTime = 0.0f;
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
		// Destroys the entities whose destruction was deferred. The scene does it at the end of each frame; editors call it
		// after each edit, between frames, so an edit of a running (or paused) scene is complete when it returns.
		void FlushPendingDestruction();
		// Copies the entity and its descendants with new UUIDs; the copy is placed right after the original.
		Entity DuplicateEntity(Entity entity);
		// Instantiates a prefab asset (see PrefabSerializer::Instantiate) under parent (a root entity when invalid) and places
		// its root at a world position and rotation, keeping its scale. While the scene runs scripts, the instance's scripts
		// exist and have run OnCreate when this returns.
		[[nodiscard]] Result<Entity> InstantiatePrefab(AssetHandle prefab, glm::vec3 const& position, glm::quat const& rotation,
		                                               Entity parent);

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

		// Starts simulating: a physics body for every entity with colliders (static without a RigidBody), a character for
		// every CharacterController (instead of a body), a sound for every audio source with a clip (playing when PlayOnStart
		// is set) and a script instance for every Script component whose class the loaded game assembly has (all are
		// created, then OnCreate runs for each); sounds and scripts only when the settings ask for them. Bodies, characters,
		// sounds and scripts follow component changes while running. Requires PhysicsSystem, AudioEngine and ScriptEngine
		// (without them the scene runs without physics, audio or scripts, logged); stop the runtime or destroy the scene
		// before shutting them down. One scene runs scripts at a time.
		void OnRuntimeStart(SceneRuntimeSettings const& settings = {});
		// Calls OnDestroy on every script, then stops the audio and physics.
		void OnRuntimeStop();
		// Advances the runtime: the UI (the mouse over buttons, clicks and OnClick), scripts' OnUpdate, fixed steps (scripts'
		// OnFixedUpdate, then physics: kinematic bodies follow their transforms, moved static and dynamic bodies and
		// characters teleport, characters move, dynamic bodies and characters write their positions back), characters drawn
		// between their last two steps (so they move smoothly at any frame rate, a step behind; OnFixedUpdate sees them where
		// the last step left them), contact events to scripts, audio (sources and the listener follow their entities),
		// particles, then deferred entity destruction (destroyed scripts get OnDestroy).
		void OnUpdateRuntime(Timestep timestep);
		bool IsRunning() const { return m_IsRunning; }
		bool IsPaused() const { return m_IsPaused; }
		// Pausing holds the simulation and every sound.
		void SetPaused(bool paused);
		// While paused, advances the given number of frames.
		void Step(uint32_t frames = 1) { m_StepFrames += frames; }
		uint64_t GetRuntimeFrame() const { return m_RuntimeFrame; }
		double GetRuntimeTime() const { return m_RuntimeTime; }
		// Seconds per physics step (and OnFixedUpdate) of the running scene: the runtime settings' step.
		float GetFixedTimestep() const { return std::max(m_RuntimeSettings.FixedTimestep, 1e-4f); }
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
		// Creates and rebuilds the bodies and characters of physics components changed since the last step. The runtime does
		// it before each update; script calls on bodies and characters do it first, so a body added this frame takes forces
		// at once.
		void ApplyPhysicsChanges();
		// Places an entity with a CharacterController at a world position at once, keeping its rotation and scale, and stops
		// its character, which finds what it stands on there.
		void TeleportCharacter(Entity entity, glm::vec3 const& position);
		// Control the sound of an entity's AudioSource while running, bringing it up to date with the component first (a
		// clip set this frame plays). Without a running audio scene or a playable clip they do nothing.
		void PlayAudioSource(Entity entity);
		void PauseAudioSource(Entity entity);
		void StopAudioSource(Entity entity);
		bool IsAudioSourcePlaying(Entity entity);
		// While the scene runs scripts: the values the fields of the entity's script hold now, which also become the stored
		// values of its Script component (Fields), so that the component shows them and edits start from them. Fails when
		// the entity runs no script.
		[[nodiscard]] Result<ScriptFieldMap> ReadScriptFields(Entity entity);
		// While the scene runs scripts: gives each script the stored values of its Script component's fields that differ
		// from the ones it was last given or read (ReadScriptFields), so that edits of a running scene reach its scripts.
		// Editors call it after each edit; a class that changed gets a new instance at the next update instead.
		void ApplyScriptFieldChanges();

		// Advances every particle system: emits its Burst when it starts (first update after the system appeared or the runtime
		// started), then simulates and emits (ParticleEmitter). While running, systems with DestroyWhenFinished destroy their
		// entity once done. The runtime calls it each update; editors call it to preview particles while editing.
		void UpdateParticles(float deltaTime);
		// Emits particles from an entity's system now; the system starts first if it has not (its burst comes too).
		void EmitParticles(Entity entity, uint32_t count);
		void ClearParticles(Entity entity);
		// The particles of an entity's system, in world space or the entity's local space (ParticleSystemComponent::WorldSpace);
		// empty before its first update.
		std::span<Particle const> GetParticles(Entity entity) const;
		// Drops every system's particles; each starts again (with its burst) at the next update.
		void ResetParticles();

		// Lines drawn while running (Debug.DrawLine; ignored otherwise), each for a duration in seconds of runtime (0 draws it
		// in the next rendered frame only). Lines beyond MaxDebugLines are dropped; stopping the runtime clears them.
		static constexpr size_t MaxDebugLines = 100000;
		void DrawDebugLine(glm::vec3 const& from, glm::vec3 const& to, glm::vec4 const& color, float duration = 0.0f);
		std::vector<SceneDebugLine> const& GetDebugLines() const { return m_DebugLines; }

		// --- Screen-space UI (see UICanvasComponent; SceneUI.cpp) ---

		// How the entity's UIButton is drawn: Disabled while it is not interactable, Hovered or Pressed by the mouse while the
		// scene runs, Normal otherwise (and for entities without a button).
		UIButtonState GetUIButtonState(Entity entity) const;
		// Whether the entity's UIButton was clicked in the latest runtime update.
		bool WasUIButtonClicked(Entity entity) const;
		// Clicks the entity's UIButton at the next runtime update as the mouse would (unless it is not interactable then).
		// Ignored while the scene does not run.
		void ClickUIButton(Entity entity);
		// Whether the mouse (Input's cursor in the scene's viewport) hovers, presses and clicks UI buttons; on by default.
		// Editors turn it off while the game does not have the input, whose cursor is then elsewhere.
		void SetUIPointerEnabled(bool enabled) { m_UIPointerEnabled = enabled; }
		bool IsUIPointerEnabled() const { return m_UIPointerEnabled; }
		// Whether the mouse is over the UI as of the latest runtime update: an element that takes clicks (an image or a button)
		// is under the cursor, so a click there goes to the UI. Games that act on clicks in the world check it. False while the
		// pointer is disabled or the cursor locked, and while the scene does not run.
		bool IsPointerOverUI() const { return m_UIPointerOver; }

		// Memory assets that belong to the run (materials scripts create): they are removed from the AssetManager when the
		// runtime stops.
		void AddRuntimeAsset(AssetHandle handle);
		bool IsRuntimeAsset(AssetHandle handle) const { return m_RuntimeAssets.contains(handle); }

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
		void AgeDebugLines(float deltaTime);
		void ReleaseRuntimeAssets();
		void RemapEntityReferences(entt::entity handle, std::unordered_map<UUID, UUID> const& remap);
		entt::entity FindHandle(UUID id) const;

		// Physics runtime (ScenePhysics.cpp).
		void StartPhysics();
		void StopPhysics();
		void UpdatePhysics(float deltaTime);
		void RebuildChangedBodies();
		// Creates, rebuilds or removes the body of an entity after its components changed; true when its body changed.
		bool UpdateBody(entt::entity handle);
		void SyncBodiesFromTransforms(float stepDelta);
		void WriteBodyTransforms();
		void OnPhysicsComponentChanged(entt::registry& registry, entt::entity handle);
		void OnMeshComponentChanged(entt::registry& registry, entt::entity handle);
		void OnJointComponentChanged(entt::registry& registry, entt::entity handle);
		// Makes every joint again from the components and where the bodies are now.
		void RebuildJoints();
		void ConnectPhysicsSignals(bool connect);
		// Creates, changes or removes the character of an entity after its CharacterController (or scale) changed.
		void UpdateCharacter(entt::entity handle);
		void SyncCharactersFromTransforms();
		void WriteCharacterTransforms();
		// Between frames, characters are drawn between their last two steps. Before the physics changes and steps of a frame,
		// those not moved since go back to where the last step left them; after the steps, they are drawn the part of the way
		// from the step before (alpha 0) to the last one (alpha 1) that the time left over makes up.
		void ReturnCharactersToSteps();
		void InterpolateCharacterTransforms(float alpha);
		void OnCharacterComponentChanged(entt::registry& registry, entt::entity handle);
		void OnCharacterComponentAddedOrRemoved(entt::registry& registry, entt::entity handle);

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

		// Particle runtime (SceneParticles.cpp).
		struct ParticleSystemState;
		// The entity's started system: created (with its burst) when missing.
		ParticleSystemState& StartParticleSystem(entt::entity handle, ParticleSystemComponent const& component);

		// Script runtime (SceneScripting.cpp).
		struct PendingScriptInstance
		{
			entt::entity Handle = entt::null;
			UUID Entity = UUID::Invalid();
			std::string ClassName;
			ScriptFieldMap Fields;
		};
		void StartScripts();
		void StopScripts();
		// Creates instances for new Script components and replaces those whose class changed; OnCreate runs for each new
		// instance once all exist.
		void SyncScriptInstances();
		// Creates the instances, then runs OnCreate for each, so scripts can find each other there.
		void CreateScriptInstances(std::vector<PendingScriptInstance> const& pending);
		// Starts the scripts of new entities (prefab instances) at once instead of at the next update.
		void StartScriptsOf(std::vector<entt::entity> const& handles);
		void UpdateScripts(float deltaTime);
		void FixedUpdateScripts(float fixedDeltaTime);
		void DispatchContactEventsToScripts();
		void DestroyScriptInstance(entt::entity handle);
		// Instances whose entity or Script component is gone.
		void DestroyRemovedScriptInstances();
		std::vector<UUID> GetScriptInstanceIDs() const;

		// UI runtime (SceneUI.cpp).
		void ResetUI();
		// Follows the mouse over the UI and its buttons, then clicks the buttons it clicked and the ones scripts clicked, calling
		// OnClick on the scripts of each button and its ancestors.
		void UpdateUI();

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
		// A joint or a body changed: the joints are made again before the next step.
		bool m_JointsChanged = false;
		// What the scene exchanged with an entity's character: the world position last written to or read from the entity's
		// transform (to tell edits from movement), where the entity's origin was after the last two steps and how far the
		// ground carried it in the last one (to draw it between steps), its scale, and the entities below it, whose bodies it
		// ignores.
		struct CharacterSyncState
		{
			UUID Entity = UUID::Invalid();
			glm::vec3 Position = glm::vec3(0.0f);
			glm::vec3 PreviousStepPosition = glm::vec3(0.0f);
			glm::vec3 StepPosition = glm::vec3(0.0f);
			glm::vec3 Carried = glm::vec3(0.0f);
			glm::vec3 Scale = glm::vec3(1.0f);
			std::vector<UUID> Parts;

			// Placed somewhere (made, moved by an edit or teleported): it is drawn there until it moves.
			void Place(glm::vec3 const& position)
			{
				Position = position;
				PreviousStepPosition = position;
				StepPosition = position;
				Carried = glm::vec3(0.0f);
			}
		};
		std::unordered_map<entt::entity, CharacterSyncState> m_Characters;
		// Entities whose CharacterController changed while running; their characters are updated before the next step.
		std::unordered_set<entt::entity> m_ChangedCharacters;
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
			// The field values the instance was last given or read, by name: stored values that differ are edits to give it.
			ScriptFieldMap Fields;
		};

		// Whether this scene runs scripts (it is the ScriptEngine's scene context).
		bool m_RunsScripts = false;
		// Set while OnDestroy runs on every script: entities created then start no scripts.
		bool m_StoppingScripts = false;
		std::unordered_map<entt::entity, ScriptInstanceState> m_ScriptInstances;

		struct ParticleSystemState
		{
			UUID Entity = UUID::Invalid();
			ParticleEmitter Emitter;
		};
		std::unordered_map<entt::entity, ParticleSystemState> m_ParticleSystems;
		// Seeds new emitters, so systems of one scene differ.
		uint64_t m_ParticleSeed = 0;

		// The interactable button under the mouse, and the one the left button went down on (a click if it comes up there).
		UUID m_UIHovered = UUID::Invalid();
		UUID m_UIPressed = UUID::Invalid();
		// Buttons clicked in the latest update, and the ones scripts clicked since (clicked at the next update).
		std::vector<UUID> m_UIClicked;
		std::vector<UUID> m_UIPendingClicks;
		// Whether the left mouse button was down at the latest update.
		bool m_UIMouseWasDown = false;
		bool m_UIPointerEnabled = true;
		bool m_UIPointerOver = false;

		std::vector<SceneDebugLine> m_DebugLines;
		bool m_DebugLinesDropped = false;
		std::unordered_set<AssetHandle> m_RuntimeAssets;

		uint32_t m_ViewportWidth = 0;
		uint32_t m_ViewportHeight = 0;
	};
}
