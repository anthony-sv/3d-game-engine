#include "stpch.h"
#include "Strada/Scene/Scene.h"

#include "Strada/Asset/AssetManager.h"
#include "Strada/Asset/CurveAsset.h"
#include "Strada/Asset/DataAsset.h"
#include "Strada/Asset/MeshSource.h"
#include "Strada/Asset/PhysicsMaterial.h"
#include "Strada/Asset/PhysicsMaterialAsset.h"
#include "Strada/Asset/Vehicle.h"
#include "Strada/Asset/VehicleAsset.h"
#include "Strada/Core/Hash.h"
#include "Strada/Core/Profiler.h"
#include "Strada/Math/Curve.h"
#include "Strada/Math/Math.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Scene/Components.h"
#include "Strada/Scene/Entity.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
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

		bool IsFinite(glm::vec3 const& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// Moves an entity's origin to a world position and leaves its rotation and scale exactly as they are: taking the
		// transform apart and putting it together again would change them in the last bits, every step.
		void SetWorldPosition(Scene& scene, Entity entity, glm::vec3 const& position)
		{
			glm::vec3 local = position;
			if (Entity const parent = scene.GetParent(entity))
			{
				local = glm::vec3(glm::inverse(scene.GetWorldTransform(parent)) * glm::vec4(position, 1.0f));
			}
			// Parents without a usable transform (zero scale) have no space to place the entity in.
			if (IsFinite(local))
			{
				entity.GetComponent<TransformComponent>().Translation = local;
			}
		}

		// Places an entity at a world position and rotation and leaves its scale exactly as it is (the simulation does not
		// change it).
		void SetWorldPose(Scene& scene, Entity entity, glm::vec3 const& position, glm::quat const& rotation)
		{
			TransformComponent& transform = entity.GetComponent<TransformComponent>();
			Entity const parent = scene.GetParent(entity);
			if (!parent)
			{
				transform.Translation = position;
				transform.Rotation = Math::NormalizeRotation(rotation);
				return;
			}
			glm::vec3 worldPosition;
			glm::quat worldRotation;
			glm::vec3 worldScale;
			if (!Math::DecomposeTransform(scene.GetWorldTransform(entity), worldPosition, worldRotation, worldScale))
			{
				return;
			}
			glm::mat4 const local = glm::inverse(scene.GetWorldTransform(parent)) * Math::ComposeTransform(position, rotation, worldScale);
			glm::vec3 localPosition;
			glm::quat localRotation;
			glm::vec3 localScale;
			if (Math::DecomposeTransform(local, localPosition, localRotation, localScale))
			{
				transform.Translation = localPosition;
				transform.Rotation = localRotation;
			}
		}

		// Places an entity at a world position and rotation computed in its parent's space from the parent's world transform
		// alone, keeping its own scale: the same pose and parent always give the same bits, whatever the entity showed before
		// (a wheel drawn between steps goes back exactly to where its step put it).
		void SetWorldPoseInParent(Scene& scene, Entity entity, glm::vec3 const& position, glm::quat const& rotation)
		{
			TransformComponent& transform = entity.GetComponent<TransformComponent>();
			Entity const parent = scene.GetParent(entity);
			if (!parent)
			{
				transform.Translation = position;
				transform.Rotation = Math::NormalizeRotation(rotation);
				return;
			}
			glm::mat4 const local =
				glm::inverse(scene.GetWorldTransform(parent)) * Math::ComposeTransform(position, rotation, glm::vec3(1.0f));
			glm::vec3 localPosition;
			glm::quat localRotation;
			glm::vec3 localScale;
			// Parents without a usable transform (zero scale) have no space to place the entity in.
			if (Math::DecomposeTransform(local, localPosition, localRotation, localScale) && IsFinite(localPosition))
			{
				transform.Translation = localPosition;
				transform.Rotation = localRotation;
			}
		}

		// The IDs of an entity's descendants, sorted.
		std::vector<UUID> GetDescendantIDs(Scene& scene, Entity entity)
		{
			std::vector<UUID> descendants;
			std::vector<UUID> pending = entity.GetComponent<RelationshipComponent>().Children;
			// Bounded by the entity count, so a corrupted hierarchy cannot loop forever.
			while (!pending.empty() && descendants.size() < scene.GetEntityCount())
			{
				UUID const id = pending.back();
				pending.pop_back();
				Entity const child = scene.GetEntityByUUID(id);
				if (!child)
				{
					continue;
				}
				descendants.push_back(id);
				std::vector<UUID> const& children = child.GetComponent<RelationshipComponent>().Children;
				pending.insert(pending.end(), children.begin(), children.end());
			}
			std::sort(descendants.begin(), descendants.end());
			return descendants;
		}

		// A character ignores the bodies of its own parts, the entities below it (a held object, a camera rig with
		// colliders, a trigger around it): they neither block it nor touch it. parts holds what the physics world was told.
		void SyncCharacterParts(Scene& scene, PhysicsScene& physics, Entity entity, UUID character, std::vector<UUID>& parts)
		{
			std::vector<UUID> current = GetDescendantIDs(scene, entity);
			if (current != parts)
			{
				physics.SetCharacterIgnoredEntities(character, current);
				parts = std::move(current);
			}
		}

		// A world position for the entity of a character.
		struct CharacterPlacement
		{
			entt::entity Handle = entt::null;
			UUID Entity = UUID::Invalid();
			glm::vec3 Position = glm::vec3(0.0f);
			size_t Depth = 0;
		};

		// Moves the entities of characters: parents first (a child's local transform depends on its parent's new world
		// transform), in a fixed order. The capsule does not turn, so the entities keep their rotations and scales.
		void PlaceCharacterEntities(Scene& scene, std::vector<CharacterPlacement>& placements)
		{
			std::sort(placements.begin(), placements.end(),
			          [](CharacterPlacement const& a, CharacterPlacement const& b)
			          {
						  return std::tuple(a.Depth, a.Entity.GetValue()) < std::tuple(b.Depth, b.Entity.GetValue());
					  });
			for (CharacterPlacement const& placement : placements)
			{
				SetWorldPosition(scene, Entity(placement.Handle, &scene), placement.Position);
			}
		}

		// The part of a character's last step (moved) that what it stands on carried it (ground: how far the ground moved
		// where it stands), as far as the character really went that way: a wall may have stopped it, or it walked against
		// the ground's motion.
		glm::vec3 GetCarriedPart(glm::vec3 const& moved, glm::vec3 const& ground)
		{
			float const distance = glm::length(ground);
			if (distance < 1e-6f)
			{
				return glm::vec3(0.0f);
			}
			glm::vec3 const direction = ground / distance;
			return direction * std::clamp(glm::dot(moved, direction), 0.0f, distance);
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

		// The data asset of type TData a reference names, when it is registered as one and loads; null otherwise (a reference
		// to anything else counts as unset: no handle, an unknown one, an asset of another type, a file that does not load).
		template<typename TData>
		Ref<TypedDataAsset<TData>> FindDataAsset(AssetHandle handle)
		{
			AssetType const type = TypedDataAsset<TData>::GetStaticType();
			if (!handle.IsValid() || !AssetManager::IsInitialized() || type == AssetType::None ||
			    AssetManager::GetAssetType(handle) != type)
			{
				return nullptr;
			}
			return AssetManager::GetAsset<TypedDataAsset<TData>>(handle);
		}

		// The physics material a collider names (FindDataAsset).
		Ref<TypedDataAsset<PhysicsMaterial>> FindPhysicsMaterial(AssetHandle handle)
		{
			return FindDataAsset<PhysicsMaterial>(handle);
		}

		// An asset reference as messages name it.
		std::string DescribeReference(AssetHandle handle)
		{
			return AssetManager::IsInitialized() ? AssetManager::GetReference(handle) : handle.ToString();
		}

		// What a vehicle is made from: its definition and the curve assets the definition names, in the order of
		// Scene::VehicleSyncState::Curves (each wheel's longitudinal and lateral friction, then the engine's torque), each
		// null when it does not apply. No curve without a definition.
		struct VehicleSources
		{
			Ref<TypedDataAsset<Vehicle>> Definition;
			std::vector<AssetHandle> CurveHandles;
			std::vector<Ref<TypedDataAsset<Curve>>> Curves;
		};

		VehicleSources ReadVehicleSources(AssetHandle definition)
		{
			VehicleSources sources;
			sources.Definition = FindDataAsset<Vehicle>(definition);
			if (sources.Definition)
			{
				Vehicle const& values = sources.Definition->GetData();
				for (VehicleWheelDefinition const& wheel : values.Wheels)
				{
					sources.CurveHandles.push_back(wheel.LongitudinalFriction);
					sources.CurveHandles.push_back(wheel.LateralFriction);
				}
				sources.CurveHandles.push_back(values.Engine.Torque);
				for (AssetHandle const curve : sources.CurveHandles)
				{
					sources.Curves.push_back(FindDataAsset<Curve>(curve));
				}
			}
			return sources;
		}

		// The values of a vehicle made from its definition and its curves (VehicleSources, with a definition), each curve
		// sampled (SampleVehicleCurve; none, or a reference that is no Curve that loads, is Jolt's default). `notes` receives
		// the references that count as unset.
		VehicleDesc MakeVehicleValues(UUID entity, VehicleSources const& sources, std::vector<std::string>& notes)
		{
			Vehicle const& definition = sources.Definition->GetData();
			VehicleDesc desc = MakeVehicleDesc(entity, definition);
			auto const sample = [&sources, &notes](size_t index, std::string const& field, float inputScale)
			{
				if (sources.Curves[index])
				{
					return SampleVehicleCurve(sources.Curves[index]->GetData(), inputScale);
				}
				if (sources.CurveHandles[index].IsValid())
				{
					notes.push_back(fmt::format("takes the default for its {}: {} is no Curve that loads", field,
					                            DescribeReference(sources.CurveHandles[index])));
				}
				return VehicleCurve();
			};
			// A definition that validates has exactly its two wheels.
			for (size_t wheel = 0; wheel < std::min(definition.Wheels.size(), VehicleWheelCount); wheel++)
			{
				std::string const prefix = "Wheels[" + std::to_string(wheel) + "].";
				desc.Wheels[wheel].LongitudinalFriction = sample(2 * wheel, prefix + "LongitudinalFriction", 1.0f);
				desc.Wheels[wheel].LateralFriction = sample(2 * wheel + 1, prefix + "LateralFriction", 1.0f);
			}
			// Jolt reads the engine's curve over the engine's speed divided by its largest (VehicleEngine.h:63 of the pinned
			// Jolt); MaxRPM is at least 1.
			desc.NormalizedTorque = sample(sources.Curves.size() - 1, "Engine.Torque", 1.0f / definition.Engine.MaxRPM);
			return desc;
		}

		// A collider's surface: its material's values and modes when it names one that applies, else its own Friction and
		// Restitution with the default modes (the rule colliders had before materials). Adds a named material to `named`; a
		// registered physics material whose file does not load is said.
		ColliderSurface MakeColliderSurface(Entity entity, AssetHandle material, float friction, float restitution,
		                                    std::vector<AssetHandle>& named)
		{
			ColliderSurface surface;
			surface.Friction = friction;
			surface.Restitution = restitution;
			if (!material.IsValid())
			{
				return surface;
			}
			named.push_back(material);
			if (Ref<TypedDataAsset<PhysicsMaterial>> const asset = FindPhysicsMaterial(material))
			{
				PhysicsMaterial const& values = asset->GetData();
				surface.Material = material;
				surface.Friction = values.Friction;
				surface.Restitution = values.Restitution;
				surface.FrictionCombine = values.FrictionCombine;
				surface.RestitutionCombine = values.RestitutionCombine;
				surface.RollingResistance = values.RollingResistance;
				surface.TyreGrip = values.TyreGrip;
			}
			else if (AssetManager::IsInitialized() &&
			         AssetManager::GetAssetType(material) == TypedDataAsset<PhysicsMaterial>::GetStaticType())
			{
				ST_CORE_WARN("Physics: the physics material {} of '{}' does not load; its collider's own Friction and Restitution "
				             "apply",
				             AssetManager::GetReference(material), entity.GetName());
			}
			return surface;
		}

		void AddCollider(BodyDesc& desc, ColliderDesc collider, glm::vec3 const& offset, bool trigger, ColliderSurface const& surface)
		{
			collider.Offset = offset;
			collider.IsTrigger = trigger;
			collider.Surface = surface;
			desc.Colliders.push_back(std::move(collider));
		}

		// The surfaces of a triangle mesh's submeshes from MeshColliderComponent::Materials: a material that applies gives its
		// submesh its values, any other entry the collider's surface; none at all when no entry applies (one surface).
		// Entries past the mesh's submeshes or past MaxMeshColliderMaterials are left out.
		std::vector<ColliderSurface> MakeSubmeshSurfaces(Entity entity, MeshColliderComponent const& component, MeshSource const& mesh,
		                                                 ColliderSurface const& colliderSurface, std::vector<AssetHandle>& named)
		{
			size_t const count = std::min({component.Materials.size(), mesh.GetSubmeshes().size(), size_t(MaxMeshColliderMaterials)});
			std::vector<ColliderSurface> surfaces;
			size_t applied = 0;
			for (size_t i = 0; i < count; i++)
			{
				ColliderSurface surface =
					MakeColliderSurface(entity, component.Materials[i], component.Friction, component.Restitution, named);
				if (surface.Material.IsValid())
				{
					applied = i + 1;
				}
				else
				{
					surface = colliderSurface;
				}
				surfaces.push_back(surface);
			}
			// Trailing entries that do not apply are the collider's surface anyway.
			surfaces.resize(applied);
			return surfaces;
		}

		// Whether the entity has components that make a body (a CharacterController replaces them).
		bool HasBodyComponents(entt::registry const& registry, entt::entity handle)
		{
			return registry
			    .any_of<RigidBodyComponent, BoxColliderComponent, SphereColliderComponent, CapsuleColliderComponent, MeshColliderComponent>(
					handle);
		}

		// The body of an entity from its components; empty without colliders, and for characters (which replace their
		// bodies; UpdateCharacter says so). `materials` receives the physics materials its colliders name, sorted and without
		// repeats, whether they apply or not.
		std::optional<BodyDesc> BuildBodyDesc(Scene& scene, Entity entity, std::vector<AssetHandle>& materials)
		{
			materials.clear();
			if (entity.HasComponent<CharacterControllerComponent>())
			{
				return std::nullopt;
			}
			BodyDesc desc;
			desc.Entity = entity.GetUUID();
			if (BoxColliderComponent const* box = entity.TryGetComponent<BoxColliderComponent>())
			{
				ColliderDesc collider;
				collider.Shape = ColliderShape::Box;
				collider.HalfExtents = box->HalfExtents;
				AddCollider(desc, std::move(collider), box->Offset, box->IsTrigger,
				            MakeColliderSurface(entity, box->Material, box->Friction, box->Restitution, materials));
			}
			if (SphereColliderComponent const* sphere = entity.TryGetComponent<SphereColliderComponent>())
			{
				ColliderDesc collider;
				collider.Shape = ColliderShape::Sphere;
				collider.Radius = sphere->Radius;
				AddCollider(desc, std::move(collider), sphere->Offset, sphere->IsTrigger,
				            MakeColliderSurface(entity, sphere->Material, sphere->Friction, sphere->Restitution, materials));
			}
			if (CapsuleColliderComponent const* capsule = entity.TryGetComponent<CapsuleColliderComponent>())
			{
				ColliderDesc collider;
				collider.Shape = ColliderShape::Capsule;
				collider.Radius = capsule->Radius;
				collider.HalfHeight = capsule->HalfHeight;
				AddCollider(desc, std::move(collider), capsule->Offset, capsule->IsTrigger,
				            MakeColliderSurface(entity, capsule->Material, capsule->Friction, capsule->Restitution, materials));
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
					ColliderSurface const surface =
						MakeColliderSurface(entity, meshCollider->Material, meshCollider->Friction, meshCollider->Restitution, materials);
					if (!meshCollider->Convex)
					{
						collider.SubmeshSurfaces = MakeSubmeshSurfaces(entity, *meshCollider, *mesh, surface, materials);
					}
					AddCollider(desc, std::move(collider), glm::vec3(0.0f), meshCollider->IsTrigger, surface);
				}
				else
				{
					ST_CORE_WARN("Physics: the mesh collider of '{}' has no mesh", entity.GetName());
				}
			}
			std::sort(materials.begin(), materials.end(),
			          [](AssetHandle a, AssetHandle b)
			          {
						  return a.GetUUID().GetValue() < b.GetUUID().GetValue();
					  });
			materials.erase(std::unique(materials.begin(), materials.end()), materials.end());
			if (desc.Colliders.empty())
			{
				if (entity.HasComponent<RigidBodyComponent>())
				{
					ST_CORE_WARN("Physics: '{}' has a RigidBody but no colliders, so it is not simulated", entity.GetName());
				}
				materials.clear();
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

		// The handles in the order of their entities' UUIDs: a live entity's own, a destroyed one's as its physics state
		// remembers it (states maps handles to records with an Entity UUID). Physics is called in this order, which neither
		// the frame rate nor the order the entities were made in changes.
		template<typename TStates>
		std::vector<entt::entity> SortByUUID(entt::registry const& registry, std::unordered_set<entt::entity> const& handles,
		                                     TStates const& states)
		{
			std::vector<std::pair<uint64_t, entt::entity>> keyed;
			keyed.reserve(handles.size());
			for (entt::entity const handle : handles)
			{
				UUID id = UUID::Invalid();
				if (registry.valid(handle) && registry.all_of<IDComponent>(handle))
				{
					id = registry.get<IDComponent>(handle).ID;
				}
				else if (auto const state = states.find(handle); state != states.end())
				{
					id = state->second.Entity;
				}
				keyed.emplace_back(id.GetValue(), handle);
			}
			// Handles that are neither (destroyed before they had a body) touch no physics; their handles keep the order total.
			std::sort(keyed.begin(), keyed.end(),
			          [](auto const& a, auto const& b)
			          {
						  return std::tuple(a.first, entt::to_integral(a.second)) < std::tuple(b.first, entt::to_integral(b.second));
					  });
			std::vector<entt::entity> sorted;
			sorted.reserve(keyed.size());
			for (auto const& [id, handle] : keyed)
			{
				sorted.push_back(handle);
			}
			return sorted;
		}

		// The keys of states (handles mapped to records with an Entity UUID) in the order of those UUIDs.
		template<typename TStates>
		void SortKeysByUUID(TStates const& states, std::vector<entt::entity>& order)
		{
			order.clear();
			order.reserve(states.size());
			for (auto const& [handle, state] : states)
			{
				order.push_back(handle);
			}
			std::sort(order.begin(), order.end(),
			          [&states](entt::entity a, entt::entity b)
			          {
						  return states.at(a).Entity.GetValue() < states.at(b).Entity.GetValue();
					  });
		}
	}

	void Scene::StartPhysics()
	{
		PhysicsWorldSettings world;
		world.Gravity = m_Settings.Physics.Gravity;
		world.LayerCount = m_RuntimeSettings.PhysicsLayerCount;
		world.IgnoredCollisions = m_RuntimeSettings.IgnoredCollisions;
		world.CollisionSteps = m_RuntimeSettings.CollisionSteps;
		Result<Scope<PhysicsScene>> physics = PhysicsScene::Create(world);
		if (!physics)
		{
			ST_CORE_ERROR("The scene runs without physics: {}", physics.GetError());
			return;
		}
		m_Physics = physics.TakeValue();
		m_PhysicsAccumulator = 0.0f;
		m_IsDrawnBetweenSteps = false;

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
		for (entt::entity const handle : m_Registry.view<VehicleComponent>())
		{
			m_ChangedVehicles.insert(handle);
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
		m_BodyOrder.clear();
		m_BodyOrderChanged = false;
		m_MovingBodies.clear();
		m_MovingBodiesChanged = false;
		m_DrawnBodies.clear();
		m_ChangedBodies.clear();
		m_PhysicsMaterials.clear();
		m_PhysicsMaterialEpoch = 0;
		m_Characters.clear();
		m_CharacterOrder.clear();
		m_CharacterOrderChanged = false;
		m_ChangedCharacters.clear();
		m_Vehicles.clear();
		m_VehicleWheelsSaid.clear();
		m_ChangedVehicles.clear();
		m_VehicleEpoch = 0;
		m_JointsChanged = false;
		m_ContactEvents.clear();
		m_PhysicsAccumulator = 0.0f;
		m_IsDrawnBetweenSteps = false;
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

		// Adding or removing a CharacterController replaces the entity's body (or gives it back); changing one changes only
		// the character, so the joints of the scene's bodies stay as they are.
		auto characterAdded = m_Registry.on_construct<CharacterControllerComponent>();
		auto characterChanged = m_Registry.on_update<CharacterControllerComponent>();
		auto characterRemoved = m_Registry.on_destroy<CharacterControllerComponent>();
		if (connect)
		{
			characterAdded.connect<&Scene::OnCharacterComponentAddedOrRemoved>(*this);
			characterChanged.connect<&Scene::OnCharacterComponentChanged>(*this);
			characterRemoved.connect<&Scene::OnCharacterComponentAddedOrRemoved>(*this);
		}
		else
		{
			characterAdded.disconnect<&Scene::OnCharacterComponentAddedOrRemoved>(*this);
			characterChanged.disconnect<&Scene::OnCharacterComponentChanged>(*this);
			characterRemoved.disconnect<&Scene::OnCharacterComponentAddedOrRemoved>(*this);
		}

		auto meshUpdated = m_Registry.on_update<MeshComponent>();
		if (connect)
		{
			meshUpdated.connect<&Scene::OnMeshComponentChanged>(*this);
		}
		else
		{
			meshUpdated.disconnect<&Scene::OnMeshComponentChanged>(*this);
		}

		auto const watchVehicles = [this, connect](auto sink)
		{
			if (connect)
			{
				sink.template connect<&Scene::OnVehicleComponentChanged>(*this);
			}
			else
			{
				sink.template disconnect<&Scene::OnVehicleComponentChanged>(*this);
			}
		};
		watchVehicles(m_Registry.on_construct<VehicleComponent>());
		watchVehicles(m_Registry.on_update<VehicleComponent>());
		watchVehicles(m_Registry.on_destroy<VehicleComponent>());
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
		m_ChangedCharacters.insert(handle);
	}

	void Scene::OnCharacterComponentAddedOrRemoved(entt::registry&, entt::entity handle)
	{
		// A character replaces the entity's body: the body is removed (or made again) too.
		m_ChangedCharacters.insert(handle);
		m_ChangedBodies.insert(handle);
	}

	void Scene::OnVehicleComponentChanged(entt::registry&, entt::entity handle)
	{
		m_ChangedVehicles.insert(handle);
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
		if (m_Physics && HasPendingPhysicsChanges())
		{
			// Between frames entities may be drawn between steps: bodies and joints are made from where the steps left them.
			ReturnBodiesToSteps();
			ReturnCharactersToSteps();
			DrawVehicleWheels(1.0f);
			RebuildChangedBodies();
			// After a frame's steps (its contact events, OnDestroy, the next OnUpdate or an edit), the frame still shows them
			// between their steps, as far as it did: the same alpha gives the same poses. Never inside the steps, which must
			// see the steps' poses alone.
			if (m_IsDrawnBetweenSteps)
			{
				float const alpha = GetDrawingAlpha();
				InterpolateBodyTransforms(alpha);
				InterpolateCharacterTransforms(alpha);
				DrawVehicleWheels(alpha);
			}
		}
	}

	std::optional<RaycastHit> Scene::Raycast(glm::vec3 const& origin, glm::vec3 const& direction, float maxDistance, uint32_t layerMask)
	{
		ApplyPhysicsChanges();
		return m_Physics ? m_Physics->Raycast(origin, direction, maxDistance, layerMask) : std::nullopt;
	}

	size_t Scene::RaycastAll(glm::vec3 const& origin, glm::vec3 const& direction, float maxDistance, std::vector<RaycastHit>& hits,
	                         PhysicsQueryFilter const& filter)
	{
		hits.clear();
		ApplyPhysicsChanges();
		return m_Physics ? m_Physics->RaycastAll(origin, direction, maxDistance, hits, filter) : 0;
	}

	std::optional<ShapeCastHit> Scene::ShapeCast(PhysicsQueryShape const& shape, glm::vec3 const& origin, glm::quat const& rotation,
	                                             glm::vec3 const& direction, float maxDistance, PhysicsQueryFilter const& filter)
	{
		ApplyPhysicsChanges();
		return m_Physics ? m_Physics->ShapeCast(shape, origin, rotation, direction, maxDistance, filter) : std::nullopt;
	}

	size_t Scene::Overlap(PhysicsQueryShape const& shape, glm::vec3 const& center, glm::quat const& rotation, std::vector<UUID>& entities,
	                      PhysicsQueryFilter const& filter)
	{
		entities.clear();
		ApplyPhysicsChanges();
		return m_Physics ? m_Physics->Overlap(shape, center, rotation, entities, filter) : 0;
	}

	float Scene::GetDrawingAlpha() const
	{
		return std::clamp(m_PhysicsAccumulator / GetFixedTimestep(), 0.0f, 1.0f);
	}

	bool Scene::HasPendingPhysicsChanges() const
	{
		return !m_ChangedBodies.empty() || !m_ChangedCharacters.empty() || !m_ChangedVehicles.empty() || m_JointsChanged;
	}

	void Scene::RebuildChangedBodies()
	{
		// First, so that the bodies rebuilt below and those kept have the same values of every material, and the vehicles
		// those of their definitions.
		SyncPhysicsMaterials();
		SyncVehicles();
		// In UUID order: Jolt numbers bodies in the order they are made and simulates them in that order.
		if (!m_ChangedBodies.empty())
		{
			std::vector<entt::entity> const changed = SortByUUID(m_Registry, std::exchange(m_ChangedBodies, {}), m_Bodies);
			for (entt::entity const handle : changed)
			{
				// Made, rebuilt or removed bodies lose their joints.
				if (UpdateBody(handle))
				{
					m_JointsChanged = true;
				}
				// A chassis's vehicle goes with its body: the world carries it over to a rebuilt dynamic body and drops it
				// otherwise, and UpdateVehicle sees which.
				if (m_Vehicles.contains(handle) || (m_Registry.valid(handle) && m_Registry.all_of<VehicleComponent>(handle)))
				{
					m_ChangedVehicles.insert(handle);
				}
			}
		}
		if (!m_ChangedCharacters.empty())
		{
			std::vector<entt::entity> const changed = SortByUUID(m_Registry, std::exchange(m_ChangedCharacters, {}), m_Characters);
			for (entt::entity const handle : changed)
			{
				UpdateCharacter(handle);
			}
		}
		// After the bodies they ride on; the world adds their constraints again in UUID order, before the joints.
		if (!m_ChangedVehicles.empty())
		{
			ST_PROFILE_SCOPE("Scene.SyncVehicles");
			std::vector<entt::entity> const changed = SortByUUID(m_Registry, std::exchange(m_ChangedVehicles, {}), m_Vehicles);
			for (entt::entity const handle : changed)
			{
				UpdateVehicle(handle);
			}
		}
		if (std::exchange(m_JointsChanged, false))
		{
			RebuildJoints();
		}
	}

	void Scene::SyncPhysicsMaterials()
	{
		uint64_t const epoch = DataAssetRegistry::GetEpoch();
		if (!m_Physics || epoch == m_PhysicsMaterialEpoch)
		{
			return;
		}
		m_PhysicsMaterialEpoch = epoch;
		if (m_PhysicsMaterials.empty())
		{
			return;
		}
		ST_PROFILE_SCOPE("Scene.SyncPhysicsMaterials");
		std::vector<AssetHandle> switched;
		std::vector<std::pair<AssetHandle, Ref<TypedDataAsset<PhysicsMaterial>>>> edited;
		for (auto& [handle, use] : m_PhysicsMaterials)
		{
			Ref<TypedDataAsset<PhysicsMaterial>> asset = FindPhysicsMaterial(handle);
			uint64_t const version = asset ? asset->GetVersion() : 0;
			if (version == use.Version)
			{
				continue;
			}
			if (asset && use.Version != 0)
			{
				// Edited (the inspector, physics-material.set, undo) or reloaded: the bodies keep moving as they were.
				edited.emplace_back(handle, std::move(asset));
			}
			else
			{
				switched.push_back(handle);
			}
			use.Version = version;
		}
		// In handle order, whatever the map's: the edits wake bodies, and what reaches Jolt keeps one order on every run.
		std::sort(edited.begin(), edited.end(),
		          [](auto const& first, auto const& second)
		          {
					  return first.first.GetUUID().GetValue() < second.first.GetUUID().GetValue();
				  });
		for (auto const& [handle, asset] : edited)
		{
			m_Physics->SetMaterialValues(handle, asset->GetData());
		}
		if (switched.empty())
		{
			return;
		}
		// A material that starts or stops applying changes which values the collider has: its bodies are made again.
		for (auto const& [handle, state] : m_Bodies)
		{
			bool const names = std::any_of(state.Materials.begin(), state.Materials.end(),
			                               [&switched](AssetHandle material)
			                               {
											   return std::find(switched.begin(), switched.end(), material) != switched.end();
										   });
			if (names)
			{
				m_ChangedBodies.insert(handle);
			}
		}
	}

	void Scene::SyncVehicles()
	{
		uint64_t const epoch = DataAssetRegistry::GetEpoch();
		if (!m_Physics || epoch == m_VehicleEpoch)
		{
			return;
		}
		m_VehicleEpoch = epoch;
		if (m_Vehicles.empty())
		{
			return;
		}
		ST_PROFILE_SCOPE("Scene.SyncVehicles");
		// In UUID order, whatever the map's (the changes reach the world in that order anyway: UpdateVehicle's are sorted).
		std::vector<entt::entity> order;
		SortKeysByUUID(m_Vehicles, order);
		for (entt::entity const handle : order)
		{
			VehicleSyncState const& state = m_Vehicles.at(handle);
			VehicleSources const sources = ReadVehicleSources(state.Definition);
			bool changed = (sources.Definition ? sources.Definition->GetVersion() : 0) != state.DefinitionVersion;
			for (size_t index = 0; index < VehicleCurveCount && !changed; index++)
			{
				AssetHandle const curve = index < sources.CurveHandles.size() ? sources.CurveHandles[index] : AssetHandle();
				uint64_t const version = index < sources.Curves.size() && sources.Curves[index] ? sources.Curves[index]->GetVersion() : 0;
				changed = curve != state.Curves[index] || version != state.CurveVersions[index];
			}
			if (changed)
			{
				m_ChangedVehicles.insert(handle);
			}
		}
	}

	void Scene::UpdateVehicle(entt::entity handle)
	{
		auto const existing = m_Vehicles.find(handle);
		bool const alive = m_Registry.valid(handle) && m_Registry.all_of<IDComponent>(handle);
		VehicleComponent const* component = alive ? m_Registry.try_get<VehicleComponent>(handle) : nullptr;
		if (component == nullptr)
		{
			// Destroyed entity, or removed component (the world removed a destroyed entity's vehicle with its body).
			if (existing != m_Vehicles.end())
			{
				m_Physics->RemoveVehicle(existing->second.Entity);
				m_Vehicles.erase(existing);
			}
			return;
		}

		Entity const entity(handle, this);
		UUID const id = entity.GetUUID();
		VehicleSyncState& state = m_Vehicles[handle];
		bool const sameDefinition = state.Definition == component->Definition;
		state.Entity = id;
		state.Definition = component->Definition;
		VehicleSources const sources = ReadVehicleSources(component->Definition);
		state.DefinitionVersion = sources.Definition ? sources.Definition->GetVersion() : 0;
		for (size_t index = 0; index < VehicleCurveCount; index++)
		{
			state.Curves[index] = index < sources.CurveHandles.size() ? sources.CurveHandles[index] : AssetHandle();
			state.CurveVersions[index] = index < sources.Curves.size() && sources.Curves[index] ? sources.Curves[index]->GetVersion() : 0;
		}

		std::string problem;
		std::vector<std::string> notes;
		RigidBodyComponent const* body = m_Registry.try_get<RigidBodyComponent>(handle);
		if (!sources.Definition)
		{
			problem = component->Definition.IsValid() ? fmt::format("makes no vehicle: its Definition {} is no Vehicle that loads",
			                                                        DescribeReference(component->Definition))
			                                          : std::string("makes no vehicle: it names no Definition");
		}
		else if (body == nullptr || body->Type != RigidBodyType::Dynamic || !m_Bodies.contains(handle) || !m_Physics->HasBody(id))
		{
			problem = "makes no vehicle: its entity has no dynamic RigidBody with a collider";
		}
		else
		{
			VehicleDesc const desc = MakeVehicleValues(id, sources, notes);
			bool const present = state.Made && m_Physics->HasVehicle(id);
			if (present && desc == state.Desc)
			{
				// The world carried it over to the entity's rebuilt body, or nothing it reads changed.
			}
			else if (present && sameDefinition && desc.Up == state.Desc.Up && desc.Forward == state.Desc.Forward)
			{
				// An edit of the definition or of a curve: the new values from the next step on, the ride and the structure
				// kept.
				if (Result<void> changed = m_Physics->SetVehicleSettings(id, desc); changed)
				{
					state.Desc = desc;
				}
				else
				{
					notes.push_back(fmt::format("keeps its values, as the new ones are refused: {}", changed.GetError()));
				}
			}
			else if (Result<void> made = m_Physics->AddVehicle(desc); made)
			{
				state.Made = true;
				state.Desc = desc;
				// Made again, its wheels are drawn from its first step on.
				state.Wheels = {};
			}
			else
			{
				problem = fmt::format("makes no vehicle: {}", made.GetError());
			}
		}
		if (!problem.empty())
		{
			// Like every refusal, it leaves the entity without a vehicle.
			m_Physics->RemoveVehicle(id);
			state.Made = false;
			state.Desc = {};
		}
		if (state.Made)
		{
			// What its wheels are drawn with: the values the world was given, its directions as the world takes them, and the
			// definition's wheel names (which no value of the world holds, so a rename applies here alone).
			glm::vec3 up = state.Desc.Up;
			glm::vec3 forward = state.Desc.Forward;
			if (NormalizeVehicleAxes(up, forward))
			{
				state.Up = up;
				state.Forward = forward;
			}
			Vehicle const& definition = sources.Definition->GetData();
			for (size_t index = 0; index < VehicleWheelCount; index++)
			{
				VehicleWheelDesc const& wheel = state.Desc.Wheels[index];
				VehicleWheelDesc& geometry = state.WheelGeometry[index];
				geometry.Position = wheel.Position;
				geometry.SuspensionDirection = glm::normalize(wheel.SuspensionDirection);
				geometry.SteeringAxis = glm::normalize(wheel.SteeringAxis);
				state.WheelNames[index] = index < definition.Wheels.size() ? definition.Wheels[index].Name : std::string();
			}
		}
		std::string said = problem;
		for (std::string const& note : notes)
		{
			said += (said.empty() ? "" : "; ") + note;
		}
		// Said once: a body rebuilt every step, or a component set again to the same value, says nothing new.
		if (said != state.Said)
		{
			if (!said.empty())
			{
				ST_CORE_WARN("Physics: the Vehicle of '{}' {}", entity.GetName(), said);
			}
			state.Said = std::move(said);
		}
	}

	std::vector<entt::entity> Scene::GetMadeVehicleOrder() const
	{
		std::vector<entt::entity> order;
		SortKeysByUUID(m_Vehicles, order);
		std::erase_if(order,
		              [this](entt::entity handle)
		              {
						  return !m_Vehicles.at(handle).Made;
					  });
		return order;
	}

	void Scene::RecordVehicleWheelSteps(float step)
	{
		for (auto& [handle, state] : m_Vehicles)
		{
			std::optional<VehicleState> const vehicle = state.Made ? m_Physics->GetVehicleState(state.Entity) : std::nullopt;
			if (!vehicle)
			{
				continue;
			}
			ScenePhysicsSnapshot::WheelDrawing& drawing = state.Wheels;
			for (size_t index = 0; index < VehicleWheelCount; index++)
			{
				VehicleWheelState const& wheel = vehicle->Wheels[index];
				ScenePhysicsSnapshot::WheelStep const now{wheel.SuspensionLength, wheel.SteerAngle, wheel.RotationAngle,
				                                          wheel.AngularVelocity};
				ScenePhysicsSnapshot::WheelStep const before = drawing.HasSteps ? drawing.Step[index] : now;
				// Jolt keeps the angle within a turn; the spin tells how many whole turns its change in the step leaves out.
				float advance = 0.0f;
				if (drawing.HasSteps)
				{
					float const turned = now.RotationAngle - before.RotationAngle;
					float const expected = 0.5f * (before.AngularVelocity + now.AngularVelocity) * step;
					float const turn = glm::two_pi<float>();
					advance = turned + std::round((expected - turned) / turn) * turn;
				}
				drawing.PreviousStep[index] = before;
				drawing.Step[index] = now;
				drawing.Advance[index] = std::isfinite(advance) ? advance : 0.0f;
			}
			drawing.HasSteps = true;
		}
		DrawVehicleWheels(1.0f);
	}

	void Scene::DrawVehicleWheels(float alpha)
	{
		auto const wheels = m_Registry.view<VehicleWheelComponent>();
		for (entt::entity const handle : wheels)
		{
			Entity const wheel(handle, this);
			// The wheel of the nearest entity above it with a Vehicle (said once when there is none to draw).
			std::string problem;
			VehicleSyncState const* vehicle = nullptr;
			Entity chassis;
			size_t index = 0;
			if (m_Bodies.contains(handle) || m_Characters.contains(handle))
			{
				problem = "draws no wheel: its entity has a body or a character of its own, which the vehicle would move";
			}
			else
			{
				Entity above = GetParent(wheel);
				for (size_t depth = 0; above && !above.HasComponent<VehicleComponent>() && depth < m_EntityMap.size(); depth++)
				{
					above = GetParent(above);
				}
				auto const found = above && above.HasComponent<VehicleComponent>() ? m_Vehicles.find(above.GetHandle()) : m_Vehicles.end();
				std::string const& name = m_Registry.get<VehicleWheelComponent>(handle).Wheel;
				if (!above || !above.HasComponent<VehicleComponent>())
				{
					problem = "draws no wheel: no entity above it has a Vehicle";
				}
				else if (found != m_Vehicles.end() && found->second.Made)
				{
					std::array<std::string, VehicleWheelCount> const& names = found->second.WheelNames;
					auto const named = std::find_if(names.begin(), names.end(),
					                                [&name](std::string const& candidate)
					                                {
														return IsSameVehicleWheelName(candidate, name);
													});
					if (named == names.end())
					{
						problem = fmt::format("draws no wheel: the definition of the Vehicle of '{}' has no wheel named '{}'",
						                      above.GetName(), name);
					}
					else
					{
						vehicle = &found->second;
						chassis = above;
						index = static_cast<size_t>(named - names.begin());
					}
				}
			}
			std::string& said = m_VehicleWheelsSaid[handle];
			if (problem != said)
			{
				if (!problem.empty())
				{
					ST_CORE_WARN("Physics: the VehicleWheel of '{}' {}", wheel.GetName(), problem);
				}
				said = std::move(problem);
			}
			// Nothing to draw before the vehicle's first step.
			if (vehicle == nullptr || !vehicle->Wheels.HasSteps)
			{
				continue;
			}
			glm::vec3 position;
			glm::quat rotation;
			glm::vec3 scale;
			if (!Math::DecomposeTransform(GetWorldTransform(chassis), position, rotation, scale))
			{
				continue;
			}
			RigidBodyComponent const* body = m_Registry.try_get<RigidBodyComponent>(chassis.GetHandle());
			float const part = body != nullptr && body->Interpolation == RigidBodyInterpolation::Interpolate ? alpha : 1.0f;
			ScenePhysicsSnapshot::WheelDrawing const& drawing = vehicle->Wheels;
			ScenePhysicsSnapshot::WheelStep const& from = drawing.PreviousStep[index];
			ScenePhysicsSnapshot::WheelStep const& to = drawing.Step[index];
			// From the last step back, as bodies are, so that the whole step (1) is exactly there.
			float const back = 1.0f - part;
			glm::mat4 const local = ComposeVehicleWheelTransform(vehicle->WheelGeometry[index], vehicle->Up, vehicle->Forward,
			                                                     to.SuspensionLength - back * (to.SuspensionLength - from.SuspensionLength),
			                                                     to.SteerAngle - back * (to.SteerAngle - from.SteerAngle),
			                                                     to.RotationAngle - back * drawing.Advance[index]);
			glm::vec3 wheelPosition;
			glm::quat wheelRotation;
			glm::vec3 wheelScale;
			if (Math::DecomposeTransform(Math::ComposeTransform(position, rotation, glm::vec3(1.0f)) * local, wheelPosition, wheelRotation,
			                             wheelScale))
			{
				SetWorldPoseInParent(*this, wheel, wheelPosition, wheelRotation);
			}
		}
	}

	void Scene::TrackPhysicsMaterials(std::vector<AssetHandle> const& previous, std::vector<AssetHandle> const& current)
	{
		// The new ones first, so that a rebuilt body naming a material again keeps its entry and version.
		for (AssetHandle const material : current)
		{
			auto const [use, inserted] = m_PhysicsMaterials.try_emplace(material);
			if (inserted)
			{
				Ref<TypedDataAsset<PhysicsMaterial>> const asset = FindPhysicsMaterial(material);
				use->second.Version = asset ? asset->GetVersion() : 0;
			}
			use->second.Bodies++;
		}
		for (AssetHandle const material : previous)
		{
			auto const use = m_PhysicsMaterials.find(material);
			if (use != m_PhysicsMaterials.end() && --use->second.Bodies == 0)
			{
				m_PhysicsMaterials.erase(use);
			}
		}
	}

	std::vector<entt::entity> const& Scene::GetBodyOrder()
	{
		if (std::exchange(m_BodyOrderChanged, false))
		{
			SortKeysByUUID(m_Bodies, m_BodyOrder);
		}
		return m_BodyOrder;
	}

	std::vector<entt::entity> const& Scene::GetMovingBodies()
	{
		if (std::exchange(m_MovingBodiesChanged, false))
		{
			m_MovingBodies.clear();
			for (entt::entity const handle : GetBodyOrder())
			{
				if (m_Bodies.at(handle).Moves)
				{
					m_MovingBodies.push_back(handle);
				}
			}
		}
		return m_MovingBodies;
	}

	std::vector<entt::entity> const& Scene::GetCharacterOrder()
	{
		if (std::exchange(m_CharacterOrderChanged, false))
		{
			SortKeysByUUID(m_Characters, m_CharacterOrder);
		}
		return m_CharacterOrder;
	}

	void Scene::RebuildJoints()
	{
		// Jolt solves joints in the order they were added: they go and come in the order of their entities' UUIDs.
		for (entt::entity const handle : GetBodyOrder())
		{
			m_Physics->RemoveJoint(m_Bodies.at(handle).Entity);
		}
		std::vector<std::pair<uint64_t, entt::entity>> joints;
		for (entt::entity const handle : m_Registry.view<JointComponent>())
		{
			joints.emplace_back(m_Registry.get<IDComponent>(handle).ID.GetValue(), handle);
		}
		std::sort(joints.begin(), joints.end());
		for (auto const& [id, handle] : joints)
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

	bool Scene::UpdateBody(entt::entity handle)
	{
		auto const existing = m_Bodies.find(handle);
		if (!m_Registry.valid(handle) || !m_Registry.all_of<IDComponent>(handle))
		{
			// Destroyed entity.
			if (existing == m_Bodies.end())
			{
				return false;
			}
			m_Physics->RemoveBody(existing->second.Entity);
			TrackPhysicsMaterials(existing->second.Materials, {});
			m_Bodies.erase(existing);
			m_BodyOrderChanged = true;
			m_MovingBodiesChanged = true;
			return true;
		}

		Entity const entity(handle, this);
		UUID const id = entity.GetUUID();
		bool const hadBody = m_Physics->HasBody(id);
		std::vector<AssetHandle> materials;
		std::optional<BodyDesc> desc = BuildBodyDesc(*this, entity, materials);
		if (!desc)
		{
			m_Physics->RemoveBody(id);
			if (existing != m_Bodies.end())
			{
				TrackPhysicsMaterials(existing->second.Materials, {});
				m_Bodies.erase(existing);
				m_BodyOrderChanged = true;
				m_MovingBodiesChanged = true;
			}
			return hadBody;
		}
		// A rebuilt body keeps moving as before.
		if (hadBody)
		{
			desc->LinearVelocity = m_Physics->GetLinearVelocity(id);
			desc->AngularVelocity = m_Physics->GetAngularVelocity(id);
		}
		if (Result<void> added = m_Physics->AddBody(*desc); !added)
		{
			ST_CORE_WARN("Physics: {}", added.GetError());
			if (existing != m_Bodies.end())
			{
				TrackPhysicsMaterials(existing->second.Materials, {});
				m_Bodies.erase(existing);
				m_BodyOrderChanged = true;
				m_MovingBodiesChanged = true;
			}
			return hadBody;
		}
		bool const created = existing == m_Bodies.end();
		if (created)
		{
			m_BodyOrderChanged = true;
		}
		m_MovingBodiesChanged = true;
		bool const moves = desc->Type != RigidBodyType::Static;
		BodyState& state = m_Bodies[handle];
		// A rebuilt body that moved before keeps being drawn on its way; a new one (or one that starts moving) is drawn where
		// it is.
		if (created || !state.Moves || !moves)
		{
			TransformComponent const& transform = entity.GetComponent<TransformComponent>();
			state.Drawing = {};
			state.Drawing.Place({transform.Translation, transform.Rotation});
		}
		state.Entity = id;
		state.Position = desc->Position;
		state.Rotation = desc->Rotation;
		state.Moves = moves;
		TrackPhysicsMaterials(state.Materials, materials);
		state.Materials = std::move(materials);
		return true;
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
				m_CharacterOrderChanged = true;
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
		bool const created = !m_Physics->HasCharacter(id);
		// Between frames the entity is drawn on its way to where the last step left it: unless something moved it since (a
		// script, the editor, its parent), the character stays there.
		bool const placed = created || existing == m_Characters.end() || HasMoved(position, existing->second.Position);
		glm::vec3 const origin = placed ? position : existing->second.StepPosition;
		if (Result<void> added = m_Physics->AddCharacter(MakeCharacterDesc(id, *component, origin, scale)); !added)
		{
			ST_CORE_WARN("Physics: {}", added.GetError());
			if (existing != m_Characters.end())
			{
				m_Characters.erase(existing);
				m_CharacterOrderChanged = true;
			}
			return;
		}
		// Said once, when the character is made: scripts may change its component every step.
		if (created && HasBodyComponents(m_Registry, handle))
		{
			ST_CORE_WARN("Physics: the CharacterController of '{}' replaces its RigidBody and colliders", entity.GetName());
		}
		if (existing == m_Characters.end())
		{
			m_CharacterOrderChanged = true;
		}
		CharacterSyncState& state = m_Characters[handle];
		if (created)
		{
			// A new character ignores nothing yet.
			state.Parts.clear();
		}
		state.Entity = id;
		state.Scale = scale;
		if (placed)
		{
			state.Place(position);
		}
		SyncCharacterParts(*this, *m_Physics, entity, id, state.Parts);
	}

	void Scene::TeleportCharacter(Entity entity, glm::vec3 const& position)
	{
		if (!entity || entity.GetScene() != this || !entity.HasComponent<CharacterControllerComponent>())
		{
			return;
		}
		SetWorldPosition(*this, entity, position);
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
		state->second.Place(position);
	}

	void Scene::SyncCharactersFromTransforms()
	{
		std::vector<entt::entity> rescaled;
		for (entt::entity const handle : GetCharacterOrder())
		{
			CharacterSyncState& state = m_Characters.at(handle);
			CharacterControllerComponent const* component =
				m_Registry.valid(handle) ? m_Registry.try_get<CharacterControllerComponent>(handle) : nullptr;
			glm::vec3 position;
			glm::quat rotation;
			glm::vec3 scale;
			Entity const entity(handle, this);
			if (component == nullptr || !Math::DecomposeTransform(GetWorldTransform(entity), position, rotation, scale))
			{
				continue;
			}
			// Entities put below it, moved away or destroyed change what it ignores.
			SyncCharacterParts(*this, *m_Physics, entity, state.Entity, state.Parts);
			if (glm::any(glm::greaterThan(glm::abs(scale - state.Scale), glm::vec3(ScaleTolerance))))
			{
				// A new size: the capsule changes (and the character moves to where the entity is).
				rescaled.push_back(handle);
			}
			else if (HasMoved(position, state.Position))
			{
				// Moved by a script, the editor or its parent; turning the entity does not turn the capsule.
				m_Physics->SetCharacterPosition(state.Entity, position + component->Offset * state.Scale);
				state.Place(position);
			}
		}
		for (entt::entity const handle : rescaled)
		{
			UpdateCharacter(handle);
		}
	}

	void Scene::WriteCharacterTransforms()
	{
		std::vector<CharacterPlacement> placements;
		for (auto& [handle, state] : m_Characters)
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
			state.PreviousStepPosition = state.StepPosition;
			state.StepPosition = position;
			state.Carried = GetCarriedPart(position - state.PreviousStepPosition, character->GroundDisplacement);
			state.Ground = character->GroundEntity;
			// Exactly where the step left it, so that what the entity's children are synced from depends on the steps alone
			// (a character standing still leaves its transform alone).
			if (position != state.Position)
			{
				placements.push_back({handle, state.Entity, position, GetDepth(*this, Entity(handle, this))});
			}
		}
		PlaceCharacterEntities(*this, placements);
		for (CharacterPlacement const& placement : placements)
		{
			m_Characters[placement.Handle].Position = placement.Position;
		}
	}

	void Scene::ReturnCharactersToSteps()
	{
		std::vector<CharacterPlacement> placements;
		for (auto const& [handle, state] : m_Characters)
		{
			// Exactly: the steps must not see where the frame drew it, however close that was.
			if (state.StepPosition == state.Position || !m_Registry.valid(handle))
			{
				continue;
			}
			// Entities moved since they were drawn stay where they were put: the next step takes their characters there.
			Entity const entity(handle, this);
			if (!HasMoved(glm::vec3(GetWorldTransform(entity)[3]), state.Position))
			{
				placements.push_back({handle, state.Entity, state.StepPosition, GetDepth(*this, entity)});
			}
		}
		PlaceCharacterEntities(*this, placements);
		for (CharacterPlacement const& placement : placements)
		{
			m_Characters[placement.Handle].Position = placement.Position;
		}
	}

	void Scene::InterpolateCharacterTransforms(float alpha)
	{
		std::vector<CharacterPlacement> placements;
		for (auto const& [handle, state] : m_Characters)
		{
			// The way it went by itself in the last step is drawn as far as the time left over makes up. So is the way what it
			// stands on carried it when that ground is drawn between its steps too; a ground drawn where its last step left it
			// keeps the character drawn on it, carried the whole way.
			glm::vec3 const way = state.StepPosition - state.PreviousStepPosition;
			glm::vec3 const drawnWay = IsBodyDrawn(state.Ground) ? way : way - state.Carried;
			glm::vec3 const drawn = state.StepPosition - (1.0f - alpha) * drawnWay;
			if (!HasMoved(drawn, state.Position) || !m_Registry.valid(handle))
			{
				continue;
			}
			// Entities moved since the last step (between steps, by a script or the editor) stay where they were put.
			Entity const entity(handle, this);
			if (!HasMoved(glm::vec3(GetWorldTransform(entity)[3]), state.Position))
			{
				placements.push_back({handle, state.Entity, drawn, GetDepth(*this, entity)});
			}
		}
		PlaceCharacterEntities(*this, placements);
		for (CharacterPlacement const& placement : placements)
		{
			m_Characters[placement.Handle].Position = placement.Position;
		}
	}

	void Scene::UpdatePhysics(float deltaTime)
	{
		m_ContactEvents.clear();
		if (m_Physics)
		{
			// Changes and steps start from where the last step left the bodies and characters, and scripts' OnFixedUpdate finds
			// them there.
			{
				ST_PROFILE_SCOPE("Scene.ReturnToSteps");
				m_IsDrawnBetweenSteps = false;
				ReturnBodiesToSteps();
				NoteBodiesMovedSinceSteps();
				ReturnCharactersToSteps();
				// From the chassis where its last step left it.
				DrawVehicleWheels(1.0f);
			}
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
			RunFixedStep(step);
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
			// The time left over, part of the next step, is how far into their last step bodies and characters are drawn: the
			// bodies first, so a character knows whether its ground is drawn so.
			{
				ST_PROFILE_SCOPE("Scene.Interpolate");
				float const alpha = GetDrawingAlpha();
				InterpolateBodyTransforms(alpha);
				InterpolateCharacterTransforms(alpha);
				// From the chassis as it is drawn.
				DrawVehicleWheels(alpha);
				m_IsDrawnBetweenSteps = true;
			}
			m_ContactEvents = m_Physics->TakeContactEvents();
		}
	}

	void Scene::RunFixedStep(float step)
	{
		// Scripts act before each step (forces, kinematic targets, teleports, character moves).
		{
			ST_PROFILE_SCOPE("Scene.FixedUpdateScripts");
			FixedUpdateScripts(step);
		}
		if (m_Physics)
		{
			// The bodies and characters scripts changed reach the world before this step, whatever the frame rate.
			RebuildChangedBodies();
			{
				ST_PROFILE_SCOPE("Scene.SyncToPhysics");
				SyncBodiesFromTransforms(step);
				SyncCharactersFromTransforms();
			}
			m_Physics->Step(step);
			{
				ST_PROFILE_SCOPE("Scene.SyncFromPhysics");
				WriteBodyTransforms();
				WriteCharacterTransforms();
				RecordBodySteps();
				// After the chassis's entity is where the step left it.
				RecordVehicleWheelSteps(step);
			}
		}
		m_FixedStepCount++;
		if (m_FixedStepCount == m_RuntimeSettings.PhysicsHashStep)
		{
			m_RecordedPhysicsHash = ComputePhysicsStateHash();
		}
	}

	void Scene::SyncBodiesFromTransforms(float stepDelta)
	{
		for (entt::entity const handle : GetBodyOrder())
		{
			BodyState& state = m_Bodies.at(handle);
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
			TransformComponent const& transform = m_Registry.get<TransformComponent>(handle);
			ScenePhysicsSnapshot::LocalPose const local{transform.Translation, transform.Rotation};
			if (body != nullptr && body->Type == RigidBodyType::Kinematic)
			{
				m_Physics->MoveKinematic(state.Entity, position, rotation, stepDelta);
				// A kinematic body moved between frames goes there in this step; it is drawn moving only when the fixed steps
				// move it.
				if (state.Drawing.Moved)
				{
					state.Drawing.Place(local);
				}
			}
			else if (HasMoved(position, rotation, state.Position, state.Rotation))
			{
				// Moved by a script, the editor or its parent: it is drawn on its way from there, never from where it was.
				m_Physics->SetBodyTransform(state.Entity, position, rotation);
				state.Drawing.Place(local);
			}
			state.Drawing.Moved = false;
			state.Position = position;
			state.Rotation = rotation;
		}
	}

	void Scene::RecordBodySteps()
	{
		for (entt::entity const handle : GetMovingBodies())
		{
			TransformComponent const* transform = m_Registry.valid(handle) ? m_Registry.try_get<TransformComponent>(handle) : nullptr;
			if (transform != nullptr)
			{
				ScenePhysicsSnapshot::BodyDrawing& drawing = m_Bodies.at(handle).Drawing;
				drawing.PreviousStep = drawing.Step;
				drawing.Step = {transform->Translation, transform->Rotation};
			}
		}
	}

	void Scene::ReturnBodiesToSteps()
	{
		for (entt::entity const handle : m_DrawnBodies)
		{
			auto const state = m_Bodies.find(handle);
			TransformComponent* transform = m_Registry.valid(handle) ? m_Registry.try_get<TransformComponent>(handle) : nullptr;
			if (state == m_Bodies.end() || transform == nullptr || !state->second.Drawing.IsDrawn())
			{
				continue;
			}
			ScenePhysicsSnapshot::BodyDrawing& drawing = state->second.Drawing;
			// Exactly the bits the step left, so the steps see what they would have seen had it not been drawn. Translation and
			// rotation each on their own: a part moved since it was drawn (a script setting only the rotation) stays where it
			// was put, the other goes back, and the next step takes the body to that pose.
			if (drawing.TranslationDrawn)
			{
				if (transform->Translation == drawing.Drawn.Translation)
				{
					transform->Translation = drawing.Step.Translation;
				}
				else
				{
					drawing.Moved = true;
				}
			}
			if (drawing.RotationDrawn)
			{
				if (transform->Rotation == drawing.Drawn.Rotation)
				{
					transform->Rotation = drawing.Step.Rotation;
				}
				else
				{
					drawing.Moved = true;
				}
			}
			drawing.TranslationDrawn = false;
			drawing.RotationDrawn = false;
		}
		m_DrawnBodies.clear();
	}

	void Scene::NoteBodiesMovedSinceSteps()
	{
		for (entt::entity const handle : GetMovingBodies())
		{
			ScenePhysicsSnapshot::BodyDrawing& drawing = m_Bodies.at(handle).Drawing;
			TransformComponent const* transform = m_Registry.valid(handle) ? m_Registry.try_get<TransformComponent>(handle) : nullptr;
			if (transform != nullptr && ScenePhysicsSnapshot::LocalPose{transform->Translation, transform->Rotation} != drawing.Step)
			{
				drawing.Moved = true;
			}
		}
	}

	void Scene::InterpolateBodyTransforms(float alpha)
	{
		for (entt::entity const handle : GetMovingBodies())
		{
			ScenePhysicsSnapshot::BodyDrawing& drawing = m_Bodies.at(handle).Drawing;
			if (drawing.IsDrawn() || drawing.PreviousStep == drawing.Step || !m_Registry.valid(handle))
			{
				continue;
			}
			RigidBodyComponent const* body = m_Registry.try_get<RigidBodyComponent>(handle);
			TransformComponent* transform = m_Registry.try_get<TransformComponent>(handle);
			if (body == nullptr || body->Interpolation != RigidBodyInterpolation::Interpolate || transform == nullptr)
			{
				continue;
			}
			// In local space: what is below the body, and a body below another, are drawn as they are at the steps.
			ScenePhysicsSnapshot::LocalPose const& from = drawing.PreviousStep;
			ScenePhysicsSnapshot::LocalPose const& to = drawing.Step;
			// Each part still where the last step left it; a part moved since (by OnUpdate or the editor, in a frame that ran
			// no step) is shown where it was put.
			bool const drawTranslation = transform->Translation == to.Translation && from.Translation != to.Translation;
			bool const drawRotation = transform->Rotation == to.Rotation && from.Rotation != to.Rotation;
			if (!drawTranslation && !drawRotation)
			{
				continue;
			}
			// From the last step back, so that the whole step (alpha 1) is drawn exactly there; slerp takes the shorter way.
			if (drawTranslation)
			{
				transform->Translation = to.Translation - (1.0f - alpha) * (to.Translation - from.Translation);
			}
			if (drawRotation)
			{
				transform->Rotation = Math::NormalizeRotation(glm::slerp(from.Rotation, to.Rotation, alpha));
			}
			drawing.Drawn = {transform->Translation, transform->Rotation};
			drawing.TranslationDrawn = drawTranslation;
			drawing.RotationDrawn = drawRotation;
			m_DrawnBodies.push_back(handle);
		}
	}

	bool Scene::IsBodyDrawn(UUID entity) const
	{
		if (!entity.IsValid())
		{
			return false;
		}
		auto const state = m_Bodies.find(FindHandle(entity));
		return state != m_Bodies.end() && state->second.Drawing.IsDrawn();
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
			SetWorldPose(*this, Entity(update.Handle, this), update.Transform.Position, update.Transform.Rotation);
			if (auto const state = m_Bodies.find(update.Handle); state != m_Bodies.end())
			{
				state->second.Position = update.Transform.Position;
				state->second.Rotation = update.Transform.Rotation;
			}
		}
	}

	uint64_t Scene::ComputePhysicsStateHash() const
	{
		HashBuilder hash;
		hash.AddUInt64(m_FixedStepCount);
		if (!m_Physics)
		{
			return hash.GetHash();
		}
		hash.AddUInt64(m_Physics->ComputeStateHash());
		// The positions characters are drawn between: in UUID order (a const function cannot make the cached order).
		std::vector<CharacterSyncState const*> characters;
		characters.reserve(m_Characters.size());
		for (auto const& [handle, state] : m_Characters)
		{
			characters.push_back(&state);
		}
		std::sort(characters.begin(), characters.end(),
		          [](CharacterSyncState const* a, CharacterSyncState const* b)
		          {
					  return a->Entity.GetValue() < b->Entity.GetValue();
				  });
		auto const addVector = [&hash](glm::vec3 const& value)
		{
			hash.AddFloat(value.x);
			hash.AddFloat(value.y);
			hash.AddFloat(value.z);
		};
		for (CharacterSyncState const* character : characters)
		{
			hash.AddUInt64(character->Entity.GetValue());
			addVector(character->PreviousStepPosition);
			addVector(character->StepPosition);
			addVector(character->Carried);
		}
		return hash.GetHash();
	}

	Result<ScenePhysicsSnapshot> Scene::SavePhysicsState()
	{
		if (!m_IsRunning || !m_Physics)
		{
			return Error{"the scene does not run physics"};
		}
		ApplyPhysicsChanges();
		ScenePhysicsSnapshot snapshot;
		snapshot.m_Physics = m_Physics->SaveState();
		snapshot.m_Step = m_FixedStepCount;
		snapshot.m_Accumulator = m_PhysicsAccumulator;
		snapshot.m_IsDrawnBetweenSteps = m_IsDrawnBetweenSteps;
		snapshot.m_IDSequence = m_RuntimeIDSequence;
		std::vector<entt::entity> const& bodies = GetBodyOrder();
		snapshot.m_Bodies.reserve(bodies.size());
		for (entt::entity const handle : bodies)
		{
			BodyState const& state = m_Bodies.at(handle);
			TransformComponent const* transform = m_Registry.try_get<TransformComponent>(handle);
			snapshot.m_Bodies.push_back(
				{state.Entity, transform != nullptr ? *transform : TransformComponent(), state.Position, state.Rotation, state.Drawing});
		}
		std::vector<entt::entity> const& characters = GetCharacterOrder();
		snapshot.m_Characters.reserve(characters.size());
		for (entt::entity const handle : characters)
		{
			CharacterSyncState const& state = m_Characters.at(handle);
			TransformComponent const* transform = m_Registry.try_get<TransformComponent>(handle);
			snapshot.m_Characters.push_back({state.Entity, transform != nullptr ? *transform : TransformComponent(), state.Position,
			                                 state.PreviousStepPosition, state.StepPosition, state.Carried, state.Ground});
		}
		for (entt::entity const handle : GetMadeVehicleOrder())
		{
			VehicleSyncState const& state = m_Vehicles.at(handle);
			snapshot.m_Vehicles.push_back({state.Entity, state.Wheels});
		}
		return snapshot;
	}

	Result<void> Scene::RestorePhysicsState(ScenePhysicsSnapshot const& snapshot)
	{
		if (!m_IsRunning || !m_Physics)
		{
			return Error{"the scene does not run physics"};
		}
		ApplyPhysicsChanges();
		// The scene's side must hold the same bodies and characters before anything changes.
		std::vector<entt::entity> const& bodies = GetBodyOrder();
		std::vector<entt::entity> const& characters = GetCharacterOrder();
		bool const sameBodies = std::equal(bodies.begin(), bodies.end(), snapshot.m_Bodies.begin(), snapshot.m_Bodies.end(),
		                                   [this](entt::entity handle, ScenePhysicsSnapshot::Body const& saved)
		                                   {
											   return m_Bodies.at(handle).Entity == saved.Entity && m_Registry.valid(handle);
										   });
		bool const sameCharacters =
			std::equal(characters.begin(), characters.end(), snapshot.m_Characters.begin(), snapshot.m_Characters.end(),
		               [this](entt::entity handle, ScenePhysicsSnapshot::Character const& saved)
		               {
						   return m_Characters.at(handle).Entity == saved.Entity && m_Registry.valid(handle);
					   });
		std::vector<entt::entity> const vehicles = GetMadeVehicleOrder();
		bool const sameVehicles = std::equal(vehicles.begin(), vehicles.end(), snapshot.m_Vehicles.begin(), snapshot.m_Vehicles.end(),
		                                     [this](entt::entity handle, ScenePhysicsSnapshot::VehicleWheels const& saved)
		                                     {
												 return m_Vehicles.at(handle).Entity == saved.Entity;
											 });
		if (!sameBodies || !sameCharacters || !sameVehicles)
		{
			return Error{"bodies, characters or vehicles were added or removed since the snapshot"};
		}
		if (Result<void> restored = m_Physics->RestoreState(snapshot.m_Physics); !restored)
		{
			return restored;
		}
		m_FixedStepCount = snapshot.m_Step;
		m_PhysicsAccumulator = snapshot.m_Accumulator;
		m_IsDrawnBetweenSteps = snapshot.m_IsDrawnBetweenSteps;
		// Entities made from here on take the IDs they took then (GenerateUniqueID skips one an entity made since still holds).
		m_RuntimeIDSequence = snapshot.m_IDSequence;
		// The entities exactly as they were, drawn as they were, so the next syncs see neither edits nor moves.
		m_DrawnBodies.clear();
		for (size_t i = 0; i < bodies.size(); i++)
		{
			ScenePhysicsSnapshot::Body const& saved = snapshot.m_Bodies[i];
			BodyState& state = m_Bodies.at(bodies[i]);
			state.Position = saved.Position;
			state.Rotation = saved.Rotation;
			state.Drawing = saved.Drawing;
			if (state.Drawing.IsDrawn())
			{
				m_DrawnBodies.push_back(bodies[i]);
			}
			if (TransformComponent* transform = m_Registry.try_get<TransformComponent>(bodies[i]))
			{
				*transform = saved.Transform;
			}
		}
		for (size_t i = 0; i < characters.size(); i++)
		{
			ScenePhysicsSnapshot::Character const& saved = snapshot.m_Characters[i];
			CharacterSyncState& state = m_Characters.at(characters[i]);
			state.Position = saved.Position;
			state.PreviousStepPosition = saved.PreviousStepPosition;
			state.StepPosition = saved.StepPosition;
			state.Carried = saved.Carried;
			state.Ground = saved.Ground;
			if (TransformComponent* transform = m_Registry.try_get<TransformComponent>(characters[i]))
			{
				*transform = saved.Transform;
			}
		}
		// The wheels drawn as they were, from their chassis where it was and the steps they were drawn between.
		for (size_t i = 0; i < vehicles.size(); i++)
		{
			m_Vehicles.at(vehicles[i]).Wheels = snapshot.m_Vehicles[i].Drawing;
		}
		DrawVehicleWheels(m_IsDrawnBetweenSteps ? GetDrawingAlpha() : 1.0f);
		// The events of the steps that are undone were taken already; the next frame's are those of the steps it runs.
		m_ContactEvents.clear();
		return {};
	}
}
