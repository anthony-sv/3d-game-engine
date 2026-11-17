#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/InputActions.h"
#include "Strada/Core/InputCapture.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/Timestep.h"
#include "Strada/Core/UUID.h"
#include "Strada/Physics/PhysicsScene.h"
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
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Strada
{
	class AudioScene;
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
		// Collision steps each physics step is divided into (PhysicsWorldSettings::CollisionSteps).
		uint32_t CollisionSteps = 1;
		// The fixed step after which the scene records its physics state hash (Scene::GetRecordedPhysicsHash); 0 records
		// none.
		uint64_t PhysicsHashStep = 0;
		// The project's input actions (an InputActions asset, ProjectSettings::InputActions), which the scene evaluates once
		// per frame for its scripts (Docs/Architecture.md §7.4); none, or an asset that does not load, reads every action as
		// zero.
		AssetHandle InputActions;
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

	// An entity's world matrices as the scene's world-transform cache holds them (Scene::GetCachedWorldTransform;
	// Docs/Architecture.md §7.1, "The world-transform cache").
	struct CachedWorldTransform
	{
		glm::mat4 World = glm::mat4(1.0f);
		// The world matrix the cache held for the entity when the scene's current frame began (Scene::BeginTransformFrame):
		// what the last refresh before it computed, which is the previous frame's when that frame was drawn (its views
		// refresh the cache, after the update and after what was written since). Equal to World in the entity's first frame,
		// and in a scene that never began a frame. Frames that were not drawn refresh nothing: a renderer that keeps history
		// tells the frame after the one it drew by Scene::GetTransformFrame and Scene::GetSerial.
		glm::mat4 Previous = glm::mat4(1.0f);
	};

	// A running scene's simulation between two fixed steps (Scene::SavePhysicsState), to go back to in the same run.
	// Opaque; held in memory only.
	class ScenePhysicsSnapshot
	{
	public:
		// The fixed steps the scene had run when it was saved (Scene::GetFixedStepCount).
		uint64_t GetStep() const { return m_Step; }
		// Bytes of physics state it holds.
		size_t GetPhysicsSize() const { return m_Physics.GetSize(); }

	private:
		friend class Scene;

		// An entity's local translation and rotation, compared and put back bit for bit.
		struct LocalPose
		{
			glm::vec3 Translation = glm::vec3(0.0f);
			glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

			bool operator==(LocalPose const& other) const = default;
		};
		// How the scene draws a moving body's entity between steps (Scene::InterpolateBodyTransforms): its local pose after
		// each of the last two steps and as it was last drawn, which parts of that pose were drawn, and whether something
		// other than the steps moved it since.
		struct BodyDrawing
		{
			LocalPose PreviousStep;
			LocalPose Step;
			LocalPose Drawn;
			// The entity shows Drawn's translation, rotation or both, which go back to Step's before the next physics changes
			// and steps. A part something else had moved since the step is shown where it was put and is not drawn.
			bool TranslationDrawn = false;
			bool RotationDrawn = false;
			// Moved since its last step by a script's OnUpdate, the editor or another frame-time change: what was moved is
			// shown where it was put, and its next step starts from there.
			bool Moved = false;

			bool IsDrawn() const { return TranslationDrawn || RotationDrawn; }

			// Placed somewhere (made, teleported, moved between frames): it is drawn there until it moves.
			void Place(LocalPose const& pose)
			{
				PreviousStep = pose;
				Step = pose;
				Moved = false;
			}
		};
		// An entity's transform, the pose last exchanged with its body and how it is drawn between steps.
		struct Body
		{
			UUID Entity = UUID::Invalid();
			TransformComponent Transform;
			glm::vec3 Position = glm::vec3(0.0f);
			glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			BodyDrawing Drawing;
		};
		// An entity's transform, where its character was drawn, where its last two steps left it, how far its ground carried
		// it and what that ground was.
		struct Character
		{
			UUID Entity = UUID::Invalid();
			TransformComponent Transform;
			glm::vec3 Position = glm::vec3(0.0f);
			glm::vec3 PreviousStepPosition = glm::vec3(0.0f);
			glm::vec3 StepPosition = glm::vec3(0.0f);
			glm::vec3 Carried = glm::vec3(0.0f);
			UUID Ground = UUID::Invalid();
		};

		PhysicsSnapshot m_Physics;
		uint64_t m_Step = 0;
		float m_Accumulator = 0.0f;
		// Whether the entities were drawn between steps when it was saved (after a frame's steps), and the state of the
		// sequence entities made while running take their UUIDs from.
		bool m_IsDrawnBetweenSteps = false;
		uint64_t m_IDSequence = 0;
		// In UUID order.
		std::vector<Body> m_Bodies;
		std::vector<Character> m_Characters;
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
		// Unique among the scenes this process made (a copy gets its own), never 0: what a renderer's history compares to tell
		// that it draws another scene.
		uint64_t GetSerial() const { return m_Serial; }
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
		// (C# and visual) exist and have run OnCreate when this returns.
		[[nodiscard]] Result<Entity> InstantiatePrefab(AssetHandle prefab, glm::vec3 const& position, glm::quat const& rotation,
		                                               Entity parent);

		bool HasEntity(UUID id) const { return m_EntityMap.contains(id); }
		// A UUID not used by any entity of this scene: a random one, or while the scene runs the next of a sequence seeded
		// from its entities' UUIDs as it started, so that every run makes the same entities (and calls physics in the same
		// order).
		UUID GenerateUniqueID();
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

		// --- World transforms (Docs/Architecture.md §7.1, "The world-transform cache") ---

		// The entity's world matrix: its TransformComponent's local matrix under its parent's world matrix, multiplied top
		// down (W(entity) = Math::ComposeWorldTransform(W(parent), L(entity)), W(root) = L(root)); identity for an invalid
		// entity or one of another scene. A parent the scene does not hold makes the entity a root, and so does a cycle of
		// parents for every entity on it. Never stale: transforms and parents written since the cache's last refresh, by any
		// path (a script, the editor, physics, a direct write of the components' public fields), are seen. When nothing in
		// the entity's chain of ancestors changed since the refresh, it compares each level's inputs and returns the cached
		// matrix; otherwise it multiplies down from the highest entity that changed. The refresh computes the same products
		// in the same order, so the matrix it returns and the one the next refresh computes have the same bits. It changes no
		// cached matrix: a write is taken into the cache by the refresh that follows it, in the frame that refresh belongs
		// to, which is what keeps CachedWorldTransform::Previous the matrix of the frame before.
		glm::mat4 GetWorldTransform(Entity entity);
		// Sets the local transform so the entity ends up with the given world transform.
		void SetWorldTransform(Entity entity, glm::mat4 const& worldTransform);
		// Begins a frame of the scene: the matrices the cache holds become each entity's previous matrix for the frame
		// (CachedWorldTransform::Previous). Called once per frame of the scene, before anything of the frame runs, by that
		// frame's one owner: OnUpdateRuntime while the scene runs (also when it is paused, and for every frame play.advance
		// runs), and the editor's play mode for a scene it shows without running it (the edited scene, a running scene the
		// debugger holds). Costs nothing per entity.
		void BeginTransformFrame();
		// Brings the cache up to date with every entity's TransformComponent and parent (the refresh, zone
		// Scene.UpdateTransforms): every entity that has a TransformComponent is visited once, after its ancestors; one
		// whose inputs or whose parent's matrix changed is computed again, every other one costs a comparison. RenderScene
		// calls it before it reads matrices, once per view: after the scripts, physics' drawing between steps and deferred
		// destruction's OnDestroy, and after the edits and tests that change transforms between a scene's update and its
		// views. A frame drawn by several views computes in the first refresh and compares in the others.
		void UpdateTransforms();
		// What the cache holds for an entity of this scene's registry as of the last refresh, without checking its chain:
		// exact when no transform or parent was written since the refresh. An entity the cache has no entry for (made since
		// the refresh) is computed as GetWorldTransform would compute it, with Previous equal to World. For phases that
		// refreshed the cache and then read many entities (the renderer).
		CachedWorldTransform GetCachedWorldTransform(entt::entity handle);
		// Calls function(handle, component, world, previous) for every entity with a TComponent, in the order of the
		// registry's view of it, with the matrices GetCachedWorldTransform gives: the walk of a phase that reads thousands of
		// entities (the renderer's meshes), which finds the cache's storage once. The matrices are valid during the call; the
		// function adds or removes no TComponent and no entity.
		template<typename TComponent, typename TFunction>
		void ForEachCachedWorldTransform(TFunction&& function)
		{
			entt::storage_for_t<WorldTransformEntry> const& entries = m_Registry.storage<WorldTransformEntry>();
			for (auto const& [handle, component] : m_Registry.view<TComponent>().each())
			{
				WorldTransformEntry const* entry = entries.contains(handle) ? &entries.get(handle) : nullptr;
				if (entry != nullptr && entry->Version != 0)
				{
					bool const changedThisFrame = m_TransformFrame != 0 && entry->ChangedFrame == m_TransformFrame;
					function(handle, component, entry->World, changedThisFrame ? entry->Previous : entry->World);
				}
				else
				{
					CachedWorldTransform const computed = GetCachedWorldTransform(handle);
					function(handle, component, computed.World, computed.Previous);
				}
			}
		}
		// The frames of the scene begun so far (BeginTransformFrame); 0 for a scene that never began one. A renderer that keeps
		// history tells from it, with GetSerial, whether it draws the frame after the one it drew last.
		uint64_t GetTransformFrame() const { return m_TransformFrame; }

		// Depth-first, parents before children, siblings in order.
		void ForEachEntityInHierarchyOrder(std::function<void(Entity)> const& function);

		// First entity (in hierarchy order) with a primary camera; invalid Entity when none.
		Entity GetPrimaryCameraEntity();

		// --- Runtime ---

		// Starts simulating: a physics body for every entity with colliders (static without a RigidBody), a character for
		// every CharacterController (instead of a body), a sound for every audio source with a clip (playing when PlayOnStart
		// is set), a script instance for every Script component whose class the loaded game assembly has and a graph
		// instance for every VisualScript component whose graph runs (all are created, then OnCreate runs for each: the C#
		// scripts first); sounds and scripts only when the settings ask for them. Bodies, characters, sounds and scripts
		// follow component changes while running, and graph instances follow edits of their graphs (live edit). Visual
		// scripts need no game assembly; C# scripts run only when one is loaded as the scene starts. Requires PhysicsSystem,
		// AudioEngine and ScriptEngine (without them the scene runs without physics, audio or scripts, logged); stop the
		// runtime or destroy the scene before shutting them down. One scene runs scripts at a time.
		void OnRuntimeStart(SceneRuntimeSettings const& settings = {});
		// Calls OnDestroy on every script (the C# scripts, then the visual scripts), then stops the audio and physics.
		void OnRuntimeStop();
		// Advances the runtime: input actions (evaluated once, then their edges' events: UpdateInputActions), the UI (the
		// mouse over buttons, clicks and OnClick), scripts' OnUpdate, fixed steps (scripts'
		// OnFixedUpdate, then physics: kinematic bodies follow their transforms, moved static and dynamic bodies and
		// characters teleport, characters move, dynamic bodies and characters write their positions back), dynamic and
		// kinematic bodies (unless their RigidBody's Interpolation is None) and characters drawn between their last two steps
		// (so they move smoothly at any frame rate, a step behind; OnFixedUpdate sees them where the last step left them, and
		// the translation or rotation of an entity moved since its last step by anything but the steps is shown where it was
		// put), contact events to scripts, audio (sources and the listener follow their entities), particles, then deferred
		// entity destruction (destroyed scripts get OnDestroy). Every update and fixed update reaches the C# scripts, then the
		// visual scripts; an event of an entity reaches its C# script, then its visual script.
		void OnUpdateRuntime(Timestep timestep);
		bool IsRunning() const { return m_IsRunning; }
		bool IsPaused() const { return m_IsPaused; }
		// Pausing holds the simulation and every sound.
		void SetPaused(bool paused);
		// While paused, advances the given number of frames.
		void Step(uint32_t frames = 1) { m_StepFrames += frames; }
		// Drops the frames Step queued that have not run yet (a debugger stopping the game keeps it where it stopped).
		void CancelSteps() { m_StepFrames = 0; }
		// The frames Step queued that have not run yet.
		uint32_t GetQueuedSteps() const { return m_StepFrames; }
		uint64_t GetRuntimeFrame() const { return m_RuntimeFrame; }
		double GetRuntimeTime() const { return m_RuntimeTime; }
		// Seconds per physics step (and OnFixedUpdate) of the running scene: the runtime settings' step.
		float GetFixedTimestep() const { return std::max(m_RuntimeSettings.FixedTimestep, 1e-4f); }
		// The fixed steps (OnFixedUpdate, then physics) the running scene has run since it started, with or without physics.
		uint64_t GetFixedStepCount() const { return m_FixedStepCount; }
		// A 64-bit hash of the running scene's simulation (Architecture.md section 8): the fixed step count, PhysicsScene's
		// hash of its bodies and characters, then each character's step positions in UUID order. The step accumulator is
		// left out, so a scene run at different frame rates hashes alike at one step count. Without physics, the step count
		// alone.
		uint64_t ComputePhysicsStateHash() const;
		// The hash taken right after the fixed step SceneRuntimeSettings::PhysicsHashStep (the latest time that step ran, when
		// a snapshot went back before it); empty before that step and when none was asked for.
		std::optional<uint64_t> GetRecordedPhysicsHash() const { return m_RecordedPhysicsHash; }
		// Saves the running scene's simulation between frames (ScenePhysicsSnapshot): the physics world's state
		// (PhysicsScene::SaveState), the fixed step count, the accumulator, the sequence of the UUIDs entities made while
		// running take, and what the scene exchanged with each body and character. Physics component changes not applied yet
		// are applied first. Fails when the scene runs no physics.
		[[nodiscard]] Result<ScenePhysicsSnapshot> SavePhysicsState();
		// Goes back to a snapshot of this run between frames: the physics world, the step count and accumulator, the UUID
		// sequence (an ID still held by an entity made since is skipped), and the entities of bodies and characters where they
		// were then, so the steps that follow run as they did. Physics component changes not applied yet are applied first, as
		// the next frame would. Then it fails, changing nothing more, when the scene runs no physics, the snapshot is of
		// another run, or a body, joint or character was added, removed or rebuilt since. What the simulation does not change
		// (scripts' state, entities without bodies, settings) stays as it is.
		[[nodiscard]] Result<void> RestorePhysicsState(ScenePhysicsSnapshot const& snapshot);
		// The physics world while running; null otherwise or when physics is unavailable.
		PhysicsScene* GetPhysicsScene() { return m_Physics.get(); }
		// The sounds while running; null otherwise or when audio is unavailable.
		AudioScene* GetAudioScene() { return m_Audio.get(); }
		// Collisions and trigger overlaps that began or ended during the last runtime update.
		std::vector<ContactEvent> const& GetContactEvents() const { return m_ContactEvents; }
		// The running scene's input actions as its latest frame evaluated them (Docs/Architecture.md §7.4): what scripts'
		// and graphs' action queries read while the scene runs them (a fixed step's edges for OnFixedUpdate's code). Empty (every
		// action zero) while the scene does not run or the runtime settings name no actions that load.
		InputActionState& GetInputActions() { return m_InputActions; }
		InputActionState const& GetInputActions() const { return m_InputActions; }
		// Rebinding the running scene's actions while it runs (Docs/Architecture.md §7.4, "Rebinding"; SceneInputActions.cpp).
		// The bindings are the process's (InputRebinding, for the runtime settings' InputActions asset), so a change reaches
		// every running scene at its next frame and outlives scene loads; a capture belongs to the scene that started it.
		// Starts listening for the control that binding `binding` of the action takes (`part` -1 the whole binding, else one
		// control of a composite), from this frame's input on: the first fitting control given (InputControlCapture), checked
		// before each of this scene's evaluations, is bound as RebindInput binds it; Escape, or InputControlCapture::Timeout
		// seconds of frames (unscaled: a game paused by a time scale of 0 still counts them), cancel it. Ends a capture
		// already listening (Cancelled). Answers Listening (the binding's controls now), or Refused as RebindInput refuses,
		// also when the scene does not run.
		InputRebindResult StartInputRebind(std::string_view action, int32_t binding, int32_t part = -1);
		// Ends a listening capture (Cancelled); nothing otherwise.
		void CancelInputRebind();
		// The latest capture's state: Listening while it listens, then how it ended (Done with the conflicts, Cancelled,
		// Refused); None before any since the scene started.
		InputRebindResult const& GetInputRebind() const { return m_InputRebind; }
		// InputRebinding::Rebind, ResetBindings and GetBindingControls for the runtime settings' actions.
		InputRebindResult RebindInput(std::string_view action, int32_t binding, std::string_view controls, int32_t part = -1);
		InputRebindResult ResetInputBindings();
		// What the latest StartInputRebind, RebindInput or ResetInputBindings of this scene answered (what scripts read their
		// answers' text from); None before any since the scene started.
		InputRebindResult const& GetInputRebindAnswer() const { return m_InputRebindAnswer; }
		[[nodiscard]] Result<std::string> GetInputBindingControls(std::string_view action, int32_t binding) const;
		// The running scene's physics queries (PhysicsScene's, Architecture.md section 8 "Queries"). Physics changes not applied
		// yet are applied first (ApplyPhysicsChanges), so a body made or rebuilt since the last step is found where its entity
		// is; every other body is found where the last step left it, which is where OnFixedUpdate reads it (an entity moved
		// since, by a script, the editor or its parent, is found where it was until the next step takes its body there, and
		// in OnUpdate a body drawn between its last two steps is found up to a step ahead of where it is drawn). They find
		// nothing while the scene runs no physics.
		std::optional<RaycastHit> Raycast(glm::vec3 const& origin, glm::vec3 const& direction, float maxDistance,
		                                  uint32_t layerMask = AllPhysicsLayers);
		size_t RaycastAll(glm::vec3 const& origin, glm::vec3 const& direction, float maxDistance, std::vector<RaycastHit>& hits,
		                  PhysicsQueryFilter const& filter = {});
		std::optional<ShapeCastHit> ShapeCast(PhysicsQueryShape const& shape, glm::vec3 const& origin, glm::quat const& rotation,
		                                      glm::vec3 const& direction, float maxDistance, PhysicsQueryFilter const& filter = {});
		size_t Overlap(PhysicsQueryShape const& shape, glm::vec3 const& center, glm::quat const& rotation, std::vector<UUID>& entities,
		               PhysicsQueryFilter const& filter = {});

		// How fast the runtime's time passes for scripts and physics (sounds are not affected); 0 freezes the simulation.
		float GetTimeScale() const { return m_TimeScale; }
		// Negative and non-finite scales are ignored.
		void SetTimeScale(float scale);
		// Creates and rebuilds the bodies and characters of physics components changed since the last step. The runtime does
		// it before each update; script calls on bodies and characters do it first, so a body added this frame takes forces
		// at once. When there are changes, the entities drawn between steps first go back to where their last steps left them
		// (as before every frame's steps), so that bodies and joints are made from where the simulation has them; between
		// frames (after a frame's steps: contact events, OnDestroy, the next OnUpdate, the editor) they are then drawn again as
		// the frame drew them, so the frame shows them where it did.
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
		// While the scene runs scripts: the values the public variables of the entity's graph instance hold now, which also
		// become the stored values of its VisualScript component (Variables, under the current names of the variables of the
		// program it runs), so that the component shows them and edits start from them. Fails when the entity runs no visual
		// script.
		[[nodiscard]] Result<ScriptFieldMap> ReadVisualScriptVariables(Entity entity);
		// While the scene runs scripts: gives each graph instance the stored values of its VisualScript component's variables
		// that differ from the ones it was last given or read (ReadVisualScriptVariables), so that edits of a running scene
		// reach its visual scripts. Editors call it after each edit; a graph that changed gets a new instance at the next
		// update instead. Values of variables the running program lacks wait for a live edit that adds them, and reach the
		// instance as it takes the edit.
		void ApplyVisualScriptVariableChanges();
		// While the scene runs scripts: gives every visual script its VisualScript components use, as the asset holds it now,
		// to the scripting runtime (VisualScriptEngine::EnsureLoaded, the update's own first step), so that an edit reaches the
		// running instances at once, also while paused and into stopped executions. No graph code runs: loads, swaps and
		// migrations only. Entities that could not run before start at the next update, as before. Editors call it after each
		// edit, before ApplyVisualScriptVariableChanges (which then gives the instances the values edited meanwhile).
		void ApplyVisualScriptEdits();

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
		// One fixed step: OnFixedUpdate, the physics changes it made, the syncs to physics, the physics step and the syncs back;
		// then it is counted (and the hash recorded after SceneRuntimeSettings::PhysicsHashStep).
		void RunFixedStep(float step);
		// Syncs the physics materials (SyncPhysicsMaterials), then rebuilds the bodies, characters and joints that changed.
		void RebuildChangedBodies();
		// When the data asset registry's epoch moved: gives the bodies the values of every physics material their colliders
		// use whose version changed (PhysicsScene::SetMaterialValues, no body rebuilt), and marks for rebuilding the bodies
		// naming a material that started or stopped applying (its file loads again, or no longer does).
		void SyncPhysicsMaterials();
		// The body's colliders named these materials (sorted, without repeats): counts them in m_PhysicsMaterials, and stops
		// counting those of its previous body.
		void TrackPhysicsMaterials(std::vector<AssetHandle> const& previous, std::vector<AssetHandle> const& current);
		// The entities with a body (or a character), in the order of their UUIDs: physics is called in this order.
		std::vector<entt::entity> const& GetBodyOrder();
		std::vector<entt::entity> const& GetCharacterOrder();
		// The entities with a dynamic or kinematic body, in the order of their UUIDs: those drawn between steps.
		std::vector<entt::entity> const& GetMovingBodies();
		// Creates, rebuilds or removes the body of an entity after its components changed; true when its body changed.
		bool UpdateBody(entt::entity handle);
		void SyncBodiesFromTransforms(float stepDelta);
		void WriteBodyTransforms();
		// Between frames, moving bodies are drawn between their last two steps, in local space (a body's children and a body
		// below another follow as they do at the steps). After each step their entities' local poses are recorded. Before the
		// physics changes and steps of a frame, the translation and the rotation of each drawn entity go back, each on its
		// own, exactly to their last step's when nothing changed them since they were drawn, and entities moved since their
		// last step by anything but the steps (OnUpdate, the editor) are noted: what was moved is shown where it was put, and
		// their next step starts there. After the steps, each part still where the last step left it is drawn the part of the
		// way from the step before (alpha 0) to the last one (alpha 1) that the time left over makes up, when the RigidBody
		// interpolates.
		void RecordBodySteps();
		void ReturnBodiesToSteps();
		void NoteBodiesMovedSinceSteps();
		void InterpolateBodyTransforms(float alpha);
		// How far into their last step the frame draws bodies and characters: the time left over, as a part of a step.
		float GetDrawingAlpha() const;
		bool IsBodyDrawn(UUID entity) const;
		// Physics component changes are waiting for RebuildChangedBodies.
		bool HasPendingPhysicsChanges() const;
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
		// those not moved since go back to where the last step left them; after the steps (and after the bodies), they are
		// drawn the part of the way from the step before (alpha 0) to the last one (alpha 1) that the time left over makes up:
		// the whole way when what they stand on is drawn so too, else the way they went by themselves.
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

		// Script runtime: C# script instances (Script components) and graph instances (VisualScript components), which start
		// together and get every event in that order (SceneScripting.cpp; the visual scripts' own part in
		// SceneVisualScripting.cpp).
		struct PendingScriptInstance
		{
			entt::entity Handle = entt::null;
			UUID Entity = UUID::Invalid();
			std::string ClassName;
			ScriptFieldMap Fields;
		};
		struct PendingVisualScriptInstance
		{
			entt::entity Handle = entt::null;
			UUID Entity = UUID::Invalid();
			AssetHandle Graph;
			ScriptFieldMap Variables;
		};
		void StartScripts();
		void StopScripts();
		// Creates instances for new Script and VisualScript components, replaces those whose class or graph changed and tries
		// again the graphs that could not start once their program changed; OnCreate runs for each new instance once all exist.
		void SyncScriptInstances();
		// The graph instances SyncScriptInstances replaces and creates. The runtime gets the current document of each graph
		// first, so the running instances of an edited graph take the edit (live edit).
		void CollectVisualScriptChanges(std::vector<entt::entity>& replaced, std::vector<PendingVisualScriptInstance>& pending);
		// Creates the instances of both kinds, then runs OnCreate for each (the C# scripts first), so scripts can find each
		// other there.
		void CreateScriptInstances(std::vector<PendingScriptInstance> const& scripts,
		                           std::vector<PendingVisualScriptInstance> const& visualScripts);
		// Creates graph instances without running OnCreate; returns the entities that got one.
		std::vector<UUID> CreateVisualScriptInstances(std::vector<PendingVisualScriptInstance> const& pending);
		// Starts the scripts of new entities (prefab instances) at once instead of at the next update.
		void StartScriptsOf(std::vector<entt::entity> const& handles);
		void UpdateScripts(float deltaTime);
		void FixedUpdateScripts(float fixedDeltaTime);
		void DispatchContactEventsToScripts();
		void DestroyScriptInstance(entt::entity handle);
		void DestroyVisualScriptInstance(entt::entity handle);
		// Instances of both kinds whose entity or component is gone, in handle order (an entity's C# script first).
		void DestroyRemovedScriptInstances();
		std::vector<UUID> GetScriptInstanceIDs() const;
		std::vector<UUID> GetVisualScriptInstanceIDs() const;
		// The profiler's zone name of an entity's C# script: "Script.<class>" (class names may hold namespaces' dots, so the
		// prefix keeps them out of the subsystem rule). Taken only while the profiler records.
		std::string GetScriptProfileName(UUID id) const;

		// Input actions (SceneInputActions.cpp; Docs/Architecture.md §7.4). The frame's one named call of input actions,
		// which OnUpdateRuntime makes before UpdateUI and the scripts, in frames that run (a paused scene's do not): makes the
		// set again when what it was made from changed (the runtime settings' InputActions asset or a response curve it
		// names, or the process's rebound bindings: InputRebinding's version; the actions' state kept by name), then evaluates
		// it from Input (InputActionState::Evaluate) and dispatches the evaluation's edges to the scripts
		// (DispatchInputActionEvents). Before that, a listening capture looks at the input (UpdateInputRebind), so a control
		// it binds is read by this very evaluation. `frameSeconds` is the frame's unscaled time, which the capture's time limit
		// counts. Returns at once when the settings name no actions. Zone Scene.InputActions.
		void UpdateInputActions(float frameSeconds);
		// Advances a listening capture (InputControlCapture::Update) and binds what it captured (InputRebinding::Rebind).
		void UpdateInputRebind(float frameSeconds);
		// Each edge of the latest evaluation, in its order (InputActionState::GetEdges), to every C# script instance
		// (OnActionPressed or OnActionReleased), then to every graph instance (Event.ActionPressed or Event.ActionReleased),
		// inside a ScriptEngine::InputActionEventScope that names the action. Nothing when no scripts run.
		void DispatchInputActionEvents();
		// At runtime start: the set made from the settings' asset, taken without an edge, logged with its action count.
		void StartInputActions();
		void StopInputActions();
		// Compares what the set was made from with the assets now and, when it changed (or `starting`), compiles again; an
		// asset that does not load or compile is logged and leaves the previous set in place.
		void RebuildInputActions(bool starting);

		// UI runtime (SceneUI.cpp).
		void ResetUI();
		// Follows the mouse over the UI and its buttons, then clicks the buttons it clicked and the ones scripts clicked, calling
		// OnClick on the scripts of each button and its ancestors.
		void UpdateUI();

		// The world-transform cache (SceneTransforms.cpp; Docs/Architecture.md §7.1, "The world-transform cache").
		//
		// An entity's entry, kept in a storage of m_Registry for this type, which ComponentRegistry does not list: it is never
		// serialized, copied (Scene::Copy, duplicates and prefab instances start without entries), shown or scripted, and it
		// is destroyed with its entity. EnTT's storage keeps its elements in pages that never move, so references to entries
		// stay valid while others are added (entt/entity/storage.hpp, assure_at_least).
		struct WorldTransformEntry
		{
			// The inputs World was computed from, compared bit for bit with the entity's TransformComponent and
			// RelationshipComponent::Parent.
			glm::vec3 Translation = glm::vec3(0.0f);
			glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			glm::vec3 Scale = glm::vec3(1.0f);
			UUID Parent = UUID::Invalid();
			// The entity's own UUID, which a child compares with its Parent to know that the handle it resolved still holds
			// its parent.
			UUID Id = UUID::Invalid();
			// The handle Parent resolved to (null when it resolves to no entity with a TransformComponent), kept so that a
			// walk up the hierarchy needs no lookup of the UUID while that handle is valid and still has the UUID.
			entt::entity ParentHandle = entt::null;
			// The entity World was computed under: ParentHandle, or null for a root (no parent, one the scene does not hold,
			// or an entity on a cycle of parents).
			entt::entity WorldParent = entt::null;
			// The Version of WorldParent's entry World was computed with, and this World's own: unique within the scene and
			// new at every computation, 0 until the first. A child is current only while its parent's Version is the one it
			// was computed with.
			uint64_t ParentVersion = 0;
			uint64_t Version = 0;
			// The frame of the scene (m_TransformFrame) in which World was last computed, and the refresh
			// (m_TransformRefresh) that last checked the entry, so that a refresh handles each entity once.
			uint64_t ChangedFrame = 0;
			uint64_t CheckedRefresh = 0;
			glm::mat4 World = glm::mat4(1.0f);
			// The matrix World replaced in the frame ChangedFrame: the entity's previous matrix during that frame only (in
			// later frames World is).
			glm::mat4 Previous = glm::mat4(1.0f);
			// The local matrix of Translation, Rotation and Scale (TransformComponent::GetTransform): an entity whose own
			// inputs did not change is computed again under its parent's new matrix with the product alone.
			glm::mat4 Local = glm::mat4(1.0f);
		};
		// One entity of a walk up the hierarchy: its transform, and the handle its Parent resolved to (null for none).
		struct TransformLink
		{
			entt::entity Handle = entt::null;
			TransformComponent const* Transform = nullptr;
			UUID ParentId = UUID::Invalid();
			entt::entity Parent = entt::null;
		};
		// How a walk ended: at a root, before an ancestor the running refresh already checked (the last link's Parent), or
		// at a cycle of parents, whose entities are the links from CycleStart on (each once).
		struct TransformWalk
		{
			bool ReachedChecked = false;
			size_t CycleStart = std::numeric_limits<size_t>::max();
		};
		// The registry's storages the cache reads, looked up once per call rather than once per entity (SceneTransforms.cpp).
		struct TransformStorages;
		// Fills m_TransformChain with the entity (which has a TransformComponent) and its ancestors, nearest first, up to a
		// root, the first ancestor this refresh checked (stopAtChecked) or the end of a cycle.
		TransformWalk WalkTransformChain(TransformStorages const& storages, entt::entity handle, bool stopAtChecked);
		// The handle of the entity's parent: null for none, and for a parent the scene does not hold or that has no
		// TransformComponent.
		entt::entity ResolveTransformParent(TransformStorages const& storages, entt::entity handle, UUID parentId) const;
		// Computes an entry's World again from its entity's transform and the entry of the entity it is computed under
		// (null for a root), keeping the matrix the frame began with (Previous) and giving World a new Version. sameInputs:
		// the transform holds the inputs the entry's Local was made from, which then serves again.
		void StoreWorldTransform(WorldTransformEntry& entry, TransformComponent const& transform, bool sameInputs, UUID parentId,
		                         entt::entity parentHandle, entt::entity worldParent, WorldTransformEntry const* worldParentEntry);
		// The refresh's step for an entity without a walk, when its entry was computed under the parent link it has now and
		// that parent was checked by this refresh already; false when it needs the walk.
		bool UpdateTransformEntry(TransformStorages const& storages, entt::entity handle, TransformComponent const& transform,
		                          WorldTransformEntry& entry);
		// Brings every entry of m_TransformChain up to date, top down (the refresh).
		void UpdateTransformChain(TransformStorages const& storages, TransformWalk const& walk);
		// The world matrix of m_TransformChain's first entity, computed from the entries that are current and the products
		// below the highest one that is not, changing no entry (GetWorldTransform).
		glm::mat4 ComputeTransformChain(TransformStorages const& storages, TransformWalk const& walk) const;

		// Frames begun (BeginTransformFrame) and refreshes run (UpdateTransforms) since the scene was made, and the last
		// Version given to a World; no frame begun means every Previous reads as World.
		uint64_t m_TransformFrame = 0;
		uint64_t m_TransformRefresh = 0;
		uint64_t m_TransformVersion = 0;
		// Reused by every walk, so that reading a transform allocates nothing once it has grown.
		std::vector<TransformLink> m_TransformChain;

		uint64_t m_Serial = 0;

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

		// The world pose last exchanged with an entity's body (to tell transform edits from simulation), whether the body
		// moves (dynamic or kinematic), how its entity is drawn between steps, and the physics materials its colliders named
		// (sorted, without repeats; applied or not).
		struct BodyState
		{
			UUID Entity = UUID::Invalid();
			glm::vec3 Position = glm::vec3(0.0f);
			glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			bool Moves = false;
			ScenePhysicsSnapshot::BodyDrawing Drawing;
			std::vector<AssetHandle> Materials;
		};
		// A physics material the scene's bodies name: the version (DataAsset::GetVersion, never 0) of the material whose values
		// they took, or 0 when it applied none (an unregistered or unloadable material, which counts as unset), and how many
		// bodies name it.
		struct PhysicsMaterialUse
		{
			uint64_t Version = 0;
			uint32_t Bodies = 0;
		};

		SceneRuntimeSettings m_RuntimeSettings;
		Scope<PhysicsScene> m_Physics;
		float m_PhysicsAccumulator = 0.0f;
		// From the end of a frame's steps to the start of the next frame's: the entities show the frame's drawing between
		// steps, which ApplyPhysicsChanges draws again after it returned them to their steps.
		bool m_IsDrawnBetweenSteps = false;
		uint64_t m_FixedStepCount = 0;
		std::optional<uint64_t> m_RecordedPhysicsHash;
		// While running: the state of the sequence GenerateUniqueID takes UUIDs from.
		uint64_t m_RuntimeIDSequence = 0;
		std::unordered_map<entt::entity, BodyState> m_Bodies;
		// The keys of m_Bodies and m_Characters in the order of their UUIDs, made again after an entry was added or removed.
		std::vector<entt::entity> m_BodyOrder;
		std::vector<entt::entity> m_CharacterOrder;
		bool m_BodyOrderChanged = false;
		bool m_CharacterOrderChanged = false;
		// The keys of m_Bodies whose bodies move, made again after a body was added, removed or rebuilt; and the entities
		// drawn between steps since the last return to the steps.
		std::vector<entt::entity> m_MovingBodies;
		bool m_MovingBodiesChanged = false;
		std::vector<entt::entity> m_DrawnBodies;
		// Entities whose physics components changed while running; their bodies are rebuilt before the next step.
		std::unordered_set<entt::entity> m_ChangedBodies;
		// A joint or a body changed: the joints are made again before the next step.
		bool m_JointsChanged = false;
		// The physics materials the bodies name, and the data asset registry's epoch when they were last synced.
		std::unordered_map<AssetHandle, PhysicsMaterialUse> m_PhysicsMaterials;
		uint64_t m_PhysicsMaterialEpoch = 0;
		// What the scene exchanged with an entity's character: the world position last written to or read from the entity's
		// transform (to tell edits from movement), where the entity's origin was after the last two steps, how far the ground
		// carried it in the last one and the entity of that ground (to draw it between steps), its scale, and the entities
		// below it, whose bodies it ignores.
		struct CharacterSyncState
		{
			UUID Entity = UUID::Invalid();
			glm::vec3 Position = glm::vec3(0.0f);
			glm::vec3 PreviousStepPosition = glm::vec3(0.0f);
			glm::vec3 StepPosition = glm::vec3(0.0f);
			glm::vec3 Carried = glm::vec3(0.0f);
			UUID Ground = UUID::Invalid();
			glm::vec3 Scale = glm::vec3(1.0f);
			std::vector<UUID> Parts;

			// Placed somewhere (made, moved by an edit or teleported): it is drawn there until it moves.
			void Place(glm::vec3 const& position)
			{
				Position = position;
				PreviousStepPosition = position;
				StepPosition = position;
				Carried = glm::vec3(0.0f);
				Ground = UUID::Invalid();
			}
		};
		std::unordered_map<entt::entity, CharacterSyncState> m_Characters;
		// Entities whose CharacterController changed while running; their characters are updated before the next step.
		std::unordered_set<entt::entity> m_ChangedCharacters;
		std::vector<ContactEvent> m_ContactEvents;

		// While running: the actions' state, set at runtime start (no edge for what is already held) and cleared at stop,
		// and what its set was made from (Docs/Architecture.md §7.4): the data asset registry's epoch it was last compared
		// at, the asset's version (0: it did not load) and the response curves' versions, the stamp a rebuild compares.
		InputActionState m_InputActions;
		uint64_t m_InputActionsEpoch = 0;
		uint64_t m_InputActionsAssetVersion = 0;
		std::vector<std::pair<AssetHandle, uint64_t>> m_InputActionCurves;
		// InputRebinding's version the set was made with.
		uint64_t m_InputRebindingVersion = 0;
		// The capture this scene started, and its latest result.
		InputControlCapture m_InputCapture;
		InputRebindResult m_InputRebind;
		InputRebindResult m_InputRebindAnswer;

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

		// The graph an entity's graph instance was made from, and the generation of its program (VisualScriptEngine) then,
		// followed by its variables while it runs; HasInstance is false when it could not be made (no graph, a graph that is
		// absent or has no program that runs) until the graph or its program changes.
		struct VisualScriptInstanceState
		{
			UUID Entity = UUID::Invalid();
			AssetHandle Graph;
			uint64_t Generation = 0;
			bool HasInstance = false;
			// The values the instance was last given or read, by the names of the variables of the program it runs (stored
			// values of variables it lacks are kept as it was given them): stored values that differ are edits to give it.
			ScriptFieldMap Variables;
		};
		// Gives a running graph instance the stored values of its component that differ from those it was given or read and
		// that its program has variables for, after following a live edit of its program (a new generation): the values it
		// was given then go under the variables' new names, so only edits reach it, those of variables the edit adds
		// included. Without giveEdits nothing happens unless the program changed.
		void UpdateVisualScriptVariables(entt::entity handle, VisualScriptInstanceState& state, VisualScriptComponent const& component,
		                                 bool giveEdits);

		// Whether this scene runs scripts (it is the ScriptEngine's scene context).
		bool m_RunsScripts = false;
		// Whether its Script components get instances: a game assembly was loaded when its scripts started (visual scripts
		// need none, so a scene with VisualScript components runs them anyway).
		bool m_RunsScriptComponents = false;
		// Set while OnDestroy runs on every script: entities created then start no scripts.
		bool m_StoppingScripts = false;
		std::unordered_map<entt::entity, ScriptInstanceState> m_ScriptInstances;
		std::unordered_map<entt::entity, VisualScriptInstanceState> m_VisualScriptInstances;

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
