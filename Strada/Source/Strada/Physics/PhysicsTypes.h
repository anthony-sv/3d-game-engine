#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Asset/PhysicsMaterial.h"
#include "Strada/Asset/Vehicle.h"
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
	struct Curve;

	// Physics layers a project can define (RigidBodyComponent::Layer indexes them).
	inline constexpr size_t MaxPhysicsLayers = 16;
	// A query's layer mask that sees every layer (PhysicsQueryFilter::LayerMask; bit i is layer i).
	inline constexpr uint32_t AllPhysicsLayers = 0xFFFFFFFFu;
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
	// The farthest a query reaches (PhysicsScene's RaycastAll and ShapeCast): farther than any two points of the world are
	// apart (positions within MaxPhysicsCoordinate on each axis, colliders within MaxPhysicsExtent of them), so a longer
	// distance is cut to it without losing a hit.
	inline constexpr float MaxPhysicsQueryDistance = 4.0e7f;
	// The largest sizes of a query's shape (PhysicsQueryShape), far below MaxPhysicsExtent: a cast runs Jolt's GJK on the moving
	// shape's core (a box whole, a capsule's segment) and adds its radius after, and casts of longer cores and wider capsules
	// lose its precision. Against closed forms over 2,000 random poses of each shape, cores of 20 m and radii of 100 m stay
	// within 0.8 mm of where they touch; cores of 100 m and capsules of radius 1000 m miss by up to 3 cm, and cores of 300 m
	// and more touch bodies they pass by (Architecture.md section 8, "Queries").
	inline constexpr float MaxPhysicsQueryExtent = 20.0f;
	inline constexpr float MaxPhysicsQueryRadius = 100.0f;

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
		// The material's values that vehicles alone read (PhysicsMaterial::RollingResistance, TyreGrip; at least 0, clamped
		// when the body is made): a collider's own values roll without resistance and keep the tyres' grip.
		float RollingResistance = 0.0f;
		float TyreGrip = 1.0f;

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

	// What a query sees (PhysicsScene's RaycastAll, ShapeCast and Overlap).
	struct PhysicsQueryFilter
	{
		// Bit i sees the bodies and characters on layer i; the bits past MaxPhysicsLayers see nothing, and 0 sees nothing.
		uint32_t LayerMask = AllPhysicsLayers;
		// Whether trigger colliders count. Without, a query passes through them, as Raycast does.
		bool IncludeTriggers = false;
	};

	enum class PhysicsQueryShapeType : uint8_t
	{
		Sphere = 0,
		Box,
		// Along the local Y axis, as a capsule collider.
		Capsule
	};

	// The shape a cast sweeps or an overlap tests, placed by the query's position and rotation (a sphere ignores the
	// rotation). Exact: a box keeps its sharp edges and corners. A size below a millimeter counts as a millimeter, and a
	// capsule whose HalfHeight is below a millimeter is a sphere; a size that is negative or not finite, a radius beyond
	// MaxPhysicsQueryRadius or a half extent or half height beyond MaxPhysicsQueryExtent makes the query find nothing.
	struct PhysicsQueryShape
	{
		PhysicsQueryShapeType Type = PhysicsQueryShapeType::Sphere;
		// Spheres and capsules.
		float Radius = 0.5f;
		// Boxes.
		glm::vec3 HalfExtents = glm::vec3(0.5f);
		// Capsules: half the length of the cylinder between the caps.
		float HalfHeight = 0.5f;

		static PhysicsQueryShape MakeSphere(float radius)
		{
			PhysicsQueryShape shape;
			shape.Type = PhysicsQueryShapeType::Sphere;
			shape.Radius = radius;
			return shape;
		}

		static PhysicsQueryShape MakeBox(glm::vec3 const& halfExtents)
		{
			PhysicsQueryShape shape;
			shape.Type = PhysicsQueryShapeType::Box;
			shape.HalfExtents = halfExtents;
			return shape;
		}

		static PhysicsQueryShape MakeCapsule(float radius, float halfHeight)
		{
			PhysicsQueryShape shape;
			shape.Type = PhysicsQueryShapeType::Capsule;
			shape.Radius = radius;
			shape.HalfHeight = halfHeight;
			return shape;
		}
	};

	// What a shape cast touches first (PhysicsScene::ShapeCast).
	struct ShapeCastHit
	{
		UUID Entity = UUID::Invalid();
		// World space: where the shape touches the collider hit, on that collider's surface; for a collider the shape
		// overlapped where it started, the deepest point of the overlap.
		glm::vec3 Point = glm::vec3(0.0f);
		// Unit length, out of the collider hit towards the shape: the way that pushes the shape off it. On a face it is the
		// face's normal; on an edge or a corner it points from there towards the shape.
		glm::vec3 Normal = glm::vec3(0.0f);
		// How far the shape travelled along the cast's direction until it touched, in meters: its center is then at origin +
		// direction * Distance. 0 for a collider the shape overlapped where it started.
		float Distance = 0.0f;
		// The physics material of the surface hit, as RaycastHit::Material.
		AssetHandle Material;
	};

	// The order of a query's hits (RaycastAll's, and which hit ShapeCast reports): by distance from the nearest, then by
	// entity UUID from the smallest. A query reports at most one hit per entity, so two different hits never tie.
	inline bool IsQueryHitBefore(float distance, UUID entity, float otherDistance, UUID otherEntity)
	{
		if (distance != otherDistance)
		{
			return distance < otherDistance;
		}
		return entity.GetValue() < otherEntity.GetValue();
	}

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
		// World space: halfway between the two surfaces' points (Jolt's solver applies its impulse to each body at that
		// body's own point). A contact found before the bodies touch (Jolt's speculative contacts, closer than 2 cm) has it
		// in the gap, one found inside (a fast body that went in during its last step) inside both.
		glm::vec3 Point = glm::vec3(0.0f);
		// Unit length, from the other body towards this one: the way this one is pushed.
		glm::vec3 Normal = glm::vec3(0.0f);
		// The impulse along the normal at this point (newton seconds, kg m/s, at least 0) that stops the bodies' approach,
		// and with the pair's restitution bounces them apart, estimated by Jolt (EstimateCollisionResponse) for the points of
		// this point's manifold together, from the two bodies alone: what the collision takes in all, also when Jolt spreads
		// it over two steps. Each manifold is estimated as if it alone stopped the pair, so when a contact begins on several
		// manifolds in one step their impulses are not shared out: for a body that comes down evenly on two, too large when
		// their normals are less than 90 degrees apart (a ball landing in a groove whose faces rise at 30 degrees: m v cos 30
		// on each face, where solved together each takes m v / (2 cos 30)), too small when more than 90 apart, exact at right
		// angles. Inaccurate as well when more than two bodies collide at once; 0 for a trigger contact and a pair without a
		// dynamic body.
		float Impulse = 0.0f;
		// This body's velocity at the point minus the other's (meters per second), as the contact began: the bodies
		// approach when it points against Normal.
		glm::vec3 RelativeVelocity = glm::vec3(0.0f);
		// The physics materials of the surfaces that touch there, this side's and the other's: the ColliderSurface::Material
		// of the collider, or of a triangle mesh's submesh; none for a collider's own values and a character's inner body.
		// A manifold names one sub-shape per side, so when Jolt merges the contacts of neighbouring sub-shapes with similar
		// normals into one (manifold reduction), all its points name the surfaces of the sub-shapes its contact started on.
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

	// The order of an event's points (ContactEvent::Points): whether a goes before b. The strongest first, by Impulse from
	// the largest; then by Point, Normal and RelativeVelocity, each by x, y and z from the smallest; then by the UUIDs of
	// Material and OtherMaterial from the smallest. The key is every value of a point, so only equal points tie, and the
	// order never depends on the order in which Jolt reports points.
	inline bool IsContactPointBefore(ContactPoint const& a, ContactPoint const& b)
	{
		if (a.Impulse != b.Impulse)
		{
			return a.Impulse > b.Impulse;
		}
		// -1, 0 or 1 as left is smaller than, equal to or larger than right: by x, then y, then z.
		auto const compare = [](glm::vec3 const& left, glm::vec3 const& right)
		{
			for (glm::length_t axis = 0; axis < 3; axis++)
			{
				if (left[axis] != right[axis])
				{
					return left[axis] < right[axis] ? -1 : 1;
				}
			}
			return 0;
		};
		if (int const byPoint = compare(a.Point, b.Point); byPoint != 0)
		{
			return byPoint < 0;
		}
		if (int const byNormal = compare(a.Normal, b.Normal); byNormal != 0)
		{
			return byNormal < 0;
		}
		if (int const byVelocity = compare(a.RelativeVelocity, b.RelativeVelocity); byVelocity != 0)
		{
			return byVelocity < 0;
		}
		if (a.Material != b.Material)
		{
			return a.Material.GetUUID().GetValue() < b.Material.GetUUID().GetValue();
		}
		return a.OtherMaterial.GetUUID().GetValue() < b.OtherMaterial.GetUUID().GetValue();
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
		// MaxContactPoints strongest, in the order of IsContactPointBefore (by Impulse from the largest, then by the rest of
		// the point's values). Exits carry none (Jolt reports no points when contacts end), and neither does a contact that sleeping
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

	// --- Vehicles (Architecture.md section 8, "Vehicles") ---

	// What the rider asks of a vehicle (PhysicsScene::SetVehicleInputs), applied to each of its steps until replaced. Values
	// outside their ranges are clamped, and values that are not finite are 0.
	struct VehicleInputs
	{
		// 0 to 1.
		float Throttle = 0.0f;
		// 0 to 1 each: the front brake and the rear brake, each applying its torque at each wheel
		// (VehicleWheelDesc::FrontBrakeTorque, RearBrakeTorque).
		float FrontBrake = 0.0f;
		float RearBrake = 0.0f;
		// -1 to 1: Jolt's steering input, which the lean controller turns into a lean. Positive steers, leans and turns the
		// vehicle to its right.
		float Lean = 0.0f;

		bool operator==(VehicleInputs const& other) const = default;
	};

	// A piecewise linear curve's points (x, y), x not decreasing, as Jolt's LinearCurve holds them: before the first point
	// and past the last it keeps their values. Empty is Jolt's default for what the curve sets. At most MaxVehicleCurvePoints,
	// each finite.
	using VehicleCurve = std::vector<glm::vec2>;
	// The most points a VehicleCurve holds (a curve asset sampled at its keys and 15 points between each two).
	inline constexpr size_t MaxVehicleCurvePoints = 16384;

	// The widest wheel whose cast is round across its whole width (meters): a wheel's cast rounds its edges by half its width,
	// but by Jolt's largest convex radius at most (cDefaultConvexRadius, 0.05 m, which VehicleWorld.cpp asserts), so a wider
	// wheel rides on a flat bottom and a motorcycle on it leans less than its turns balance (Scene warns of it).
	inline constexpr float MaxRoundVehicleWheelWidth = 0.1f;

	// A wheel of a vehicle, in the chassis's local space without its scale (meters), angles in radians.
	struct VehicleWheelDesc
	{
		glm::vec3 Position = glm::vec3(0.0f);
		// Normalized when the vehicle is made; zero is refused.
		glm::vec3 SuspensionDirection = glm::vec3(0.0f, -1.0f, 0.0f);
		glm::vec3 SteeringAxis = glm::vec3(0.0f, 1.0f, 0.0f);
		float SuspensionMinLength = 0.3f;
		float SuspensionMaxLength = 0.5f;
		float SuspensionPreload = 0.0f;
		float SuspensionFrequency = 1.5f;
		float SuspensionDamping = 0.5f;
		float MaxSteerAngle = 0.0f;
		float Radius = 0.31f;
		// The width of the cylinder the wheel casts against the ground, round only up to MaxRoundVehicleWheelWidth
		// (VehicleWheelDefinition::Width).
		float Width = 0.05f;
		float Inertia = 0.9f;
		float AngularDamping = 0.2f;
		// Newton meters at full input: the wheel's torque for Jolt's brake input and for its hand-brake input.
		float FrontBrakeTorque = 0.0f;
		float RearBrakeTorque = 0.0f;
		// The tyre's friction over the longitudinal slip ratio, and over the slip angle in degrees.
		VehicleCurve LongitudinalFriction;
		VehicleCurve LateralFriction;

		bool operator==(VehicleWheelDesc const& other) const = default;
	};

	// A motorcycle on an entity's dynamic body (PhysicsScene::AddVehicle): Jolt's motorcycle controller on a vehicle
	// constraint, its front wheel steering and its rear wheel driven through the final drive. The scene makes it from a
	// Vehicle definition (Strada/Asset/Vehicle.h), its angles in radians and its curves sampled.
	struct VehicleDesc
	{
		UUID Entity = UUID::Invalid();
		// Wheels[0] is the front, Wheels[1] the rear.
		std::array<VehicleWheelDesc, VehicleWheelCount> Wheels{};
		// The chassis's up and front in its local space; normalized when the vehicle is made (the front made square to the up),
		// zero or parallel refused.
		glm::vec3 Up = glm::vec3(0.0f, 1.0f, 0.0f);
		glm::vec3 Forward = glm::vec3(0.0f, 0.0f, -1.0f);
		// Radians; pi does not limit them.
		float MaxPitchRoll = 1.0471976f;
		float MaxTorque = 150.0f;
		float MinRPM = 1000.0f;
		float MaxRPM = 10000.0f;
		// The torque's fraction of MaxTorque over the engine's speed divided by MaxRPM, as Jolt reads it
		// (VehicleEngine::GetTorque).
		VehicleCurve NormalizedTorque;
		float EngineInertia = 0.5f;
		float EngineAngularDamping = 0.2f;
		VehicleTransmissionMode TransmissionMode = VehicleTransmissionMode::Automatic;
		// 1 to MaxVehicleGears forward gears, each above 0.
		std::vector<float> GearRatios = {2.27f, 1.63f, 1.3f, 1.09f, 0.96f, 0.88f};
		float FinalDrive = 4.825f;
		float SwitchTime = 0.5f;
		float ClutchReleaseTime = 0.3f;
		float SwitchLatency = 0.5f;
		float ShiftUpRPM = 8000.0f;
		float ShiftDownRPM = 2000.0f;
		float ClutchStrength = 2.0f;
		// Radians.
		float MaxLeanAngle = 0.7853982f;
		float LeanSpringConstant = 5000.0f;
		float LeanSpringDamping = 1000.0f;
		float LeanSmoothingFactor = 0.8f;
		bool LeanSteeringLimit = true;

		bool operator==(VehicleDesc const& other) const = default;
	};

	// A wheel as the last step left it (PhysicsScene::GetVehicleState).
	struct VehicleWheelState
	{
		// What the wheel stands on; invalid without contact. Surface is the physics material of the collider (or submesh)
		// under it, none when that collider has its own values, and TyreGrip and RollingResistance are the values the step
		// used (1 and 0 off a material).
		UUID ContactEntity = UUID::Invalid();
		AssetHandle Surface;
		float TyreGrip = 1.0f;
		float RollingResistance = 0.0f;
		// World space; zero without contact.
		glm::vec3 ContactPoint = glm::vec3(0.0f);
		glm::vec3 ContactNormal = glm::vec3(0.0f);
		// Meters from the attachment to the wheel's center along the suspension.
		float SuspensionLength = 0.0f;
		// Radians per second around the axle (positive rolls forward), the angle it has turned (radians within (-2 pi, 2 pi):
		// Jolt keeps it as the remainder of a division by 2 pi, so its sign is the sign of the turning that brought it there,
		// WheeledVehicleController.cpp:105), and its steering angle (radians, positive to the vehicle's left, as Jolt turns it).
		float AngularVelocity = 0.0f;
		float RotationAngle = 0.0f;
		float SteerAngle = 0.0f;
		// Jolt's slips of the step, both sizes (WheeledVehicleController.cpp:120, :125): the longitudinal slip ratio, 0
		// rolling with the ground, 1 locked, never negative whether the tyre spins or brakes, and the slip angle in radians,
		// 0 to pi / 2 whichever side it slides to; 0 without contact.
		float LongitudinalSlip = 0.0f;
		float LateralSlip = 0.0f;
	};

	// A vehicle as the last step left it, and the inputs it was given since.
	struct VehicleState
	{
		UUID Entity = UUID::Invalid();
		// The latest inputs (SetVehicleInputs), which its next steps apply, and those its last step applied.
		VehicleInputs Inputs;
		VehicleInputs AppliedInputs;
		// Revolutions per minute.
		float EngineSpeed = 0.0f;
		// Newton meters: the torque the engine gives at EngineSpeed with the applied throttle (in Automatic mode times the
		// clutch's friction, as Jolt cuts the throttle while it shifts), from the definition's torque curve. Jolt keeps no
		// torque of its own; this is its VehicleEngine::GetTorque at the state the last step left.
		float EngineTorque = 0.0f;
		// 0 is neutral, 1 the first gear.
		int32_t Gear = 0;
		// 0 open to 1 closed, and whether a gear change is under way (the clutch open, or closing after one).
		float ClutchFriction = 1.0f;
		bool IsShifting = false;
		// Meters per second along the chassis's front; the chassis's lean from the world's up around its front, and the lean
		// controller's target lean (radians, positive to its right).
		float ForwardSpeed = 0.0f;
		float LeanAngle = 0.0f;
		float TargetLeanAngle = 0.0f;
		std::array<VehicleWheelState, VehicleWheelCount> Wheels{};
	};

	// The chassis's up and front as a vehicle uses them (PhysicsScene::AddVehicle): up normalized, and the front made square
	// to it and normalized. False, changing neither, when either is zero or not finite or they are parallel. Pure; any
	// thread: the scene's wheel drawing and the editor's overlay take the axes from it as the world does.
	[[nodiscard]] bool NormalizeVehicleAxes(glm::vec3& up, glm::vec3& forward);

	// The transform of a wheel in its chassis's local space without the chassis's scale, at a suspension length, a steering
	// angle and a rotation angle (radians), for a wheel model whose axle is its local X and whose up is its local Y: what
	// Jolt's VehicleConstraint::GetWheelLocalTransform composes (VehicleConstraint.cpp:127-145 of the pinned Jolt) from the
	// vehicle's up and forward (normalized and square, NormalizeVehicleAxes) and the wheel's normalized suspension direction
	// and steering axis, so that the scene draws a wheel between two steps from the values each step left
	// (VehicleWheelState). PhysicsScene::GetVehicleWheelTransform gives Jolt's own at the last step. Pure; any thread.
	glm::mat4 ComposeVehicleWheelTransform(VehicleWheelDesc const& wheel, glm::vec3 up, glm::vec3 forward, float suspensionLength,
	                                       float steerAngle, float rotationAngle);

	// A vehicle's values from its definition (Strada/Asset/Vehicle.h): the angles in radians, Wheels[0] the definition's
	// first wheel (the front) and Wheels[1] its second (the rear; a wheel the definition lacks keeps VehicleWheelDesc's
	// defaults), and every curve empty, which is Jolt's default: the caller samples the definition's curve assets into
	// LongitudinalFriction, LateralFriction and NormalizedTorque (SampleVehicleCurve). Pure; any thread.
	VehicleDesc MakeVehicleDesc(UUID entity, Vehicle const& definition);

	// The largest value a sampled curve's point keeps (SampleVehicleCurve): a friction coefficient or a fraction of the
	// engine's largest torque far beyond any tyre's or engine's, which keeps Jolt's impulses finite.
	inline constexpr float MaxVehicleCurveValue = 100.0f;

	// A curve asset's keys as a VehicleCurve, Jolt's piecewise linear curve (Docs/Architecture.md §8, "Vehicles"): a point
	// at each key, and 15 points evenly between each two keys of different times (Curve::Evaluate there), so that a cubic
	// segment is followed closely; keys at one time (a jump) keep both values at that time. Each point's x is its key time
	// times inputScale (the engine's torque curve, over revolutions per minute, is read by Jolt over the speed divided by
	// MaxRPM: 1 / MaxRPM), and its y the curve's value within 0 to MaxVehicleCurveValue (a negative friction or torque is
	// none). A curve of one key is that value everywhere, as Jolt's curve keeps its end values; a curve of no key is 0
	// everywhere (one point), as Curve::Evaluate gives. At most MaxVehicleCurvePoints points, which a curve of at most
	// MaxCurveKeys keys never reaches. The curve's extrapolation does not apply. Pure; any thread.
	VehicleCurve SampleVehicleCurve(Curve const& curve, float inputScale = 1.0f);
}
