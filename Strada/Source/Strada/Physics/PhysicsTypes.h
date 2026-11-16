#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/PhysicsMaterial.h"
#include "Strada/Core/Base.h"
#include "Strada/Core/UUID.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace Strada
{
	class MeshSource;

	// Physics layers a project can define (RigidBodyComponent::Layer indexes them).
	inline constexpr size_t MaxPhysicsLayers = 16;
	// Collision steps a physics step may be divided into (PhysicsWorldSettings::CollisionSteps).
	inline constexpr uint32_t MaxPhysicsCollisionSteps = 4;
	// Physics materials of a triangle-mesh collider's submeshes (MeshColliderComponent::Materials, ColliderDesc::
	// SubmeshSurfaces). Jolt's mesh shape holds at most 32 materials (MeshShape::FLAGS_MATERIAL_BITS, refused beyond at
	// MeshShape.cpp's "Supporting max %d materials per mesh"): one per submesh entry, and the collider's own for the
	// submeshes without one.
	inline constexpr uint32_t MaxMeshColliderMaterials = 31;

	// Jolt computes in single precision, and its math would overflow long before larger worlds became usable, so the
	// physics world keeps within these limits: positions within MaxPhysicsCoordinate meters of the origin on every axis
	// (bodies beyond stay at the edge), colliders and their offsets at most MaxPhysicsExtent meters (larger bodies are not
	// simulated), masses within [MinPhysicsMass, MaxPhysicsMass] kilograms, forces, torques and impulses at most
	// MaxPhysicsForce, gravity at most MaxPhysicsGravity meters per second squared and gravity factors within
	// ±MaxPhysicsGravityFactor; larger values are clamped. A single step's change of velocity then stays finite, as Jolt
	// requires, and velocities keep within Jolt's limits (500 m/s, 47 rad/s). Characters move at most MaxCharacterSpeed.
	inline constexpr float MaxPhysicsCoordinate = 1.0e7f;
	inline constexpr float MaxPhysicsExtent = 1.0e6f;
	inline constexpr float MinPhysicsMass = 1.0e-4f;
	inline constexpr float MaxPhysicsMass = 1.0e12f;
	inline constexpr float MaxPhysicsForce = 1.0e9f;
	inline constexpr float MaxPhysicsGravity = 1.0e6f;
	inline constexpr float MaxPhysicsGravityFactor = 1.0e3f;
	inline constexpr float MaxCharacterSpeed = 500.0f;

	enum class RigidBodyType : uint8_t
	{
		// Never moves (unless teleported); collides with dynamic bodies.
		Static = 0,
		// Moved by forces, gravity and collisions.
		Dynamic,
		// Moved by its transform; pushes dynamic bodies and is not affected by them.
		Kinematic
	};

	// How AddForce and AddTorque apply their vector.
	enum class ForceMode : uint8_t
	{
		// Newtons (newton meters for torque), applied during the next step.
		Force = 0,
		// Newton seconds: an instant change of momentum.
		Impulse,
		// Meters per second squared, independent of the mass, applied during the next step.
		Acceleration,
		// Meters per second: an instant change of velocity, independent of the mass.
		VelocityChange
	};

	// Settings of a physics world that come from the project.
	struct PhysicsWorldSettings
	{
		glm::vec3 Gravity = glm::vec3(0.0f, -9.81f, 0.0f);
		// Layers in use (1 to MaxPhysicsLayers); bodies on other layers use layer 0.
		uint32_t LayerCount = 1;
		// Pairs of layer indices whose bodies do not collide.
		std::vector<glm::uvec2> IgnoredCollisions;
		// Collision steps per physics step (1 to MaxPhysicsCollisionSteps; Jolt's sub-steps: more resolve fast or stiff
		// bodies better, at their cost).
		uint32_t CollisionSteps = 1;
	};

	enum class ColliderShape : uint8_t
	{
		Box = 0,
		Sphere,
		// Along the local Y axis.
		Capsule,
		ConvexMesh,
		// Only for static and kinematic bodies; dynamic bodies use the mesh's convex hull.
		TriangleMesh
	};

	// How the contacts of a collider, or of a triangle mesh's submesh, slide and bounce: a physics material's values, or the
	// collider's own. Each contact combines both sides' values by their modes (CombinePhysicsMaterialValues).
	struct ColliderSurface
	{
		// The physics material the values are from, which raycasts report (RaycastHit::Material); none for a collider's own
		// values. The physics world only reports it and finds the surfaces of a material by it (SetMaterialValues).
		AssetHandle Material;
		// At least 0, and Restitution at most 1 (clamped when the body is made).
		float Friction = 0.6f;
		float Restitution = 0.0f;
		PhysicsMaterialCombine FrictionCombine = PhysicsMaterialCombine::GeometricMean;
		PhysicsMaterialCombine RestitutionCombine = PhysicsMaterialCombine::Maximum;

		bool operator==(ColliderSurface const& other) const = default;
	};

	// One collider of a body, in the body's local space before the body's scale.
	struct ColliderDesc
	{
		ColliderShape Shape = ColliderShape::Box;
		glm::vec3 HalfExtents = glm::vec3(0.5f);
		float Radius = 0.5f;
		// Capsules: half the length of the cylinder between the caps.
		float HalfHeight = 0.5f;
		// Mesh shapes.
		Ref<MeshSource> Mesh;
		glm::vec3 Offset = glm::vec3(0.0f);
		// Detects overlaps (trigger events) without a collision response.
		bool IsTrigger = false;
		ColliderSurface Surface;
		// Triangle meshes (ColliderShape::TriangleMesh on a static or kinematic body): the surfaces of the mesh's submeshes in
		// submesh order, at most MaxMeshColliderMaterials (AddBody refuses more); submeshes past the list use Surface. Empty
		// for one surface, which every other shape has (the convex hull of a dynamic body's triangle mesh included).
		std::vector<ColliderSurface> SubmeshSurfaces;
	};

	enum class JointType : uint8_t
	{
		// Holds the bodies in their relative position and orientation.
		Fixed = 0,
		// Lets them turn around an axis through a point (doors, wheels, pendulums), optionally limited and motorized.
		Hinge,
		// Lets them turn freely around a point (ropes, chains, ragdoll shoulders).
		Ball,
		// Keeps two points within a range of distances (ropes, springs without stretch).
		Distance,
		// Lets them slide along an axis without turning (pistons, drawers), optionally limited and motorized.
		Slider
	};

	// A joint between an entity's body and another body (or the world), in world space as the bodies are placed when the
	// joint is made.
	struct JointDesc
	{
		// The entity owning the joint (one per entity); its body is the first.
		UUID Entity = UUID::Invalid();
		// The second body; invalid joins the first body to the world.
		UUID ConnectedEntity = UUID::Invalid();
		JointType Type = JointType::Hinge;
		// Where the bodies are joined. Distance joints keep Point (on the first body) and ConnectedPoint (on the second)
		// apart.
		glm::vec3 Point = glm::vec3(0.0f);
		glm::vec3 ConnectedPoint = glm::vec3(0.0f);
		// The hinge or slider axis (normalized when the joint is made).
		glm::vec3 Axis = glm::vec3(0.0f, 1.0f, 0.0f);
		bool LimitsEnabled = false;
		// Hinges: radians, Min in [-pi, 0] and Max in [0, pi] (clamped); sliders: meters, Min <= 0 <= Max (clamped).
		float MinLimit = 0.0f;
		float MaxLimit = 0.0f;
		// Distance joints: the range of distances; negative values take the distance when the joint is made.
		float MinDistance = -1.0f;
		float MaxDistance = -1.0f;
		bool MotorEnabled = false;
		// Hinges: radians per second; sliders: meters per second.
		float MotorSpeed = 0.0f;
		// The most torque (hinges, newton meters) or force (sliders, newtons) the motor uses.
		float MotorMaxForce = 1000.0f;
	};

	// A body of an entity: its colliders form one compound shape.
	struct BodyDesc
	{
		UUID Entity = UUID::Invalid();
		RigidBodyType Type = RigidBodyType::Static;
		// World transform; the scale is applied to the colliders.
		glm::vec3 Position = glm::vec3(0.0f);
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec3 Scale = glm::vec3(1.0f);
		std::vector<ColliderDesc> Colliders;
		float Mass = 1.0f;
		float LinearDamping = 0.05f;
		float AngularDamping = 0.05f;
		float GravityFactor = 1.0f;
		uint32_t Layer = 0;
		glm::bvec3 LockTranslation = glm::bvec3(false);
		glm::bvec3 LockRotation = glm::bvec3(false);
		// Sweeps fast bodies so they do not tunnel through thin geometry.
		bool ContinuousCollision = false;
		bool AllowSleep = true;
		glm::vec3 LinearVelocity = glm::vec3(0.0f);
		glm::vec3 AngularVelocity = glm::vec3(0.0f);
	};

	// A character a game moves (the player, a walking creature): an upright capsule that walks at the velocity it is given,
	// slides along walls, climbs steps and slopes up to its limits, slides down steeper slopes, falls with gravity, is
	// carried by what it stands on (moving platforms and elevators) and pushes dynamic bodies (Jolt's CharacterVirtual). A
	// kinematic body in its place gives it presence in the world: raycasts hit it, dynamic bodies bump into it, and triggers
	// and bodies report contacts with it like with a kinematic body. Trigger colliders never block it, and neither do the
	// bodies of the entities it ignores (PhysicsScene::SetCharacterIgnoredEntities: its own parts).
	struct CharacterDesc
	{
		UUID Entity = UUID::Invalid();
		// World position of the capsule's center.
		glm::vec3 Position = glm::vec3(0.0f);
		float Radius = 0.5f;
		// Of the whole capsule, both rounded ends included; heights below 2 * Radius make a sphere.
		float Height = 2.0f;
		// The highest step it climbs, and the deepest drop it walks down without leaving the ground, in meters.
		float StepHeight = 0.3f;
		// The steepest slope it walks up, in radians (45 degrees; kept within 1 and 90 degrees).
		float MaxSlopeAngle = 0.7853982f;
		// Kilograms with which it weighs on what it stands on.
		float Mass = 70.0f;
		// The largest force with which it pushes dynamic bodies, in newtons.
		float PushForce = 200.0f;
		// Multiplier of the world's gravity.
		float GravityFactor = 1.0f;
		uint32_t Layer = 0;
	};

	// What a character did in the last step.
	struct CharacterState
	{
		UUID Entity = UUID::Invalid();
		// World position of the capsule's center.
		glm::vec3 Position = glm::vec3(0.0f);
		// Its own velocity (falling, jumping, launched), without the walking velocity it was given.
		glm::vec3 Velocity = glm::vec3(0.0f);
		// Standing on ground it can walk on.
		bool IsGrounded = false;
		// The normal of the ground under it, walkable or too steep; zero when nothing is under it.
		glm::vec3 GroundNormal = glm::vec3(0.0f);
		// What it stands on; invalid when nothing is under it.
		UUID GroundEntity = UUID::Invalid();
		// How far what it stands on moved during the last step where the character stands (the ground's velocity there
		// times the step); zero when it is not on the ground. A moving platform carries it that far.
		glm::vec3 GroundDisplacement = glm::vec3(0.0f);
	};

	struct RaycastHit
	{
		UUID Entity = UUID::Invalid();
		glm::vec3 Point = glm::vec3(0.0f);
		glm::vec3 Normal = glm::vec3(0.0f);
		float Distance = 0.0f;
		// The physics material of the surface hit (the collider's, or the submesh's of a triangle mesh's triangle): its
		// ColliderSurface::Material, none when it has its own values.
		AssetHandle Material;
	};

	enum class ContactEventType : uint8_t
	{
		CollisionEnter = 0,
		CollisionExit,
		TriggerEnter,
		TriggerExit
	};

	// The contact points an event carries at most (ContactEvent::Points): the strongest when it is offered more. Jolt reduces
	// a manifold found by its step to at most four points before it reports it (PhysicsSystem.cpp's PruneContactPoints of
	// the pinned Jolt), but not one found by the sweep of a body with continuous collision, nor several manifolds together.
	inline constexpr uint32_t MaxContactPoints = 4;

	// Where and how two bodies touch as their contact begins, seen from one side ("this" body; the other is the other side
	// of the event). Taken from Jolt's contact manifold when the contact is added: before the step resolves it, or for a
	// contact found by a continuous collision sweep, after the step's velocity solve (with the velocities it left).
	struct ContactPoint
	{
		// World space: halfway between the two surfaces' points, where Jolt's solver applies its impulse. A contact found
		// before the bodies touch (Jolt's speculative contacts, closer than 2 cm) has it in the gap, one found inside
		// (a fast body that went in during its last step) inside both.
		glm::vec3 Point = glm::vec3(0.0f);
		// Unit length, from the other body towards this one: the way this one is pushed.
		glm::vec3 Normal = glm::vec3(0.0f);
		// The impulse along the normal at this point (newton seconds, kg m/s, at least 0) that stops the bodies' approach,
		// and with the pair's restitution bounces them apart, estimated by Jolt (EstimateCollisionResponse) from the two
		// bodies alone: what the collision takes in all, also when Jolt spreads it over two steps. Inaccurate when more
		// than two bodies collide at once; 0 for a trigger contact and a pair without a dynamic body.
		float Impulse = 0.0f;
		// This body's velocity at the point minus the other's (meters per second), as the contact began: the bodies
		// approach when it points against Normal.
		glm::vec3 RelativeVelocity = glm::vec3(0.0f);
		// The physics materials of the surfaces that touch there, this side's and the other's: the ColliderSurface::Material
		// of the collider, or of a triangle mesh's submesh; none for a collider's own values and a character's inner body.
		AssetHandle Material;
		AssetHandle OtherMaterial;

		bool operator==(ContactPoint const& other) const = default;
	};

	// The same point seen from the other side: the normal and the relative velocity reversed, the materials exchanged.
	inline ContactPoint ReverseContactPoint(ContactPoint const& point)
	{
		ContactPoint reversed;
		reversed.Point = point.Point;
		reversed.Normal = -point.Normal;
		reversed.Impulse = point.Impulse;
		reversed.RelativeVelocity = -point.RelativeVelocity;
		reversed.Material = point.OtherMaterial;
		reversed.OtherMaterial = point.Material;
		return reversed;
	}

	// Two entities started or stopped touching (collision) or overlapping (when either collider is a trigger).
	struct ContactEvent
	{
		ContactEventType Type = ContactEventType::CollisionEnter;
		// The two entities, in the order Jolt reports their bodies (the lower body ID first).
		UUID First = UUID::Invalid();
		UUID Second = UUID::Invalid();
		// The physics world's step it happened in, counting the world's steps from 1 (PhysicsScene::GetStepCount as that
		// step ends); the exits of a body removed between steps have the number of the step before. A scene's world steps
		// once in each of its fixed steps from its start, so this is the scene's fixed step count as that step ends.
		uint64_t Step = 0;
		// Enter events: the points where the contact began, seen from First (ReverseContactPoint gives Second's view): those
		// of every manifold (one pair of sub-shapes, one normal) with which the pair's contact began in that step, the
		// MaxContactPoints strongest, strongest first: by Impulse from the largest, then by Point's x, y and z from the
		// smallest. Exits carry none (Jolt reports no points when contacts end), and neither does a contact that sleeping
		// bodies resume on waking, which makes no event.
		uint32_t PointCount = 0;
		std::array<ContactPoint, MaxContactPoints> Points{};

		bool operator==(ContactEvent const& other) const = default;
	};

	struct BodyTransform
	{
		UUID Entity = UUID::Invalid();
		glm::vec3 Position = glm::vec3(0.0f);
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	};
}
