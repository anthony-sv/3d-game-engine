#include "stpch.h"
#include "Strada/Script/ScriptGlue.h"

#include "Strada/Math/Math.h"
#include "Strada/Physics/PhysicsScene.h"
#include "Strada/Scene/SceneCamera.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace Strada::ScriptGlue
{
	namespace
	{
		// The layout of Strada.Ray.
		struct Ray
		{
			Vector3 Origin;
			Vector3 Direction;
		};

		// --- Transform ---

		void TransformComponent_GetEulerAngles(uint64_t id, Vector3* value)
		{
			TransformComponent const* transform = FindComponent<TransformComponent>(id);
			*value = ToScript(transform != nullptr ? transform->GetRotationEuler() : glm::vec3(0.0f));
		}

		void TransformComponent_SetEulerAngles(uint64_t id, Vector3 const* value)
		{
			if (TransformComponent* transform = FindComponent<TransformComponent>(id))
			{
				transform->SetRotationEuler(FromScript(*value));
			}
		}

		void TransformComponent_GetWorldTransform(uint64_t id, Matrix4* value)
		{
			glm::mat4 world(1.0f);
			if (Entity const entity = FindEntity(id, "TransformComponent.WorldTransform"))
			{
				world = entity.GetScene()->GetWorldTransform(entity);
			}
			std::memcpy(value->Values, &world[0][0], sizeof(value->Values));
		}

		void TransformComponent_GetWorldTranslation(uint64_t id, Vector3* value)
		{
			glm::vec3 translation(0.0f);
			if (Entity const entity = FindEntity(id, "TransformComponent.WorldTranslation"))
			{
				translation = glm::vec3(entity.GetScene()->GetWorldTransform(entity)[3]);
			}
			*value = ToScript(translation);
		}

		// Moves the entity in world space, keeping its rotation and scale.
		void TransformComponent_SetWorldTranslation(uint64_t id, Vector3 const* value)
		{
			if (Entity const entity = FindEntity(id, "TransformComponent.WorldTranslation"))
			{
				glm::mat4 world = entity.GetScene()->GetWorldTransform(entity);
				world[3] = glm::vec4(FromScript(*value), 1.0f);
				entity.GetScene()->SetWorldTransform(entity, world);
			}
		}

		// World-space axes of the entity (forward is -Z); zero for degenerate transforms.
		glm::vec3 GetWorldAxis(uint64_t id, int axis, float sign, char const* function)
		{
			Entity const entity = FindEntity(id, function);
			if (!entity)
			{
				return glm::vec3(0.0f);
			}
			glm::vec3 const direction = sign * glm::vec3(entity.GetScene()->GetWorldTransform(entity)[axis]);
			float const length = glm::length(direction);
			return length > 1e-12f ? direction / length : glm::vec3(0.0f);
		}

		void TransformComponent_GetForward(uint64_t id, Vector3* value)
		{
			*value = ToScript(GetWorldAxis(id, 2, -1.0f, "TransformComponent.Forward"));
		}

		void TransformComponent_GetRight(uint64_t id, Vector3* value)
		{
			*value = ToScript(GetWorldAxis(id, 0, 1.0f, "TransformComponent.Right"));
		}

		void TransformComponent_GetUp(uint64_t id, Vector3* value)
		{
			*value = ToScript(GetWorldAxis(id, 1, 1.0f, "TransformComponent.Up"));
		}

		// --- Camera ---

		// The ray from the camera through a point of the scene's viewport (pixels, origin top-left).
		void CameraComponent_ScreenToWorldRay(uint64_t id, Vector2 const* screenPosition, Ray* ray)
		{
			*ray = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}};
			Entity const entity = FindEntity(id, "CameraComponent.ScreenToWorldRay");
			CameraComponent const* camera = entity ? FindComponent<CameraComponent>(id) : nullptr;
			if (camera == nullptr)
			{
				return;
			}
			Scene& scene = *entity.GetScene();
			if (scene.GetViewportWidth() == 0 || scene.GetViewportHeight() == 0)
			{
				Log::GetScriptLogger().error("CameraComponent.ScreenToWorldRay: the scene has no viewport size");
				return;
			}
			glm::vec3 position;
			glm::quat rotation;
			glm::vec3 scale;
			if (!Math::DecomposeTransform(scene.GetWorldTransform(entity), position, rotation, scale))
			{
				return;
			}
			float const width = static_cast<float>(scene.GetViewportWidth());
			float const height = static_cast<float>(scene.GetViewportHeight());
			glm::mat4 const view = glm::inverse(glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation));
			glm::mat4 const inverseViewProjection = glm::inverse(ComputeCameraGizmoProjection(*camera, width / height) * view);
			// Clip space has +Y up; the viewport has its origin at the top.
			glm::vec2 const ndc(2.0f * screenPosition->X / width - 1.0f, 1.0f - 2.0f * screenPosition->Y / height);
			glm::vec4 const nearPoint = inverseViewProjection * glm::vec4(ndc, 0.0f, 1.0f);
			glm::vec4 const farPoint = inverseViewProjection * glm::vec4(ndc, 1.0f, 1.0f);
			glm::vec3 const origin = glm::vec3(nearPoint) / nearPoint.w;
			glm::vec3 const direction = glm::normalize(glm::vec3(farPoint) / farPoint.w - origin);
			*ray = {ToScript(origin), ToScript(direction)};
		}

		// --- Mesh materials ---

		int32_t MeshComponent_GetMaterialCount(uint64_t id)
		{
			MeshComponent const* mesh = FindComponent<MeshComponent>(id);
			return mesh != nullptr ? static_cast<int32_t>(mesh->Materials.size()) : 0;
		}

		uint64_t MeshComponent_GetMaterial(uint64_t id, int32_t index)
		{
			MeshComponent const* mesh = FindComponent<MeshComponent>(id);
			if (mesh == nullptr || index < 0 || static_cast<size_t>(index) >= mesh->Materials.size())
			{
				return 0;
			}
			return mesh->Materials[static_cast<size_t>(index)].GetUUID().GetValue();
		}

		// Overrides the material of a submesh, growing the list as needed (missing entries use the mesh's materials).
		void MeshComponent_SetMaterial(uint64_t id, int32_t index, uint64_t material)
		{
			if (index < 0)
			{
				Log::GetScriptLogger().error("MeshComponent.SetMaterial: index {} is negative", index);
				return;
			}
			PatchComponent<MeshComponent>(id,
			                              [index, material](MeshComponent& mesh)
			                              {
											  if (static_cast<size_t>(index) >= mesh.Materials.size())
											  {
												  mesh.Materials.resize(static_cast<size_t>(index) + 1);
											  }
											  mesh.Materials[static_cast<size_t>(index)] = AssetHandle(UUID(material));
										  });
		}

		// --- Rigid body: the live body while the scene runs, the component otherwise ---

		// The entity's body once pending physics changes are applied; null (logged) without one.
		PhysicsScene* FindBody(uint64_t id, char const* function)
		{
			Entity const entity = FindEntity(id, function);
			if (!entity || FindComponent<RigidBodyComponent>(id) == nullptr)
			{
				return nullptr;
			}
			Scene& scene = *entity.GetScene();
			scene.ApplyPhysicsChanges();
			PhysicsScene* const physics = scene.GetPhysicsScene();
			if (physics == nullptr || !physics->HasBody(UUID(id)))
			{
				Log::GetScriptLogger().error("{}: entity {} has no physics body (the scene is not simulating it)", function, id);
				return nullptr;
			}
			return physics;
		}

		// The live body without logging (properties fall back to the component).
		PhysicsScene* FindLiveBody(uint64_t id)
		{
			Scene* const scene = ScriptEngine::GetSceneContext();
			if (scene == nullptr)
			{
				return nullptr;
			}
			scene->ApplyPhysicsChanges();
			PhysicsScene* const physics = scene->GetPhysicsScene();
			return physics != nullptr && physics->HasBody(UUID(id)) ? physics : nullptr;
		}

		void RigidBodyComponent_GetLinearVelocity(uint64_t id, Vector3* value)
		{
			RigidBodyComponent const* body = FindComponent<RigidBodyComponent>(id);
			PhysicsScene const* physics = body != nullptr ? FindLiveBody(id) : nullptr;
			glm::vec3 const velocity = physics != nullptr ? physics->GetLinearVelocity(UUID(id))
			                                              : (body != nullptr ? body->InitialLinearVelocity : glm::vec3(0.0f));
			*value = ToScript(velocity);
		}

		void RigidBodyComponent_SetLinearVelocity(uint64_t id, Vector3 const* value)
		{
			RigidBodyComponent* body = FindComponent<RigidBodyComponent>(id);
			if (body == nullptr)
			{
				return;
			}
			if (PhysicsScene* physics = FindLiveBody(id))
			{
				physics->SetLinearVelocity(UUID(id), FromScript(*value));
			}
			else
			{
				body->InitialLinearVelocity = FromScript(*value);
			}
		}

		void RigidBodyComponent_GetAngularVelocity(uint64_t id, Vector3* value)
		{
			RigidBodyComponent const* body = FindComponent<RigidBodyComponent>(id);
			PhysicsScene const* physics = body != nullptr ? FindLiveBody(id) : nullptr;
			glm::vec3 const velocity = physics != nullptr ? physics->GetAngularVelocity(UUID(id))
			                                              : (body != nullptr ? body->InitialAngularVelocity : glm::vec3(0.0f));
			*value = ToScript(velocity);
		}

		void RigidBodyComponent_SetAngularVelocity(uint64_t id, Vector3 const* value)
		{
			RigidBodyComponent* body = FindComponent<RigidBodyComponent>(id);
			if (body == nullptr)
			{
				return;
			}
			if (PhysicsScene* physics = FindLiveBody(id))
			{
				physics->SetAngularVelocity(UUID(id), FromScript(*value));
			}
			else
			{
				body->InitialAngularVelocity = FromScript(*value);
			}
		}

		// Gravity factor and damping change the live body directly: no rebuild.
		void RigidBodyComponent_SetGravityFactor(uint64_t id, float const* value)
		{
			if (RigidBodyComponent* body = FindComponent<RigidBodyComponent>(id))
			{
				body->GravityFactor = *value;
				if (PhysicsScene* physics = FindLiveBody(id))
				{
					physics->SetGravityFactor(UUID(id), *value);
				}
			}
		}

		void SetDamping(uint64_t id, float RigidBodyComponent::* member, float value)
		{
			if (RigidBodyComponent* body = FindComponent<RigidBodyComponent>(id))
			{
				body->*member = value;
				if (PhysicsScene* physics = FindLiveBody(id))
				{
					physics->SetDamping(UUID(id), body->LinearDamping, body->AngularDamping);
				}
			}
		}

		void RigidBodyComponent_SetLinearDamping(uint64_t id, float const* value)
		{
			SetDamping(id, &RigidBodyComponent::LinearDamping, *value);
		}

		void RigidBodyComponent_SetAngularDamping(uint64_t id, float const* value)
		{
			SetDamping(id, &RigidBodyComponent::AngularDamping, *value);
		}

		bool IsForceMode(int32_t mode)
		{
			return mode >= static_cast<int32_t>(ForceMode::Force) && mode <= static_cast<int32_t>(ForceMode::VelocityChange);
		}

		void RigidBodyComponent_AddForce(uint64_t id, Vector3 const* force, int32_t mode)
		{
			if (!IsForceMode(mode))
			{
				Log::GetScriptLogger().error("RigidBodyComponent.AddForce: {} is not a ForceMode", mode);
			}
			else if (PhysicsScene* physics = FindBody(id, "RigidBodyComponent.AddForce"))
			{
				physics->AddForce(UUID(id), FromScript(*force), static_cast<ForceMode>(mode));
			}
		}

		void RigidBodyComponent_AddTorque(uint64_t id, Vector3 const* torque, int32_t mode)
		{
			if (!IsForceMode(mode))
			{
				Log::GetScriptLogger().error("RigidBodyComponent.AddTorque: {} is not a ForceMode", mode);
			}
			else if (PhysicsScene* physics = FindBody(id, "RigidBodyComponent.AddTorque"))
			{
				physics->AddTorque(UUID(id), FromScript(*torque), static_cast<ForceMode>(mode));
			}
		}

		// Places the entity (in world space, keeping its scale); the body follows before the next physics step, moving
		// there for kinematic bodies and teleporting otherwise.
		void PlaceBody(uint64_t id, Vector3 const* position, Quaternion const* rotation, char const* function)
		{
			Entity const entity = FindEntity(id, function);
			if (!entity || FindComponent<RigidBodyComponent>(id) == nullptr)
			{
				return;
			}
			glm::vec3 currentPosition;
			glm::quat currentRotation;
			glm::vec3 scale(1.0f);
			Math::DecomposeTransform(entity.GetScene()->GetWorldTransform(entity), currentPosition, currentRotation, scale);
			entity.GetScene()->SetWorldTransform(
				entity, Math::ComposeTransform(FromScript(*position), glm::normalize(FromScript(*rotation)), scale));
		}

		void RigidBodyComponent_MoveKinematic(uint64_t id, Vector3 const* position, Quaternion const* rotation)
		{
			RigidBodyComponent const* body = FindComponent<RigidBodyComponent>(id);
			if (body != nullptr && body->Type != RigidBodyType::Kinematic)
			{
				Log::GetScriptLogger().error("RigidBodyComponent.MoveKinematic: entity {} is not kinematic (use Teleport)", id);
				return;
			}
			PlaceBody(id, position, rotation, "RigidBodyComponent.MoveKinematic");
		}

		void RigidBodyComponent_Teleport(uint64_t id, Vector3 const* position, Quaternion const* rotation)
		{
			PlaceBody(id, position, rotation, "RigidBodyComponent.Teleport");
		}

		uint8_t RigidBodyComponent_IsSleeping(uint64_t id)
		{
			PhysicsScene const* physics = FindComponent<RigidBodyComponent>(id) != nullptr ? FindLiveBody(id) : nullptr;
			return physics != nullptr && physics->IsSleeping(UUID(id)) ? 1 : 0;
		}

		void RigidBodyComponent_WakeUp(uint64_t id)
		{
			if (PhysicsScene* physics = FindBody(id, "RigidBodyComponent.WakeUp"))
			{
				physics->WakeUp(UUID(id));
			}
		}

		// --- Audio source playback ---

		Entity FindAudioSource(uint64_t id)
		{
			return FindComponent<AudioSourceComponent>(id) != nullptr ? ScriptEngine::GetSceneContext()->GetEntityByUUID(UUID(id))
			                                                          : Entity();
		}

		void AudioSourceComponent_Play(uint64_t id)
		{
			if (Entity const entity = FindAudioSource(id))
			{
				entity.GetScene()->PlayAudioSource(entity);
			}
		}

		void AudioSourceComponent_Pause(uint64_t id)
		{
			if (Entity const entity = FindAudioSource(id))
			{
				entity.GetScene()->PauseAudioSource(entity);
			}
		}

		void AudioSourceComponent_Stop(uint64_t id)
		{
			if (Entity const entity = FindAudioSource(id))
			{
				entity.GetScene()->StopAudioSource(entity);
			}
		}

		uint8_t AudioSourceComponent_IsPlaying(uint64_t id)
		{
			Entity const entity = FindAudioSource(id);
			return entity && entity.GetScene()->IsAudioSourcePlaying(entity) ? 1 : 0;
		}

		// --- Strings ---

		template<typename TComponent, std::string TComponent::* Member>
		char const* GetText(uint64_t id, int32_t* length)
		{
			*length = 0;
			TComponent const* component = FindComponent<TComponent>(id);
			if (component == nullptr)
			{
				return nullptr;
			}
			std::string const& text = component->*Member;
			*length = static_cast<int32_t>(text.size());
			return text.data();
		}

		template<typename TComponent, std::string TComponent::* Member>
		void SetText(uint64_t id, char const* text, int32_t length)
		{
			std::string value(ToStringView(text, length));
			PatchComponent<TComponent>(id,
			                           [&value](TComponent& component)
			                           {
										   component.*Member = std::move(value);
									   });
		}
	}

	void RegisterComponentBindings(BindingTable& table)
	{
		AddFieldBindings<&TransformComponent::Translation>(table, "TransformComponent", "Translation");
		AddFieldBindings<&TransformComponent::Rotation>(table, "TransformComponent", "Rotation");
		AddFieldBindings<&TransformComponent::Scale>(table, "TransformComponent", "Scale");
		table.Add("TransformComponent_GetEulerAngles", &TransformComponent_GetEulerAngles);
		table.Add("TransformComponent_SetEulerAngles", &TransformComponent_SetEulerAngles);
		table.Add("TransformComponent_GetWorldTransform", &TransformComponent_GetWorldTransform);
		table.Add("TransformComponent_GetWorldTranslation", &TransformComponent_GetWorldTranslation);
		table.Add("TransformComponent_SetWorldTranslation", &TransformComponent_SetWorldTranslation);
		table.Add("TransformComponent_GetForward", &TransformComponent_GetForward);
		table.Add("TransformComponent_GetRight", &TransformComponent_GetRight);
		table.Add("TransformComponent_GetUp", &TransformComponent_GetUp);

		AddFieldBindings<&CameraComponent::Projection>(table, "CameraComponent", "Projection");
		AddFieldBindings<&CameraComponent::PerspectiveFOV>(table, "CameraComponent", "PerspectiveFOV");
		AddFieldBindings<&CameraComponent::PerspectiveNear>(table, "CameraComponent", "PerspectiveNear");
		AddFieldBindings<&CameraComponent::PerspectiveFar>(table, "CameraComponent", "PerspectiveFar");
		AddFieldBindings<&CameraComponent::OrthographicSize>(table, "CameraComponent", "OrthographicSize");
		AddFieldBindings<&CameraComponent::OrthographicNear>(table, "CameraComponent", "OrthographicNear");
		AddFieldBindings<&CameraComponent::OrthographicFar>(table, "CameraComponent", "OrthographicFar");
		AddFieldBindings<&CameraComponent::Primary>(table, "CameraComponent", "Primary");
		AddFieldBindings<&CameraComponent::FixedAspectRatio>(table, "CameraComponent", "FixedAspectRatio");
		AddFieldBindings<&CameraComponent::AspectRatio>(table, "CameraComponent", "AspectRatio");
		table.Add("CameraComponent_ScreenToWorldRay", &CameraComponent_ScreenToWorldRay);

		AddFieldBindings<&MeshComponent::Mesh>(table, "MeshComponent", "Mesh");
		AddFieldBindings<&MeshComponent::CastShadows>(table, "MeshComponent", "CastShadows");
		AddFieldBindings<&MeshComponent::Visible>(table, "MeshComponent", "Visible");
		table.Add("MeshComponent_GetMaterialCount", &MeshComponent_GetMaterialCount);
		table.Add("MeshComponent_GetMaterial", &MeshComponent_GetMaterial);
		table.Add("MeshComponent_SetMaterial", &MeshComponent_SetMaterial);

		AddFieldBindings<&DirectionalLightComponent::Color>(table, "DirectionalLightComponent", "Color");
		AddFieldBindings<&DirectionalLightComponent::Intensity>(table, "DirectionalLightComponent", "Intensity");
		AddFieldBindings<&DirectionalLightComponent::CastShadows>(table, "DirectionalLightComponent", "CastShadows");
		AddFieldBindings<&DirectionalLightComponent::LightSize>(table, "DirectionalLightComponent", "LightSize");

		AddFieldBindings<&PointLightComponent::Color>(table, "PointLightComponent", "Color");
		AddFieldBindings<&PointLightComponent::Intensity>(table, "PointLightComponent", "Intensity");
		AddFieldBindings<&PointLightComponent::Range>(table, "PointLightComponent", "Range");
		AddFieldBindings<&PointLightComponent::CastShadows>(table, "PointLightComponent", "CastShadows");

		AddFieldBindings<&SpotLightComponent::Color>(table, "SpotLightComponent", "Color");
		AddFieldBindings<&SpotLightComponent::Intensity>(table, "SpotLightComponent", "Intensity");
		AddFieldBindings<&SpotLightComponent::Range>(table, "SpotLightComponent", "Range");
		AddFieldBindings<&SpotLightComponent::InnerConeAngle>(table, "SpotLightComponent", "InnerConeAngle");
		AddFieldBindings<&SpotLightComponent::OuterConeAngle>(table, "SpotLightComponent", "OuterConeAngle");
		AddFieldBindings<&SpotLightComponent::CastShadows>(table, "SpotLightComponent", "CastShadows");

		AddFieldBindings<&SkyLightComponent::Environment>(table, "SkyLightComponent", "Environment");
		AddFieldBindings<&SkyLightComponent::Intensity>(table, "SkyLightComponent", "Intensity");
		AddFieldBindings<&SkyLightComponent::Rotation>(table, "SkyLightComponent", "Rotation");
		AddFieldBindings<&SkyLightComponent::SkyboxBlur>(table, "SkyLightComponent", "SkyboxBlur");
		AddFieldBindings<&SkyLightComponent::DrawSkybox>(table, "SkyLightComponent", "DrawSkybox");
		AddFieldBindings<&SkyLightComponent::AmbientColor>(table, "SkyLightComponent", "AmbientColor");

		// Type, mass and layer rebuild the body; the others act on it directly.
		AddFieldBindings<&RigidBodyComponent::Type>(table, "RigidBodyComponent", "Type");
		AddFieldBindings<&RigidBodyComponent::Mass>(table, "RigidBodyComponent", "Mass");
		AddFieldBindings<&RigidBodyComponent::Layer>(table, "RigidBodyComponent", "Layer");
		table.Add("RigidBodyComponent_GetGravityFactor", &GetField<&RigidBodyComponent::GravityFactor>);
		table.Add("RigidBodyComponent_SetGravityFactor", &RigidBodyComponent_SetGravityFactor);
		table.Add("RigidBodyComponent_GetLinearDamping", &GetField<&RigidBodyComponent::LinearDamping>);
		table.Add("RigidBodyComponent_SetLinearDamping", &RigidBodyComponent_SetLinearDamping);
		table.Add("RigidBodyComponent_GetAngularDamping", &GetField<&RigidBodyComponent::AngularDamping>);
		table.Add("RigidBodyComponent_SetAngularDamping", &RigidBodyComponent_SetAngularDamping);
		table.Add("RigidBodyComponent_GetLinearVelocity", &RigidBodyComponent_GetLinearVelocity);
		table.Add("RigidBodyComponent_SetLinearVelocity", &RigidBodyComponent_SetLinearVelocity);
		table.Add("RigidBodyComponent_GetAngularVelocity", &RigidBodyComponent_GetAngularVelocity);
		table.Add("RigidBodyComponent_SetAngularVelocity", &RigidBodyComponent_SetAngularVelocity);
		table.Add("RigidBodyComponent_AddForce", &RigidBodyComponent_AddForce);
		table.Add("RigidBodyComponent_AddTorque", &RigidBodyComponent_AddTorque);
		table.Add("RigidBodyComponent_MoveKinematic", &RigidBodyComponent_MoveKinematic);
		table.Add("RigidBodyComponent_Teleport", &RigidBodyComponent_Teleport);
		table.Add("RigidBodyComponent_IsSleeping", &RigidBodyComponent_IsSleeping);
		table.Add("RigidBodyComponent_WakeUp", &RigidBodyComponent_WakeUp);

		AddFieldBindings<&BoxColliderComponent::HalfExtents>(table, "BoxColliderComponent", "HalfExtents");
		AddFieldBindings<&BoxColliderComponent::Offset>(table, "BoxColliderComponent", "Offset");
		AddFieldBindings<&BoxColliderComponent::IsTrigger>(table, "BoxColliderComponent", "IsTrigger");
		AddFieldBindings<&BoxColliderComponent::Friction>(table, "BoxColliderComponent", "Friction");
		AddFieldBindings<&BoxColliderComponent::Restitution>(table, "BoxColliderComponent", "Restitution");

		AddFieldBindings<&SphereColliderComponent::Radius>(table, "SphereColliderComponent", "Radius");
		AddFieldBindings<&SphereColliderComponent::Offset>(table, "SphereColliderComponent", "Offset");
		AddFieldBindings<&SphereColliderComponent::IsTrigger>(table, "SphereColliderComponent", "IsTrigger");
		AddFieldBindings<&SphereColliderComponent::Friction>(table, "SphereColliderComponent", "Friction");
		AddFieldBindings<&SphereColliderComponent::Restitution>(table, "SphereColliderComponent", "Restitution");

		AddFieldBindings<&CapsuleColliderComponent::Radius>(table, "CapsuleColliderComponent", "Radius");
		AddFieldBindings<&CapsuleColliderComponent::HalfHeight>(table, "CapsuleColliderComponent", "HalfHeight");
		AddFieldBindings<&CapsuleColliderComponent::Offset>(table, "CapsuleColliderComponent", "Offset");
		AddFieldBindings<&CapsuleColliderComponent::IsTrigger>(table, "CapsuleColliderComponent", "IsTrigger");
		AddFieldBindings<&CapsuleColliderComponent::Friction>(table, "CapsuleColliderComponent", "Friction");
		AddFieldBindings<&CapsuleColliderComponent::Restitution>(table, "CapsuleColliderComponent", "Restitution");

		AddFieldBindings<&MeshColliderComponent::Mesh>(table, "MeshColliderComponent", "Mesh");
		AddFieldBindings<&MeshColliderComponent::Convex>(table, "MeshColliderComponent", "Convex");
		AddFieldBindings<&MeshColliderComponent::IsTrigger>(table, "MeshColliderComponent", "IsTrigger");
		AddFieldBindings<&MeshColliderComponent::Friction>(table, "MeshColliderComponent", "Friction");
		AddFieldBindings<&MeshColliderComponent::Restitution>(table, "MeshColliderComponent", "Restitution");

		AddFieldBindings<&AudioSourceComponent::Clip>(table, "AudioSourceComponent", "Clip");
		AddFieldBindings<&AudioSourceComponent::Volume>(table, "AudioSourceComponent", "Volume");
		AddFieldBindings<&AudioSourceComponent::Pitch>(table, "AudioSourceComponent", "Pitch");
		AddFieldBindings<&AudioSourceComponent::Loop>(table, "AudioSourceComponent", "Loop");
		AddFieldBindings<&AudioSourceComponent::PlayOnStart>(table, "AudioSourceComponent", "PlayOnStart");
		AddFieldBindings<&AudioSourceComponent::Spatial>(table, "AudioSourceComponent", "Spatial");
		AddFieldBindings<&AudioSourceComponent::MinDistance>(table, "AudioSourceComponent", "MinDistance");
		AddFieldBindings<&AudioSourceComponent::MaxDistance>(table, "AudioSourceComponent", "MaxDistance");
		table.Add("AudioSourceComponent_Play", &AudioSourceComponent_Play);
		table.Add("AudioSourceComponent_Pause", &AudioSourceComponent_Pause);
		table.Add("AudioSourceComponent_Stop", &AudioSourceComponent_Stop);
		table.Add("AudioSourceComponent_IsPlaying", &AudioSourceComponent_IsPlaying);

		AddFieldBindings<&AudioListenerComponent::Active>(table, "AudioListenerComponent", "Active");

		table.Add("TextComponent_GetText", &GetText<TextComponent, &TextComponent::Text>);
		table.Add("TextComponent_SetText", &SetText<TextComponent, &TextComponent::Text>);
		AddFieldBindings<&TextComponent::Font>(table, "TextComponent", "Font");
		AddFieldBindings<&TextComponent::Color>(table, "TextComponent", "Color");
		AddFieldBindings<&TextComponent::FontSize>(table, "TextComponent", "FontSize");
		AddFieldBindings<&TextComponent::ScreenSpace>(table, "TextComponent", "ScreenSpace");
		AddFieldBindings<&TextComponent::Alignment>(table, "TextComponent", "Alignment");
		AddFieldBindings<&TextComponent::LineSpacing>(table, "TextComponent", "LineSpacing");

		AddFieldBindings<&SpriteRendererComponent::Color>(table, "SpriteRendererComponent", "Color");
		AddFieldBindings<&SpriteRendererComponent::Texture>(table, "SpriteRendererComponent", "Texture");
		AddFieldBindings<&SpriteRendererComponent::Tiling>(table, "SpriteRendererComponent", "Tiling");
		AddFieldBindings<&SpriteRendererComponent::ScreenSpace>(table, "SpriteRendererComponent", "ScreenSpace");

		table.Add("ScriptComponent_GetClassName", &GetText<ScriptComponent, &ScriptComponent::ClassName>);
		table.Add("ScriptComponent_SetClassName", &SetText<ScriptComponent, &ScriptComponent::ClassName>);
	}
}
