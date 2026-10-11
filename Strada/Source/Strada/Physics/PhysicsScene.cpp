#include "stpch.h"
#include "Strada/Physics/PhysicsScene.h"

#include "Strada/Asset/MeshSource.h"
#include "Strada/Core/Hash.h"
#include "Strada/Math/Math.h"
#include "Strada/Physics/JoltContext.h"
#include "Strada/Physics/PhysicsSystem.h"

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
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
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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
		// How far characters keep from what they touch (Jolt's default), which makes their sweeps cheap and robust.
		constexpr float CharacterPadding = 0.02f;

		bool IsFinite(glm::vec3 const& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		bool IsFinite(glm::quat const& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
		}

		// Whether every component's magnitude is within MaxPhysicsExtent (false for NaN).
		bool FitsExtent(glm::vec3 const& value)
		{
			glm::vec3 const magnitude = glm::abs(value);
			return magnitude.x <= MaxPhysicsExtent && magnitude.y <= MaxPhysicsExtent && magnitude.z <= MaxPhysicsExtent;
		}

		glm::vec3 ClampPosition(glm::vec3 const& position)
		{
			return glm::clamp(position, glm::vec3(-MaxPhysicsCoordinate), glm::vec3(MaxPhysicsCoordinate));
		}

		// Shortens a finite vector to at most maxLength; its length is computed without overflowing.
		glm::vec3 ClampLength(glm::vec3 const& vector, float maxLength)
		{
			float const largest = std::max({std::abs(vector.x), std::abs(vector.y), std::abs(vector.z)});
			if (largest == 0.0f)
			{
				return vector;
			}
			glm::vec3 const scaled = vector / largest;
			float const scaledLength = glm::length(scaled);
			return largest * scaledLength <= maxLength ? vector : scaled * (maxLength / scaledLength);
		}

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
		//
		// Jolt removes the contacts of bodies that fall asleep (and finds them again when they wake up). Those contacts
		// persist here: a removed contact whose bodies are all inactive after the step goes dormant instead of ending,
		// resumes without a new enter event when Jolt finds it again, and ends once its bodies are active without it.
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
				if (m_DormantPairs.erase(pair) > 0)
				{
					return;
				}
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

			// The bodies cannot be read here (to tell sleep from separation): FinishStep decides.
			void OnContactRemoved(JPH::SubShapeIDPair const& pair) override
			{
				std::scoped_lock const lock(m_Mutex);
				m_RemovedPairs.push_back(pair);
			}

			// After each step, on the main thread.
			void FinishStep(JPH::BodyInterface const& bodies)
			{
				std::scoped_lock const lock(m_Mutex);
				for (JPH::SubShapeIDPair const& pair : m_RemovedPairs)
				{
					auto const active = m_ActivePairs.find(pair);
					if (active == m_ActivePairs.end())
					{
						continue;
					}
					bool const trigger = active->second;
					m_ActivePairs.erase(active);
					if (IsAsleep(bodies, pair))
					{
						m_DormantPairs.emplace(pair, trigger);
					}
					else
					{
						Release(pair, trigger);
					}
				}
				m_RemovedPairs.clear();

				// Awake again without Jolt finding the contact: the bodies have moved apart.
				for (auto dormant = m_DormantPairs.begin(); dormant != m_DormantPairs.end();)
				{
					if (IsAsleep(bodies, dormant->first))
					{
						++dormant;
						continue;
					}
					Release(dormant->first, dormant->second);
					dormant = m_DormantPairs.erase(dormant);
				}

				// The events of a step arrive in thread scheduling order; they are simultaneous, so they are sorted for an
				// order that is the same on every run.
				SortEvents(m_StepEventsBegin);
				m_StepEventsBegin = m_Events.size();
			}

			// Ends the contacts of a removed body with exit events (Jolt does not report them).
			void ForgetBody(JPH::BodyID body)
			{
				std::scoped_lock const lock(m_Mutex);
				auto const involvesBody = [body](JPH::SubShapeIDPair const& pair)
				{
					return pair.GetBody1ID() == body || pair.GetBody2ID() == body;
				};
				std::erase_if(m_ActivePairs,
				              [&involvesBody](auto const& entry)
				              {
								  return involvesBody(entry.first);
							  });
				std::erase_if(m_DormantPairs,
				              [&involvesBody](auto const& entry)
				              {
								  return involvesBody(entry.first);
							  });
				std::erase_if(m_RemovedPairs, involvesBody);
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
				// Before the next step's events, in a fixed order (the map's order is not).
				SortEvents(m_StepEventsBegin);
				m_StepEventsBegin = m_Events.size();
			}

			std::vector<ContactEvent> TakeEvents()
			{
				std::scoped_lock const lock(m_Mutex);
				m_StepEventsBegin = 0;
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

			void SortEvents(size_t begin)
			{
				std::sort(m_Events.begin() + static_cast<ptrdiff_t>(begin), m_Events.end(),
				          [](ContactEvent const& a, ContactEvent const& b)
				          {
							  return std::tuple(a.First.GetValue(), a.Second.GetValue(), a.Type) <
					                 std::tuple(b.First.GetValue(), b.Second.GetValue(), b.Type);
						  });
			}

			// Static bodies are never active: a pair is asleep when none of its bodies is.
			static bool IsAsleep(JPH::BodyInterface const& bodies, JPH::SubShapeIDPair const& pair)
			{
				return !bodies.IsActive(pair.GetBody1ID()) && !bodies.IsActive(pair.GetBody2ID());
			}

			// Ends one sub-shape contact; the entity pair's contact ends with its last one.
			void Release(JPH::SubShapeIDPair const& pair, bool trigger)
			{
				auto const state = m_PairStates.find(BodyPairKey(pair.GetBody1ID(), pair.GetBody2ID(), trigger));
				if (state != m_PairStates.end() && --state->second.Count == 0)
				{
					m_Events.push_back({trigger ? ContactEventType::TriggerExit : ContactEventType::CollisionExit, state->second.First,
					                    state->second.Second});
					m_PairStates.erase(state);
				}
			}

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
			// Contacts of sleeping bodies, which Jolt no longer reports.
			std::unordered_map<JPH::SubShapeIDPair, bool, SubShapePairHash> m_DormantPairs;
			// Removed during the current step, classified by FinishStep.
			std::vector<JPH::SubShapeIDPair> m_RemovedPairs;
			// Every sub-shape contact (active and dormant) counts towards its entity pair.
			std::unordered_map<BodyPairKey, PairState, BodyPairKeyHash> m_PairStates;
			std::vector<ContactEvent> m_Events;
			// Where the events of the current step start (earlier ones are sorted).
			size_t m_StepEventsBegin = 0;
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
			for (Vertex const& vertex : vertices)
			{
				if (!FitsExtent(vertex.Position * scale))
				{
					return MakeError("the mesh collider is larger than the physics world allows ({} m)", MaxPhysicsExtent);
				}
			}
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
			// Submesh indices are relative to the submesh's first vertex (MeshSource validated the ranges).
			std::vector<uint32_t> const& indices = collider.Mesh->GetIndices();
			JPH::IndexedTriangleList triangles;
			triangles.reserve(indices.size() / 3);
			for (Submesh const& submesh : collider.Mesh->GetSubmeshes())
			{
				for (uint32_t i = 0; i + 2 < submesh.IndexCount; i += 3)
				{
					uint32_t const* triangle = indices.data() + submesh.BaseIndex + i;
					triangles.push_back(JPH::IndexedTriangle(submesh.BaseVertex + triangle[0], submesh.BaseVertex + triangle[1],
					                                         submesh.BaseVertex + triangle[2]));
				}
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
			auto const tooLarge = []
			{
				return MakeError("the collider is larger than the physics world allows ({} m)", MaxPhysicsExtent);
			};
			JPH::ShapeSettings::ShapeResult result;
			switch (collider.Shape)
			{
				case ColliderShape::Box:
				{
					glm::vec3 const halfExtents = glm::max(collider.HalfExtents * size, glm::vec3(MinimumExtent));
					if (!FitsExtent(halfExtents))
					{
						return tooLarge();
					}
					float const smallest = std::min({halfExtents.x, halfExtents.y, halfExtents.z});
					JPH::BoxShapeSettings settings(Jolt::ToJolt(halfExtents), std::min(JPH::cDefaultConvexRadius, 0.5f * smallest));
					settings.mUserData = userData;
					result = settings.Create();
					break;
				}
				case ColliderShape::Sphere:
				{
					float const radius = std::max(collider.Radius * std::max({size.x, size.y, size.z}), MinimumExtent);
					if (!(radius <= MaxPhysicsExtent))
					{
						return tooLarge();
					}
					JPH::SphereShapeSettings settings(radius);
					settings.mUserData = userData;
					result = settings.Create();
					break;
				}
				case ColliderShape::Capsule:
				{
					float const radius = std::max(collider.Radius * std::max(size.x, size.z), MinimumExtent);
					float const halfHeight = collider.HalfHeight * size.y;
					if (!(radius <= MaxPhysicsExtent) || !(halfHeight <= MaxPhysicsExtent))
					{
						return tooLarge();
					}
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

		// Lets character queries pass through trigger colliders (the user data of a collider's shapes indexes its material).
		class SolidShapeFilter final : public JPH::ShapeFilter
		{
		public:
			explicit SolidShapeFilter(std::vector<ColliderMaterial> const& materials)
				: m_Materials(materials)
			{
			}

			bool ShouldCollide(JPH::Shape const* shape, JPH::SubShapeID const&) const override { return !IsTrigger(*shape); }

			bool ShouldCollide(JPH::Shape const*, JPH::SubShapeID const&, JPH::Shape const* shape, JPH::SubShapeID const&) const override
			{
				return !IsTrigger(*shape);
			}

		private:
			bool IsTrigger(JPH::Shape const& shape) const
			{
				uint64_t const index = shape.GetUserData();
				return index > 0 && index <= m_Materials.size() && m_Materials[index - 1].IsTrigger;
			}

			std::vector<ColliderMaterial> const& m_Materials;
		};

		// What a character collides with: the bodies of the layers its layer collides with, except trigger colliders.
		struct CharacterFilters
		{
			CharacterFilters(JPH::PhysicsSystem const& system, uint32_t layer, std::vector<ColliderMaterial> const& materials)
				: BroadPhaseFilter(system.GetDefaultBroadPhaseLayerFilter(Layers::Make(layer, true))),
				  LayerFilter(system.GetDefaultLayerFilter(Layers::Make(layer, true))),
				  ShapeFilter(materials)
			{
			}

			JPH::DefaultBroadPhaseLayerFilter BroadPhaseFilter;
			JPH::DefaultObjectLayerFilter LayerFilter;
			JPH::BodyFilter BodyFilter;
			SolidShapeFilter ShapeFilter;
		};

		struct CharacterRecord
		{
			JPH::Ref<JPH::CharacterVirtual> Character;
			uint32_t Layer = 0;
			// The capsule's size, to tell when its shape changes.
			float Radius = 0.0f;
			float HalfCylinder = 0.0f;
			float StepHeight = 0.0f;
			float GravityFactor = 1.0f;
			// Its own velocity, and the walking velocity of the next step.
			glm::vec3 Velocity = glm::vec3(0.0f);
			glm::vec3 Walk = glm::vec3(0.0f);
		};

		// A character's capsule (a sphere when it has no cylinder part), centered on the character's position.
		Result<JPH::RefConst<JPH::Shape>> CreateCharacterShape(float radius, float halfCylinder)
		{
			JPH::ShapeSettings::ShapeResult result;
			if (halfCylinder < MinimumExtent)
			{
				JPH::SphereShapeSettings const settings(radius);
				result = settings.Create();
			}
			else
			{
				JPH::CapsuleShapeSettings const settings(halfCylinder, radius);
				result = settings.Create();
			}
			if (result.HasError())
			{
				return MakeError("the capsule cannot be built: {}", result.GetError().c_str());
			}
			return JPH::RefConst<JPH::Shape>(result.Get());
		}

		// Contacts below the center of the capsule's lower cap carry the character; higher ones only block it.
		JPH::Plane MakeSupportingVolume(float halfCylinder)
		{
			return JPH::Plane(JPH::Vec3::sAxisY(), halfCylinder);
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
		struct JointRecord
		{
			JPH::Ref<JPH::TwoBodyConstraint> Constraint;
			UUID ConnectedEntity = UUID::Invalid();
		};
		std::unordered_map<UUID, JointRecord> Joints;
		// Ordered by entity: characters move one after the other, in the same order on every run. After the system, whose
		// bodies their destructors remove.
		std::map<UUID, CharacterRecord> Characters;
		bool OptimizeBroadPhase = false;

		BodyRecord const* Find(UUID entity) const
		{
			auto const it = Bodies.find(entity);
			return it != Bodies.end() ? &it->second : nullptr;
		}

		CharacterRecord* FindCharacter(UUID entity)
		{
			auto const it = Characters.find(entity);
			return it != Characters.end() ? &it->second : nullptr;
		}

		// Finds what the character touches where it is, and what it stands on.
		void RefreshContacts(CharacterRecord& record)
		{
			CharacterFilters const filters(System, record.Layer, Materials);
			record.Character->RefreshContacts(filters.BroadPhaseFilter, filters.LayerFilter, filters.BodyFilter, filters.ShapeFilter,
			                                  TempAllocator);
		}

		// One step of a character: it walks (carried by the ground it stands on), falls, climbs steps and keeps to the
		// ground when it walks down slopes and steps.
		void StepCharacter(CharacterRecord& record, float deltaTime)
		{
			JPH::CharacterVirtual& character = *record.Character;
			glm::vec3 const up(0.0f, 1.0f, 0.0f);
			glm::vec3 const gravity = Jolt::ToGlm(System.GetGravity()) * record.GravityFactor;
			glm::vec3 velocity = record.Velocity;
			glm::vec3 carried(0.0f);
			glm::vec3 fall = gravity;
			if (character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround)
			{
				glm::vec3 const ground = Jolt::ToGlm(character.GetGroundVelocity());
				// Rounding leaves a standing character's velocity within a hair of zero; jumps are far faster.
				if (glm::dot(velocity, up) > 1e-3f)
				{
					// Jumping off: it keeps the ground's velocity.
					velocity += ground;
				}
				else
				{
					// Standing: its feet stop it, and the ground carries it. Gravity only presses it against the ground, so
					// it keeps in touch with it without creeping down walkable slopes.
					glm::vec3 const normal = Jolt::ToGlm(character.GetGroundNormal());
					velocity = glm::vec3(0.0f);
					carried = ground;
					fall = glm::dot(gravity, normal) * normal;
				}
			}
			velocity = ClampLength(velocity + fall * deltaTime, MaxCharacterSpeed);
			character.SetLinearVelocity(Jolt::ToJolt(ClampLength(velocity + record.Walk + carried, MaxCharacterSpeed)));

			JPH::CharacterVirtual::ExtendedUpdateSettings settings;
			settings.mStickToFloorStepDown = JPH::Vec3(0.0f, -record.StepHeight, 0.0f);
			settings.mWalkStairsStepUp = JPH::Vec3(0.0f, record.StepHeight, 0.0f);
			CharacterFilters const filters(System, record.Layer, Materials);
			character.ExtendedUpdate(deltaTime, Jolt::ToJolt(gravity), settings, filters.BroadPhaseFilter, filters.LayerFilter,
			                         filters.BodyFilter, filters.ShapeFilter, TempAllocator);

			// Its own velocity loses what moves it into the surfaces it hit: it lands, stops under ceilings and slides along
			// walls and down slopes too steep to stand on. Only its own motion is removed (moving surfaces push it, they do
			// not launch it).
			for (JPH::CharacterContact const& contact : character.GetActiveContacts())
			{
				if (!contact.mHadCollision || contact.mWasDiscarded)
				{
					continue;
				}
				glm::vec3 const normal = Jolt::ToGlm(contact.mContactNormal);
				float const into = glm::dot(velocity, normal);
				if (into < 0.0f)
				{
					velocity -= into * normal;
				}
			}
			record.Velocity = velocity;
			record.Walk = glm::vec3(0.0f);
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
		data->System.SetGravity(
			Jolt::ToJolt(IsFinite(settings.Gravity) ? ClampLength(settings.Gravity, MaxPhysicsGravity) : glm::vec3(0.0f)));
		return CreateScope<PhysicsScene>(PrivateTag{}, std::move(data));
	}

	PhysicsScene::PhysicsScene(PrivateTag, Scope<Data> data)
		: m_Data(std::move(data))
	{
	}

	PhysicsScene::~PhysicsScene()
	{
		// Constraints go before the bodies they join.
		for (auto const& [entity, joint] : m_Data->Joints)
		{
			m_Data->System.RemoveConstraint(joint.Constraint);
		}
		m_Data->Joints.clear();
		// Characters remove their inner bodies.
		m_Data->Characters.clear();
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
		// Like every failure below, a refused description leaves the entity without a body. An entity has a body or a
		// character.
		RemoveBody(desc.Entity);
		RemoveCharacter(desc.Entity);
		if (!IsFinite(desc.Position) || !IsFinite(desc.Rotation) || !IsFinite(desc.Scale) || !IsFinite(desc.LinearVelocity) ||
		    !IsFinite(desc.AngularVelocity))
		{
			return MakeError("entity {} has a transform or velocity that is not finite", desc.Entity);
		}
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
			glm::vec3 const offset = collider.Offset * desc.Scale;
			if (!FitsExtent(offset))
			{
				releaseMaterials();
				return MakeError("entity {}: a collider's offset is larger than the physics world allows ({} m)", desc.Entity,
				                 MaxPhysicsExtent);
			}
			materials.push_back(data.AllocateMaterial(collider));
			Result<JPH::RefConst<JPH::Shape>> shape = CreateColliderShape(collider, desc.Scale, dynamic, materials.back() + 1);
			if (!shape)
			{
				releaseMaterials();
				return MakeError("entity {}: {}", desc.Entity, shape.GetError());
			}
			shapes.emplace_back(shape.TakeValue(), offset);
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
		glm::vec3 const position = ClampPosition(desc.Position);
		if (position != desc.Position)
		{
			ST_CORE_WARN("Physics: entity {} is farther from the origin than the physics world allows ({} m); its body stays at the edge",
			             desc.Entity, MaxPhysicsCoordinate);
		}
		JPH::BodyCreationSettings settings(shape, Jolt::ToJolt(position), Jolt::ToJolt(Math::NormalizeRotation(desc.Rotation)), motionType,
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
			settings.mMassPropertiesOverride.mMass = std::clamp(desc.Mass, MinPhysicsMass, MaxPhysicsMass);
			settings.mLinearDamping = std::max(desc.LinearDamping, 0.0f);
			settings.mAngularDamping = std::max(desc.AngularDamping, 0.0f);
			settings.mGravityFactor = std::clamp(desc.GravityFactor, -MaxPhysicsGravityFactor, MaxPhysicsGravityFactor);
			settings.mAllowedDOFs = allowedDofs;
			settings.mMotionQuality = desc.ContinuousCollision ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
		}
		JPH::BodyInterface& bodies = data.System.GetBodyInterface();
		JPH::Body* body = bodies.CreateBody(settings);
		if (body == nullptr)
		{
			releaseMaterials();
			return MakeError("entity {}: the physics world is full ({} bodies)", desc.Entity, MaxBodies);
		}
		if (type != RigidBodyType::Static)
		{
			// Not through the creation settings: Jolt asserts that those are within the body's limits, which clamping
			// cannot guarantee to the last bit, while these setters clamp. Jolt clamps by the squared length, which
			// overflows for huge values, so those are shortened first.
			body->SetLinearVelocityClamped(Jolt::ToJolt(ClampLength(desc.LinearVelocity, settings.mMaxLinearVelocity)));
			body->SetAngularVelocityClamped(Jolt::ToJolt(ClampLength(desc.AngularVelocity, settings.mMaxAngularVelocity)));
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
		// The body's joints go first, whichever side of them it is on.
		for (auto joint = data.Joints.begin(); joint != data.Joints.end();)
		{
			if (joint->first == entity || joint->second.ConnectedEntity == entity)
			{
				data.System.RemoveConstraint(joint->second.Constraint);
				joint = data.Joints.erase(joint);
			}
			else
			{
				++joint;
			}
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

	Result<void> PhysicsScene::AddJoint(JointDesc const& desc)
	{
		RemoveJoint(desc.Entity);
		Data& data = *m_Data;
		BodyRecord const* const body = data.Find(desc.Entity);
		if (body == nullptr)
		{
			return MakeError("entity {} has no body to join", desc.Entity);
		}
		JPH::BodyID connected;
		if (desc.ConnectedEntity.IsValid())
		{
			if (desc.ConnectedEntity == desc.Entity)
			{
				return MakeError("entity {} cannot be joined to itself", desc.Entity);
			}
			BodyRecord const* const other = data.Find(desc.ConnectedEntity);
			if (other == nullptr)
			{
				return MakeError("entity {} is joined to entity {}, which has no body", desc.Entity, desc.ConnectedEntity);
			}
			connected = other->Id;
		}
		float const values[] = {desc.MinLimit, desc.MaxLimit, desc.MinDistance, desc.MaxDistance, desc.MotorSpeed, desc.MotorMaxForce};
		bool const finite = std::all_of(std::begin(values), std::end(values),
		                                [](float value)
		                                {
											return std::isfinite(value);
										});
		if (!finite || !IsFinite(desc.Point) || !IsFinite(desc.ConnectedPoint) || !IsFinite(desc.Axis) || !FitsExtent(desc.Point) ||
		    !FitsExtent(desc.ConnectedPoint))
		{
			return MakeError("the joint of entity {} has values that are not finite or too large", desc.Entity);
		}
		float const axisLength = glm::length(desc.Axis);
		bool const needsAxis = desc.Type == JointType::Hinge || desc.Type == JointType::Slider;
		if (needsAxis && axisLength < 1e-6f)
		{
			return MakeError("the joint of entity {} has no axis", desc.Entity);
		}
		glm::vec3 const axis = needsAxis ? desc.Axis / axisLength : glm::vec3(0.0f, 1.0f, 0.0f);
		// Any direction perpendicular to the axis.
		glm::vec3 const helper = std::abs(axis.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
		glm::vec3 const normal = glm::normalize(glm::cross(axis, helper));
		float const maxForce = std::clamp(desc.MotorMaxForce, 0.0f, MaxPhysicsForce);

		JPH::Ref<JPH::TwoBodyConstraintSettings> settings;
		switch (desc.Type)
		{
			case JointType::Fixed:
			{
				JPH::Ref<JPH::FixedConstraintSettings> fixed = new JPH::FixedConstraintSettings();
				fixed->mSpace = JPH::EConstraintSpace::WorldSpace;
				fixed->mAutoDetectPoint = true;
				settings = fixed;
				break;
			}
			case JointType::Hinge:
			{
				JPH::Ref<JPH::HingeConstraintSettings> hinge = new JPH::HingeConstraintSettings();
				hinge->mSpace = JPH::EConstraintSpace::WorldSpace;
				hinge->mPoint1 = hinge->mPoint2 = JPH::RVec3(Jolt::ToJolt(desc.Point));
				hinge->mHingeAxis1 = hinge->mHingeAxis2 = Jolt::ToJolt(axis);
				hinge->mNormalAxis1 = hinge->mNormalAxis2 = Jolt::ToJolt(normal);
				if (desc.LimitsEnabled)
				{
					hinge->mLimitsMin = std::clamp(desc.MinLimit, -JPH::JPH_PI, 0.0f);
					hinge->mLimitsMax = std::clamp(desc.MaxLimit, 0.0f, JPH::JPH_PI);
				}
				hinge->mMotorSettings.SetTorqueLimit(maxForce);
				settings = hinge;
				break;
			}
			case JointType::Ball:
			{
				JPH::Ref<JPH::PointConstraintSettings> point = new JPH::PointConstraintSettings();
				point->mSpace = JPH::EConstraintSpace::WorldSpace;
				point->mPoint1 = point->mPoint2 = JPH::RVec3(Jolt::ToJolt(desc.Point));
				settings = point;
				break;
			}
			case JointType::Distance:
			{
				JPH::Ref<JPH::DistanceConstraintSettings> distance = new JPH::DistanceConstraintSettings();
				distance->mSpace = JPH::EConstraintSpace::WorldSpace;
				// The connected body is the first.
				distance->mPoint1 = JPH::RVec3(Jolt::ToJolt(desc.ConnectedPoint));
				distance->mPoint2 = JPH::RVec3(Jolt::ToJolt(desc.Point));
				float const current = glm::length(desc.ConnectedPoint - desc.Point);
				float minimum = desc.MinDistance < 0.0f ? current : desc.MinDistance;
				float maximum = desc.MaxDistance < 0.0f ? current : desc.MaxDistance;
				if (minimum > maximum)
				{
					std::swap(minimum, maximum);
				}
				distance->mMinDistance = minimum;
				distance->mMaxDistance = maximum;
				settings = distance;
				break;
			}
			case JointType::Slider:
			{
				JPH::Ref<JPH::SliderConstraintSettings> slider = new JPH::SliderConstraintSettings();
				slider->mSpace = JPH::EConstraintSpace::WorldSpace;
				slider->mAutoDetectPoint = true;
				slider->SetSliderAxis(Jolt::ToJolt(axis));
				if (desc.LimitsEnabled)
				{
					slider->mLimitsMin = std::min(desc.MinLimit, 0.0f);
					slider->mLimitsMax = std::max(desc.MaxLimit, 0.0f);
				}
				slider->mMotorSettings.SetForceLimit(maxForce);
				settings = slider;
				break;
			}
		}
		if (settings == nullptr)
		{
			return MakeError("entity {} has a joint of an unknown type", desc.Entity);
		}

		JPH::BodyInterface& bodies = data.System.GetBodyInterface();
		// Jolt measures angles and drives motors of the second body relative to the first: the entity's body is second, so
		// it turns and slides along the axis relative to what it is joined to.
		JPH::Ref<JPH::TwoBodyConstraint> constraint = bodies.CreateConstraint(settings, connected, body->Id);
		if (constraint == nullptr)
		{
			return MakeError("the joint of entity {} could not be made", desc.Entity);
		}
		if (desc.MotorEnabled)
		{
			// Jolt may be built without RTTI: its own subtype tells the constraint's class.
			if (constraint->GetSubType() == JPH::EConstraintSubType::Hinge)
			{
				auto* hinge = static_cast<JPH::HingeConstraint*>(constraint.GetPtr());
				hinge->SetMotorState(JPH::EMotorState::Velocity);
				hinge->SetTargetAngularVelocity(desc.MotorSpeed);
			}
			else if (constraint->GetSubType() == JPH::EConstraintSubType::Slider)
			{
				auto* slider = static_cast<JPH::SliderConstraint*>(constraint.GetPtr());
				slider->SetMotorState(JPH::EMotorState::Velocity);
				slider->SetTargetVelocity(desc.MotorSpeed);
			}
		}
		data.System.AddConstraint(constraint);
		bodies.ActivateConstraint(constraint);
		data.Joints[desc.Entity] = Data::JointRecord{constraint, desc.ConnectedEntity};
		return {};
	}

	void PhysicsScene::RemoveJoint(UUID entity)
	{
		Data& data = *m_Data;
		auto const it = data.Joints.find(entity);
		if (it == data.Joints.end())
		{
			return;
		}
		data.System.RemoveConstraint(it->second.Constraint);
		// The bodies it held may now fall.
		JPH::BodyID const ids[] = {it->second.Constraint->GetBody1()->GetID(), it->second.Constraint->GetBody2()->GetID()};
		for (JPH::BodyID const id : ids)
		{
			if (!id.IsInvalid())
			{
				data.System.GetBodyInterface().ActivateBody(id);
			}
		}
		data.Joints.erase(it);
	}

	bool PhysicsScene::HasJoint(UUID entity) const
	{
		return m_Data->Joints.contains(entity);
	}

	size_t PhysicsScene::GetJointCount() const
	{
		return m_Data->Joints.size();
	}

	Result<void> PhysicsScene::AddCharacter(CharacterDesc const& desc)
	{
		Data& data = *m_Data;
		// An entity has a body or a character.
		RemoveBody(desc.Entity);
		float const values[] = {desc.Radius, desc.Height,    desc.StepHeight,   desc.MaxSlopeAngle,
		                        desc.Mass,   desc.PushForce, desc.GravityFactor};
		bool const finite = std::all_of(std::begin(values), std::end(values),
		                                [](float value)
		                                {
											return std::isfinite(value);
										});
		if (!finite || !IsFinite(desc.Position))
		{
			RemoveCharacter(desc.Entity);
			return MakeError("the character of entity {} has values that are not finite", desc.Entity);
		}
		if (desc.Radius > MaxPhysicsExtent || desc.Height > MaxPhysicsExtent)
		{
			RemoveCharacter(desc.Entity);
			return MakeError("entity {}: the character is larger than the physics world allows ({} m)", desc.Entity, MaxPhysicsExtent);
		}

		glm::vec3 const position = ClampPosition(desc.Position);
		if (position != desc.Position)
		{
			ST_CORE_WARN("Physics: entity {} is farther from the origin than the physics world allows ({} m); its character stays at "
			             "the edge",
			             desc.Entity, MaxPhysicsCoordinate);
		}
		float const radius = std::max(desc.Radius, MinimumExtent);
		float const halfCylinder = std::max(0.5f * desc.Height - radius, 0.0f);
		uint32_t const layer = desc.Layer < data.LayerCount ? desc.Layer : 0u;
		// Jolt takes slopes of (nearly) zero degrees as no limit at all.
		float const maxSlopeAngle = std::clamp(desc.MaxSlopeAngle, glm::radians(1.0f), glm::half_pi<float>());
		float const mass = std::clamp(desc.Mass, 0.0f, MaxPhysicsMass);
		float const pushForce = std::clamp(desc.PushForce, 0.0f, MaxPhysicsForce);

		CharacterRecord* record = data.FindCharacter(desc.Entity);
		bool const reshape = record == nullptr || record->Radius != radius || record->HalfCylinder != halfCylinder;
		JPH::RefConst<JPH::Shape> shape;
		JPH::RefConst<JPH::Shape> innerShape;
		if (reshape)
		{
			// The inner body is larger by the character's padding, so it touches what the character stands on and leans
			// against, and contacts with it are reported steadily.
			Result<JPH::RefConst<JPH::Shape>> created = CreateCharacterShape(radius, halfCylinder);
			Result<JPH::RefConst<JPH::Shape>> createdInner = CreateCharacterShape(radius + CharacterPadding, halfCylinder);
			if (!created || !createdInner)
			{
				RemoveCharacter(desc.Entity);
				return MakeError("entity {}: {}", desc.Entity, created ? createdInner.GetError() : created.GetError());
			}
			shape = created.TakeValue();
			innerShape = createdInner.TakeValue();
		}

		if (record == nullptr)
		{
			JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
			settings->mShape = shape;
			settings->mCharacterPadding = CharacterPadding;
			settings->mSupportingVolume = MakeSupportingVolume(halfCylinder);
			settings->mMaxSlopeAngle = maxSlopeAngle;
			settings->mMass = mass;
			settings->mMaxStrength = pushForce;
			// Smoother walking over the inner edges of meshes (terrain), at a small cost.
			settings->mEnhancedInternalEdgeRemoval = true;
			settings->mInnerBodyShape = innerShape;
			settings->mInnerBodyLayer = Layers::Make(layer, true);
			JPH::Ref<JPH::CharacterVirtual> character =
				new JPH::CharacterVirtual(settings, Jolt::ToJolt(position), JPH::Quat::sIdentity(), desc.Entity.GetValue(), &data.System);
			if (character->GetInnerBodyID().IsInvalid())
			{
				return MakeError("entity {}: the physics world is full ({} bodies)", desc.Entity, MaxBodies);
			}
			{
				// The inner body reports touching static and kinematic bodies like kinematic bodies do: the character enters
				// static triggers.
				JPH::BodyLockWrite const lock(data.System.GetBodyLockInterface(), character->GetInnerBodyID());
				if (lock.Succeeded())
				{
					lock.GetBody().SetCollideKinematicVsNonDynamic(true);
				}
			}
			record = &data.Characters[desc.Entity];
			record->Character = std::move(character);
			data.OptimizeBroadPhase = true;
		}
		else
		{
			JPH::CharacterVirtual& character = *record->Character;
			if (reshape)
			{
				// Without a check for room (FLT_MAX): what the larger shape overlaps pushes it out.
				CharacterFilters const filters(data.System, record->Layer, data.Materials);
				character.SetShape(shape, FLT_MAX, filters.BroadPhaseFilter, filters.LayerFilter, filters.BodyFilter, filters.ShapeFilter,
				                   data.TempAllocator);
				character.SetInnerBodyShape(innerShape);
				character.SetSupportingVolume(MakeSupportingVolume(halfCylinder));
			}
			character.SetPosition(Jolt::ToJolt(position));
			character.SetMaxSlopeAngle(maxSlopeAngle);
			character.SetMass(mass);
			character.SetMaxStrength(pushForce);
			if (layer != record->Layer)
			{
				data.System.GetBodyInterface().SetObjectLayer(character.GetInnerBodyID(), Layers::Make(layer, true));
			}
		}
		record->Layer = layer;
		record->Radius = radius;
		record->HalfCylinder = halfCylinder;
		record->StepHeight = std::clamp(desc.StepHeight, 0.0f, MaxPhysicsExtent);
		record->GravityFactor = std::clamp(desc.GravityFactor, -MaxPhysicsGravityFactor, MaxPhysicsGravityFactor);
		data.RefreshContacts(*record);
		return {};
	}

	void PhysicsScene::RemoveCharacter(UUID entity)
	{
		Data& data = *m_Data;
		auto const it = data.Characters.find(entity);
		if (it == data.Characters.end())
		{
			return;
		}
		JPH::BodyID const innerBody = it->second.Character->GetInnerBodyID();
		// The character removes its inner body, whose contacts end.
		data.Characters.erase(it);
		data.Contacts.ForgetBody(innerBody);
	}

	bool PhysicsScene::HasCharacter(UUID entity) const
	{
		return m_Data->Characters.contains(entity);
	}

	size_t PhysicsScene::GetCharacterCount() const
	{
		return m_Data->Characters.size();
	}

	void PhysicsScene::MoveCharacter(UUID entity, glm::vec3 const& velocity)
	{
		CharacterRecord* record = m_Data->FindCharacter(entity);
		if (record != nullptr && IsFinite(velocity))
		{
			record->Walk = ClampLength(velocity, MaxCharacterSpeed);
		}
	}

	void PhysicsScene::SetCharacterVelocity(UUID entity, glm::vec3 const& velocity)
	{
		CharacterRecord* record = m_Data->FindCharacter(entity);
		if (record != nullptr && IsFinite(velocity))
		{
			record->Velocity = ClampLength(velocity, MaxCharacterSpeed);
		}
	}

	void PhysicsScene::SetCharacterPosition(UUID entity, glm::vec3 const& position)
	{
		CharacterRecord* record = m_Data->FindCharacter(entity);
		if (record != nullptr && IsFinite(position))
		{
			record->Character->SetPosition(Jolt::ToJolt(ClampPosition(position)));
			m_Data->RefreshContacts(*record);
		}
	}

	std::optional<CharacterState> PhysicsScene::GetCharacterState(UUID entity) const
	{
		auto const it = m_Data->Characters.find(entity);
		if (it == m_Data->Characters.end())
		{
			return std::nullopt;
		}
		JPH::CharacterVirtual const& character = *it->second.Character;
		bool const supported = character.IsSupported();
		CharacterState state;
		state.Entity = entity;
		state.Position = Jolt::ToGlm(character.GetPosition());
		state.Velocity = it->second.Velocity;
		state.IsGrounded = character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
		state.GroundNormal = supported ? Jolt::ToGlm(character.GetGroundNormal()) : glm::vec3(0.0f);
		state.GroundEntity = supported ? UUID(character.GetGroundUserData()) : UUID::Invalid();
		return state;
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
		// Characters first: their pushes, and their inner bodies where they moved, act in this step.
		for (auto& [entity, record] : data.Characters)
		{
			data.StepCharacter(record, deltaTime);
		}
		JPH::EPhysicsUpdateError const error = data.System.Update(deltaTime, 1, &data.TempAllocator, &Jolt::GetJobSystem());
		if (error != JPH::EPhysicsUpdateError::None)
		{
			ST_CORE_WARN("The physics step ran out of space (error flags {:#x}); some contacts were dropped", static_cast<uint32_t>(error));
		}
		data.Contacts.FinishStep(data.System.GetBodyInterface());
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
		if (IsFinite(gravity))
		{
			m_Data->System.SetGravity(Jolt::ToJolt(ClampLength(gravity, MaxPhysicsGravity)));
		}
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
		BodyRecord const* record = m_Data->Find(entity);
		if (record != nullptr && IsFinite(position) && IsFinite(rotation))
		{
			JPH::EActivation const activation =
				record->Type == RigidBodyType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate;
			m_Data->System.GetBodyInterface().SetPositionAndRotation(record->Id, Jolt::ToJolt(ClampPosition(position)),
			                                                         Jolt::ToJolt(Math::NormalizeRotation(rotation)), activation);
		}
	}

	void PhysicsScene::MoveKinematic(UUID entity, glm::vec3 const& position, glm::quat const& rotation, float deltaTime)
	{
		BodyRecord const* record = m_Data->Find(entity);
		if (record != nullptr && record->Type == RigidBodyType::Kinematic && deltaTime > 0.0f && IsFinite(position) && IsFinite(rotation))
		{
			m_Data->System.GetBodyInterface().MoveKinematic(record->Id, Jolt::ToJolt(ClampPosition(position)),
			                                                Jolt::ToJolt(Math::NormalizeRotation(rotation)), deltaTime);
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
		if (record != nullptr && record->Type != RigidBodyType::Static && IsFinite(velocity))
		{
			// Jolt clamps the velocity to the body's limit by its squared length, which overflows for huge values.
			JPH::BodyInterface& bodies = m_Data->System.GetBodyInterface();
			bodies.SetLinearVelocity(record->Id, Jolt::ToJolt(ClampLength(velocity, bodies.GetMaxLinearVelocity(record->Id))));
			bodies.ActivateBody(record->Id);
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
		if (record != nullptr && record->Type != RigidBodyType::Static && IsFinite(velocity))
		{
			JPH::BodyInterface& bodies = m_Data->System.GetBodyInterface();
			bodies.SetAngularVelocity(record->Id, Jolt::ToJolt(ClampLength(velocity, bodies.GetMaxAngularVelocity(record->Id))));
			bodies.ActivateBody(record->Id);
		}
	}

	void PhysicsScene::AddForce(UUID entity, glm::vec3 const& requestedForce, ForceMode mode)
	{
		BodyRecord const* record = m_Data->Find(entity);
		if (record == nullptr || record->Type != RigidBodyType::Dynamic || !IsFinite(requestedForce))
		{
			return;
		}
		glm::vec3 const force = ClampLength(requestedForce, MaxPhysicsForce);
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

	void PhysicsScene::AddTorque(UUID entity, glm::vec3 const& requestedTorque, ForceMode mode)
	{
		BodyRecord const* record = m_Data->Find(entity);
		if (record == nullptr || record->Type != RigidBodyType::Dynamic || !IsFinite(requestedTorque))
		{
			return;
		}
		glm::vec3 const torque = ClampLength(requestedTorque, MaxPhysicsForce);
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
		if (record != nullptr && record->Type == RigidBodyType::Dynamic && std::isfinite(factor))
		{
			m_Data->System.GetBodyInterface().SetGravityFactor(record->Id,
			                                                   std::clamp(factor, -MaxPhysicsGravityFactor, MaxPhysicsGravityFactor));
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
