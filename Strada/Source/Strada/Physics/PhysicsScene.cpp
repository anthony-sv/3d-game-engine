#include "stpch.h"
#include "Strada/Physics/PhysicsScene.h"

#include "Strada/Asset/MeshSource.h"
#include "Strada/Core/Hash.h"
#include "Strada/Physics/JoltContext.h"
#include "Strada/Physics/PhysicsSystem.h"

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace Strada
{
	namespace
	{
		constexpr JPH::uint MaxBodies = 65536;
		constexpr JPH::uint MaxBodyPairs = 65536;
		constexpr JPH::uint MaxContactConstraints = 16384;
		constexpr size_t TempAllocatorSize = 16 * 1024 * 1024;
		// Smallest extent of a shape: degenerate sizes (zero scale) would make Jolt reject the shape.
		constexpr float MinimumExtent = 1e-3f;

		// Object layers encode the project layer and whether the body moves: 2 * layer + moving. The broad phase keeps
		// static and moving bodies in separate trees.
		namespace Layers
		{
			constexpr JPH::BroadPhaseLayer Static(0);
			constexpr JPH::BroadPhaseLayer Moving(1);

			JPH::ObjectLayer Make(uint32_t layer, bool moving)
			{
				return static_cast<JPH::ObjectLayer>(layer * 2 + (moving ? 1u : 0u));
			}

			uint32_t GetLayer(JPH::ObjectLayer objectLayer)
			{
				return static_cast<uint32_t>(objectLayer) >> 1;
			}

			bool IsMoving(JPH::ObjectLayer objectLayer)
			{
				return (objectLayer & 1u) != 0;
			}
		}

		class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface
		{
		public:
			JPH::uint GetNumBroadPhaseLayers() const override { return 2; }

			JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
			{
				return Layers::IsMoving(layer) ? Layers::Moving : Layers::Static;
			}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
			char const* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
			{
				return layer == Layers::Moving ? "Moving" : "Static";
			}
#endif
		};

		class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
		{
		public:
			// Static bodies only meet moving ones.
			bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhaseLayer) const override
			{
				return Layers::IsMoving(layer) || broadPhaseLayer == Layers::Moving;
			}
		};

		class LayerPairFilter final : public JPH::ObjectLayerPairFilter
		{
		public:
			explicit LayerPairFilter(PhysicsWorldSettings const& settings)
			{
				for (std::array<bool, MaxPhysicsLayers>& row : m_Collides)
				{
					row.fill(true);
				}
				for (glm::uvec2 const& pair : settings.IgnoredCollisions)
				{
					if (pair.x < MaxPhysicsLayers && pair.y < MaxPhysicsLayers)
					{
						m_Collides[pair.x][pair.y] = false;
						m_Collides[pair.y][pair.x] = false;
					}
				}
			}

			bool ShouldCollide(JPH::ObjectLayer first, JPH::ObjectLayer second) const override
			{
				if (!Layers::IsMoving(first) && !Layers::IsMoving(second))
				{
					return false;
				}
				return m_Collides[Layers::GetLayer(first)][Layers::GetLayer(second)];
			}

		private:
			std::array<std::array<bool, MaxPhysicsLayers>, MaxPhysicsLayers> m_Collides{};
		};

		class LayerMaskFilter final : public JPH::ObjectLayerFilter
		{
		public:
			explicit LayerMaskFilter(uint32_t mask)
				: m_Mask(mask)
			{
			}

			bool ShouldCollide(JPH::ObjectLayer layer) const override { return ((m_Mask >> Layers::GetLayer(layer)) & 1u) != 0; }

		private:
			uint32_t m_Mask;
		};

		// Collider properties referenced by the user data of their shapes (index + 1; 0 = none).
		struct ColliderMaterial
		{
			bool IsTrigger = false;
			float Friction = 0.6f;
			float Restitution = 0.0f;
		};

		// Two bodies (Jolt reports them with the lower ID first) and whether they touch through a trigger collider.
		struct BodyPairKey
		{
			uint32_t First = 0;
			uint32_t Second = 0;
			bool Trigger = false;

			BodyPairKey(JPH::BodyID first, JPH::BodyID second, bool trigger)
				: First(first.GetIndexAndSequenceNumber()),
				  Second(second.GetIndexAndSequenceNumber()),
				  Trigger(trigger)
			{
			}

			bool operator==(BodyPairKey const& other) const = default;
		};

		struct BodyPairKeyHash
		{
			size_t operator()(BodyPairKey const& key) const
			{
				uint64_t const bodies = (static_cast<uint64_t>(key.First) << 32) | key.Second;
				return static_cast<size_t>(Hash::Combine(bodies, key.Trigger ? 1u : 0u));
			}
		};

		struct SubShapePairHash
		{
			size_t operator()(JPH::SubShapeIDPair const& pair) const { return static_cast<size_t>(pair.GetHash()); }
		};

		// Turns Jolt's per sub-shape contacts into enter/exit events per entity pair, applies per-collider friction and
		// restitution, and makes contacts with trigger colliders sensor contacts. Jolt calls it from its worker threads.
		class ContactCollector final : public JPH::ContactListener
		{
		public:
			explicit ContactCollector(std::vector<ColliderMaterial> const& materials)
				: m_Materials(materials)
			{
			}

			void OnContactAdded(JPH::Body const& first, JPH::Body const& second, JPH::ContactManifold const& manifold,
			                    JPH::ContactSettings& settings) override
			{
				ApplyMaterials(first, second, manifold, settings);
				JPH::SubShapeIDPair const pair(first.GetID(), manifold.mSubShapeID1, second.GetID(), manifold.mSubShapeID2);
				std::scoped_lock const lock(m_Mutex);
				m_ActivePairs[pair] = settings.mIsSensor;
				PairState& state = m_PairStates[BodyPairKey(first.GetID(), second.GetID(), settings.mIsSensor)];
				if (state.Count++ == 0)
				{
					state.First = UUID(first.GetUserData());
					state.Second = UUID(second.GetUserData());
					state.FirstBody = first.GetID();
					state.SecondBody = second.GetID();
					state.Trigger = settings.mIsSensor;
					m_Events.push_back({settings.mIsSensor ? ContactEventType::TriggerEnter : ContactEventType::CollisionEnter, state.First,
					                    state.Second});
				}
			}

			void OnContactPersisted(JPH::Body const& first, JPH::Body const& second, JPH::ContactManifold const& manifold,
			                        JPH::ContactSettings& settings) override
			{
				ApplyMaterials(first, second, manifold, settings);
			}

			void OnContactRemoved(JPH::SubShapeIDPair const& pair) override
			{
				std::scoped_lock const lock(m_Mutex);
				auto const active = m_ActivePairs.find(pair);
				if (active == m_ActivePairs.end())
				{
					return;
				}
				bool const trigger = active->second;
				m_ActivePairs.erase(active);
				auto const state = m_PairStates.find(BodyPairKey(pair.GetBody1ID(), pair.GetBody2ID(), trigger));
				if (state != m_PairStates.end() && --state->second.Count == 0)
				{
					m_Events.push_back({trigger ? ContactEventType::TriggerExit : ContactEventType::CollisionExit, state->second.First,
					                    state->second.Second});
					m_PairStates.erase(state);
				}
			}

			// Ends the contacts of a removed body with exit events (Jolt does not report them).
			void ForgetBody(JPH::BodyID body)
			{
				std::scoped_lock const lock(m_Mutex);
				std::erase_if(m_ActivePairs,
				              [body](auto const& entry)
				              {
								  return entry.first.GetBody1ID() == body || entry.first.GetBody2ID() == body;
							  });
				for (auto it = m_PairStates.begin(); it != m_PairStates.end();)
				{
					PairState const& state = it->second;
					if (state.FirstBody == body || state.SecondBody == body)
					{
						m_Events.push_back(
							{state.Trigger ? ContactEventType::TriggerExit : ContactEventType::CollisionExit, state.First, state.Second});
						it = m_PairStates.erase(it);
					}
					else
					{
						++it;
					}
				}
			}

			std::vector<ContactEvent> TakeEvents()
			{
				std::scoped_lock const lock(m_Mutex);
				return std::exchange(m_Events, {});
			}

		private:
			struct PairState
			{
				uint32_t Count = 0;
				UUID First = UUID::Invalid();
				UUID Second = UUID::Invalid();
				JPH::BodyID FirstBody;
				JPH::BodyID SecondBody;
				bool Trigger = false;
			};

			ColliderMaterial const* FindMaterial(JPH::Body const& body, JPH::SubShapeID const& subShape) const
			{
				uint64_t const index = body.GetShape()->GetSubShapeUserData(subShape);
				return index > 0 && index <= m_Materials.size() ? &m_Materials[index - 1] : nullptr;
			}

			void ApplyMaterials(JPH::Body const& first, JPH::Body const& second, JPH::ContactManifold const& manifold,
			                    JPH::ContactSettings& settings) const
			{
				ColliderMaterial const* a = FindMaterial(first, manifold.mSubShapeID1);
				ColliderMaterial const* b = FindMaterial(second, manifold.mSubShapeID2);
				settings.mIsSensor = (a != nullptr && a->IsTrigger) || (b != nullptr && b->IsTrigger);
				if (a != nullptr && b != nullptr)
				{
					settings.mCombinedFriction = std::sqrt(a->Friction * b->Friction);
					settings.mCombinedRestitution = std::max(a->Restitution, b->Restitution);
				}
			}

			// Read during steps (no changes while Jolt runs).
			std::vector<ColliderMaterial> const& m_Materials;
			std::mutex m_Mutex;
			// Sub-shape pairs in contact, and whether the contact is a trigger contact.
			std::unordered_map<JPH::SubShapeIDPair, bool, SubShapePairHash> m_ActivePairs;
			std::unordered_map<BodyPairKey, PairState, BodyPairKeyHash> m_PairStates;
			std::vector<ContactEvent> m_Events;
		};

		struct BodyRecord
		{
			JPH::BodyID Id;
			RigidBodyType Type = RigidBodyType::Static;
			// Indices into the material table, released with the body.
			std::vector<uint32_t> Materials;
		};

		JPH::EAllowedDOFs MakeAllowedDOFs(BodyDesc const& desc)
		{
			auto dofs = static_cast<JPH::EAllowedDOFs>(0);
			auto const allow = [&dofs](bool locked, JPH::EAllowedDOFs dof)
			{
				if (!locked)
				{
					dofs = static_cast<JPH::EAllowedDOFs>(static_cast<uint8_t>(dofs) | static_cast<uint8_t>(dof));
				}
			};
			allow(desc.LockTranslation.x, JPH::EAllowedDOFs::TranslationX);
			allow(desc.LockTranslation.y, JPH::EAllowedDOFs::TranslationY);
			allow(desc.LockTranslation.z, JPH::EAllowedDOFs::TranslationZ);
			allow(desc.LockRotation.x, JPH::EAllowedDOFs::RotationX);
			allow(desc.LockRotation.y, JPH::EAllowedDOFs::RotationY);
			allow(desc.LockRotation.z, JPH::EAllowedDOFs::RotationZ);
			return dofs;
		}

		Result<JPH::RefConst<JPH::Shape>> CreateMeshShape(ColliderDesc const& collider, glm::vec3 const& scale, bool convex,
		                                                  uint64_t userData)
		{
			if (!collider.Mesh || collider.Mesh->GetVertices().empty() || collider.Mesh->GetIndices().size() < 3)
			{
				return Error{"the mesh collider has no mesh"};
			}
			std::vector<Vertex> const& vertices = collider.Mesh->GetVertices();
			if (convex)
			{
				JPH::Array<JPH::Vec3> points;
				points.reserve(vertices.size());
				for (Vertex const& vertex : vertices)
				{
					points.push_back(Jolt::ToJolt(vertex.Position * scale));
				}
				JPH::ConvexHullShapeSettings settings(points);
				settings.mUserData = userData;
				JPH::ShapeSettings::ShapeResult result = settings.Create();
				if (result.HasError())
				{
					return MakeError("the convex hull cannot be built: {}", result.GetError().c_str());
				}
				return JPH::RefConst<JPH::Shape>(result.Get());
			}

			JPH::VertexList points;
			points.reserve(vertices.size());
			for (Vertex const& vertex : vertices)
			{
				glm::vec3 const position = vertex.Position * scale;
				points.push_back(JPH::Float3(position.x, position.y, position.z));
			}
			std::vector<uint32_t> const& indices = collider.Mesh->GetIndices();
			JPH::IndexedTriangleList triangles;
			triangles.reserve(indices.size() / 3);
			for (size_t i = 0; i + 2 < indices.size(); i += 3)
			{
				triangles.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1], indices[i + 2]));
			}
			JPH::MeshShapeSettings settings(std::move(points), std::move(triangles));
			settings.mUserData = userData;
			JPH::ShapeSettings::ShapeResult result = settings.Create();
			if (result.HasError())
			{
				return MakeError("the triangle mesh cannot be built: {}", result.GetError().c_str());
			}
			return JPH::RefConst<JPH::Shape>(result.Get());
		}

		// The collider's shape with the body's scale applied (mirroring is ignored: shapes are symmetric), without its offset.
		Result<JPH::RefConst<JPH::Shape>> CreateColliderShape(ColliderDesc const& collider, glm::vec3 const& scale, bool dynamic,
		                                                      uint64_t userData)
		{
			glm::vec3 const size = glm::abs(scale);
			JPH::ShapeSettings::ShapeResult result;
			switch (collider.Shape)
			{
				case ColliderShape::Box:
				{
					glm::vec3 const halfExtents = glm::max(collider.HalfExtents * size, glm::vec3(MinimumExtent));
					float const smallest = std::min({halfExtents.x, halfExtents.y, halfExtents.z});
					JPH::BoxShapeSettings settings(Jolt::ToJolt(halfExtents), std::min(JPH::cDefaultConvexRadius, 0.5f * smallest));
					settings.mUserData = userData;
					result = settings.Create();
					break;
				}
				case ColliderShape::Sphere:
				{
					JPH::SphereShapeSettings settings(std::max(collider.Radius * std::max({size.x, size.y, size.z}), MinimumExtent));
					settings.mUserData = userData;
					result = settings.Create();
					break;
				}
				case ColliderShape::Capsule:
				{
					float const radius = std::max(collider.Radius * std::max(size.x, size.z), MinimumExtent);
					float const halfHeight = collider.HalfHeight * size.y;
					if (halfHeight < MinimumExtent)
					{
						JPH::SphereShapeSettings settings(radius);
						settings.mUserData = userData;
						result = settings.Create();
						break;
					}
					JPH::CapsuleShapeSettings settings(halfHeight, radius);
					settings.mUserData = userData;
					result = settings.Create();
					break;
				}
				case ColliderShape::ConvexMesh:
					return CreateMeshShape(collider, scale, true, userData);
				case ColliderShape::TriangleMesh:
					// Jolt cannot simulate dynamic triangle meshes: they fall back to the convex hull.
					return CreateMeshShape(collider, scale, dynamic, userData);
			}
			if (result.HasError())
			{
				return MakeError("the shape cannot be built: {}", result.GetError().c_str());
			}
			return JPH::RefConst<JPH::Shape>(result.Get());
		}
	}

	struct PhysicsScene::Data
	{
		explicit Data(PhysicsWorldSettings const& settings)
			: LayerCount(std::clamp<uint32_t>(settings.LayerCount, 1, static_cast<uint32_t>(MaxPhysicsLayers))),
			  LayerPairs(settings),
			  Contacts(Materials)
		{
		}

		uint32_t LayerCount;
		BroadPhaseLayers BroadPhase;
		ObjectVsBroadPhaseFilter ObjectVsBroadPhase;
		LayerPairFilter LayerPairs;
		std::vector<ColliderMaterial> Materials;
		std::vector<uint32_t> FreeMaterials;
		ContactCollector Contacts;
		JPH::TempAllocatorImpl TempAllocator{TempAllocatorSize};
		// After the listeners and filters it references.
		JPH::PhysicsSystem System;
		std::unordered_map<UUID, BodyRecord> Bodies;
		bool OptimizeBroadPhase = false;

		BodyRecord const* Find(UUID entity) const
		{
			auto const it = Bodies.find(entity);
			return it != Bodies.end() ? &it->second : nullptr;
		}

		uint32_t AllocateMaterial(ColliderDesc const& collider)
		{
			ColliderMaterial const material{collider.IsTrigger, std::max(collider.Friction, 0.0f),
			                                std::clamp(collider.Restitution, 0.0f, 1.0f)};
			if (!FreeMaterials.empty())
			{
				uint32_t const index = FreeMaterials.back();
				FreeMaterials.pop_back();
				Materials[index] = material;
				return index;
			}
			Materials.push_back(material);
			return static_cast<uint32_t>(Materials.size() - 1);
		}
	};

	Result<Scope<PhysicsScene>> PhysicsScene::Create(PhysicsWorldSettings const& settings)
	{
		if (!PhysicsSystem::IsInitialized())
		{
			return Error{"physics is not initialized"};
		}
		Scope<Data> data = CreateScope<Data>(settings);
		data->System.Init(MaxBodies, 0, MaxBodyPairs, MaxContactConstraints, data->BroadPhase, data->ObjectVsBroadPhase, data->LayerPairs);
		data->System.SetContactListener(&data->Contacts);
		data->System.SetGravity(Jolt::ToJolt(settings.Gravity));
		return CreateScope<PhysicsScene>(PrivateTag{}, std::move(data));
	}

	PhysicsScene::PhysicsScene(PrivateTag, Scope<Data> data)
		: m_Data(std::move(data))
	{
	}

	PhysicsScene::~PhysicsScene()
	{
		JPH::BodyInterface& bodies = m_Data->System.GetBodyInterface();
		for (auto const& [entity, record] : m_Data->Bodies)
		{
			bodies.RemoveBody(record.Id);
			bodies.DestroyBody(record.Id);
		}
	}

	Result<void> PhysicsScene::AddBody(BodyDesc const& desc)
	{
		if (desc.Colliders.empty())
		{
			return MakeError("entity {} has no colliders", desc.Entity);
		}
		RemoveBody(desc.Entity);
		Data& data = *m_Data;

		RigidBodyType type = desc.Type;
		JPH::EAllowedDOFs const allowedDofs = MakeAllowedDOFs(desc);
		if (type == RigidBodyType::Dynamic && allowedDofs == static_cast<JPH::EAllowedDOFs>(0))
		{
			// Every degree of freedom locked: it cannot move by itself.
			type = RigidBodyType::Kinematic;
		}
		bool const dynamic = type == RigidBodyType::Dynamic;

		std::vector<uint32_t> materials;
		std::vector<std::pair<JPH::RefConst<JPH::Shape>, glm::vec3>> shapes;
		auto const releaseMaterials = [&data, &materials]
		{
			data.FreeMaterials.insert(data.FreeMaterials.end(), materials.begin(), materials.end());
		};
		for (ColliderDesc const& collider : desc.Colliders)
		{
			materials.push_back(data.AllocateMaterial(collider));
			Result<JPH::RefConst<JPH::Shape>> shape = CreateColliderShape(collider, desc.Scale, dynamic, materials.back() + 1);
			if (!shape)
			{
				releaseMaterials();
				return MakeError("entity {}: {}", desc.Entity, shape.GetError());
			}
			shapes.emplace_back(shape.TakeValue(), collider.Offset * desc.Scale);
		}

		JPH::RefConst<JPH::Shape> shape;
		if (shapes.size() == 1 && shapes.front().second == glm::vec3(0.0f))
		{
			shape = shapes.front().first;
		}
		else if (shapes.size() == 1)
		{
			JPH::RotatedTranslatedShapeSettings settings(Jolt::ToJolt(shapes.front().second), JPH::Quat::sIdentity(), shapes.front().first);
			settings.mUserData = materials.front() + 1;
			JPH::ShapeSettings::ShapeResult result = settings.Create();
			if (result.HasError())
			{
				releaseMaterials();
				return MakeError("entity {}: the collider offset cannot be applied: {}", desc.Entity, result.GetError().c_str());
			}
			shape = result.Get();
		}
		else
		{
			JPH::StaticCompoundShapeSettings settings;
			for (auto const& [colliderShape, offset] : shapes)
			{
				settings.AddShape(Jolt::ToJolt(offset), JPH::Quat::sIdentity(), colliderShape);
			}
			JPH::ShapeSettings::ShapeResult result = settings.Create();
			if (result.HasError())
			{
				releaseMaterials();
				return MakeError("entity {}: the colliders cannot be combined: {}", desc.Entity, result.GetError().c_str());
			}
			shape = result.Get();
		}

		uint32_t const layer = desc.Layer < data.LayerCount ? desc.Layer : 0u;
		JPH::EMotionType const motionType = type == RigidBodyType::Dynamic     ? JPH::EMotionType::Dynamic
		                                    : type == RigidBodyType::Kinematic ? JPH::EMotionType::Kinematic
		                                                                       : JPH::EMotionType::Static;
		JPH::BodyCreationSettings settings(shape, Jolt::ToJolt(desc.Position), Jolt::ToJolt(glm::normalize(desc.Rotation)), motionType,
		                                   Layers::Make(layer, type != RigidBodyType::Static));
		settings.mUserData = desc.Entity.GetValue();
		settings.mFriction = std::max(desc.Colliders.front().Friction, 0.0f);
		settings.mRestitution = std::clamp(desc.Colliders.front().Restitution, 0.0f, 1.0f);
		settings.mAllowSleeping = desc.AllowSleep;
		// Kinematic bodies report touching static and kinematic ones, so they can enter static triggers.
		settings.mCollideKinematicVsNonDynamic = type == RigidBodyType::Kinematic;
		if (dynamic)
		{
			settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
			settings.mMassPropertiesOverride.mMass = std::max(desc.Mass, 1e-4f);
			settings.mLinearDamping = std::max(desc.LinearDamping, 0.0f);
			settings.mAngularDamping = std::max(desc.AngularDamping, 0.0f);
			settings.mGravityFactor = desc.GravityFactor;
			settings.mAllowedDOFs = allowedDofs;
			settings.mMotionQuality = desc.ContinuousCollision ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
		}
		if (type != RigidBodyType::Static)
		{
			settings.mLinearVelocity = Jolt::ToJolt(desc.LinearVelocity);
			settings.mAngularVelocity = Jolt::ToJolt(desc.AngularVelocity);
		}

		JPH::BodyInterface& bodies = data.System.GetBodyInterface();
		JPH::Body* body = bodies.CreateBody(settings);
		if (body == nullptr)
		{
			releaseMaterials();
			return MakeError("entity {}: the physics world is full ({} bodies)", desc.Entity, MaxBodies);
		}
		bodies.AddBody(body->GetID(), type == RigidBodyType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
		data.Bodies[desc.Entity] = BodyRecord{body->GetID(), type, std::move(materials)};
		data.OptimizeBroadPhase = true;
		return {};
	}

	void PhysicsScene::RemoveBody(UUID entity)
	{
		Data& data = *m_Data;
		auto const it = data.Bodies.find(entity);
		if (it == data.Bodies.end())
		{
			return;
		}
		JPH::BodyInterface& bodies = data.System.GetBodyInterface();
		bodies.RemoveBody(it->second.Id);
		bodies.DestroyBody(it->second.Id);
		data.Contacts.ForgetBody(it->second.Id);
		data.FreeMaterials.insert(data.FreeMaterials.end(), it->second.Materials.begin(), it->second.Materials.end());
		data.Bodies.erase(it);
	}

	bool PhysicsScene::HasBody(UUID entity) const
	{
		return m_Data->Bodies.contains(entity);
	}

	size_t PhysicsScene::GetBodyCount() const
	{
		return m_Data->Bodies.size();
	}

	void PhysicsScene::Step(float deltaTime)
	{
		Data& data = *m_Data;
		if (deltaTime <= 0.0f)
		{
			return;
		}
		if (data.OptimizeBroadPhase)
		{
			data.System.OptimizeBroadPhase();
			data.OptimizeBroadPhase = false;
		}
		JPH::EPhysicsUpdateError const error = data.System.Update(deltaTime, 1, &data.TempAllocator, &Jolt::GetJobSystem());
		if (error != JPH::EPhysicsUpdateError::None)
		{
			ST_CORE_WARN("The physics step ran out of space (error flags {:#x}); some contacts were dropped", static_cast<uint32_t>(error));
		}
	}

	std::vector<BodyTransform> PhysicsScene::GetActiveBodyTransforms() const
	{
		Data const& data = *m_Data;
		JPH::BodyIDVector active;
		data.System.GetActiveBodies(JPH::EBodyType::RigidBody, active);
		std::vector<BodyTransform> transforms;
		transforms.reserve(active.size());
		JPH::BodyLockInterfaceLocking const& locks = data.System.GetBodyLockInterface();
		for (JPH::BodyID const id : active)
		{
			JPH::BodyLockRead const lock(locks, id);
			if (lock.Succeeded() && lock.GetBody().IsDynamic())
			{
				JPH::Body const& body = lock.GetBody();
				transforms.push_back({UUID(body.GetUserData()), Jolt::ToGlm(body.GetPosition()), Jolt::ToGlm(body.GetRotation())});
			}
		}
		// Deterministic order regardless of Jolt's internal bookkeeping.
		std::sort(transforms.begin(), transforms.end(),
		          [](BodyTransform const& a, BodyTransform const& b)
		          {
					  return a.Entity.GetValue() < b.Entity.GetValue();
				  });
		return transforms;
	}

	std::vector<ContactEvent> PhysicsScene::TakeContactEvents()
	{
		return m_Data->Contacts.TakeEvents();
	}

	glm::vec3 PhysicsScene::GetGravity() const
	{
		return Jolt::ToGlm(m_Data->System.GetGravity());
	}

	void PhysicsScene::SetGravity(glm::vec3 const& gravity)
	{
		m_Data->System.SetGravity(Jolt::ToJolt(gravity));
	}

	std::optional<BodyTransform> PhysicsScene::GetBodyTransform(UUID entity) const
	{
		BodyRecord const* record = m_Data->Find(entity);
		if (record == nullptr)
		{
			return std::nullopt;
		}
		JPH::RVec3 position;
		JPH::Quat rotation;
		m_Data->System.GetBodyInterface().GetPositionAndRotation(record->Id, position, rotation);
		return BodyTransform{entity, Jolt::ToGlm(position), Jolt::ToGlm(rotation)};
	}

	void PhysicsScene::SetBodyTransform(UUID entity, glm::vec3 const& position, glm::quat const& rotation)
	{
		if (BodyRecord const* record = m_Data->Find(entity))
		{
			JPH::EActivation const activation =
				record->Type == RigidBodyType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate;
			m_Data->System.GetBodyInterface().SetPositionAndRotation(record->Id, Jolt::ToJolt(position),
			                                                         Jolt::ToJolt(glm::normalize(rotation)), activation);
		}
	}

	void PhysicsScene::MoveKinematic(UUID entity, glm::vec3 const& position, glm::quat const& rotation, float deltaTime)
	{
		BodyRecord const* record = m_Data->Find(entity);
		if (record != nullptr && record->Type == RigidBodyType::Kinematic && deltaTime > 0.0f)
		{
			m_Data->System.GetBodyInterface().MoveKinematic(record->Id, Jolt::ToJolt(position), Jolt::ToJolt(glm::normalize(rotation)),
			                                                deltaTime);
		}
	}

	glm::vec3 PhysicsScene::GetLinearVelocity(UUID entity) const
	{
		BodyRecord const* record = m_Data->Find(entity);
		return record != nullptr ? Jolt::ToGlm(m_Data->System.GetBodyInterface().GetLinearVelocity(record->Id)) : glm::vec3(0.0f);
	}

	void PhysicsScene::SetLinearVelocity(UUID entity, glm::vec3 const& velocity)
	{
		BodyRecord const* record = m_Data->Find(entity);
		if (record != nullptr && record->Type != RigidBodyType::Static)
		{
			m_Data->System.GetBodyInterface().SetLinearVelocity(record->Id, Jolt::ToJolt(velocity));
			m_Data->System.GetBodyInterface().ActivateBody(record->Id);
		}
	}

	glm::vec3 PhysicsScene::GetAngularVelocity(UUID entity) const
	{
		BodyRecord const* record = m_Data->Find(entity);
		return record != nullptr ? Jolt::ToGlm(m_Data->System.GetBodyInterface().GetAngularVelocity(record->Id)) : glm::vec3(0.0f);
	}

	void PhysicsScene::SetAngularVelocity(UUID entity, glm::vec3 const& velocity)
	{
		BodyRecord const* record = m_Data->Find(entity);
		if (record != nullptr && record->Type != RigidBodyType::Static)
		{
			m_Data->System.GetBodyInterface().SetAngularVelocity(record->Id, Jolt::ToJolt(velocity));
			m_Data->System.GetBodyInterface().ActivateBody(record->Id);
		}
	}

	void PhysicsScene::AddForce(UUID entity, glm::vec3 const& force, ForceMode mode)
	{
		BodyRecord const* record = m_Data->Find(entity);
		if (record == nullptr || record->Type != RigidBodyType::Dynamic)
		{
			return;
		}
		JPH::BodyInterface& bodies = m_Data->System.GetBodyInterface();
		float const mass = GetMass(entity);
		switch (mode)
		{
			case ForceMode::Force:
				bodies.AddForce(record->Id, Jolt::ToJolt(force));
				break;
			case ForceMode::Impulse:
				bodies.AddImpulse(record->Id, Jolt::ToJolt(force));
				break;
			case ForceMode::Acceleration:
				bodies.AddForce(record->Id, Jolt::ToJolt(force * mass));
				break;
			case ForceMode::VelocityChange:
				bodies.AddImpulse(record->Id, Jolt::ToJolt(force * mass));
				break;
		}
		bodies.ActivateBody(record->Id);
	}

	void PhysicsScene::AddTorque(UUID entity, glm::vec3 const& torque, ForceMode mode)
	{
		BodyRecord const* record = m_Data->Find(entity);
		if (record == nullptr || record->Type != RigidBodyType::Dynamic)
		{
			return;
		}
		// Acceleration and velocity-change modes scale by the world-space inertia (locked axes have none).
		glm::vec3 scaled = torque;
		if (mode == ForceMode::Acceleration || mode == ForceMode::VelocityChange)
		{
			JPH::BodyLockRead const lock(m_Data->System.GetBodyLockInterface(), record->Id);
			if (!lock.Succeeded())
			{
				return;
			}
			JPH::Body const& body = lock.GetBody();
			JPH::MotionProperties const* motion = body.GetMotionProperties();
			glm::quat const inertiaRotation = Jolt::ToGlm(body.GetRotation() * motion->GetInertiaRotation());
			glm::vec3 const local = glm::inverse(inertiaRotation) * torque;
			glm::vec3 const inverseInertia = Jolt::ToGlm(motion->GetInverseInertiaDiagonal());
			glm::vec3 inertial(0.0f);
			for (int axis = 0; axis < 3; axis++)
			{
				inertial[axis] = inverseInertia[axis] > 0.0f ? local[axis] / inverseInertia[axis] : 0.0f;
			}
			scaled = inertiaRotation * inertial;
		}
		JPH::BodyInterface& bodies = m_Data->System.GetBodyInterface();
		if (mode == ForceMode::Force || mode == ForceMode::Acceleration)
		{
			bodies.AddTorque(record->Id, Jolt::ToJolt(scaled));
		}
		else
		{
			bodies.AddAngularImpulse(record->Id, Jolt::ToJolt(scaled));
		}
		bodies.ActivateBody(record->Id);
	}

	float PhysicsScene::GetMass(UUID entity) const
	{
		BodyRecord const* record = m_Data->Find(entity);
		if (record == nullptr || record->Type != RigidBodyType::Dynamic)
		{
			return 0.0f;
		}
		JPH::BodyLockRead const lock(m_Data->System.GetBodyLockInterface(), record->Id);
		if (!lock.Succeeded())
		{
			return 0.0f;
		}
		float const inverseMass = lock.GetBody().GetMotionProperties()->GetInverseMass();
		return inverseMass > 0.0f ? 1.0f / inverseMass : 0.0f;
	}

	void PhysicsScene::SetGravityFactor(UUID entity, float factor)
	{
		BodyRecord const* record = m_Data->Find(entity);
		if (record != nullptr && record->Type == RigidBodyType::Dynamic)
		{
			m_Data->System.GetBodyInterface().SetGravityFactor(record->Id, factor);
		}
	}

	void PhysicsScene::SetDamping(UUID entity, float linear, float angular)
	{
		BodyRecord const* record = m_Data->Find(entity);
		if (record == nullptr || record->Type != RigidBodyType::Dynamic)
		{
			return;
		}
		JPH::BodyLockWrite const lock(m_Data->System.GetBodyLockInterface(), record->Id);
		if (lock.Succeeded())
		{
			JPH::MotionProperties* motion = lock.GetBody().GetMotionProperties();
			motion->SetLinearDamping(std::max(linear, 0.0f));
			motion->SetAngularDamping(std::max(angular, 0.0f));
		}
	}

	bool PhysicsScene::IsSleeping(UUID entity) const
	{
		BodyRecord const* record = m_Data->Find(entity);
		return record != nullptr && record->Type != RigidBodyType::Static && !m_Data->System.GetBodyInterface().IsActive(record->Id);
	}

	void PhysicsScene::WakeUp(UUID entity)
	{
		BodyRecord const* record = m_Data->Find(entity);
		if (record != nullptr && record->Type != RigidBodyType::Static)
		{
			m_Data->System.GetBodyInterface().ActivateBody(record->Id);
		}
	}

	std::optional<RaycastHit> PhysicsScene::Raycast(glm::vec3 const& origin, glm::vec3 const& direction, float maxDistance,
	                                                uint32_t layerMask) const
	{
		float const length = glm::length(direction);
		if (!(length > 0.0f) || !(maxDistance > 0.0f) || !std::isfinite(maxDistance))
		{
			return std::nullopt;
		}
		glm::vec3 const unit = direction / length;
		JPH::RRayCast const ray(Jolt::ToJolt(origin), Jolt::ToJolt(unit * maxDistance));
		// Rays starting inside a collider ignore it (like its back faces).
		JPH::RayCastSettings settings;
		settings.mTreatConvexAsSolid = false;
		JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
		m_Data->System.GetNarrowPhaseQuery().CastRay(ray, settings, collector, {}, LayerMaskFilter(layerMask));
		collector.Sort();

		JPH::BodyLockInterfaceLocking const& locks = m_Data->System.GetBodyLockInterface();
		for (JPH::RayCastResult const& hit : collector.mHits)
		{
			JPH::BodyLockRead const lock(locks, hit.mBodyID);
			if (!lock.Succeeded())
			{
				continue;
			}
			JPH::Body const& body = lock.GetBody();
			uint64_t const material = body.GetShape()->GetSubShapeUserData(hit.mSubShapeID2);
			if (material > 0 && material <= m_Data->Materials.size() && m_Data->Materials[material - 1].IsTrigger)
			{
				continue;
			}
			JPH::RVec3 const point = ray.GetPointOnRay(hit.mFraction);
			RaycastHit result;
			result.Entity = UUID(body.GetUserData());
			result.Point = Jolt::ToGlm(point);
			result.Normal = Jolt::ToGlm(body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point));
			result.Distance = hit.mFraction * maxDistance;
			return result;
		}
		return std::nullopt;
	}
}
