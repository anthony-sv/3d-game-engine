#pragma once

#include "Strada/Scene/Components.h"
#include "Strada/Script/ScriptFieldSerialization.h"
#include "Strada/Serialization/JsonSerialization.h"
#include "Strada/Serialization/StructSerialization.h"

#include <array>
#include <string_view>
#include <tuple>
#include <utility>

namespace Strada
{
	template<>
	struct EnumTraits<ProjectionType>
	{
		static constexpr std::array<std::pair<ProjectionType, std::string_view>, 2> Values = {{
			{ProjectionType::Perspective, "Perspective"},
			{ProjectionType::Orthographic, "Orthographic"},
		}};
	};

	template<>
	struct EnumTraits<RigidBodyType>
	{
		static constexpr std::array<std::pair<RigidBodyType, std::string_view>, 3> Values = {{
			{RigidBodyType::Static, "Static"},
			{RigidBodyType::Dynamic, "Dynamic"},
			{RigidBodyType::Kinematic, "Kinematic"},
		}};
	};

	template<>
	struct EnumTraits<TextAlignment>
	{
		static constexpr std::array<std::pair<TextAlignment, std::string_view>, 3> Values = {{
			{TextAlignment::Left, "Left"},
			{TextAlignment::Center, "Center"},
			{TextAlignment::Right, "Right"},
		}};
	};

	template<>
	struct EnumTraits<TextVerticalAlignment>
	{
		static constexpr std::array<std::pair<TextVerticalAlignment, std::string_view>, 3> Values = {{
			{TextVerticalAlignment::Top, "Top"},
			{TextVerticalAlignment::Middle, "Middle"},
			{TextVerticalAlignment::Bottom, "Bottom"},
		}};
	};

	template<>
	struct EnumTraits<UIScaleMode>
	{
		static constexpr std::array<std::pair<UIScaleMode, std::string_view>, 2> Values = {{
			{UIScaleMode::ConstantPixelSize, "ConstantPixelSize"},
			{UIScaleMode::ScaleWithScreenHeight, "ScaleWithScreenHeight"},
		}};
	};

	template<>
	struct EnumTraits<ParticleEmitterShape>
	{
		static constexpr std::array<std::pair<ParticleEmitterShape, std::string_view>, 4> Values = {{
			{ParticleEmitterShape::Point, "Point"},
			{ParticleEmitterShape::Sphere, "Sphere"},
			{ParticleEmitterShape::Cone, "Cone"},
			{ParticleEmitterShape::Box, "Box"},
		}};
	};

	template<>
	struct EnumTraits<ParticleBlendMode>
	{
		static constexpr std::array<std::pair<ParticleBlendMode, std::string_view>, 2> Values = {{
			{ParticleBlendMode::Additive, "Additive"},
			{ParticleBlendMode::Alpha, "Alpha"},
		}};
	};

	template<>
	struct EnumTraits<JointType>
	{
		static constexpr std::array<std::pair<JointType, std::string_view>, 5> Values = {{
			{JointType::Fixed, "Fixed"},
			{JointType::Hinge, "Hinge"},
			{JointType::Ball, "Ball"},
			{JointType::Distance, "Distance"},
			{JointType::Slider, "Slider"},
		}};
	};

	enum ComponentFlags : uint32_t
	{
		ComponentFlagsNone = 0,
		// Present on every entity; cannot be added or removed by users (ID, Tag, Transform, Relationship).
		ComponentFlagsCore = ST_BIT(0),
		// Managed by the engine (not shown in "Add Component" menus or editable through generic component commands).
		ComponentFlagsInternal = ST_BIT(1)
	};

	// Specialized for every component:
	//   static constexpr std::string_view Name;          serialized name
	//   static constexpr uint32_t Flags;                 ComponentFlags
	//   static constexpr std::string_view Description;   one sentence for users and agents
	//   static constexpr auto Fields;                    tuple of StructField with hints (see Serialization/StructSerialization.h)
	// Optionally:
	//   static Result<bool> ReadExtraField(std::string_view key, Json const& value, TComponent& component,
	//                                      DeserializationContext const& context);   alternative input keys
	template<typename T>
	struct ComponentTraits;

	template<>
	struct ComponentTraits<IDComponent>
	{
		static constexpr std::string_view Name = "ID";
		static constexpr uint32_t Flags = ComponentFlagsCore | ComponentFlagsInternal;
		static constexpr std::string_view Description = "Unique, persistent ID of the entity.";
		static constexpr auto Fields = std::make_tuple(Field("ID", &IDComponent::ID));
	};

