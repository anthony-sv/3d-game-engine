#include "stpch.h"
#include "Strada/Scene/Scene.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Math/Math.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Strada
{
	namespace
	{
		// Transform edits smaller than this are not worth teleporting a body for (decomposition round trips are noisy).
		constexpr float PositionTolerance = 1e-4f;
		constexpr float RotationTolerance = 1e-6f;
		constexpr float ScaleTolerance = 1e-4f;

		bool HasMoved(glm::vec3 const& positionA, glm::vec3 const& positionB)
		{
			glm::vec3 const delta = positionA - positionB;
			return glm::dot(delta, delta) > PositionTolerance * PositionTolerance;
		}

		bool HasMoved(glm::vec3 const& positionA, glm::quat const& rotationA, glm::vec3 const& positionB, glm::quat const& rotationB)
		{
			// q and -q are the same rotation.
			return HasMoved(positionA, positionB) || 1.0f - std::abs(glm::dot(rotationA, rotationB)) > RotationTolerance;
		}

		// A character's capsule: scaled with the entity (the radius by the larger horizontal scale), not turned with it.
		CharacterDesc MakeCharacterDesc(UUID entity, CharacterControllerComponent const& component, glm::vec3 const& position,
		                                glm::vec3 const& scale)
		{
			glm::vec3 const size = glm::abs(scale);
			CharacterDesc desc;
			desc.Entity = entity;
			desc.Position = position + component.Offset * scale;
			desc.Radius = component.Radius * std::max(size.x, size.z);
			desc.Height = component.Height * size.y;
			desc.StepHeight = component.StepHeight * size.y;
			desc.MaxSlopeAngle = glm::radians(component.MaxSlopeAngle);
			desc.Mass = component.Mass;
			desc.PushForce = component.PushForce;
			desc.GravityFactor = component.GravityFactor;
			desc.Layer = component.Layer;
			return desc;
		}

		void AddCollider(BodyDesc& desc, ColliderDesc collider, glm::vec3 const& offset, bool trigger, float friction, float restitution)
		{
			collider.Offset = offset;
			collider.IsTrigger = trigger;
			collider.Friction = friction;
			collider.Restitution = restitution;
			desc.Colliders.push_back(std::move(collider));
		}

		// The body of an entity from its components; empty without colliders, and for characters.
		std::optional<BodyDesc> BuildBodyDesc(Scene& scene, Entity entity)
		{
			if (entity.HasComponent<CharacterControllerComponent>())
			{
				if (scene.GetRegistry()
				        .any_of<RigidBodyComponent, BoxColliderComponent, SphereColliderComponent, CapsuleColliderComponent,
				                MeshColliderComponent>(entity.GetHandle()))
				{
					ST_CORE_WARN("Physics: the CharacterController of '{}' replaces its RigidBody and colliders", entity.GetName());
				}
				return std::nullopt;
			}
			BodyDesc desc;
			desc.Entity = entity.GetUUID();
			if (BoxColliderComponent const* box = entity.TryGetComponent<BoxColliderComponent>())
			{
				ColliderDesc collider;
				collider.Shape = ColliderShape::Box;
				collider.HalfExtents = box->HalfExtents;
				AddCollider(desc, std::move(collider), box->Offset, box->IsTrigger, box->Friction, box->Restitution);
			}
			if (SphereColliderComponent const* sphere = entity.TryGetComponent<SphereColliderComponent>())
			{
				ColliderDesc collider;
				collider.Shape = ColliderShape::Sphere;
				collider.Radius = sphere->Radius;
				AddCollider(desc, std::move(collider), sphere->Offset, sphere->IsTrigger, sphere->Friction, sphere->Restitution);
			}
			if (CapsuleColliderComponent const* capsule = entity.TryGetComponent<CapsuleColliderComponent>())
			{
				ColliderDesc collider;
				collider.Shape = ColliderShape::Capsule;
				collider.Radius = capsule->Radius;
				collider.HalfHeight = capsule->HalfHeight;
				AddCollider(desc, std::move(collider), capsule->Offset, capsule->IsTrigger, capsule->Friction, capsule->Restitution);
			}
			if (MeshColliderComponent const* meshCollider = entity.TryGetComponent<MeshColliderComponent>())
			{
				AssetHandle meshHandle = meshCollider->Mesh;
				if (!meshHandle.IsValid())
				{
					if (MeshComponent const* mesh = entity.TryGetComponent<MeshComponent>())
					{
						meshHandle = mesh->Mesh;
					}
				}
				Ref<MeshSource> const mesh = AssetManager::IsInitialized() ? AssetManager::GetAsset<MeshSource>(meshHandle) : nullptr;
				if (mesh)
				{
					ColliderDesc collider;
					collider.Shape = meshCollider->Convex ? ColliderShape::ConvexMesh : ColliderShape::TriangleMesh;
					collider.Mesh = mesh;
					AddCollider(desc, std::move(collider), glm::vec3(0.0f), meshCollider->IsTrigger, meshCollider->Friction,
					            meshCollider->Restitution);
				}
				else
				{
					ST_CORE_WARN("Physics: the mesh collider of '{}' has no mesh", entity.GetName());
				}
			}
			if (desc.Colliders.empty())
			{
				if (entity.HasComponent<RigidBodyComponent>())
				{
					ST_CORE_WARN("Physics: '{}' has a RigidBody but no colliders, so it is not simulated", entity.GetName());
				}
				return std::nullopt;
			}

			glm::mat4 const world = scene.GetWorldTransform(entity);
			if (!Math::DecomposeTransform(world, desc.Position, desc.Rotation, desc.Scale))
			{
				ST_CORE_WARN("Physics: the transform of '{}' cannot be decomposed", entity.GetName());
				return std::nullopt;
			}
			if (RigidBodyComponent const* body = entity.TryGetComponent<RigidBodyComponent>())
			{
				desc.Type = body->Type;
				desc.Mass = body->Mass;
				desc.LinearDamping = body->LinearDamping;
				desc.AngularDamping = body->AngularDamping;
				desc.GravityFactor = body->GravityFactor;
				desc.Layer = body->Layer;
				desc.LockTranslation = body->LockTranslation;
				desc.LockRotation = body->LockRotation;
				desc.ContinuousCollision = body->ContinuousCollision;
				desc.AllowSleep = body->AllowSleep;
				desc.LinearVelocity = body->InitialLinearVelocity;
				desc.AngularVelocity = body->InitialAngularVelocity;
			}
			return desc;
		}

		size_t GetDepth(Scene& scene, Entity entity)
		{
			size_t depth = 0;
			for (Entity parent = scene.GetParent(entity); parent; parent = scene.GetParent(parent))
			{
				depth++;
			}
			return depth;
		}
	}

	void Scene::StartPhysics()
	{
		PhysicsWorldSettings world;
		world.Gravity = m_Settings.Physics.Gravity;
		world.LayerCount = m_RuntimeSettings.PhysicsLayerCount;
		world.IgnoredCollisions = m_RuntimeSettings.IgnoredCollisions;
		Result<Scope<PhysicsScene>> physics = PhysicsScene::Create(world);
		if (!physics)
		{
			ST_CORE_ERROR("The scene runs without physics: {}", physics.GetError());
			return;
		}
		m_Physics = physics.TakeValue();
		m_PhysicsAccumulator = 0.0f;

		auto const addBodies = [this](auto view)
		{
			for (entt::entity const handle : view)
			{
				m_ChangedBodies.insert(handle);
			}
		};
		addBodies(m_Registry.view<BoxColliderComponent>());
		addBodies(m_Registry.view<SphereColliderComponent>());
		addBodies(m_Registry.view<CapsuleColliderComponent>());
		addBodies(m_Registry.view<MeshColliderComponent>());
		addBodies(m_Registry.view<RigidBodyComponent>());
		for (entt::entity const handle : m_Registry.view<CharacterControllerComponent>())
		{
			m_ChangedCharacters.insert(handle);
		}
		m_JointsChanged = true;
		RebuildChangedBodies();
		ConnectPhysicsSignals(true);
	}

	void Scene::StopPhysics()
	{
		if (!m_Physics)
		{
			return;
		}
		ConnectPhysicsSignals(false);
		m_Physics.reset();
		m_Bodies.clear();
		m_ChangedBodies.clear();
		m_Characters.clear();
		m_ChangedCharacters.clear();
		m_JointsChanged = false;
		m_ContactEvents.clear();
		m_PhysicsAccumulator = 0.0f;
	}

	void Scene::ConnectPhysicsSignals(bool connect)
	{
		auto const update = [this, connect](auto& sink)
		{
			if (connect)
			{
				sink.template connect<&Scene::OnPhysicsComponentChanged>(*this);
			}
			else
			{
				sink.template disconnect<&Scene::OnPhysicsComponentChanged>(*this);
			}
		};
		auto const watch = [this, &update]<typename T>(std::type_identity<T>)
		{
			auto constructed = m_Registry.on_construct<T>();
			auto updated = m_Registry.on_update<T>();
			auto destroyed = m_Registry.on_destroy<T>();
			update(constructed);
			update(updated);
			update(destroyed);
		};
		watch(std::type_identity<RigidBodyComponent>());
		watch(std::type_identity<BoxColliderComponent>());
		watch(std::type_identity<SphereColliderComponent>());
		watch(std::type_identity<CapsuleColliderComponent>());
		watch(std::type_identity<MeshColliderComponent>());

		auto const watchJoints = [this, connect](auto sink)
		{
			if (connect)
			{
				sink.template connect<&Scene::OnJointComponentChanged>(*this);
			}
			else
			{
				sink.template disconnect<&Scene::OnJointComponentChanged>(*this);
			}
		};
		watchJoints(m_Registry.on_construct<JointComponent>());
		watchJoints(m_Registry.on_update<JointComponent>());
		watchJoints(m_Registry.on_destroy<JointComponent>());

		auto const watchCharacters = [this, connect](auto sink)
		{
			if (connect)
			{
				sink.template connect<&Scene::OnCharacterComponentChanged>(*this);
			}
			else
			{
				sink.template disconnect<&Scene::OnCharacterComponentChanged>(*this);
			}
		};
		watchCharacters(m_Registry.on_construct<CharacterControllerComponent>());
		watchCharacters(m_Registry.on_update<CharacterControllerComponent>());
		watchCharacters(m_Registry.on_destroy<CharacterControllerComponent>());

		auto meshUpdated = m_Registry.on_update<MeshComponent>();
		if (connect)
		{
			meshUpdated.connect<&Scene::OnMeshComponentChanged>(*this);
		}
		else
		{
			meshUpdated.disconnect<&Scene::OnMeshComponentChanged>(*this);
		}
	}

	void Scene::OnPhysicsComponentChanged(entt::registry&, entt::entity handle)
	{
		m_ChangedBodies.insert(handle);
	}

	void Scene::OnJointComponentChanged(entt::registry&, entt::entity)
	{
		m_JointsChanged = true;
	}

	void Scene::OnCharacterComponentChanged(entt::registry&, entt::entity handle)
	{
		// A character replaces the entity's body: the body is made again (or removed) too.
		m_ChangedCharacters.insert(handle);
		m_ChangedBodies.insert(handle);
	}

	void Scene::OnMeshComponentChanged(entt::registry& registry, entt::entity handle)
	{
		// Mesh colliders without their own mesh use the entity's mesh.
		if (registry.all_of<MeshColliderComponent>(handle))
		{
			m_ChangedBodies.insert(handle);
		}
	}

	void Scene::ApplyPhysicsChanges()
	{
		if (m_Physics)
		{
			RebuildChangedBodies();
		}
	}

	void Scene::RebuildChangedBodies()
	{
		if (!m_ChangedBodies.empty())
		{
			// Rebuilt bodies lose their joints.
			m_JointsChanged = true;
			std::unordered_set<entt::entity> const changed = std::exchange(m_ChangedBodies, {});
			for (entt::entity const handle : changed)
			{
				UpdateBody(handle);
			}
		}
		if (!m_ChangedCharacters.empty())
		{
			std::unordered_set<entt::entity> const changed = std::exchange(m_ChangedCharacters, {});
			for (entt::entity const handle : changed)
			{
				UpdateCharacter(handle);
			}
		}
		if (std::exchange(m_JointsChanged, false))
		{
			RebuildJoints();
		}
	}

	void Scene::RebuildJoints()
	{
		for (auto const& [handle, state] : m_Bodies)
		{
			m_Physics->RemoveJoint(state.Entity);
		}
		for (entt::entity const handle : m_Registry.view<JointComponent>())
		{
			Entity const entity(handle, this);
			JointComponent const& component = entity.GetComponent<JointComponent>();
			glm::mat4 const world = GetWorldTransform(entity);
			JointDesc desc;
			desc.Entity = entity.GetUUID();
			desc.Type = component.Type;
			desc.Point = glm::vec3(world * glm::vec4(component.Anchor, 1.0f));
			desc.ConnectedPoint = component.ConnectedAnchor;
			if (component.ConnectedEntity.IsValid())
			{
				Entity const connected = GetEntityByUUID(component.ConnectedEntity);
				// A connected entity that was destroyed (a broken chain) leaves the joint out.
				if (!connected)
				{
					continue;
				}
				desc.ConnectedEntity = component.ConnectedEntity;
				desc.ConnectedPoint = glm::vec3(GetWorldTransform(connected) * glm::vec4(component.ConnectedAnchor, 1.0f));
			}
			desc.Axis = glm::vec3(world * glm::vec4(component.Axis, 0.0f));
			desc.LimitsEnabled = component.LimitsEnabled;
			bool const hinge = component.Type == JointType::Hinge;
			desc.MinLimit = hinge ? glm::radians(component.MinLimit) : component.MinLimit;
			desc.MaxLimit = hinge ? glm::radians(component.MaxLimit) : component.MaxLimit;
			desc.MinDistance = component.MinDistance;
			desc.MaxDistance = component.MaxDistance;
			desc.MotorEnabled = component.MotorEnabled;
			desc.MotorSpeed = hinge ? glm::radians(component.MotorSpeed) : component.MotorSpeed;
			desc.MotorMaxForce = component.MotorMaxForce;
			if (Result<void> added = m_Physics->AddJoint(desc); !added)
			{
				ST_CORE_WARN("Physics: {}", added.GetError());
			}
		}
	}

	void Scene::UpdateBody(entt::entity handle)
	{
		auto const existing = m_Bodies.find(handle);
		if (!m_Registry.valid(handle) || !m_Registry.all_of<IDComponent>(handle))
		{
			// Destroyed entity.
			if (existing != m_Bodies.end())
			{
				m_Physics->RemoveBody(existing->second.Entity);
				m_Bodies.erase(existing);
			}
			return;
		}

		Entity const entity(handle, this);
		UUID const id = entity.GetUUID();
		std::optional<BodyDesc> desc = BuildBodyDesc(*this, entity);
		if (!desc)
		{
			m_Physics->RemoveBody(id);
			if (existing != m_Bodies.end())
			{
				m_Bodies.erase(existing);
			}
			return;
		}
		// A rebuilt body keeps moving as before.
		if (m_Physics->HasBody(id))
		{
			desc->LinearVelocity = m_Physics->GetLinearVelocity(id);
			desc->AngularVelocity = m_Physics->GetAngularVelocity(id);
		}
		if (Result<void> added = m_Physics->AddBody(*desc); !added)
		{
			ST_CORE_WARN("Physics: {}", added.GetError());
			if (existing != m_Bodies.end())
			{
				m_Bodies.erase(existing);
			}
			return;
		}
		m_Bodies[handle] = BodyState{id, desc->Position, desc->Rotation};
	}

	void Scene::UpdateCharacter(entt::entity handle)
	{
		auto const existing = m_Characters.find(handle);
		bool const alive = m_Registry.valid(handle) && m_Registry.all_of<IDComponent>(handle);
		CharacterControllerComponent const* component = alive ? m_Registry.try_get<CharacterControllerComponent>(handle) : nullptr;
		auto const forget = [this, &existing]
		{
			if (existing != m_Characters.end())
			{
				m_Physics->RemoveCharacter(existing->second.Entity);
				m_Characters.erase(existing);
			}
		};
		if (component == nullptr)
		{
			// Destroyed entity, or removed component.
			forget();
			return;
		}

		Entity const entity(handle, this);
		glm::vec3 position;
		glm::quat rotation;
		glm::vec3 scale;
		if (!Math::DecomposeTransform(GetWorldTransform(entity), position, rotation, scale))
		{
			ST_CORE_WARN("Physics: the transform of '{}' cannot be decomposed", entity.GetName());
			forget();
			return;
		}
		UUID const id = entity.GetUUID();
		if (Result<void> added = m_Physics->AddCharacter(MakeCharacterDesc(id, *component, position, scale)); !added)
		{
			ST_CORE_WARN("Physics: {}", added.GetError());
			if (existing != m_Characters.end())
			{
				m_Characters.erase(existing);
			}
			return;
		}
		m_Characters[handle] = CharacterSyncState{id, position, scale};
	}

	void Scene::TeleportCharacter(Entity entity, glm::vec3 const& position)
	{
		if (!entity || entity.GetScene() != this || !entity.HasComponent<CharacterControllerComponent>())
		{
			return;
		}
		glm::vec3 current;
		glm::quat rotation;
		glm::vec3 scale;
		if (!Math::DecomposeTransform(GetWorldTransform(entity), current, rotation, scale))
		{
			return;
		}
		SetWorldTransform(entity, Math::ComposeTransform(position, rotation, scale));
		if (!m_Physics)
		{
			return;
		}
		// A character added this frame exists before it is placed.
		ApplyPhysicsChanges();
		auto const state = m_Characters.find(entity.GetHandle());
		if (state == m_Characters.end())
		{
			return;
		}
		CharacterControllerComponent const& component = entity.GetComponent<CharacterControllerComponent>();
		m_Physics->SetCharacterPosition(state->second.Entity, position + component.Offset * state->second.Scale);
		m_Physics->SetCharacterVelocity(state->second.Entity, glm::vec3(0.0f));
		state->second.Position = position;
	}

	void Scene::SyncCharactersFromTransforms()
	{
		std::vector<entt::entity> rescaled;
		for (auto& [handle, state] : m_Characters)
		{
			CharacterControllerComponent const* component =
				m_Registry.valid(handle) ? m_Registry.try_get<CharacterControllerComponent>(handle) : nullptr;
			glm::vec3 position;
			glm::quat rotation;
			glm::vec3 scale;
			if (component == nullptr || !Math::DecomposeTransform(GetWorldTransform(Entity(handle, this)), position, rotation, scale))
			{
				continue;
			}
			if (glm::any(glm::greaterThan(glm::abs(scale - state.Scale), glm::vec3(ScaleTolerance))))
			{
				// A new size: the capsule changes (and the character moves to where the entity is).
				rescaled.push_back(handle);
			}
			else if (HasMoved(position, state.Position))
			{
				// Moved by a script, the editor or its parent; turning the entity does not turn the capsule.
				m_Physics->SetCharacterPosition(state.Entity, position + component->Offset * state.Scale);
				state.Position = position;
			}
		}
		for (entt::entity const handle : rescaled)
		{
			UpdateCharacter(handle);
		}
	}

	void Scene::WriteCharacterTransforms()
	{
		struct Update
		{
			entt::entity Handle = entt::null;
			UUID Entity = UUID::Invalid();
			glm::vec3 Position = glm::vec3(0.0f);
			size_t Depth = 0;
		};
		std::vector<Update> updates;
		for (auto const& [handle, state] : m_Characters)
		{
			CharacterControllerComponent const* component =
				m_Registry.valid(handle) ? m_Registry.try_get<CharacterControllerComponent>(handle) : nullptr;
			std::optional<CharacterState> const character =
				component != nullptr ? m_Physics->GetCharacterState(state.Entity) : std::nullopt;
			if (!character)
			{
				continue;
			}
			glm::vec3 const position = character->Position - component->Offset * state.Scale;
			// A character standing still leaves its transform alone.
			if (HasMoved(position, state.Position))
			{
				updates.push_back({handle, state.Entity, position, GetDepth(*this, Entity(handle, this))});
			}
		}
		// Parents first (a child's local transform depends on its parent's new world transform), in a fixed order.
		std::sort(updates.begin(), updates.end(),
		          [](Update const& a, Update const& b)
		          {
					  return std::tuple(a.Depth, a.Entity.GetValue()) < std::tuple(b.Depth, b.Entity.GetValue());
				  });
		for (Update const& update : updates)
		{
			Entity const entity(update.Handle, this);
			glm::vec3 position;
			glm::quat rotation;
			glm::vec3 scale;
			if (!Math::DecomposeTransform(GetWorldTransform(entity), position, rotation, scale))
			{
				continue;
			}
			SetWorldTransform(entity, Math::ComposeTransform(update.Position, rotation, scale));
			m_Characters[update.Handle].Position = update.Position;
		}
	}

	void Scene::UpdatePhysics(float deltaTime)
	{
		m_ContactEvents.clear();
		if (m_Physics)
		{
			RebuildChangedBodies();
		}

		// The fixed steps run without physics too: scripts' OnFixedUpdate keeps its rate.
		float const step = GetFixedTimestep();
		uint32_t const maxSteps = std::max(m_RuntimeSettings.MaxStepsPerFrame, 1u);
		m_PhysicsAccumulator += std::max(deltaTime, 0.0f);
		uint32_t steps = 0;
		// The tolerance keeps frames of exactly one step from alternating between zero and two steps.
		while (m_PhysicsAccumulator >= step * (1.0f - 1e-4f) && steps < maxSteps)
		{
			// Scripts act before each step (forces, kinematic targets, teleports, character moves).
			FixedUpdateScripts(step);
			if (m_Physics)
			{
				SyncBodiesFromTransforms(step);
				SyncCharactersFromTransforms();
				m_Physics->Step(step);
				WriteBodyTransforms();
				WriteCharacterTransforms();
			}
			m_PhysicsAccumulator = std::max(m_PhysicsAccumulator - step, 0.0f);
			steps++;
		}
		if (steps == maxSteps)
		{
			// Drop the backlog rather than spiraling.
			m_PhysicsAccumulator = std::min(m_PhysicsAccumulator, step);
		}
		if (m_Physics)
		{
			m_ContactEvents = m_Physics->TakeContactEvents();
		}
	}

	void Scene::SyncBodiesFromTransforms(float stepDelta)
	{
		for (auto& [handle, state] : m_Bodies)
		{
			if (!m_Registry.valid(handle))
			{
				continue;
			}
			glm::vec3 position;
			glm::quat rotation;
			glm::vec3 scale;
			if (!Math::DecomposeTransform(GetWorldTransform(Entity(handle, this)), position, rotation, scale))
			{
				continue;
			}
			RigidBodyComponent const* body = m_Registry.try_get<RigidBodyComponent>(handle);
			if (body != nullptr && body->Type == RigidBodyType::Kinematic)
			{
				m_Physics->MoveKinematic(state.Entity, position, rotation, stepDelta);
			}
			else if (HasMoved(position, rotation, state.Position, state.Rotation))
			{
				// Moved by a script, the editor or its parent.
				m_Physics->SetBodyTransform(state.Entity, position, rotation);
			}
			state.Position = position;
			state.Rotation = rotation;
		}
	}

	void Scene::WriteBodyTransforms()
	{
		struct Update
		{
			BodyTransform Transform;
			entt::entity Handle = entt::null;
			size_t Depth = 0;
		};
		std::vector<Update> updates;
		for (BodyTransform const& transform : m_Physics->GetActiveBodyTransforms())
		{
			entt::entity const handle = FindHandle(transform.Entity);
			if (handle != entt::null)
			{
				updates.push_back({transform, handle, GetDepth(*this, Entity(handle, this))});
			}
		}
		// Parents first: a child's local transform depends on its parent's new world transform.
		std::stable_sort(updates.begin(), updates.end(),
		                 [](Update const& a, Update const& b)
		                 {
							 return a.Depth < b.Depth;
						 });
		for (Update const& update : updates)
		{
			Entity const entity(update.Handle, this);
			glm::vec3 position;
			glm::quat rotation;
			glm::vec3 scale;
			if (!Math::DecomposeTransform(GetWorldTransform(entity), position, rotation, scale))
			{
				continue;
			}
			SetWorldTransform(entity, Math::ComposeTransform(update.Transform.Position, update.Transform.Rotation, scale));
			if (auto const state = m_Bodies.find(update.Handle); state != m_Bodies.end())
			{
				state->second.Position = update.Transform.Position;
				state->second.Rotation = update.Transform.Rotation;
			}
		}
	}
}
