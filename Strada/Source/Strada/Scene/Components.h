#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/UUID.h"
#include "Strada/Math/Math.h"
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

	enum class RigidBodyType : uint8_t
	{
		Static = 0,
		Dynamic,
		Kinematic
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

	enum class TextAlignment : uint8_t
	{
		Left = 0,
		Center,
		Right
	};

	// Screen-space text is positioned with the entity's translation in normalized viewport coordinates (x and y in
	// [0, 1], origin top-left) and sized by FontSize in pixels; world-space text uses the full transform and FontSize in
	// world units per line.
	struct TextComponent
	{
		std::string Text;
		AssetHandle Font;
		glm::vec4 Color = {1.0f, 1.0f, 1.0f, 1.0f};
		float FontSize = 32.0f;
		bool ScreenSpace = false;
		TextAlignment Alignment = TextAlignment::Left;
		float LineSpacing = 1.0f;
	};

	// Textured or colored quad. Screen-space sprites follow the TextComponent placement rules (scale in pixels).
	struct SpriteRendererComponent
	{
		glm::vec4 Color = {1.0f, 1.0f, 1.0f, 1.0f};
		AssetHandle Texture;
		float Tiling = 1.0f;
		bool ScreenSpace = false;
	};

	// Links an entity to the prefab it was instantiated from.
	struct PrefabComponent
	{
		AssetHandle Prefab;
		// UUID of the corresponding entity inside the prefab file.
		UUID SourceEntity = UUID::Invalid();
	};
}
