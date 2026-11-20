#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/Result.h"
#include "Strada/Core/UUID.h"
#include "Strada/Physics/PhysicsTypes.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Strada
{
	// What a physics world's simulation changes, at one moment (PhysicsScene::SaveState), to go back to in the same world.
	// Opaque; held in memory only, never written to a file.
	class PhysicsSnapshot
	{
	public:
		// Bytes of state it holds.
		size_t GetSize() const { return m_State.size(); }

	private:
		friend class PhysicsScene;

		// The world it was saved from, and that world's structure version then.
		UUID m_World = UUID::Invalid();
		uint64_t m_Structure = 0;
		std::string m_State;
	};

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

		// Creates the body of an entity, replacing an existing one (or its character). Fails without colliders, when a shape
		// is invalid or when a triangle mesh has more than MaxMeshColliderMaterials submesh surfaces. Characters standing on a
		// replaced or removed body find what they stand on at once.
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

		// --- Vehicles (Architecture.md section 8, "Vehicles") ---
		//
		// A vehicle is a motorcycle on an entity's dynamic body: Jolt's motorcycle controller on a vehicle constraint. The world
		// steps its vehicles through one step listener of its own, one after another in the order of their entities' UUIDs, on
		// the thread that runs Jolt's step-listener job (a worker, or this thread while it waits for the step); their callbacks
		// read the vehicle's own values and the surfaces, fixed for the step, and write only the vehicle's own constraint,
		// wheels and record. Making, replacing or removing a vehicle moves the structure version; changing its values in place
		// (SetVehicleSettings) does not. After each of them the world takes every vehicle's constraint and every joint out of
		// Jolt and adds them again, the vehicles in the order of their entities' UUIDs and then the joints in theirs, so that
		// Jolt solves them in an order that never depends on what happened before.

		// Makes the entity's vehicle on its body, replacing the vehicle it has (which keeps its latest inputs, its engine's
		// speed within the new range, its gear within the new gears and each wheel's spin and angle). Fails, leaving the
		// entity without a vehicle, when the entity has no dynamic body, a value is not finite or is outside the range of its
		// Vehicle field (Strada/Asset/VehicleAsset.h, in radians where the field is in degrees), a direction is zero, Up and
		// Forward are parallel, or the engine's or the gearbox's speeds are out of order (as ValidateVehicle refuses them).
		// Removing the entity's body removes its vehicle; replacing the body (AddBody, a collider changed) makes the vehicle
		// again on the new body when it is dynamic, keeping what a replaced vehicle keeps, and removes it otherwise.
		[[nodiscard]] Result<void> AddVehicle(VehicleDesc const& desc);
		void RemoveVehicle(UUID entity);
		bool HasVehicle(UUID entity) const;
		size_t GetVehicleCount() const;
		// Gives the entity's vehicle new values from its next step on, keeping its state: the wheels, the engine, the gearbox,
		// the final drive and the lean (an edit of its definition while the world runs). The state stays within the new
		// values: the gear (Jolt's, and a Manual gearbox's own) within the new gears, the engine's speed within the new range.
		// A change of mode hands the gear over: a gearbox made Manual takes the gear it is in (its timers at 0, pending shift
		// requests dropped), one made Automatic goes on from the gear it is in. Up and Forward are taken by Jolt when the
		// vehicle is made, so values that change them are refused, as are values AddVehicle refuses; the caller then makes
		// the vehicle again with AddVehicle. A refusal changes nothing. Between steps.
		[[nodiscard]] Result<void> SetVehicleSettings(UUID entity, VehicleDesc const& desc);
		// The inputs the vehicle's next steps apply (clamped, VehicleInputs), replacing the latest; ignored for an entity
		// without a vehicle. Inputs that are not all 0 wake the vehicle's body before each step.
		void SetVehicleInputs(UUID entity, VehicleInputs const& inputs);
		// Asks a Manual gearbox to shift by `gears` (+1 up, -1 down): the requests add up until the next step, which moves the
		// gear by their sum within neutral and the top gear. An Automatic gearbox shifts by itself and ignores them.
		void RequestVehicleShift(UUID entity, int32_t gears);
		// The vehicle as the last step left it, with the inputs given since; empty for an entity without a vehicle.
		std::optional<VehicleState> GetVehicleState(UUID entity) const;
		// The transform of one of the vehicle's wheels (0 the front, 1 the rear) in its chassis's local space without the
		// chassis's scale, as Jolt's VehicleConstraint::GetWheelLocalTransform composes it from the last step's suspension
		// length, steering angle and rotation angle, for a wheel model whose axle is its local X and whose up is its local Y
		// (what ComposeVehicleWheelTransform composes from VehicleWheelState); empty for an entity without a vehicle or an
		// index past the wheels.
		std::optional<glm::mat4> GetVehicleWheelTransform(UUID entity, size_t wheel) const;
		// The values the vehicle runs with (AddVehicle's or the latest SetVehicleSettings'), its directions normalized and its
		// Forward made square to its Up (NormalizeVehicleAxes): what the editor's overlay draws its wheels with. Null for an
		// entity without a vehicle; valid until the vehicle is made again, given new values or removed.
		VehicleDesc const* FindVehicleSettings(UUID entity) const;
		// The chassis's roll inertia about its front (kg m²: 1 over the front's component of the body's inverse inertia along
		// the front), which the lean spring's impulse turns (Architecture.md section 8, "Vehicles": its iterations settle only
		// while LeanSpringDamping times the collision step stays below it). Empty for an entity without a vehicle, or a body
		// that cannot roll about its front.
		std::optional<float> GetVehicleRollInertia(UUID entity) const;

		// --- Simulation ---

		// Moves the characters (in the order of their entities' IDs), carried by what they stand on, then simulates the
		// bodies; characters standing on bodies that went down (descending platforms) then follow them down.
		void Step(float deltaTime);
		// Positions and rotations of the dynamic bodies that are awake.
		std::vector<BodyTransform> GetActiveBodyTransforms() const;
		// Events since the last call, in the order of their steps (ContactEvent::Step); a removed body's exits come between
		// the steps they happened between. The events of one step are simultaneous, so they are sorted by First, then
		// Second, then Type, which gives the same order on every run whatever order Jolt's worker threads found them in.
		// An enter event carries the points of the manifolds with which its contact began in that step (contacts Jolt finds
		// again when sleeping bodies wake make no event); the listener takes them on the worker threads into the event,
		// whose points are a fixed array.
		std::vector<ContactEvent> TakeContactEvents();
		// The steps this world has run (0 before the first): the Step of the events of the last step.
		uint64_t GetStepCount() const;
		// Collision steps each Step is divided into (1 to MaxPhysicsCollisionSteps; PhysicsWorldSettings::CollisionSteps).
		uint32_t GetCollisionSteps() const;

		// --- State ---

		// Saves what the simulation changes: everything Jolt's state recorder saves (the last step's length and gravity, the
		// bodies' positions, rotations, velocities, forces and sleep state, the contacts, the joints' warm starts and the
		// vehicles' constraints, controllers and wheels), each character's state with its own and walking velocity and ground
		// displacement, the contacts reported so far with the events not taken yet and the step count, and each vehicle's
		// inputs, pending shift, gearbox mode and shift state, lean spring integral and wheels' steering, slips and surfaces,
		// which Jolt does not save. A gearbox whose mode was edited in place since a snapshot is handed over when it is
		// restored, as SetVehicleSettings hands it over.
		[[nodiscard]] PhysicsSnapshot SaveState() const;
		// Goes back to a snapshot of this world, so that the steps that follow run as they did from there. Fails, changing
		// nothing, for a snapshot of another world or once a body, joint, character or vehicle was added, removed or replaced
		// since (Jolt restores state, not structure). Settings the simulation does not change stay as they are now (gravity
		// factors, damping, layers, what characters ignore, a vehicle's values): a vehicle whose gears were changed in place
		// since (SetVehicleSettings) is brought back within the gears it has now, as SetVehicleSettings does.
		[[nodiscard]] Result<void> RestoreState(PhysicsSnapshot const& snapshot);
		// Changes whenever a body, joint, character or vehicle is added, removed or replaced (a vehicle's values changed in
		// place do not count): snapshots of other versions are refused.
		uint64_t GetStructureVersion() const;
		// A 64-bit hash (HashBuilder) of the simulation's state: every body in entity order (entity, motion type, position,
		// rotation, linear and angular velocity, awake), then every character in entity order (position, velocity, its own
		// and walking velocity, ground state, ground entity and displacement), then, only in a world with vehicles, their
		// count and every vehicle in entity order (Architecture.md section 8, "Vehicles"). Equal worlds stepped alike hash
		// alike.
		uint64_t ComputeStateHash() const;

		glm::vec3 GetGravity() const;
		void SetGravity(glm::vec3 const& gravity);

		// --- Surfaces (ColliderSurface) ---
		//
		// Every collider of a body has a surface (ColliderDesc::Surface), and a triangle mesh with submesh surfaces one per
		// submesh (ColliderDesc::SubmeshSurfaces). What touches or is hit is a sub-shape of a body (a Jolt SubShapeID): its
		// leaf shape names its collider, and on such a mesh its triangle's material index names its submesh (0 for the
		// collider's surface, i for SubmeshSurfaces[i - 1]). Contacts combine the two sub-shapes' surfaces, and queries report
		// the surface's Material (RaycastHit::Material), and a vehicle's wheels its TyreGrip and RollingResistance. The lookup
		// from a sub-shape to its surface is internal to the Physics module (FindSubShapeMaterial in Physics/PhysicsSurfaces.h,
		// any thread while the surfaces do not change), which the contact listener, the queries and the vehicles share. Only a
		// character's inner body has no surface: its contacts bring Jolt's default friction (0.2) and restitution (0) with
		// the default modes.

		// Gives every collider and submesh surface whose ColliderSurface::Material is this material the material's friction,
		// restitution and modes, rolling resistance and tyre grip (an edit of the material while the world runs): their
		// contacts and the wheels on them use them from the next step,
		// contacts that persist included, and the bodies keep everything else. When the values contacts read differ from what
		// a body had, it wakes (a sleeping body has no contacts to take them), and so does every non-static body whose bounds
		// meet a static one's; when the tyre grip or the rolling resistance differ, the chassis of every vehicle one of whose
		// wheels touched the material at its last step wakes, and no other body; in entity order. Returns how many surfaces
		// name the material. Between steps: Jolt's worker threads
		// read the surfaces during a step.
		size_t SetMaterialValues(AssetHandle material, PhysicsMaterial const& values);

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

		// --- Queries (Architecture.md section 8, "Queries") ---
		//
		// They read the world as the last step left it (and bodies added or changed since), on the main thread between
		// steps, and change nothing. A query sees the bodies and characters on the layers of its mask (bit i = layer i), and
		// trigger colliders only when its filter includes them (PhysicsQueryFilter). It reports each entity at most once:
		// a ray or cast its nearest hit over the entity's colliders and triangles (at equal distances the one of the smaller
		// Jolt sub-shape ID), an overlap the entity alone. Directions need not be normalized. A query whose origin or center
		// is not finite or beyond MaxPhysicsCoordinate, whose direction is zero or not finite, whose distance is not finite
		// and positive, whose rotation is not finite or whose shape is not valid (PhysicsQueryShape) finds nothing;
		// distances beyond MaxPhysicsQueryDistance are cut to it and rotations normalized (zero is the identity). The
		// many-hit forms replace the vector's contents (keeping its capacity) with every hit, in order, and return the count.

		// The closest solid collider along a ray (trigger colliders are ignored) on the layers of the mask (bit i =
		// layer i), with the physics material of the surface hit. The direction need not be normalized. A ray starting
		// inside a convex collider passes out of it. Of entities hit at exactly the same distance it reports the one of the
		// smaller UUID, as RaycastAll's first hit; its checks are its own (finite positive distance, non-zero direction).
		std::optional<RaycastHit> Raycast(glm::vec3 const& origin, glm::vec3 const& direction, float maxDistance,
		                                  uint32_t layerMask = AllPhysicsLayers) const;
		// Every entity along a ray, each at its nearest hit, ordered by IsQueryHitBefore (distance, then UUID). As Raycast, a
		// ray starting inside a convex collider passes out of it and triangles are hit from their front only.
		size_t RaycastAll(glm::vec3 const& origin, glm::vec3 const& direction, float maxDistance, std::vector<RaycastHit>& hits,
		                  PhysicsQueryFilter const& filter = {}) const;
		// The first entity a shape touches as it moves from origin (its center), turned by rotation, along direction for at
		// most maxDistance without turning: of the entities touched, the first by IsQueryHitBefore (distance, then UUID).
		// A collider the shape overlaps where it starts is touched at distance 0, at the deepest point of the overlap,
		// whichever way the cast goes; triangles are touched from their front only, as by rays. That includes the caller's own
		// colliders: PhysicsQueryFilter excludes no entity, so a cast meant to find what lies below or ahead of a body (a
		// wheel under a chassis, a foot under a character) starts outside that body's colliders or leaves its layer out of
		// the mask.
		std::optional<ShapeCastHit> ShapeCast(PhysicsQueryShape const& shape, glm::vec3 const& origin, glm::quat const& rotation,
		                                      glm::vec3 const& direction, float maxDistance, PhysicsQueryFilter const& filter = {}) const;
		// Every entity with a collider the shape overlaps at center, turned by rotation, ordered by UUID. A triangle counts
		// from either side; a shape wholly inside a closed triangle mesh does not meet its surface and does not overlap it.
		// Box colliders count with their edges and corners rounded by their convex radius (at most 5 cm), as Jolt's collide
		// test sees them, so a shape that only grazes an edge may be touched by a cast at distance 0 and not overlap it.
		size_t Overlap(PhysicsQueryShape const& shape, glm::vec3 const& center, glm::quat const& rotation, std::vector<UUID>& entities,
		               PhysicsQueryFilter const& filter = {}) const;

	private:
		struct Data;
		struct PrivateTag
		{
		};

		// AddBody without what it does for a vehicle on the replaced body (it takes the vehicle before and makes it again
		// after).
		[[nodiscard]] Result<void> MakeBody(BodyDesc const& desc);
		// After a vehicle was made, made again or removed: Jolt's constraints in the order of their entities, the vehicles'
		// first, then the joints' (Architecture.md section 8, "Vehicles").
		void ReaddVehicleConstraints();

	public:
		PhysicsScene(PrivateTag, Scope<Data> data);

	private:
		Scope<Data> m_Data;
	};
}
