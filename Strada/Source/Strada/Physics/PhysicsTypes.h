#pragma once

#include "Strada/Core/Base.h"
#include "Strada/Core/UUID.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <vector>

namespace Strada
{
	class MeshSource;

	// Physics layers a project can define (RigidBodyComponent::Layer indexes them).
	inline constexpr size_t MaxPhysicsLayers = 16;

	// Jolt computes in single precision, and its math would overflow long before larger worlds became usable, so the
	// physics world keeps within these limits: positions within MaxPhysicsCoordinate meters of the origin on every axis
	// (bodies beyond stay at the edge), colliders and their offsets at most MaxPhysicsExtent meters (larger bodies are not
	// simulated), masses within [MinPhysicsMass, MaxPhysicsMass] kilograms, forces, torques and impulses at most
	// MaxPhysicsForce, gravity at most MaxPhysicsGravity meters per second squared and gravity factors within
	// ±MaxPhysicsGravityFactor; larger values are clamped. A single step's change of velocity then stays finite, as Jolt
	// requires, and velocities keep within Jolt's limits (500 m/s, 47 rad/s).
	inline constexpr float MaxPhysicsCoordinate = 1.0e7f;
	inline constexpr float MaxPhysicsExtent = 1.0e6f;
	inline constexpr float MinPhysicsMass = 1.0e-4f;
	inline constexpr float MaxPhysicsMass = 1.0e12f;
	inline constexpr float MaxPhysicsForce = 1.0e9f;
	inline constexpr float MaxPhysicsGravity = 1.0e6f;
	inline constexpr float MaxPhysicsGravityFactor = 1.0e3f;

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
		float Friction = 0.6f;
		float Restitution = 0.0f;
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

	struct RaycastHit
	{
		UUID Entity = UUID::Invalid();
		glm::vec3 Point = glm::vec3(0.0f);
		glm::vec3 Normal = glm::vec3(0.0f);
		float Distance = 0.0f;
	};

	enum class ContactEventType : uint8_t
	{
		CollisionEnter = 0,
		CollisionExit,
		TriggerEnter,
		TriggerExit
	};

	// Two entities started or stopped touching (collision) or overlapping (when either collider is a trigger).
	struct ContactEvent
	{
		ContactEventType Type = ContactEventType::CollisionEnter;
		UUID First = UUID::Invalid();
		UUID Second = UUID::Invalid();
	};

	struct BodyTransform
	{
		UUID Entity = UUID::Invalid();
		glm::vec3 Position = glm::vec3(0.0f);
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	};
}
