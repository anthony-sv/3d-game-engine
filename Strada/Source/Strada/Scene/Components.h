#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/UUID.h"
#include "Strada/Math/Math.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Renderer/TextLayout.h"
#include "Strada/Script/ScriptField.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Component data. Components are plain, copyable structs with PascalCase public fields; every component is registered
// in ComponentRegistry (see ComponentTraits.h for serialized names and fields). Angles are in degrees unless noted.

namespace Strada
{
	struct IDComponent
	{
		UUID ID;
	};

	struct TagComponent
	{
		std::string Tag = "Entity";
	};

	// Transform relative to the parent entity (or the world for root entities).
	struct TransformComponent
	{
		glm::vec3 Translation = {0.0f, 0.0f, 0.0f};
		glm::quat Rotation = {1.0f, 0.0f, 0.0f, 0.0f};
		glm::vec3 Scale = {1.0f, 1.0f, 1.0f};

		glm::mat4 GetTransform() const { return Math::ComposeTransform(Translation, Rotation, Scale); }
		glm::vec3 GetRotationEuler() const { return Math::QuaternionToEulerDegrees(Rotation); }
		void SetRotationEuler(glm::vec3 const& degrees) { Rotation = Math::EulerDegreesToQuaternion(degrees); }
	};

	// Hierarchy links. Children are kept in sibling order. Maintained by Scene (use Scene::SetParent to change it).
	struct RelationshipComponent
	{
		UUID Parent = UUID::Invalid();
		std::vector<UUID> Children;
	};

	enum class ProjectionType : uint8_t
	{
		Perspective = 0,
		Orthographic
	};

	struct CameraComponent
	{
		ProjectionType Projection = ProjectionType::Perspective;
		// Vertical field of view in degrees.
		float PerspectiveFOV = 60.0f;
		float PerspectiveNear = 0.1f;
		// Maximum view distance for culling and shadows (the projection itself uses an infinite far plane).
		float PerspectiveFar = 1000.0f;
		// Full view height in world units.
		float OrthographicSize = 10.0f;
		float OrthographicNear = -100.0f;
		float OrthographicFar = 100.0f;
		// The scene renders from the first primary camera.
		bool Primary = true;
		// Keeps AspectRatio instead of following the viewport.
		bool FixedAspectRatio = false;
		float AspectRatio = 16.0f / 9.0f;
	};

	struct MeshComponent
	{
		AssetHandle Mesh;
		// Per-submesh material overrides; an invalid handle (or a missing entry) uses the mesh's own material.
		std::vector<AssetHandle> Materials;
		bool CastShadows = true;
		bool Visible = true;
	};

	// Shines along the entity's forward direction (-Z).
	struct DirectionalLightComponent
	{
		glm::vec3 Color = {1.0f, 1.0f, 1.0f};
		float Intensity = 3.0f;
		bool CastShadows = true;
		// Apparent angular diameter of the light source in degrees; larger values give softer shadows.
		float LightSize = 0.5f;
	};

	struct PointLightComponent
	{
		glm::vec3 Color = {1.0f, 1.0f, 1.0f};
		float Intensity = 10.0f;
		// Distance at which the light's contribution reaches zero.
		float Range = 10.0f;
		bool CastShadows = false;
	};

	// Shines along the entity's forward direction (-Z).
	struct SpotLightComponent
	{
		glm::vec3 Color = {1.0f, 1.0f, 1.0f};
		float Intensity = 10.0f;
		float Range = 10.0f;
		// Half-angles of the full-intensity cone and the falloff cone, in degrees.
		float InnerConeAngle = 20.0f;
		float OuterConeAngle = 30.0f;
		bool CastShadows = false;
	};

	// Image-based lighting and sky from an HDR environment map.
	struct SkyLightComponent
	{
		AssetHandle Environment;
		float Intensity = 1.0f;
		// Rotation of the environment around the world Y axis in degrees.
		float Rotation = 0.0f;
		// 0 = sharp sky, 1 = fully blurred (uses the prefiltered environment).
		float SkyboxBlur = 0.0f;
		bool DrawSkybox = true;
		// Ambient light used when no environment map is set.
		glm::vec3 AmbientColor = {0.03f, 0.03f, 0.03f};
	};

	struct RigidBodyComponent
	{
		RigidBodyType Type = RigidBodyType::Static;
		float Mass = 1.0f;
		float LinearDamping = 0.05f;
		float AngularDamping = 0.05f;
		float GravityFactor = 1.0f;
		// Physics layer index (0-15) as configured in the project settings.
		uint32_t Layer = 0;
		glm::bvec3 LockTranslation = {false, false, false};
		glm::bvec3 LockRotation = {false, false, false};
		bool ContinuousCollision = false;
		bool AllowSleep = true;
		// When play starts, in meters and radians per second.
		glm::vec3 InitialLinearVelocity = {0.0f, 0.0f, 0.0f};
		glm::vec3 InitialAngularVelocity = {0.0f, 0.0f, 0.0f};
	};