	template<>
	struct ComponentTraits<TagComponent>
	{
		static constexpr std::string_view Name = "Tag";
		static constexpr uint32_t Flags = ComponentFlagsCore;
		static constexpr std::string_view Description = "The entity's name.";
		static constexpr auto Fields = std::make_tuple(Field("Tag", &TagComponent::Tag));
	};

	template<>
	struct ComponentTraits<TransformComponent>
	{
		static constexpr std::string_view Name = "Transform";
		static constexpr uint32_t Flags = ComponentFlagsCore;
		static constexpr std::string_view Description = "Position, rotation and scale relative to the parent entity.";
		static constexpr auto Fields =
			std::make_tuple(Field("Translation", &TransformComponent::Translation).Doc("Position relative to the parent, in meters."),
		                    Field("Rotation", &TransformComponent::Rotation)
		                        .Doc("Orientation relative to the parent as a quaternion [x, y, z, w]; input also accepts RotationEuler, "
		                             "[pitch, yaw, roll] in degrees."),
		                    Field("Scale", &TransformComponent::Scale).Doc("Scale relative to the parent."));

		// "RotationEuler": [pitch, yaw, roll] in degrees is accepted as an alternative to the quaternion.
		static Result<bool> ReadExtraField(std::string_view key, Json const& value, TransformComponent& component,
		                                   DeserializationContext const& context)
		{
			if (key != "RotationEuler")
			{
				return false;
			}
			glm::vec3 degrees(0.0f);
			if (Result<void> result = JsonTraits<glm::vec3>::FromJson(value, degrees, context); !result)
			{
				return Error{result.GetError()};
			}
			component.SetRotationEuler(degrees);
			return true;
		}
	};

	template<>
	struct ComponentTraits<RelationshipComponent>
	{
		static constexpr std::string_view Name = "Relationship";
		static constexpr uint32_t Flags = ComponentFlagsCore | ComponentFlagsInternal;
		static constexpr std::string_view Description = "Parent and children in the hierarchy.";
		static constexpr auto Fields =
			std::make_tuple(Field("Parent", &RelationshipComponent::Parent).Doc("The parent entity; \"0\" for root entities."),
		                    Field("Children", &RelationshipComponent::Children).Doc("The child entities in sibling order."));
	};

	template<>
	struct ComponentTraits<CameraComponent>
	{
		static constexpr std::string_view Name = "Camera";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description = "Renders the scene in play mode and in exported games.";
		static constexpr auto Fields = std::make_tuple(
			Field("Projection", &CameraComponent::Projection),
			Field("PerspectiveFOV", &CameraComponent::PerspectiveFOV).Range(1.0, 179.0).AsAngle().Doc("Vertical field of view in degrees."),
			Field("PerspectiveNear", &CameraComponent::PerspectiveNear).AtLeast(0.001).Doc("Near clip distance in meters."),
			Field("PerspectiveFar", &CameraComponent::PerspectiveFar)
				.AtLeast(0.01)
				.Doc("Maximum view distance for culling and shadows; the projection itself has no far plane."),
			Field("OrthographicSize", &CameraComponent::OrthographicSize).AtLeast(0.001).Doc("Full view height in world units."),
			Field("OrthographicNear", &CameraComponent::OrthographicNear).Doc("Near clip plane distance in meters (may be negative)."),
			Field("OrthographicFar", &CameraComponent::OrthographicFar).Doc("Far clip plane distance in meters."),
			Field("Primary", &CameraComponent::Primary).Doc("The scene renders from the first primary camera."),
			Field("FixedAspectRatio", &CameraComponent::FixedAspectRatio).Doc("Uses AspectRatio instead of following the viewport."),
			Field("AspectRatio", &CameraComponent::AspectRatio).AtLeast(0.01).Doc("Width divided by height when FixedAspectRatio is set."));
	};

	template<>
	struct ComponentTraits<MeshComponent>
	{
		static constexpr std::string_view Name = "Mesh";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description = "Draws a mesh with materials.";
		static constexpr auto Fields =
			std::make_tuple(Field("Mesh", &MeshComponent::Mesh).References("Mesh"),
		                    Field("Materials", &MeshComponent::Materials)
		                        .References("Material")
		                        .Doc("Material overrides per submesh; an empty entry keeps the mesh's own material."),
		                    Field("CastShadows", &MeshComponent::CastShadows), Field("Visible", &MeshComponent::Visible));
	};

