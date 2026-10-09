#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Physics/PhysicsTypes.h"

#include <glm/glm.hpp>

#include <optional>
#include <vector>

namespace Strada
{
	// A physics world: one rigid body per entity (its colliders form a compound shape), simulated in fixed steps on
	// PhysicsSystem's worker threads. Object layers are the project's physics layers; broad-phase layers separate static
	// from moving bodies. Collision and trigger events are collected during steps and taken on the main thread. Knows
	// nothing about the ECS: the Scene creates bodies from components and writes transforms back. Requires
	// PhysicsSystem. Main thread only.
	class PhysicsScene
	{
	public:
		[[nodiscard]] static Result<Scope<PhysicsScene>> Create(PhysicsWorldSettings const& settings);
		~PhysicsScene();

		PhysicsScene(PhysicsScene const&) = delete;
		PhysicsScene& operator=(PhysicsScene const&) = delete;

		// --- Bodies ---

		// Creates the body of an entity, replacing an existing one. Fails without colliders or when a shape is invalid.
		[[nodiscard]] Result<void> AddBody(BodyDesc const& desc);
		void RemoveBody(UUID entity);
		bool HasBody(UUID entity) const;
		size_t GetBodyCount() const;

		// --- Simulation ---

		void Step(float deltaTime);
		// Positions and rotations of the dynamic bodies that are awake.
		std::vector<BodyTransform> GetActiveBodyTransforms() const;
		// Events since the last call, in the order they happened.
		std::vector<ContactEvent> TakeContactEvents();

		glm::vec3 GetGravity() const;
		void SetGravity(glm::vec3 const& gravity);

		// --- Body state (ignored for entities without a body) ---

		std::optional<BodyTransform> GetBodyTransform(UUID entity) const;
		// Moves the body there instantly (no collision response on the way).
		void SetBodyTransform(UUID entity, glm::vec3 const& position, glm::quat const& rotation);
		// Kinematic bodies: velocities that reach the target at the end of the next step of deltaTime seconds.
		void MoveKinematic(UUID entity, glm::vec3 const& position, glm::quat const& rotation, float deltaTime);
		glm::vec3 GetLinearVelocity(UUID entity) const;
		void SetLinearVelocity(UUID entity, glm::vec3 const& velocity);
		glm::vec3 GetAngularVelocity(UUID entity) const;
		void SetAngularVelocity(UUID entity, glm::vec3 const& velocity);
		// Dynamic bodies only; wakes them up.
		void AddForce(UUID entity, glm::vec3 const& force, ForceMode mode);
		void AddTorque(UUID entity, glm::vec3 const& torque, ForceMode mode);
		float GetMass(UUID entity) const;
		void SetGravityFactor(UUID entity, float factor);
		void SetDamping(UUID entity, float linear, float angular);
		bool IsSleeping(UUID entity) const;
		void WakeUp(UUID entity);

		// --- Queries ---

		// The closest solid collider along a ray (trigger colliders are ignored) on the layers of the mask (bit i =
		// layer i). The direction need not be normalized.
		std::optional<RaycastHit> Raycast(glm::vec3 const& origin, glm::vec3 const& direction, float maxDistance,
		                                  uint32_t layerMask = 0xFFFFFFFFu) const;

	private:
		struct Data;
		struct PrivateTag
		{
		};

	public:
		PhysicsScene(PrivateTag, Scope<Data> data);

	private:
		Scope<Data> m_Data;
	};
}