	struct BoxColliderComponent
	{
		glm::vec3 HalfExtents = {0.5f, 0.5f, 0.5f};
		glm::vec3 Offset = {0.0f, 0.0f, 0.0f};
		bool IsTrigger = false;
		float Friction = 0.6f;
		float Restitution = 0.0f;
	};

	struct SphereColliderComponent
	{
		float Radius = 0.5f;
		glm::vec3 Offset = {0.0f, 0.0f, 0.0f};
		bool IsTrigger = false;
		float Friction = 0.6f;
		float Restitution = 0.0f;
	};

	// Capsule along the local Y axis; HalfHeight excludes the hemispherical caps.
	struct CapsuleColliderComponent
	{
		float Radius = 0.5f;
		float HalfHeight = 0.5f;
		glm::vec3 Offset = {0.0f, 0.0f, 0.0f};
		bool IsTrigger = false;
		float Friction = 0.6f;
		float Restitution = 0.0f;
	};

	struct MeshColliderComponent
	{
		// Collision mesh; an invalid handle uses the entity's MeshComponent mesh.
		AssetHandle Mesh;
		// Convex hulls can move (dynamic bodies); triangle meshes are only valid for static and kinematic bodies.
		bool Convex = true;
		bool IsTrigger = false;
		float Friction = 0.6f;
		float Restitution = 0.0f;
	};

	// Joins this entity's body to another entity's body, or to the world, while the scene runs (both need a RigidBody and
	// a collider; a connected entity that no longer exists leaves the joint out). The joint is made where the bodies are
	// when the scene starts, and again whenever the joint or either body changes.
	struct JointComponent
	{
		JointType Type = JointType::Hinge;
		// None joins the body to the world.
		UUID ConnectedEntity = UUID::Invalid();
		// Where the joint is, in this entity's local space.
		glm::vec3 Anchor = {0.0f, 0.0f, 0.0f};
		// Distance joints: the point kept at a distance, in the connected entity's local space (world space without one).
		glm::vec3 ConnectedAnchor = {0.0f, 0.0f, 0.0f};
		// The hinge or slider axis in this entity's local space.
		glm::vec3 Axis = {0.0f, 1.0f, 0.0f};
		bool LimitsEnabled = false;
		// Hinges: degrees (MinLimit -180 to 0, MaxLimit 0 to 180); sliders: meters.
		float MinLimit = -45.0f;
		float MaxLimit = 45.0f;
		// Distance joints: negative values keep the distance the bodies have when the joint is made.
		float MinDistance = -1.0f;
		float MaxDistance = -1.0f;
		bool MotorEnabled = false;
		// Hinges: degrees per second; sliders: meters per second.
		float MotorSpeed = 90.0f;
		// The most torque (hinges, newton meters) or force (sliders, newtons) the motor uses.
		float MotorMaxForce = 1000.0f;
	};

	// Makes the entity a character while the scene runs (the player, walking creatures): an upright capsule that scripts
	// move, which slides along walls, climbs steps and slopes up to its limits, slides down steeper slopes, falls with
	// gravity, rides what it stands on and pushes dynamic bodies; the entity's transform follows it. The capsule is scaled
	// with the entity but does not turn with it. It replaces the entity's colliders and RigidBody; colliders of child
	// entities are the character's own parts (a held object, a trigger around it), which neither block it nor touch it.
	struct CharacterControllerComponent
	{
		float Radius = 0.5f;
		// Of the whole capsule, both rounded ends included; heights below twice the radius make a sphere.
		float Height = 2.0f;
		// Center of the capsule relative to the entity's origin.
		glm::vec3 Offset = {0.0f, 0.0f, 0.0f};
		// The highest step it climbs, and the deepest drop it walks down without leaving the ground.
		float StepHeight = 0.3f;
		// The steepest slope it walks up; it slides down steeper ones.
		float MaxSlopeAngle = 45.0f;
		// Kilograms with which it weighs on what it stands on.
		float Mass = 70.0f;
		// The largest force with which it pushes dynamic bodies, in newtons.
		float PushForce = 200.0f;
		float GravityFactor = 1.0f;
		// Physics layer index (0-15) as configured in the project settings.
		uint32_t Layer = 0;
	};

