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
#include <type_traits>
#include <utility>

namespace Strada
{
	namespace
	{
		// Transform edits smaller than this are not worth teleporting a body for (decomposition round trips are noisy).
		constexpr float PositionTolerance = 1e-4f;
		constexpr float RotationTolerance = 1e-6f;

		bool HasMoved(glm::vec3 const& positionA, glm::quat const& rotationA, glm::vec3 const& positionB, glm::quat const& rotationB)
		{
			glm::vec3 const delta = positionA - positionB;
			// q and -q are the same rotation.
			return glm::dot(delta, delta) > PositionTolerance * PositionTolerance ||
			       1.0f - std::abs(glm::dot(rotationA, rotationB)) > RotationTolerance;
		}

		void AddCollider(BodyDesc& desc, ColliderDesc collider, glm::vec3 const& offset, bool trigger, float friction, float restitution)
		{
			collider.Offset = offset;
			collider.IsTrigger = trigger;
			collider.Friction = friction;
			collider.Restitution = restitution;
			desc.Colliders.push_back(std::move(collider));
		}

		// The body of an entity from its components; empty without colliders.
		std::optional<BodyDesc> BuildBodyDesc(Scene& scene, Entity entity)
		{
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
		if (m_ChangedBodies.empty())
		{
			return;
		}
		std::unordered_set<entt::entity> const changed = std::exchange(m_ChangedBodies, {});
		for (entt::entity const handle : changed)
		{
			UpdateBody(handle);
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

	void Scene::UpdatePhysics(float deltaTime)
	{
		m_ContactEvents.clear();
		if (m_Physics)
		{
			RebuildChangedBodies();
		}

		// The fixed steps run without physics too: scripts' OnFixedUpdate keeps its rate.
		float const step = std::max(m_RuntimeSettings.FixedTimestep, 1e-4f);
		uint32_t const maxSteps = std::max(m_RuntimeSettings.MaxStepsPerFrame, 1u);
		m_PhysicsAccumulator += std::max(deltaTime, 0.0f);
		uint32_t steps = 0;
		// The tolerance keeps frames of exactly one step from alternating between zero and two steps.
		while (m_PhysicsAccumulator >= step * (1.0f - 1e-4f) && steps < maxSteps)
		{
			// Scripts act before each step (forces, kinematic targets, teleports).
			FixedUpdateScripts(step);
			if (m_Physics)
			{
				SyncBodiesFromTransforms(step);
				m_Physics->Step(step);
				WriteBodyTransforms();
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