	template<>
	struct ComponentTraits<DirectionalLightComponent>
	{
		static constexpr std::string_view Name = "DirectionalLight";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description = "Sun-like light shining along the entity's forward direction (-Z).";
		static constexpr auto Fields =
			std::make_tuple(Field("Color", &DirectionalLightComponent::Color).Range(0.0, 1.0).AsColor(),
		                    Field("Intensity", &DirectionalLightComponent::Intensity).AtLeast(0.0),
		                    Field("CastShadows", &DirectionalLightComponent::CastShadows),
		                    Field("LightSize", &DirectionalLightComponent::LightSize)
		                        .Range(0.0, 20.0)
		                        .AsAngle()
		                        .Doc("Apparent angular diameter of the light source in degrees; larger values give softer shadows."));
	};

	template<>
	struct ComponentTraits<PointLightComponent>
	{
		static constexpr std::string_view Name = "PointLight";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description = "Light shining in every direction from the entity's position.";
		static constexpr auto Fields = std::make_tuple(
			Field("Color", &PointLightComponent::Color).Range(0.0, 1.0).AsColor(),
			Field("Intensity", &PointLightComponent::Intensity).AtLeast(0.0),
			Field("Range", &PointLightComponent::Range).AtLeast(0.0).Doc("Distance in meters at which the light fades out completely."),
			Field("CastShadows", &PointLightComponent::CastShadows));
	};

	template<>
	struct ComponentTraits<SpotLightComponent>
	{
		static constexpr std::string_view Name = "SpotLight";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description = "Cone of light shining along the entity's forward direction (-Z).";
		static constexpr auto Fields = std::make_tuple(
			Field("Color", &SpotLightComponent::Color).Range(0.0, 1.0).AsColor(),
			Field("Intensity", &SpotLightComponent::Intensity).AtLeast(0.0),
			Field("Range", &SpotLightComponent::Range).AtLeast(0.0).Doc("Distance in meters at which the light fades out completely."),
			Field("InnerConeAngle", &SpotLightComponent::InnerConeAngle)
				.Range(0.0, 89.9)
				.AsAngle()
				.Doc("Half-angle of the full-intensity cone in degrees."),
			Field("OuterConeAngle", &SpotLightComponent::OuterConeAngle)
				.Range(0.1, 89.9)
				.AsAngle()
				.Doc("Half-angle of the cone at which the light fades out, in degrees."),
			Field("CastShadows", &SpotLightComponent::CastShadows));
	};

	template<>
	struct ComponentTraits<SkyLightComponent>
	{
		static constexpr std::string_view Name = "SkyLight";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description = "Image-based lighting and the sky from an HDR environment map.";
		static constexpr auto Fields = std::make_tuple(
			Field("Environment", &SkyLightComponent::Environment)
				.References("Environment")
				.Doc("HDR environment map; without one, AmbientColor lights the scene."),
			Field("Intensity", &SkyLightComponent::Intensity).AtLeast(0.0),
			Field("Rotation", &SkyLightComponent::Rotation)
				.AsAngle()
				.Doc("Rotation of the environment around the world Y axis in degrees."),
			Field("SkyboxBlur", &SkyLightComponent::SkyboxBlur).Range(0.0, 1.0).Doc("0 shows a sharp sky, 1 a fully blurred one."),
			Field("DrawSkybox", &SkyLightComponent::DrawSkybox).Doc("Draws the environment as the background."),
			Field("AmbientColor", &SkyLightComponent::AmbientColor)
				.Range(0.0, 1.0)
				.AsColor()
				.Doc("Ambient light used when no environment map is set."));
	};

	template<>
	struct ComponentTraits<RigidBodyComponent>
	{
		static constexpr std::string_view Name = "RigidBody";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description = "Simulates the entity with physics; the entity's colliders give it its shape.";
		static constexpr auto Fields = std::make_tuple(
			Field("Type", &RigidBodyComponent::Type)
				.Doc("Static bodies never move, dynamic bodies are simulated and kinematic bodies are moved by scripts."),
			Field("Mass", &RigidBodyComponent::Mass).AtLeast(0.001).Doc("Mass in kilograms."),
			Field("LinearDamping", &RigidBodyComponent::LinearDamping).AtLeast(0.0),
			Field("AngularDamping", &RigidBodyComponent::AngularDamping).AtLeast(0.0),
			Field("GravityFactor", &RigidBodyComponent::GravityFactor).Doc("Multiplier of the scene's gravity."),
			Field("Layer", &RigidBodyComponent::Layer).Range(0.0, 15.0).Doc("Physics layer index as configured in the project settings."),
			Field("LockTranslation", &RigidBodyComponent::LockTranslation).Doc("Prevents movement along the X, Y and Z axes."),
			Field("LockRotation", &RigidBodyComponent::LockRotation).Doc("Prevents rotation around the X, Y and Z axes."),
			Field("ContinuousCollision", &RigidBodyComponent::ContinuousCollision)
				.Doc("Keeps fast bodies from passing through thin geometry, at a performance cost."),
			Field("AllowSleep", &RigidBodyComponent::AllowSleep).Doc("Lets the body stop simulating while it is at rest."),
			Field("InitialLinearVelocity", &RigidBodyComponent::InitialLinearVelocity)
				.Doc("Velocity in meters per second when play starts (the simulation allows up to 500)."),
			Field("InitialAngularVelocity", &RigidBodyComponent::InitialAngularVelocity)
				.Doc("Angular velocity in radians per second when play starts (the simulation allows up to about 47)."));
	};

