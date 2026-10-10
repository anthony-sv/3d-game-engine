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
			if (!CheckFinite("TransformComponent.EulerAngles", *value))
			{
				return;
			}
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
			if (!CheckFinite("TransformComponent.WorldTranslation", *value))
			{
				return;
			}
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
			if (!CheckFinite("CameraComponent.ScreenToWorldRay", *screenPosition))
			{
				return;
			}
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
			if (!CheckFinite("RigidBodyComponent.LinearVelocity", *value))
			{
				return;
			}
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
			if (!CheckFinite("RigidBodyComponent.AngularVelocity", *value))
			{
				return;
			}
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
			if (!CheckFinite("RigidBodyComponent.GravityFactor", *value))
			{
				return;
			}
			if (RigidBodyComponent* body = FindComponent<RigidBodyComponent>(id))
			{
				body->GravityFactor = *value;
				if (PhysicsScene* physics = FindLiveBody(id))
				{
					physics->SetGravityFactor(UUID(id), *value);
				}
			}
		}

		void SetDamping(uint64_t id, float RigidBodyComponent::* member, float value, char const* function)
		{
			if (!CheckFinite(function, value))
			{
				return;
			}
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
			SetDamping(id, &RigidBodyComponent::LinearDamping, *value, "RigidBodyComponent.LinearDamping");
		}

		void RigidBodyComponent_SetAngularDamping(uint64_t id, float const* value)
		{
			SetDamping(id, &RigidBodyComponent::AngularDamping, *value, "RigidBodyComponent.AngularDamping");
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
				return;
			}
			if (!CheckFinite("RigidBodyComponent.AddForce", *force))
			{
				return;
			}
			if (PhysicsScene* physics = FindBody(id, "RigidBodyComponent.AddForce"))
			{
				physics->AddForce(UUID(id), FromScript(*force), static_cast<ForceMode>(mode));
			}
		}

		void RigidBodyComponent_AddTorque(uint64_t id, Vector3 const* torque, int32_t mode)
		{
			if (!IsForceMode(mode))
			{
				Log::GetScriptLogger().error("RigidBodyComponent.AddTorque: {} is not a ForceMode", mode);
				return;
			}
			if (!CheckFinite("RigidBodyComponent.AddTorque", *torque))
			{
				return;
			}
			if (PhysicsScene* physics = FindBody(id, "RigidBodyComponent.AddTorque"))
			{
				physics->AddTorque(UUID(id), FromScript(*torque), static_cast<ForceMode>(mode));
			}
		}

		// Places the entity (in world space, keeping its scale); the body follows before the next physics step, moving
		// there for kinematic bodies and teleporting otherwise.
		void PlaceBody(uint64_t id, Vector3 const* position, Quaternion const* rotation, char const* function)
		{
			if (!CheckFinite(function, *position, *rotation))
			{
				return;
			}
			Entity const entity = FindEntity(id, function);
			if (!entity || FindComponent<RigidBodyComponent>(id) == nullptr)
			{
				return;
			}
			glm::vec3 currentPosition;
			glm::quat currentRotation;
			glm::vec3 scale(1.0f);
			Math::DecomposeTransform(entity.GetScene()->GetWorldTransform(entity), currentPosition, currentRotation, scale);
			entity.GetScene()->SetWorldTransform(entity, Math::ComposeTransform(FromScript(*position), ToRotation(*rotation), scale));
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
		AddFieldBindings<&TransformComponent::Translation, "TransformComponent", "Translation">(table);
		AddFieldBindings<&TransformComponent::Rotation, "TransformComponent", "Rotation">(table);
		AddFieldBindings<&TransformComponent::Scale, "TransformComponent", "Scale">(table);
		table.Add("TransformComponent_GetEulerAngles", &TransformComponent_GetEulerAngles);
		table.Add("TransformComponent_SetEulerAngles", &TransformComponent_SetEulerAngles);
		table.Add("TransformComponent_GetWorldTransform", &TransformComponent_GetWorldTransform);
		table.Add("TransformComponent_GetWorldTranslation", &TransformComponent_GetWorldTranslation);
		table.Add("TransformComponent_SetWorldTranslation", &TransformComponent_SetWorldTranslation);
		table.Add("TransformComponent_GetForward", &TransformComponent_GetForward);
		table.Add("TransformComponent_GetRight", &TransformComponent_GetRight);
		table.Add("TransformComponent_GetUp", &TransformComponent_GetUp);

		AddFieldBindings<&CameraComponent::Projection, "CameraComponent", "Projection">(table);
		AddFieldBindings<&CameraComponent::PerspectiveFOV, "CameraComponent", "PerspectiveFOV">(table);
		AddFieldBindings<&CameraComponent::PerspectiveNear, "CameraComponent", "PerspectiveNear">(table);
		AddFieldBindings<&CameraComponent::PerspectiveFar, "CameraComponent", "PerspectiveFar">(table);
		AddFieldBindings<&CameraComponent::OrthographicSize, "CameraComponent", "OrthographicSize">(table);
		AddFieldBindings<&CameraComponent::OrthographicNear, "CameraComponent", "OrthographicNear">(table);
		AddFieldBindings<&CameraComponent::OrthographicFar, "CameraComponent", "OrthographicFar">(table);
		AddFieldBindings<&CameraComponent::Primary, "CameraComponent", "Primary">(table);
		AddFieldBindings<&CameraComponent::FixedAspectRatio, "CameraComponent", "FixedAspectRatio">(table);
		AddFieldBindings<&CameraComponent::AspectRatio, "CameraComponent", "AspectRatio">(table);
		table.Add("CameraComponent_ScreenToWorldRay", &CameraComponent_ScreenToWorldRay);

		AddFieldBindings<&MeshComponent::Mesh, "MeshComponent", "Mesh">(table);
		AddFieldBindings<&MeshComponent::CastShadows, "MeshComponent", "CastShadows">(table);
		AddFieldBindings<&MeshComponent::Visible, "MeshComponent", "Visible">(table);
		table.Add("MeshComponent_GetMaterialCount", &MeshComponent_GetMaterialCount);
		table.Add("MeshComponent_GetMaterial", &MeshComponent_GetMaterial);
		table.Add("MeshComponent_SetMaterial", &MeshComponent_SetMaterial);

		AddFieldBindings<&DirectionalLightComponent::Color, "DirectionalLightComponent", "Color">(table);
		AddFieldBindings<&DirectionalLightComponent::Intensity, "DirectionalLightComponent", "Intensity">(table);
		AddFieldBindings<&DirectionalLightComponent::CastShadows, "DirectionalLightComponent", "CastShadows">(table);
		AddFieldBindings<&DirectionalLightComponent::LightSize, "DirectionalLightComponent", "LightSize">(table);

		AddFieldBindings<&PointLightComponent::Color, "PointLightComponent", "Color">(table);
		AddFieldBindings<&PointLightComponent::Intensity, "PointLightComponent", "Intensity">(table);
		AddFieldBindings<&PointLightComponent::Range, "PointLightComponent", "Range">(table);
		AddFieldBindings<&PointLightComponent::CastShadows, "PointLightComponent", "CastShadows">(table);

		AddFieldBindings<&SpotLightComponent::Color, "SpotLightComponent", "Color">(table);
		AddFieldBindings<&SpotLightComponent::Intensity, "SpotLightComponent", "Intensity">(table);
		AddFieldBindings<&SpotLightComponent::Range, "SpotLightComponent", "Range">(table);
		AddFieldBindings<&SpotLightComponent::InnerConeAngle, "SpotLightComponent", "InnerConeAngle">(table);
		AddFieldBindings<&SpotLightComponent::OuterConeAngle, "SpotLightComponent", "OuterConeAngle">(table);
		AddFieldBindings<&SpotLightComponent::CastShadows, "SpotLightComponent", "CastShadows">(table);

		AddFieldBindings<&SkyLightComponent::Environment, "SkyLightComponent", "Environment">(table);
		AddFieldBindings<&SkyLightComponent::Intensity, "SkyLightComponent", "Intensity">(table);
		AddFieldBindings<&SkyLightComponent::Rotation, "SkyLightComponent", "Rotation">(table);
		AddFieldBindings<&SkyLightComponent::SkyboxBlur, "SkyLightComponent", "SkyboxBlur">(table);
		AddFieldBindings<&SkyLightComponent::DrawSkybox, "SkyLightComponent", "DrawSkybox">(table);
		AddFieldBindings<&SkyLightComponent::AmbientColor, "SkyLightComponent", "AmbientColor">(table);

		// Type, mass and layer rebuild the body; the others act on it directly.
		AddFieldBindings<&RigidBodyComponent::Type, "RigidBodyComponent", "Type">(table);
		AddFieldBindings<&RigidBodyComponent::Mass, "RigidBodyComponent", "Mass">(table);
		AddFieldBindings<&RigidBodyComponent::Layer, "RigidBodyComponent", "Layer">(table);
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

		AddFieldBindings<&BoxColliderComponent::HalfExtents, "BoxColliderComponent", "HalfExtents">(table);
		AddFieldBindings<&BoxColliderComponent::Offset, "BoxColliderComponent", "Offset">(table);
		AddFieldBindings<&BoxColliderComponent::IsTrigger, "BoxColliderComponent", "IsTrigger">(table);
		AddFieldBindings<&BoxColliderComponent::Friction, "BoxColliderComponent", "Friction">(table);
		AddFieldBindings<&BoxColliderComponent::Restitution, "BoxColliderComponent", "Restitution">(table);

		AddFieldBindings<&SphereColliderComponent::Radius, "SphereColliderComponent", "Radius">(table);
		AddFieldBindings<&SphereColliderComponent::Offset, "SphereColliderComponent", "Offset">(table);
		AddFieldBindings<&SphereColliderComponent::IsTrigger, "SphereColliderComponent", "IsTrigger">(table);
		AddFieldBindings<&SphereColliderComponent::Friction, "SphereColliderComponent", "Friction">(table);
		AddFieldBindings<&SphereColliderComponent::Restitution, "SphereColliderComponent", "Restitution">(table);

		AddFieldBindings<&CapsuleColliderComponent::Radius, "CapsuleColliderComponent", "Radius">(table);
		AddFieldBindings<&CapsuleColliderComponent::HalfHeight, "CapsuleColliderComponent", "HalfHeight">(table);
		AddFieldBindings<&CapsuleColliderComponent::Offset, "CapsuleColliderComponent", "Offset">(table);
		AddFieldBindings<&CapsuleColliderComponent::IsTrigger, "CapsuleColliderComponent", "IsTrigger">(table);
		AddFieldBindings<&CapsuleColliderComponent::Friction, "CapsuleColliderComponent", "Friction">(table);
		AddFieldBindings<&CapsuleColliderComponent::Restitution, "CapsuleColliderComponent", "Restitution">(table);

		AddFieldBindings<&MeshColliderComponent::Mesh, "MeshColliderComponent", "Mesh">(table);
		AddFieldBindings<&MeshColliderComponent::Convex, "MeshColliderComponent", "Convex">(table);
		AddFieldBindings<&MeshColliderComponent::IsTrigger, "MeshColliderComponent", "IsTrigger">(table);
		AddFieldBindings<&MeshColliderComponent::Friction, "MeshColliderComponent", "Friction">(table);
		AddFieldBindings<&MeshColliderComponent::Restitution, "MeshColliderComponent", "Restitution">(table);

		AddFieldBindings<&AudioSourceComponent::Clip, "AudioSourceComponent", "Clip">(table);
		AddFieldBindings<&AudioSourceComponent::Volume, "AudioSourceComponent", "Volume">(table);
		AddFieldBindings<&AudioSourceComponent::Pitch, "AudioSourceComponent", "Pitch">(table);
		AddFieldBindings<&AudioSourceComponent::Loop, "AudioSourceComponent", "Loop">(table);
		AddFieldBindings<&AudioSourceComponent::PlayOnStart, "AudioSourceComponent", "PlayOnStart">(table);
		AddFieldBindings<&AudioSourceComponent::Spatial, "AudioSourceComponent", "Spatial">(table);
		AddFieldBindings<&AudioSourceComponent::MinDistance, "AudioSourceComponent", "MinDistance">(table);
		AddFieldBindings<&AudioSourceComponent::MaxDistance, "AudioSourceComponent", "MaxDistance">(table);
		table.Add("AudioSourceComponent_Play", &AudioSourceComponent_Play);
		table.Add("AudioSourceComponent_Pause", &AudioSourceComponent_Pause);
		table.Add("AudioSourceComponent_Stop", &AudioSourceComponent_Stop);
		table.Add("AudioSourceComponent_IsPlaying", &AudioSourceComponent_IsPlaying);

		AddFieldBindings<&AudioListenerComponent::Active, "AudioListenerComponent", "Active">(table);

		table.Add("TextComponent_GetText", &GetText<TextComponent, &TextComponent::Text>);
		table.Add("TextComponent_SetText", &SetText<TextComponent, &TextComponent::Text>);
		AddFieldBindings<&TextComponent::Font, "TextComponent", "Font">(table);
		AddFieldBindings<&TextComponent::Color, "TextComponent", "Color">(table);
		AddFieldBindings<&TextComponent::FontSize, "TextComponent", "FontSize">(table);
		AddFieldBindings<&TextComponent::ScreenSpace, "TextComponent", "ScreenSpace">(table);
		AddFieldBindings<&TextComponent::Alignment, "TextComponent", "Alignment">(table);
		AddFieldBindings<&TextComponent::LineSpacing, "TextComponent", "LineSpacing">(table);

		AddFieldBindings<&SpriteRendererComponent::Color, "SpriteRendererComponent", "Color">(table);
		AddFieldBindings<&SpriteRendererComponent::Texture, "SpriteRendererComponent", "Texture">(table);
		AddFieldBindings<&SpriteRendererComponent::Tiling, "SpriteRendererComponent", "Tiling">(table);
		AddFieldBindings<&SpriteRendererComponent::ScreenSpace, "SpriteRendererComponent", "ScreenSpace">(table);

		table.Add("ScriptComponent_GetClassName", &GetText<ScriptComponent, &ScriptComponent::ClassName>);
		table.Add("ScriptComponent_SetClassName", &SetText<ScriptComponent, &ScriptComponent::ClassName>);
	}
}
