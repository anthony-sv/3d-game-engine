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
	// PhysicsSystem's worker threads, and characters that games move. Object layers are the project's physics layers;
	// broad-phase layers separate static from moving bodies. Collision and trigger events are collected during steps and
	// taken on the main thread. Knows nothing about the ECS: the Scene creates bodies and characters from components and
	// writes transforms back. Requires PhysicsSystem. Main thread only.
	class PhysicsScene
	{
	public:
		[[nodiscard]] static Result<Scope<PhysicsScene>> Create(PhysicsWorldSettings const& settings);
		~PhysicsScene();

		PhysicsScene(PhysicsScene const&) = delete;
		PhysicsScene& operator=(PhysicsScene const&) = delete;

		// --- Bodies ---

		// Creates the body of an entity, replacing an existing one (or its character). Fails without colliders or when a
		// shape is invalid. Characters standing on a replaced or removed body find what they stand on at once.
		[[nodiscard]] Result<void> AddBody(BodyDesc const& desc);
		void RemoveBody(UUID entity);
		bool HasBody(UUID entity) const;
		size_t GetBodyCount() const;

		// --- Joints ---

		// Joins the entity's body to another body or the world, replacing the entity's joint. Fails when a body is missing,
		// the connected entity is the entity itself, or a value is not finite (axes must not be zero). Removing either body
		// removes the joint.
		[[nodiscard]] Result<void> AddJoint(JointDesc const& desc);
		void RemoveJoint(UUID entity);
		bool HasJoint(UUID entity) const;
		size_t GetJointCount() const;

		// --- Characters (see CharacterDesc) ---

		// Creates the character of an entity at desc.Position; an entity's existing character moves there (without
		// colliding on the way), keeps its velocity and takes the new shape and settings (a larger shape is not checked for
		// room: what it overlaps pushes it out). An entity has a body or a character: each replaces the other. Fails when a
		// value is not finite, the capsule is larger than MaxPhysicsExtent or the world is full; a refused description
		// leaves the entity without a character.
		[[nodiscard]] Result<void> AddCharacter(CharacterDesc const& desc);
		void RemoveCharacter(UUID entity);
		bool HasCharacter(UUID entity) const;
		size_t GetCharacterCount() const;
		// The velocity (meters per second) at which the character walks during the next step, on top of its own velocity;
		// a later call before the step replaces it.
		void MoveCharacter(UUID entity, glm::vec3 const& velocity);
		// Its own velocity: gravity speeds it up while it is not standing on walkable ground, landing stops it, and what it
		// runs into removes the part towards it.
		void SetCharacterVelocity(UUID entity, glm::vec3 const& velocity);
		// Places the character's capsule center there at once (without colliding on the way), keeping its velocity, and
		// finds what it stands on there.
		void SetCharacterPosition(UUID entity, glm::vec3 const& position);
		// The bodies of these entities (the character's own parts: a held object, a trigger around it) neither block the
		// character nor touch its inner body, so they report no contacts with it; replaces the earlier list.
		void SetCharacterIgnoredEntities(UUID entity, std::vector<UUID> entities);
		std::optional<CharacterState> GetCharacterState(UUID entity) const;

		// --- Simulation ---

		// Moves the characters (in the order of their entities' IDs), carried by what they stand on, then simulates the
		// bodies; characters standing on bodies that went down (descending platforms) then follow them down.
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