	template<>
	struct ComponentTraits<BoxColliderComponent>
	{
		static constexpr std::string_view Name = "BoxCollider";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description = "Box collision shape, scaled by the entity.";
		static constexpr auto Fields =
			std::make_tuple(Field("HalfExtents", &BoxColliderComponent::HalfExtents)
		                        .AtLeast(0.001)
		                        .Doc("Half the size of the box along each axis, in meters."),
		                    Field("Offset", &BoxColliderComponent::Offset).Doc("Center of the shape relative to the entity, in meters."),
		                    Field("IsTrigger", &BoxColliderComponent::IsTrigger).Doc("Reports overlaps instead of colliding."),
		                    Field("Friction", &BoxColliderComponent::Friction).AtLeast(0.0),
		                    Field("Restitution", &BoxColliderComponent::Restitution)
		                        .Range(0.0, 1.0)
		                        .Doc("Bounciness: 0 does not bounce, 1 is perfectly elastic."));
	};

	template<>
	struct ComponentTraits<SphereColliderComponent>
	{
		static constexpr std::string_view Name = "SphereCollider";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description = "Sphere collision shape, scaled by the entity.";
		static constexpr auto Fields =
			std::make_tuple(Field("Radius", &SphereColliderComponent::Radius).AtLeast(0.001).Doc("Radius in meters."),
		                    Field("Offset", &SphereColliderComponent::Offset).Doc("Center of the shape relative to the entity, in meters."),
		                    Field("IsTrigger", &SphereColliderComponent::IsTrigger).Doc("Reports overlaps instead of colliding."),
		                    Field("Friction", &SphereColliderComponent::Friction).AtLeast(0.0),
		                    Field("Restitution", &SphereColliderComponent::Restitution)
		                        .Range(0.0, 1.0)
		                        .Doc("Bounciness: 0 does not bounce, 1 is perfectly elastic."));
	};

	template<>
	struct ComponentTraits<CapsuleColliderComponent>
	{
		static constexpr std::string_view Name = "CapsuleCollider";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description = "Capsule collision shape along the local Y axis, scaled by the entity.";
		static constexpr auto Fields = std::make_tuple(
			Field("Radius", &CapsuleColliderComponent::Radius).AtLeast(0.001).Doc("Radius in meters."),
			Field("HalfHeight", &CapsuleColliderComponent::HalfHeight)
				.AtLeast(0.0)
				.Doc("Half the height of the cylindrical part (without the caps), in meters."),
			Field("Offset", &CapsuleColliderComponent::Offset).Doc("Center of the shape relative to the entity, in meters."),
			Field("IsTrigger", &CapsuleColliderComponent::IsTrigger).Doc("Reports overlaps instead of colliding."),
			Field("Friction", &CapsuleColliderComponent::Friction).AtLeast(0.0),
			Field("Restitution", &CapsuleColliderComponent::Restitution)
				.Range(0.0, 1.0)
				.Doc("Bounciness: 0 does not bounce, 1 is perfectly elastic."));
	};

	template<>
	struct ComponentTraits<MeshColliderComponent>
	{
		static constexpr std::string_view Name = "MeshCollider";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description = "Collision shape made from a mesh, as a convex hull or as triangles.";
		static constexpr auto Fields = std::make_tuple(
			Field("Mesh", &MeshColliderComponent::Mesh).References("Mesh").Doc("Collision mesh; none uses the entity's Mesh component."),
			Field("Convex", &MeshColliderComponent::Convex)
				.Doc("Convex hulls can move; triangle meshes are only valid for static and kinematic bodies."),
			Field("IsTrigger", &MeshColliderComponent::IsTrigger).Doc("Reports overlaps instead of colliding."),
			Field("Friction", &MeshColliderComponent::Friction).AtLeast(0.0),
			Field("Restitution", &MeshColliderComponent::Restitution)
				.Range(0.0, 1.0)
				.Doc("Bounciness: 0 does not bounce, 1 is perfectly elastic."));
	};

