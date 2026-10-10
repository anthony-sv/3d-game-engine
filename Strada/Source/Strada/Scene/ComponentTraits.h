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
				.Doc("Velocity in meters per second when play starts."),
			Field("InitialAngularVelocity", &RigidBodyComponent::InitialAngularVelocity)
				.Doc("Angular velocity in degrees per second when play starts."));
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
	                  BoxColliderComponent, SphereColliderComponent, CapsuleColliderComponent, MeshColliderComponent, AudioSourceComponent,
	                  AudioListenerComponent, ScriptComponent, TextComponent, SpriteRendererComponent, PrefabComponent>;
}
