#pragma once

#include "Strada/Asset/AssetHandle.h"
#include "Strada/Core/UUID.h"
#include "Strada/Math/Math.h"
#include "Strada/Physics/PhysicsTypes.h"
#include "Strada/Renderer/SceneRendererSettings.h"
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

	// A post-process volume: changes the scene's post-processing (its renderer settings) where the camera is. Each setting
	// applies only while its Override flag is set; the values default to the scene settings' defaults. Global volumes
	// apply everywhere. Local volumes apply inside their box, the unit cube placed, rotated and scaled by the entity (so
	// the entity's scale is the box's size in meters), and fade out over BlendDistance meters around it. Volumes apply in
	// order of Priority, lowest first (equal priorities in hierarchy order), each scaled by its Weight: numbers move towards
	// the volume's values by the volume's blend, Bloom fades in or out through the bloom's intensity, and AmbientOcclusion
	// and Tonemapper take the volume's value once the blend reaches one half.
	struct PostProcessComponent
	{
		bool Global = true;
		// Meters around a local volume's box over which it fades out.
		float BlendDistance = 1.0f;
		// Volumes with higher priorities apply later, over those with lower ones.
		float Priority = 0.0f;
		// How much of the volume applies, 0 to 1.
		float Weight = 1.0f;

		bool OverrideEV100 = false;
		float EV100 = SceneRendererSettings{}.EV100;
		bool OverrideTonemapper = false;
		TonemapOperator Tonemapper = SceneRendererSettings{}.Tonemapper;
		bool OverrideAmbientOcclusion = false;
		bool AmbientOcclusion = SceneRendererSettings{}.AmbientOcclusion;
		bool OverrideAmbientOcclusionIntensity = false;
		float AmbientOcclusionIntensity = SceneRendererSettings{}.AmbientOcclusionIntensity;
		bool OverrideBloom = false;
		bool Bloom = SceneRendererSettings{}.Bloom;
		bool OverrideBloomIntensity = false;
		float BloomIntensity = SceneRendererSettings{}.BloomIntensity;
		bool OverrideContrast = false;
		float Contrast = SceneRendererSettings{}.Contrast;
		bool OverrideSaturation = false;
		float Saturation = SceneRendererSettings{}.Saturation;
		bool OverrideTemperature = false;
		float Temperature = SceneRendererSettings{}.Temperature;
		bool OverrideTint = false;
		float Tint = SceneRendererSettings{}.Tint;
		bool OverrideColorFilter = false;
		glm::vec3 ColorFilter = SceneRendererSettings{}.ColorFilter;
		bool OverrideChromaticAberration = false;
		float ChromaticAberration = SceneRendererSettings{}.ChromaticAberration;
		bool OverrideVignette = false;
		float Vignette = SceneRendererSettings{}.Vignette;
		bool OverrideVignetteSmoothness = false;
		float VignetteSmoothness = SceneRendererSettings{}.VignetteSmoothness;
		bool OverrideFilmGrain = false;
		float FilmGrain = SceneRendererSettings{}.FilmGrain;
	};

	// How a UI canvas's units relate to the screen's pixels.
	enum class UIScaleMode : uint8_t
	{
		// A canvas unit is a pixel: the UI keeps its size in pixels on every screen.
		ConstantPixelSize = 0,
		// The canvas is ReferenceResolution.y units high on every screen: the UI grows and shrinks with the screen's height.
		ScaleWithScreenHeight
	};

	// The root of a screen-space UI (HUDs, menus), drawn over the rendered scene after screen-space sprites and text. The
	// canvas covers the screen and its descendants are the UI, drawn in hierarchy order: children over their parents, later
	// siblings over earlier ones. Descendants with a UIRect are laid out in their parent's rectangle, the others take their
	// parent's rectangle. The Transform of UI entities, and a UIRect on the canvas entity itself, are not used.
	struct UICanvasComponent
	{
		UIScaleMode ScaleMode = UIScaleMode::ScaleWithScreenHeight;
		// The screen size the UI is designed for, in canvas units (ScaleWithScreenHeight fits its height to the screen's).
		glm::vec2 ReferenceResolution = {1920.0f, 1080.0f};
	};

	// A rectangle of the screen-space UI, placed in its parent's rectangle (the screen for the canvas's children) in canvas
	// units, x to the right and y downwards. On each axis the anchors mark a span of the parent (0 is its left or top edge, 1
	// its right or bottom edge): the rectangle is Size larger than the span, so equal anchors give it exactly Size, and its
	// pivot sits at the pivot's point of the span, moved by Position.
	struct UIRectComponent
	{
		glm::vec2 AnchorMin = {0.5f, 0.5f};
		glm::vec2 AnchorMax = {0.5f, 0.5f};
		glm::vec2 Position = {0.0f, 0.0f};
		glm::vec2 Size = {200.0f, 60.0f};
		// The point of the rectangle that Position places, from its top-left corner (0, 0) to its bottom-right corner (1, 1).
		glm::vec2 Pivot = {0.5f, 0.5f};
		// Hidden rectangles are neither drawn nor clicked, and neither are their descendants.
		bool Visible = true;
		// Multiplies the alpha of the element and of its descendants (fades).
		float Opacity = 1.0f;
	};

	// Fills its UI element's rectangle with a color or a texture, optionally with rounded corners. A UIButton on the entity
	// colors it by its state. Images take the clicks over the UI they cover.
	struct UIImageComponent
	{
		// Linear color with straight alpha, multiplied with the texture.
		glm::vec4 Color = {1.0f, 1.0f, 1.0f, 1.0f};
		// Sampled as sRGB color; none fills the rectangle with the color.
		AssetHandle Texture;
		// Draws the texture at its own aspect ratio: the largest rectangle of that shape centered in the element's.
		bool PreserveAspect = false;
		// In canvas units; at most half the rectangle's shorter side.
		float CornerRadius = 0.0f;
	};

	// Text in its UI element's rectangle: lines wrap at the rectangle's width and the text block is aligned in it. An invalid
	// or unloadable font uses the default font.
	struct UITextComponent
	{
		std::string Text;
		AssetHandle Font;
		glm::vec4 Color = {1.0f, 1.0f, 1.0f, 1.0f};
		// Line height in canvas units.
		float FontSize = 32.0f;
		TextAlignment Alignment = TextAlignment::Center;
		TextVerticalAlignment VerticalAlignment = TextVerticalAlignment::Middle;
		// Breaks lines that do not fit the rectangle's width at spaces (or inside words longer than a line).
		bool Wrap = true;
		float LineSpacing = 1.0f;
	};

	// Makes its UI element a button while the scene runs: the mouse hovers and presses it, and a click (the left button
	// pressed and released over it) sets WasClicked for the frame and calls OnClick on the scripts of the button and of its
	// ancestors. Its colors replace the UIImage's color in each state.
	struct UIButtonComponent
	{
		// A disabled button takes no clicks (and still covers the UI below it).
		bool Interactable = true;
		// Linear colors with straight alpha.
		glm::vec4 NormalColor = {0.022f, 0.1473f, 0.7106f, 1.0f};
		glm::vec4 HoveredColor = {0.055f, 0.2429f, 0.9331f, 1.0f};
		glm::vec4 PressedColor = {0.01f, 0.0732f, 0.3931f, 1.0f};
		glm::vec4 DisabledColor = {0.1065f, 0.1193f, 0.1473f, 0.6f};
	};

	// Links an entity to the prefab it was instantiated from.
	struct PrefabComponent
	{
		AssetHandle Prefab;
		// UUID of the corresponding entity inside the prefab file.
		UUID SourceEntity = UUID::Invalid();
	};
}