	template<>
	struct ComponentTraits<JointComponent>
	{
		static constexpr std::string_view Name = "Joint";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description =
			"Joins the body to another body or the world: fixed, hinge (doors, wheels, pendulums), ball (chains, ropes), distance "
			"or slider (pistons), with limits and motors.";
		static constexpr auto Fields = std::make_tuple(
			Field("Type", &JointComponent::Type)
				.Doc("Fixed (welded), Hinge (turns around Axis), Ball (turns freely), Distance (keeps the anchors apart) or Slider "
		             "(slides along Axis)."),
			Field("ConnectedEntity", &JointComponent::ConnectedEntity)
				.Doc("The entity whose body this one is joined to (it needs a RigidBody and a collider); none joins it to the world."),
			Field("Anchor", &JointComponent::Anchor).Range(-1.0e6, 1.0e6).Doc("Where the joint is, in this entity's local space."),
			Field("ConnectedAnchor", &JointComponent::ConnectedAnchor)
				.Range(-1.0e6, 1.0e6)
				.Doc("Distance joints: the far point, in the connected entity's local space (world space without one)."),
			Field("Axis", &JointComponent::Axis).Range(-1.0, 1.0).Doc("Hinge or slider axis in this entity's local space."),
			Field("LimitsEnabled", &JointComponent::LimitsEnabled).Doc("Limits a hinge's angle or a slider's travel."),
			Field("MinLimit", &JointComponent::MinLimit)
				.Range(-1.0e6, 0.0)
				.Doc("Hinges: degrees (down to -180); sliders: meters (0 is where the bodies start)."),
			Field("MaxLimit", &JointComponent::MaxLimit).Range(0.0, 1.0e6).Doc("Hinges: degrees (up to 180); sliders: meters."),
			Field("MinDistance", &JointComponent::MinDistance)
				.Range(-1.0, 1.0e6)
				.Doc("Distance joints: the shortest distance; -1 keeps the starting distance."),
			Field("MaxDistance", &JointComponent::MaxDistance)
				.Range(-1.0, 1.0e6)
				.Doc("Distance joints: the longest distance; -1 keeps the starting distance."),
			Field("MotorEnabled", &JointComponent::MotorEnabled).Doc("Drives a hinge or slider at MotorSpeed."),
			Field("MotorSpeed", &JointComponent::MotorSpeed)
				.Range(-1.0e4, 1.0e4)
				.Doc("Hinges: degrees per second; sliders: meters per second."),
			Field("MotorMaxForce", &JointComponent::MotorMaxForce)
				.Range(0.0, 1.0e9)
				.Doc("The most torque (hinges, newton meters) or force (sliders, newtons) the motor uses."));
	};

	template<>
	struct ComponentTraits<AudioSourceComponent>
	{
		static constexpr std::string_view Name = "AudioSource";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description = "Plays an audio clip, optionally positioned in 3D.";
		static constexpr auto Fields = std::make_tuple(
			Field("Clip", &AudioSourceComponent::Clip).References("AudioClip"),
			Field("Volume", &AudioSourceComponent::Volume).AtLeast(0.0).Doc("Linear gain; 1 plays the clip unchanged."),
			Field("Pitch", &AudioSourceComponent::Pitch).AtLeast(0.01).Doc("Playback speed multiplier; 1 plays the clip unchanged."),
			Field("Loop", &AudioSourceComponent::Loop),
			Field("PlayOnStart", &AudioSourceComponent::PlayOnStart).Doc("Starts playing when play mode starts."),
			Field("Spatial", &AudioSourceComponent::Spatial).Doc("Positions the sound in 3D; otherwise it plays at a constant level."),
			Field("MinDistance", &AudioSourceComponent::MinDistance)
				.AtLeast(0.0)
				.Doc("Distance in meters within which the sound plays at full volume."),
			Field("MaxDistance", &AudioSourceComponent::MaxDistance)
				.AtLeast(0.0)
				.Doc("Distance in meters beyond which the sound gets no quieter."));
	};

	template<>
	struct ComponentTraits<AudioListenerComponent>
	{
		static constexpr std::string_view Name = "AudioListener";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description = "The ears of the scene, usually on the camera.";
		static constexpr auto Fields =
			std::make_tuple(Field("Active", &AudioListenerComponent::Active).Doc("The first active listener hears the scene."));
	};

