#pragma once

#include "Strada/Scene/Components.h"
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

	// Script fields serialize as { "<Name>": { "Type": "<ScriptFieldType>", "Value": <value> } }.
	template<>
	struct JsonTraits<ScriptFieldMap>
	{
		static Json ToJson(ScriptFieldMap const& fields);
		static Result<void> FromJson(Json const& json, ScriptFieldMap& out, DeserializationContext const& context);
		static std::string TypeName() { return "scriptFields"; }
	};

	Json ScriptFieldValueToJson(ScriptFieldValue const& value);
	[[nodiscard]] Result<ScriptFieldValue> ScriptFieldValueFromJson(ScriptFieldType type, Json const& json,
	                                                                DeserializationContext const& context);

	enum ComponentFlags : uint32_t
	{
		ComponentFlagsNone = 0,
		// Present on every entity; cannot be added or removed by users (ID, Tag, Transform, Relationship).
		ComponentFlagsCore = ST_BIT(0),
		// Managed by the engine (not shown in "Add Component" menus or editable through generic component commands).
		ComponentFlagsInternal = ST_BIT(1)
	};

	// Specialized for every component:
	//   static constexpr std::string_view Name;      serialized name
	//   static constexpr uint32_t Flags;             ComponentFlags
	//   static constexpr auto Fields;                tuple of StructField (see Serialization/StructSerialization.h)
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
		static constexpr auto Fields = std::make_tuple(Field("ID", &IDComponent::ID));
	};

	template<>
	struct ComponentTraits<TagComponent>
	{
		static constexpr std::string_view Name = "Tag";
		static constexpr uint32_t Flags = ComponentFlagsCore;
		static constexpr auto Fields = std::make_tuple(Field("Tag", &TagComponent::Tag));
	};

	template<>
	struct ComponentTraits<TransformComponent>
	{
		static constexpr std::string_view Name = "Transform";
		static constexpr uint32_t Flags = ComponentFlagsCore;
		static constexpr auto Fields =
			std::make_tuple(Field("Translation", &TransformComponent::Translation), Field("Rotation", &TransformComponent::Rotation),
		                    Field("Scale", &TransformComponent::Scale));

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
		static constexpr auto Fields =
			std::make_tuple(Field("Parent", &RelationshipComponent::Parent), Field("Children", &RelationshipComponent::Children));
	};

	template<>
	struct ComponentTraits<CameraComponent>
	{
		static constexpr std::string_view Name = "Camera";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr auto Fields = std::make_tuple(
			Field("Projection", &CameraComponent::Projection), Field("PerspectiveFOV", &CameraComponent::PerspectiveFOV),
			Field("PerspectiveNear", &CameraComponent::PerspectiveNear), Field("PerspectiveFar", &CameraComponent::PerspectiveFar),
			Field("OrthographicSize", &CameraComponent::OrthographicSize), Field("OrthographicNear", &CameraComponent::OrthographicNear),
			Field("OrthographicFar", &CameraComponent::OrthographicFar), Field("Primary", &CameraComponent::Primary),
			Field("FixedAspectRatio", &CameraComponent::FixedAspectRatio), Field("AspectRatio", &CameraComponent::AspectRatio));
	};

	template<>
	struct ComponentTraits<MeshComponent>
	{
		static constexpr std::string_view Name = "Mesh";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr auto Fields =
			std::make_tuple(Field("Mesh", &MeshComponent::Mesh), Field("Materials", &MeshComponent::Materials),
		                    Field("CastShadows", &MeshComponent::CastShadows), Field("Visible", &MeshComponent::Visible));
	};

	template<>
	struct ComponentTraits<DirectionalLightComponent>
	{
		static constexpr std::string_view Name = "DirectionalLight";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr auto Fields = std::make_tuple(
			Field("Color", &DirectionalLightComponent::Color), Field("Intensity", &DirectionalLightComponent::Intensity),
			Field("CastShadows", &DirectionalLightComponent::CastShadows), Field("LightSize", &DirectionalLightComponent::LightSize));
	};

	template<>
	struct ComponentTraits<PointLightComponent>
	{
		static constexpr std::string_view Name = "PointLight";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr auto Fields =
			std::make_tuple(Field("Color", &PointLightComponent::Color), Field("Intensity", &PointLightComponent::Intensity),
		                    Field("Range", &PointLightComponent::Range), Field("CastShadows", &PointLightComponent::CastShadows));
	};

	template<>
	struct ComponentTraits<SpotLightComponent>
	{
		static constexpr std::string_view Name = "SpotLight";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr auto Fields = std::make_tuple(
			Field("Color", &SpotLightComponent::Color), Field("Intensity", &SpotLightComponent::Intensity),
			Field("Range", &SpotLightComponent::Range), Field("InnerConeAngle", &SpotLightComponent::InnerConeAngle),
			Field("OuterConeAngle", &SpotLightComponent::OuterConeAngle), Field("CastShadows", &SpotLightComponent::CastShadows));
	};

	template<>
	struct ComponentTraits<SkyLightComponent>
	{
		static constexpr std::string_view Name = "SkyLight";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr auto Fields =
			std::make_tuple(Field("Environment", &SkyLightComponent::Environment), Field("Intensity", &SkyLightComponent::Intensity),
		                    Field("Rotation", &SkyLightComponent::Rotation), Field("SkyboxBlur", &SkyLightComponent::SkyboxBlur),
		                    Field("DrawSkybox", &SkyLightComponent::DrawSkybox), Field("AmbientColor", &SkyLightComponent::AmbientColor));
	};

	template<>
	struct ComponentTraits<RigidBodyComponent>
	{
		static constexpr std::string_view Name = "RigidBody";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr auto Fields = std::make_tuple(
			Field("Type", &RigidBodyComponent::Type), Field("Mass", &RigidBodyComponent::Mass),
			Field("LinearDamping", &RigidBodyComponent::LinearDamping), Field("AngularDamping", &RigidBodyComponent::AngularDamping),
			Field("GravityFactor", &RigidBodyComponent::GravityFactor), Field("Layer", &RigidBodyComponent::Layer),
			Field("LockTranslation", &RigidBodyComponent::LockTranslation), Field("LockRotation", &RigidBodyComponent::LockRotation),
			Field("ContinuousCollision", &RigidBodyComponent::ContinuousCollision), Field("AllowSleep", &RigidBodyComponent::AllowSleep),
			Field("InitialLinearVelocity", &RigidBodyComponent::InitialLinearVelocity),
			Field("InitialAngularVelocity", &RigidBodyComponent::InitialAngularVelocity));
	};

	template<>
	struct ComponentTraits<BoxColliderComponent>
	{
		static constexpr std::string_view Name = "BoxCollider";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr auto Fields =
			std::make_tuple(Field("HalfExtents", &BoxColliderComponent::HalfExtents), Field("Offset", &BoxColliderComponent::Offset),
		                    Field("IsTrigger", &BoxColliderComponent::IsTrigger), Field("Friction", &BoxColliderComponent::Friction),
		                    Field("Restitution", &BoxColliderComponent::Restitution));
	};

	template<>
	struct ComponentTraits<SphereColliderComponent>
	{
		static constexpr std::string_view Name = "SphereCollider";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr auto Fields =
			std::make_tuple(Field("Radius", &SphereColliderComponent::Radius), Field("Offset", &SphereColliderComponent::Offset),
		                    Field("IsTrigger", &SphereColliderComponent::IsTrigger), Field("Friction", &SphereColliderComponent::Friction),
		                    Field("Restitution", &SphereColliderComponent::Restitution));
	};

	template<>
	struct ComponentTraits<CapsuleColliderComponent>
	{
		static constexpr std::string_view Name = "CapsuleCollider";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr auto Fields = std::make_tuple(
			Field("Radius", &CapsuleColliderComponent::Radius), Field("HalfHeight", &CapsuleColliderComponent::HalfHeight),
			Field("Offset", &CapsuleColliderComponent::Offset), Field("IsTrigger", &CapsuleColliderComponent::IsTrigger),
			Field("Friction", &CapsuleColliderComponent::Friction), Field("Restitution", &CapsuleColliderComponent::Restitution));
	};

	template<>
	struct ComponentTraits<MeshColliderComponent>
	{
		static constexpr std::string_view Name = "MeshCollider";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr auto Fields =
			std::make_tuple(Field("Mesh", &MeshColliderComponent::Mesh), Field("Convex", &MeshColliderComponent::Convex),
		                    Field("IsTrigger", &MeshColliderComponent::IsTrigger), Field("Friction", &MeshColliderComponent::Friction),
		                    Field("Restitution", &MeshColliderComponent::Restitution));
	};

	template<>
	struct ComponentTraits<AudioSourceComponent>
	{
		static constexpr std::string_view Name = "AudioSource";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr auto Fields = std::make_tuple(
			Field("Clip", &AudioSourceComponent::Clip), Field("Volume", &AudioSourceComponent::Volume),
			Field("Pitch", &AudioSourceComponent::Pitch), Field("Loop", &AudioSourceComponent::Loop),
			Field("PlayOnStart", &AudioSourceComponent::PlayOnStart), Field("Spatial", &AudioSourceComponent::Spatial),
			Field("MinDistance", &AudioSourceComponent::MinDistance), Field("MaxDistance", &AudioSourceComponent::MaxDistance));
	};

	template<>
	struct ComponentTraits<AudioListenerComponent>
	{
		static constexpr std::string_view Name = "AudioListener";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr auto Fields = std::make_tuple(Field("Active", &AudioListenerComponent::Active));
	};

	template<>
	struct ComponentTraits<ScriptComponent>
	{
		static constexpr std::string_view Name = "Script";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr auto Fields =
			std::make_tuple(Field("ClassName", &ScriptComponent::ClassName), Field("Fields", &ScriptComponent::Fields));
	};

	template<>
	struct ComponentTraits<TextComponent>
	{
		static constexpr std::string_view Name = "Text";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr auto Fields =
			std::make_tuple(Field("Text", &TextComponent::Text), Field("Font", &TextComponent::Font), Field("Color", &TextComponent::Color),
		                    Field("FontSize", &TextComponent::FontSize), Field("ScreenSpace", &TextComponent::ScreenSpace),
		                    Field("Alignment", &TextComponent::Alignment), Field("LineSpacing", &TextComponent::LineSpacing));
	};

	template<>
	struct ComponentTraits<SpriteRendererComponent>
	{
		static constexpr std::string_view Name = "SpriteRenderer";
		static constexpr uint32_t Flags = ComponentFlagsNone;
		static constexpr auto Fields =
			std::make_tuple(Field("Color", &SpriteRendererComponent::Color), Field("Texture", &SpriteRendererComponent::Texture),
		                    Field("Tiling", &SpriteRendererComponent::Tiling), Field("ScreenSpace", &SpriteRendererComponent::ScreenSpace));
	};

	template<>
	struct ComponentTraits<PrefabComponent>
	{
		static constexpr std::string_view Name = "Prefab";
		static constexpr uint32_t Flags = ComponentFlagsInternal;
		static constexpr auto Fields =
			std::make_tuple(Field("Prefab", &PrefabComponent::Prefab), Field("SourceEntity", &PrefabComponent::SourceEntity));
	};

	template<typename T>
	concept RegisteredComponent = FieldTable<ComponentTraits<T>>;

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