	struct AudioSourceComponent
	{
		AssetHandle Clip;
		float Volume = 1.0f;
		float Pitch = 1.0f;
		bool Loop = false;
		bool PlayOnStart = false;
		// 3D positional sound; when false the sound plays at a constant level.
		bool Spatial = true;
		float MinDistance = 1.0f;
		float MaxDistance = 100.0f;
	};

	struct AudioListenerComponent
	{
		bool Active = true;
	};

	struct ScriptComponent
	{
		// Fully qualified C# class name (e.g. "Game.PlayerController").
		std::string ClassName;
		ScriptFieldMap Fields;
	};

	// Screen-space text is positioned with the entity's translation in normalized viewport coordinates (x and y in
	// [0, 1], origin top-left; z orders screen-space items, higher on top) and sized by FontSize in pixels; world-space
	// text lies in the entity's XY plane with FontSize world units per line. The origin is the top of the first line,
	// where lines start, are centered or end. Text is unlit; an invalid or unloadable font uses the default font.
	struct TextComponent
	{
		std::string Text;
		AssetHandle Font;
		glm::vec4 Color = {1.0f, 1.0f, 1.0f, 1.0f};
		// One world unit per line; screen-space text needs a size in pixels (around 16-64).
		float FontSize = 1.0f;
		bool ScreenSpace = false;
		TextAlignment Alignment = TextAlignment::Left;
		float LineSpacing = 1.0f;
	};

	// Unlit textured or colored quad: the unit quad in the entity's XY plane, or on the screen following the TextComponent
	// placement rules (scale in pixels).
	struct SpriteRendererComponent
	{
		glm::vec4 Color = {1.0f, 1.0f, 1.0f, 1.0f};
		AssetHandle Texture;
		float Tiling = 1.0f;
		bool ScreenSpace = false;
	};

	// Where a particle system's particles start.
	enum class ParticleEmitterShape : uint8_t
	{
		// At the entity's position.
		Point = 0,
		// Inside a sphere of Radius, moving outwards.
		Sphere,
		// On a disc of Radius, moving along the entity's forward axis (-Z) spread by ConeAngle.
		Cone,
		// Inside a box of BoxHalfExtents, moving along -Z spread by ConeAngle.
		Box
	};

	// How particles combine with what is behind them.
	enum class ParticleBlendMode : uint8_t
	{
		// Light adds up (fire, sparks, magic); bright colors bloom.
		Additive = 0,
		// Covers what is behind by the particle's alpha (smoke, dust).
		Alpha
	};

	// Emits camera-facing particles while the scene runs (and in the editor's preview): each lives between LifetimeMin and
	// LifetimeMax seconds, starts with a speed between SpeedMin and SpeedMax along the shape's direction, falls with the
	// scene's gravity times GravityFactor, slows down with Drag, and blends from the start to the end color and size.
	struct ParticleSystemComponent
	{
		bool Emitting = true;
		// Particles per second while emitting.
		float Rate = 20.0f;
		// Particles emitted at once when the system starts (with the scene, or when the entity is created).
		uint32_t Burst = 0;
		uint32_t MaxParticles = 1000;
		float LifetimeMin = 1.0f;
		float LifetimeMax = 2.0f;
		float SpeedMin = 1.0f;
		float SpeedMax = 3.0f;
		ParticleEmitterShape Shape = ParticleEmitterShape::Cone;
		// Degrees from the forward axis.
		float ConeAngle = 25.0f;
		float Radius = 0.1f;
		glm::vec3 BoxHalfExtents = {0.5f, 0.5f, 0.5f};
		float GravityFactor = 0.0f;
		// Fraction of the velocity lost per second.
		float Drag = 0.0f;
		float StartSize = 0.2f;
		float EndSize = 0.05f;
		// Linear colors with straight alpha.
		glm::vec4 StartColor = {1.0f, 0.75f, 0.35f, 1.0f};
		glm::vec4 EndColor = {1.0f, 0.2f, 0.05f, 0.0f};
		// Multiplies the colors; above 1 particles glow with bloom.
		float Intensity = 1.0f;
		// None uses a soft round dot.
		AssetHandle Texture;
		ParticleBlendMode BlendMode = ParticleBlendMode::Additive;
		// Particles stay where they were emitted when the entity moves; otherwise they move with it.
		bool WorldSpace = true;
		// The entity is destroyed once it no longer emits and its last particle died (one-shot effects).
		bool DestroyWhenFinished = false;
	};

	// Links an entity to the prefab it was instantiated from.
	struct PrefabComponent
	{
		AssetHandle Prefab;
		// UUID of the corresponding entity inside the prefab file.
		UUID SourceEntity = UUID::Invalid();
	};
}