	template<>
	struct ComponentTraits<ScriptComponent>
	{
		static constexpr std::string_view Name = "Script";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description = "Attaches a C# script class to the entity.";
		static constexpr auto Fields = std::make_tuple(
			Field("ClassName", &ScriptComponent::ClassName).Doc("Fully qualified C# class name, e.g. Game.PlayerController."),
			Field("Fields", &ScriptComponent::Fields).Doc("Values of the script's public fields."));
	};

	template<>
	struct ComponentTraits<TextComponent>
	{
		static constexpr std::string_view Name = "Text";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description = "Draws text in the world or on the screen.";
		static constexpr auto Fields = std::make_tuple(
			Field("Text", &TextComponent::Text).AsMultilineText(),
			Field("Font", &TextComponent::Font).References("Font").Doc("None uses the default font."),
			Field("Color", &TextComponent::Color).Range(0.0, 1.0).AsColor(),
			Field("FontSize", &TextComponent::FontSize)
				.AtLeast(0.001)
				.Doc("Line height: pixels for screen-space text, world units otherwise."),
			Field("ScreenSpace", &TextComponent::ScreenSpace)
				.Doc("Places the text on the screen: translation x and y are viewport coordinates in [0, 1] from the top-left."),
			Field("Alignment", &TextComponent::Alignment),
			Field("LineSpacing", &TextComponent::LineSpacing).AtLeast(0.0).Doc("Multiplier of the font's line height."));
	};

	template<>
	struct ComponentTraits<SpriteRendererComponent>
	{
		static constexpr std::string_view Name = "SpriteRenderer";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description = "Draws a textured or colored quad in the world or on the screen.";
		static constexpr auto Fields = std::make_tuple(
			Field("Color", &SpriteRendererComponent::Color).Range(0.0, 1.0).AsColor().Doc("Tint multiplied with the texture."),
			Field("Texture", &SpriteRendererComponent::Texture).References("Texture").Doc("None draws a solid color."),
			Field("Tiling", &SpriteRendererComponent::Tiling).Doc("Number of texture repetitions across the sprite."),
			Field("ScreenSpace", &SpriteRendererComponent::ScreenSpace)
				.Doc("Places the sprite on the screen like screen-space text; scale is in pixels."));
	};

	template<>
	struct ComponentTraits<ParticleSystemComponent>
	{
		static constexpr std::string_view Name = "ParticleSystem";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description =
			"Emits glowing or smoky camera-facing particles (fire, sparks, smoke, dust, magic) while the scene runs.";
		static constexpr auto Fields = std::make_tuple(
			Field("Emitting", &ParticleSystemComponent::Emitting).Doc("Emits Rate particles per second while set."),
			Field("Rate", &ParticleSystemComponent::Rate).Range(0.0, 10000.0).Doc("Particles per second while emitting."),
			Field("Burst", &ParticleSystemComponent::Burst)
				.Range(0.0, 10000.0)
				.Doc("Particles emitted at once when the system starts (with the scene, or when the entity is created)."),
			Field("MaxParticles", &ParticleSystemComponent::MaxParticles)
				.Range(1.0, 100000.0)
				.Doc("Particles alive at once; emission waits while the system is full."),
			Field("LifetimeMin", &ParticleSystemComponent::LifetimeMin).Range(0.01, 600.0).Doc("Shortest life in seconds."),
			Field("LifetimeMax", &ParticleSystemComponent::LifetimeMax).Range(0.01, 600.0).Doc("Longest life in seconds."),
			Field("SpeedMin", &ParticleSystemComponent::SpeedMin).Range(0.0, 1000.0).Doc("Slowest start speed in meters per second."),
			Field("SpeedMax", &ParticleSystemComponent::SpeedMax).Range(0.0, 1000.0).Doc("Fastest start speed in meters per second."),
			Field("Shape", &ParticleSystemComponent::Shape)
				.Doc("Point, Sphere (outwards), Cone (along -Z from a disc) or Box (along -Z from inside a box)."),
			Field("ConeAngle", &ParticleSystemComponent::ConeAngle)
				.Range(0.0, 180.0)
				.AsAngle()
				.Doc("Spread around the forward axis (-Z) in degrees, for cones and boxes."),
			Field("Radius", &ParticleSystemComponent::Radius)
				.Range(0.0, 1000.0)
				.Doc("Radius of the sphere or of the cone's base, in meters."),
			Field("BoxHalfExtents", &ParticleSystemComponent::BoxHalfExtents).Range(0.0, 1000.0).Doc("Half size of the box in meters."),
			Field("GravityFactor", &ParticleSystemComponent::GravityFactor)
				.Range(-100.0, 100.0)
				.Doc("Multiplies the scene's gravity; negative values make particles rise."),
			Field("Drag", &ParticleSystemComponent::Drag).Range(0.0, 100.0).Doc("Fraction of the velocity lost per second."),
			Field("StartSize", &ParticleSystemComponent::StartSize).Range(0.0, 1000.0).Doc("Width in meters when a particle is born."),
			Field("EndSize", &ParticleSystemComponent::EndSize).Range(0.0, 1000.0).Doc("Width in meters when a particle dies."),
			Field("StartColor", &ParticleSystemComponent::StartColor).Range(0.0, 1.0).AsColor().Doc("Color and opacity at birth."),
			Field("EndColor", &ParticleSystemComponent::EndColor).Range(0.0, 1.0).AsColor().Doc("Color and opacity at death."),
			Field("Intensity", &ParticleSystemComponent::Intensity)
				.Range(0.0, 1000.0)
				.Doc("Multiplies the colors; values above 1 glow with bloom."),
			Field("Texture", &ParticleSystemComponent::Texture).References("Texture").Doc("None draws a soft round dot."),
			Field("BlendMode", &ParticleSystemComponent::BlendMode)
				.Doc("Additive adds light (fire, sparks); Alpha covers what is behind (smoke, dust)."),
			Field("WorldSpace", &ParticleSystemComponent::WorldSpace)
				.Doc("Particles stay where they were emitted when the entity moves; otherwise they move with it."),
			Field("DestroyWhenFinished", &ParticleSystemComponent::DestroyWhenFinished)
				.Doc("Destroys the entity once it no longer emits and its last particle died (one-shot effects)."));
	};

	template<>
	struct ComponentTraits<UICanvasComponent>
	{
		static constexpr std::string_view Name = "UICanvas";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description =
			"The root of a screen-space UI (HUDs, menus): its descendants are laid out on the screen by their UIRect and drawn over "
			"the scene in hierarchy order.";
		static constexpr auto Fields = std::make_tuple(
			Field("ScaleMode", &UICanvasComponent::ScaleMode)
				.Doc("ConstantPixelSize keeps the UI's size in pixels; ScaleWithScreenHeight scales it with the screen's height."),
			Field("ReferenceResolution", &UICanvasComponent::ReferenceResolution)
				.Range(1.0, 100000.0)
				.Doc("The screen size the UI is designed for, in canvas units; ScaleWithScreenHeight fits its height to the screen's."));
	};

	template<>
	struct ComponentTraits<UIRectComponent>
	{
		static constexpr std::string_view Name = "UIRect";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description =
			"Places a UI element in its parent's rectangle (the screen for a canvas's children) with anchors, a position, a size and "
			"a pivot.";
		static constexpr auto Fields = std::make_tuple(
			Field("AnchorMin", &UIRectComponent::AnchorMin)
				.Range(0.0, 1.0)
				.Doc("Where the rectangle's span starts in the parent's: 0 is its left or top edge, 1 its right or bottom edge."),
			Field("AnchorMax", &UIRectComponent::AnchorMax)
				.Range(0.0, 1.0)
				.Doc("Where the span ends; equal anchors attach the rectangle to a point, others stretch it with the parent."),
			Field("Position", &UIRectComponent::Position)
				.Range(-1.0e6, 1.0e6)
				.Doc("Moves the pivot from its point of the anchors' span, in canvas units (y downwards)."),
			Field("Size", &UIRectComponent::Size)
				.Range(-1.0e6, 1.0e6)
				.Doc("Canvas units added to the anchors' span: the size itself when the anchors are equal."),
			Field("Pivot", &UIRectComponent::Pivot)
				.Range(0.0, 1.0)
				.Doc("The point of the rectangle Position places, from its top-left corner (0, 0) to its bottom-right corner (1, 1)."),
			Field("Visible", &UIRectComponent::Visible).Doc("Hidden rectangles and their descendants are neither drawn nor clicked."),
			Field("Opacity", &UIRectComponent::Opacity).Range(0.0, 1.0).Doc("Multiplies the alpha of the element and its descendants."));
	};

	template<>
	struct ComponentTraits<UIImageComponent>
	{
		static constexpr std::string_view Name = "UIImage";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description =
			"Fills a UI element's rectangle with a color or a texture, with optionally rounded corners (panels, icons, buttons).";
		static constexpr auto Fields = std::make_tuple(
			Field("Color", &UIImageComponent::Color)
				.Range(0.0, 1.0)
				.AsColor()
				.Doc("Multiplied with the texture; a UIButton on the entity replaces it with the color of its state."),
			Field("Texture", &UIImageComponent::Texture).References("Texture").Doc("None fills the rectangle with the color."),
			Field("PreserveAspect", &UIImageComponent::PreserveAspect)
				.Doc("Draws the texture at its own aspect ratio, as large as fits and centered in the rectangle."),
			Field("CornerRadius", &UIImageComponent::CornerRadius)
				.Range(0.0, 1.0e6)
				.Doc("Radius of the rounded corners in canvas units (at most half the rectangle's shorter side)."));
	};

	template<>
	struct ComponentTraits<UITextComponent>
	{
		static constexpr std::string_view Name = "UIText";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description = "Draws text in a UI element's rectangle, wrapped at its width and aligned in it.";
		static constexpr auto Fields = std::make_tuple(
			Field("Text", &UITextComponent::Text).AsMultilineText(),
			Field("Font", &UITextComponent::Font).References("Font").Doc("None uses the default font."),
			Field("Color", &UITextComponent::Color).Range(0.0, 1.0).AsColor(),
			Field("FontSize", &UITextComponent::FontSize).Range(1.0, 4096.0).Doc("Line height in canvas units."),
			Field("Alignment", &UITextComponent::Alignment).Doc("Places the lines at the left, center or right of the rectangle."),
			Field("VerticalAlignment", &UITextComponent::VerticalAlignment)
				.Doc("Places the text block at the top, middle or bottom of the rectangle."),
			Field("Wrap", &UITextComponent::Wrap).Doc("Breaks lines that do not fit the rectangle's width at spaces."),
			Field("LineSpacing", &UITextComponent::LineSpacing).Range(0.0, 100.0).Doc("Multiplier of the font's line height."));
	};

	template<>
	struct ComponentTraits<UIButtonComponent>
	{
		static constexpr std::string_view Name = "UIButton";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr std::string_view Description =
			"Makes a UI element clickable: scripts read WasClicked and get OnClick; its colors replace the UIImage's color in each "
			"state.";
		static constexpr auto Fields = std::make_tuple(
			Field("Interactable", &UIButtonComponent::Interactable).Doc("Disabled buttons take no clicks and show DisabledColor."),
			Field("NormalColor", &UIButtonComponent::NormalColor).Range(0.0, 1.0).AsColor().Doc("The image's color at rest."),
			Field("HoveredColor", &UIButtonComponent::HoveredColor).Range(0.0, 1.0).AsColor().Doc("The image's color under the mouse."),
			Field("PressedColor", &UIButtonComponent::PressedColor)
				.Range(0.0, 1.0)
				.AsColor()
				.Doc("The image's color while the mouse button is held on it."),
			Field("DisabledColor", &UIButtonComponent::DisabledColor)
				.Range(0.0, 1.0)
				.AsColor()
				.Doc("The image's color while the button is not interactable."));
	};

	template<>
	struct ComponentTraits<PrefabComponent>
	{
		static constexpr std::string_view Name = "Prefab";
		static constexpr uint32_t Flags = ComponentFlagsInternal;
		static constexpr std::string_view Description = "Links the entity to the prefab it was instantiated from.";
		static constexpr auto Fields = std::make_tuple(
			Field("Prefab", &PrefabComponent::Prefab).References("Prefab"),
			Field("SourceEntity", &PrefabComponent::SourceEntity).Doc("ID of the corresponding entity in the prefab file."));
	};

	template<typename T>
	concept RegisteredComponent = FieldTable<ComponentTraits<T>> && requires {
		{ ComponentTraits<T>::Flags } -> std::convertible_to<uint32_t>;
		{ ComponentTraits<T>::Description } -> std::convertible_to<std::string_view>;
	};

	// Every component type, in registration (and serialization) order.
	template<typename... T>
	struct ComponentList
	{
	};

	using AllComponents =
		ComponentList<IDComponent, TagComponent, TransformComponent, RelationshipComponent, CameraComponent, MeshComponent,
	                  DirectionalLightComponent, PointLightComponent, SpotLightComponent, SkyLightComponent, RigidBodyComponent,
	                  BoxColliderComponent, SphereColliderComponent, CapsuleColliderComponent, MeshColliderComponent, JointComponent,
	                  AudioSourceComponent, AudioListenerComponent, ScriptComponent, TextComponent, SpriteRendererComponent,
	                  ParticleSystemComponent, UICanvasComponent, UIRectComponent, UIImageComponent, UITextComponent, UIButtonComponent,
	                  PrefabComponent>;
}
