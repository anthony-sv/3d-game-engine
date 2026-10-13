using System;
using System.Collections.Generic;
using System.Reflection;

namespace Strada.Interop;

// Engine functions scripts call. Each field is bound at startup to the native function of the same name
// (ScriptBindings.cpp); both sides change together, and a missing or extra binding fails the startup.
internal static unsafe class InternalCalls
{
#pragma warning disable CS0649 // Assigned by Bind through reflection.
	internal static delegate* unmanaged<int, byte*, int, void> Log_Write;

	internal static delegate* unmanaged<ulong, byte> Entity_IsValid;
	internal static delegate* unmanaged<ulong, int*, byte*> Entity_GetName;
	internal static delegate* unmanaged<ulong, byte*, int, void> Entity_SetName;
	internal static delegate* unmanaged<ulong, byte*, int, byte> Entity_HasComponent;
	internal static delegate* unmanaged<ulong, byte*, int, void> Entity_AddComponent;
	internal static delegate* unmanaged<ulong, byte*, int, void> Entity_RemoveComponent;
	internal static delegate* unmanaged<byte*, int, ulong> Entity_Create;
	internal static delegate* unmanaged<ulong, Vector3*, Quaternion*, ulong, ulong> Entity_Instantiate;
	internal static delegate* unmanaged<ulong, void> Entity_Destroy;
	internal static delegate* unmanaged<byte*, int, ulong> Entity_FindByName;
	internal static delegate* unmanaged<ulong, ulong> Entity_GetParent;
	internal static delegate* unmanaged<ulong, ulong, void> Entity_SetParent;
	internal static delegate* unmanaged<ulong, ulong*, int, int> Entity_GetChildren;

	internal static delegate* unmanaged<ulong, Vector3*, void> TransformComponent_GetTranslation;
	internal static delegate* unmanaged<ulong, Vector3*, void> TransformComponent_SetTranslation;
	internal static delegate* unmanaged<ulong, Quaternion*, void> TransformComponent_GetRotation;
	internal static delegate* unmanaged<ulong, Quaternion*, void> TransformComponent_SetRotation;
	internal static delegate* unmanaged<ulong, Vector3*, void> TransformComponent_GetScale;
	internal static delegate* unmanaged<ulong, Vector3*, void> TransformComponent_SetScale;
	internal static delegate* unmanaged<ulong, Vector3*, void> TransformComponent_GetEulerAngles;
	internal static delegate* unmanaged<ulong, Vector3*, void> TransformComponent_SetEulerAngles;
	internal static delegate* unmanaged<ulong, Matrix4*, void> TransformComponent_GetWorldTransform;
	internal static delegate* unmanaged<ulong, Vector3*, void> TransformComponent_GetWorldTranslation;
	internal static delegate* unmanaged<ulong, Vector3*, void> TransformComponent_SetWorldTranslation;
	internal static delegate* unmanaged<ulong, Vector3*, void> TransformComponent_GetForward;
	internal static delegate* unmanaged<ulong, Vector3*, void> TransformComponent_GetRight;
	internal static delegate* unmanaged<ulong, Vector3*, void> TransformComponent_GetUp;

	internal static delegate* unmanaged<ulong, int*, void> CameraComponent_GetProjection;
	internal static delegate* unmanaged<ulong, int*, void> CameraComponent_SetProjection;
	internal static delegate* unmanaged<ulong, float*, void> CameraComponent_GetPerspectiveFOV;
	internal static delegate* unmanaged<ulong, float*, void> CameraComponent_SetPerspectiveFOV;
	internal static delegate* unmanaged<ulong, float*, void> CameraComponent_GetPerspectiveNear;
	internal static delegate* unmanaged<ulong, float*, void> CameraComponent_SetPerspectiveNear;
	internal static delegate* unmanaged<ulong, float*, void> CameraComponent_GetPerspectiveFar;
	internal static delegate* unmanaged<ulong, float*, void> CameraComponent_SetPerspectiveFar;
	internal static delegate* unmanaged<ulong, float*, void> CameraComponent_GetOrthographicSize;
	internal static delegate* unmanaged<ulong, float*, void> CameraComponent_SetOrthographicSize;
	internal static delegate* unmanaged<ulong, float*, void> CameraComponent_GetOrthographicNear;
	internal static delegate* unmanaged<ulong, float*, void> CameraComponent_SetOrthographicNear;
	internal static delegate* unmanaged<ulong, float*, void> CameraComponent_GetOrthographicFar;
	internal static delegate* unmanaged<ulong, float*, void> CameraComponent_SetOrthographicFar;
	internal static delegate* unmanaged<ulong, byte*, void> CameraComponent_GetPrimary;
	internal static delegate* unmanaged<ulong, byte*, void> CameraComponent_SetPrimary;
	internal static delegate* unmanaged<ulong, byte*, void> CameraComponent_GetFixedAspectRatio;
	internal static delegate* unmanaged<ulong, byte*, void> CameraComponent_SetFixedAspectRatio;
	internal static delegate* unmanaged<ulong, float*, void> CameraComponent_GetAspectRatio;
	internal static delegate* unmanaged<ulong, float*, void> CameraComponent_SetAspectRatio;
	internal static delegate* unmanaged<ulong, Vector2*, Ray*, void> CameraComponent_ScreenToWorldRay;
	internal static delegate* unmanaged<ulong, Vector3*, Vector3*, void> CameraComponent_WorldToScreenPoint;

	internal static delegate* unmanaged<ulong, ulong*, void> MeshComponent_GetMesh;
	internal static delegate* unmanaged<ulong, ulong*, void> MeshComponent_SetMesh;
	internal static delegate* unmanaged<ulong, byte*, void> MeshComponent_GetCastShadows;
	internal static delegate* unmanaged<ulong, byte*, void> MeshComponent_SetCastShadows;
	internal static delegate* unmanaged<ulong, byte*, void> MeshComponent_GetVisible;
	internal static delegate* unmanaged<ulong, byte*, void> MeshComponent_SetVisible;
	internal static delegate* unmanaged<ulong, int> MeshComponent_GetMaterialCount;
	internal static delegate* unmanaged<ulong, int, ulong> MeshComponent_GetMaterial;
	internal static delegate* unmanaged<ulong, int, ulong, void> MeshComponent_SetMaterial;

	internal static delegate* unmanaged<ulong, Vector3*, void> DirectionalLightComponent_GetColor;
	internal static delegate* unmanaged<ulong, Vector3*, void> DirectionalLightComponent_SetColor;
	internal static delegate* unmanaged<ulong, float*, void> DirectionalLightComponent_GetIntensity;
	internal static delegate* unmanaged<ulong, float*, void> DirectionalLightComponent_SetIntensity;
	internal static delegate* unmanaged<ulong, byte*, void> DirectionalLightComponent_GetCastShadows;
	internal static delegate* unmanaged<ulong, byte*, void> DirectionalLightComponent_SetCastShadows;
	internal static delegate* unmanaged<ulong, float*, void> DirectionalLightComponent_GetLightSize;
	internal static delegate* unmanaged<ulong, float*, void> DirectionalLightComponent_SetLightSize;

	internal static delegate* unmanaged<ulong, Vector3*, void> PointLightComponent_GetColor;
	internal static delegate* unmanaged<ulong, Vector3*, void> PointLightComponent_SetColor;
	internal static delegate* unmanaged<ulong, float*, void> PointLightComponent_GetIntensity;
	internal static delegate* unmanaged<ulong, float*, void> PointLightComponent_SetIntensity;
	internal static delegate* unmanaged<ulong, float*, void> PointLightComponent_GetRange;
	internal static delegate* unmanaged<ulong, float*, void> PointLightComponent_SetRange;
	internal static delegate* unmanaged<ulong, byte*, void> PointLightComponent_GetCastShadows;
	internal static delegate* unmanaged<ulong, byte*, void> PointLightComponent_SetCastShadows;

	internal static delegate* unmanaged<ulong, Vector3*, void> SpotLightComponent_GetColor;
	internal static delegate* unmanaged<ulong, Vector3*, void> SpotLightComponent_SetColor;
	internal static delegate* unmanaged<ulong, float*, void> SpotLightComponent_GetIntensity;
	internal static delegate* unmanaged<ulong, float*, void> SpotLightComponent_SetIntensity;
	internal static delegate* unmanaged<ulong, float*, void> SpotLightComponent_GetRange;
	internal static delegate* unmanaged<ulong, float*, void> SpotLightComponent_SetRange;
	internal static delegate* unmanaged<ulong, float*, void> SpotLightComponent_GetInnerConeAngle;
	internal static delegate* unmanaged<ulong, float*, void> SpotLightComponent_SetInnerConeAngle;
	internal static delegate* unmanaged<ulong, float*, void> SpotLightComponent_GetOuterConeAngle;
	internal static delegate* unmanaged<ulong, float*, void> SpotLightComponent_SetOuterConeAngle;
	internal static delegate* unmanaged<ulong, byte*, void> SpotLightComponent_GetCastShadows;
	internal static delegate* unmanaged<ulong, byte*, void> SpotLightComponent_SetCastShadows;

	internal static delegate* unmanaged<ulong, ulong*, void> SkyLightComponent_GetEnvironment;
	internal static delegate* unmanaged<ulong, ulong*, void> SkyLightComponent_SetEnvironment;
	internal static delegate* unmanaged<ulong, float*, void> SkyLightComponent_GetIntensity;
	internal static delegate* unmanaged<ulong, float*, void> SkyLightComponent_SetIntensity;
	internal static delegate* unmanaged<ulong, float*, void> SkyLightComponent_GetRotation;
	internal static delegate* unmanaged<ulong, float*, void> SkyLightComponent_SetRotation;
	internal static delegate* unmanaged<ulong, float*, void> SkyLightComponent_GetSkyboxBlur;
	internal static delegate* unmanaged<ulong, float*, void> SkyLightComponent_SetSkyboxBlur;
	internal static delegate* unmanaged<ulong, byte*, void> SkyLightComponent_GetDrawSkybox;
	internal static delegate* unmanaged<ulong, byte*, void> SkyLightComponent_SetDrawSkybox;
	internal static delegate* unmanaged<ulong, Vector3*, void> SkyLightComponent_GetAmbientColor;
	internal static delegate* unmanaged<ulong, Vector3*, void> SkyLightComponent_SetAmbientColor;

	internal static delegate* unmanaged<ulong, int*, void> RigidBodyComponent_GetType;
	internal static delegate* unmanaged<ulong, int*, void> RigidBodyComponent_SetType;
	internal static delegate* unmanaged<ulong, float*, void> RigidBodyComponent_GetMass;
	internal static delegate* unmanaged<ulong, float*, void> RigidBodyComponent_SetMass;
	internal static delegate* unmanaged<ulong, uint*, void> RigidBodyComponent_GetLayer;
	internal static delegate* unmanaged<ulong, uint*, void> RigidBodyComponent_SetLayer;
	internal static delegate* unmanaged<ulong, float*, void> RigidBodyComponent_GetGravityFactor;
	internal static delegate* unmanaged<ulong, float*, void> RigidBodyComponent_SetGravityFactor;
	internal static delegate* unmanaged<ulong, float*, void> RigidBodyComponent_GetLinearDamping;
	internal static delegate* unmanaged<ulong, float*, void> RigidBodyComponent_SetLinearDamping;
	internal static delegate* unmanaged<ulong, float*, void> RigidBodyComponent_GetAngularDamping;
	internal static delegate* unmanaged<ulong, float*, void> RigidBodyComponent_SetAngularDamping;
	internal static delegate* unmanaged<ulong, Vector3*, void> RigidBodyComponent_GetLinearVelocity;
	internal static delegate* unmanaged<ulong, Vector3*, void> RigidBodyComponent_SetLinearVelocity;
	internal static delegate* unmanaged<ulong, Vector3*, void> RigidBodyComponent_GetAngularVelocity;
	internal static delegate* unmanaged<ulong, Vector3*, void> RigidBodyComponent_SetAngularVelocity;
	internal static delegate* unmanaged<ulong, Vector3*, int, void> RigidBodyComponent_AddForce;
	internal static delegate* unmanaged<ulong, Vector3*, int, void> RigidBodyComponent_AddTorque;
	internal static delegate* unmanaged<ulong, Vector3*, Quaternion*, void> RigidBodyComponent_MoveKinematic;
	internal static delegate* unmanaged<ulong, Vector3*, Quaternion*, void> RigidBodyComponent_Teleport;
	internal static delegate* unmanaged<ulong, byte> RigidBodyComponent_IsSleeping;
	internal static delegate* unmanaged<ulong, void> RigidBodyComponent_WakeUp;

	internal static delegate* unmanaged<ulong, Vector3*, void> BoxColliderComponent_GetHalfExtents;
	internal static delegate* unmanaged<ulong, Vector3*, void> BoxColliderComponent_SetHalfExtents;
	internal static delegate* unmanaged<ulong, Vector3*, void> BoxColliderComponent_GetOffset;
	internal static delegate* unmanaged<ulong, Vector3*, void> BoxColliderComponent_SetOffset;
	internal static delegate* unmanaged<ulong, byte*, void> BoxColliderComponent_GetIsTrigger;
	internal static delegate* unmanaged<ulong, byte*, void> BoxColliderComponent_SetIsTrigger;
	internal static delegate* unmanaged<ulong, float*, void> BoxColliderComponent_GetFriction;
	internal static delegate* unmanaged<ulong, float*, void> BoxColliderComponent_SetFriction;
	internal static delegate* unmanaged<ulong, float*, void> BoxColliderComponent_GetRestitution;
	internal static delegate* unmanaged<ulong, float*, void> BoxColliderComponent_SetRestitution;

	internal static delegate* unmanaged<ulong, float*, void> SphereColliderComponent_GetRadius;
	internal static delegate* unmanaged<ulong, float*, void> SphereColliderComponent_SetRadius;
	internal static delegate* unmanaged<ulong, Vector3*, void> SphereColliderComponent_GetOffset;
	internal static delegate* unmanaged<ulong, Vector3*, void> SphereColliderComponent_SetOffset;
	internal static delegate* unmanaged<ulong, byte*, void> SphereColliderComponent_GetIsTrigger;
	internal static delegate* unmanaged<ulong, byte*, void> SphereColliderComponent_SetIsTrigger;
	internal static delegate* unmanaged<ulong, float*, void> SphereColliderComponent_GetFriction;
	internal static delegate* unmanaged<ulong, float*, void> SphereColliderComponent_SetFriction;
	internal static delegate* unmanaged<ulong, float*, void> SphereColliderComponent_GetRestitution;
	internal static delegate* unmanaged<ulong, float*, void> SphereColliderComponent_SetRestitution;

	internal static delegate* unmanaged<ulong, float*, void> CapsuleColliderComponent_GetRadius;
	internal static delegate* unmanaged<ulong, float*, void> CapsuleColliderComponent_SetRadius;
	internal static delegate* unmanaged<ulong, float*, void> CapsuleColliderComponent_GetHalfHeight;
	internal static delegate* unmanaged<ulong, float*, void> CapsuleColliderComponent_SetHalfHeight;
	internal static delegate* unmanaged<ulong, Vector3*, void> CapsuleColliderComponent_GetOffset;
	internal static delegate* unmanaged<ulong, Vector3*, void> CapsuleColliderComponent_SetOffset;
	internal static delegate* unmanaged<ulong, byte*, void> CapsuleColliderComponent_GetIsTrigger;
	internal static delegate* unmanaged<ulong, byte*, void> CapsuleColliderComponent_SetIsTrigger;
	internal static delegate* unmanaged<ulong, float*, void> CapsuleColliderComponent_GetFriction;
	internal static delegate* unmanaged<ulong, float*, void> CapsuleColliderComponent_SetFriction;
	internal static delegate* unmanaged<ulong, float*, void> CapsuleColliderComponent_GetRestitution;
	internal static delegate* unmanaged<ulong, float*, void> CapsuleColliderComponent_SetRestitution;

	internal static delegate* unmanaged<ulong, ulong*, void> MeshColliderComponent_GetMesh;
	internal static delegate* unmanaged<ulong, ulong*, void> MeshColliderComponent_SetMesh;
	internal static delegate* unmanaged<ulong, byte*, void> MeshColliderComponent_GetConvex;
	internal static delegate* unmanaged<ulong, byte*, void> MeshColliderComponent_SetConvex;
	internal static delegate* unmanaged<ulong, byte*, void> MeshColliderComponent_GetIsTrigger;
	internal static delegate* unmanaged<ulong, byte*, void> MeshColliderComponent_SetIsTrigger;
	internal static delegate* unmanaged<ulong, float*, void> MeshColliderComponent_GetFriction;
	internal static delegate* unmanaged<ulong, float*, void> MeshColliderComponent_SetFriction;
	internal static delegate* unmanaged<ulong, float*, void> MeshColliderComponent_GetRestitution;
	internal static delegate* unmanaged<ulong, float*, void> MeshColliderComponent_SetRestitution;

	internal static delegate* unmanaged<ulong, int*, void> JointComponent_GetType;
	internal static delegate* unmanaged<ulong, int*, void> JointComponent_SetType;
	internal static delegate* unmanaged<ulong, ulong*, void> JointComponent_GetConnectedEntity;
	internal static delegate* unmanaged<ulong, ulong*, void> JointComponent_SetConnectedEntity;
	internal static delegate* unmanaged<ulong, Vector3*, void> JointComponent_GetAnchor;
	internal static delegate* unmanaged<ulong, Vector3*, void> JointComponent_SetAnchor;
	internal static delegate* unmanaged<ulong, Vector3*, void> JointComponent_GetConnectedAnchor;
	internal static delegate* unmanaged<ulong, Vector3*, void> JointComponent_SetConnectedAnchor;
	internal static delegate* unmanaged<ulong, Vector3*, void> JointComponent_GetAxis;
	internal static delegate* unmanaged<ulong, Vector3*, void> JointComponent_SetAxis;
	internal static delegate* unmanaged<ulong, byte*, void> JointComponent_GetLimitsEnabled;
	internal static delegate* unmanaged<ulong, byte*, void> JointComponent_SetLimitsEnabled;
	internal static delegate* unmanaged<ulong, float*, void> JointComponent_GetMinLimit;
	internal static delegate* unmanaged<ulong, float*, void> JointComponent_SetMinLimit;
	internal static delegate* unmanaged<ulong, float*, void> JointComponent_GetMaxLimit;
	internal static delegate* unmanaged<ulong, float*, void> JointComponent_SetMaxLimit;
	internal static delegate* unmanaged<ulong, float*, void> JointComponent_GetMinDistance;
	internal static delegate* unmanaged<ulong, float*, void> JointComponent_SetMinDistance;
	internal static delegate* unmanaged<ulong, float*, void> JointComponent_GetMaxDistance;
	internal static delegate* unmanaged<ulong, float*, void> JointComponent_SetMaxDistance;
	internal static delegate* unmanaged<ulong, byte*, void> JointComponent_GetMotorEnabled;
	internal static delegate* unmanaged<ulong, byte*, void> JointComponent_SetMotorEnabled;
	internal static delegate* unmanaged<ulong, float*, void> JointComponent_GetMotorSpeed;
	internal static delegate* unmanaged<ulong, float*, void> JointComponent_SetMotorSpeed;
	internal static delegate* unmanaged<ulong, float*, void> JointComponent_GetMotorMaxForce;
	internal static delegate* unmanaged<ulong, float*, void> JointComponent_SetMotorMaxForce;

	internal static delegate* unmanaged<ulong, float*, void> CharacterControllerComponent_GetRadius;
	internal static delegate* unmanaged<ulong, float*, void> CharacterControllerComponent_SetRadius;
	internal static delegate* unmanaged<ulong, float*, void> CharacterControllerComponent_GetHeight;
	internal static delegate* unmanaged<ulong, float*, void> CharacterControllerComponent_SetHeight;
	internal static delegate* unmanaged<ulong, Vector3*, void> CharacterControllerComponent_GetOffset;
	internal static delegate* unmanaged<ulong, Vector3*, void> CharacterControllerComponent_SetOffset;
	internal static delegate* unmanaged<ulong, float*, void> CharacterControllerComponent_GetStepHeight;
	internal static delegate* unmanaged<ulong, float*, void> CharacterControllerComponent_SetStepHeight;
	internal static delegate* unmanaged<ulong, float*, void> CharacterControllerComponent_GetMaxSlopeAngle;
	internal static delegate* unmanaged<ulong, float*, void> CharacterControllerComponent_SetMaxSlopeAngle;
	internal static delegate* unmanaged<ulong, float*, void> CharacterControllerComponent_GetMass;
	internal static delegate* unmanaged<ulong, float*, void> CharacterControllerComponent_SetMass;
	internal static delegate* unmanaged<ulong, float*, void> CharacterControllerComponent_GetPushForce;
	internal static delegate* unmanaged<ulong, float*, void> CharacterControllerComponent_SetPushForce;
	internal static delegate* unmanaged<ulong, float*, void> CharacterControllerComponent_GetGravityFactor;
	internal static delegate* unmanaged<ulong, float*, void> CharacterControllerComponent_SetGravityFactor;
	internal static delegate* unmanaged<ulong, uint*, void> CharacterControllerComponent_GetLayer;
	internal static delegate* unmanaged<ulong, uint*, void> CharacterControllerComponent_SetLayer;
	internal static delegate* unmanaged<ulong, Vector3*, void> CharacterControllerComponent_Move;
	internal static delegate* unmanaged<ulong, Vector3*, void> CharacterControllerComponent_GetVelocity;
	internal static delegate* unmanaged<ulong, Vector3*, void> CharacterControllerComponent_SetVelocity;
	internal static delegate* unmanaged<ulong, byte> CharacterControllerComponent_IsGrounded;
	internal static delegate* unmanaged<ulong, Vector3*, void> CharacterControllerComponent_GetGroundNormal;
	internal static delegate* unmanaged<ulong, Vector3*, void> CharacterControllerComponent_Teleport;

	internal static delegate* unmanaged<ulong, ulong*, void> AudioSourceComponent_GetClip;
	internal static delegate* unmanaged<ulong, ulong*, void> AudioSourceComponent_SetClip;
	internal static delegate* unmanaged<ulong, byte*, void> AudioSourceComponent_GetStream;
	internal static delegate* unmanaged<ulong, byte*, void> AudioSourceComponent_SetStream;
	internal static delegate* unmanaged<ulong, float*, void> AudioSourceComponent_GetVolume;
	internal static delegate* unmanaged<ulong, float*, void> AudioSourceComponent_SetVolume;
	internal static delegate* unmanaged<ulong, float*, void> AudioSourceComponent_GetPitch;
	internal static delegate* unmanaged<ulong, float*, void> AudioSourceComponent_SetPitch;
	internal static delegate* unmanaged<ulong, byte*, void> AudioSourceComponent_GetLoop;
	internal static delegate* unmanaged<ulong, byte*, void> AudioSourceComponent_SetLoop;
	internal static delegate* unmanaged<ulong, byte*, void> AudioSourceComponent_GetPlayOnStart;
	internal static delegate* unmanaged<ulong, byte*, void> AudioSourceComponent_SetPlayOnStart;
	internal static delegate* unmanaged<ulong, byte*, void> AudioSourceComponent_GetSpatial;
	internal static delegate* unmanaged<ulong, byte*, void> AudioSourceComponent_SetSpatial;
	internal static delegate* unmanaged<ulong, float*, void> AudioSourceComponent_GetMinDistance;
	internal static delegate* unmanaged<ulong, float*, void> AudioSourceComponent_SetMinDistance;
	internal static delegate* unmanaged<ulong, float*, void> AudioSourceComponent_GetMaxDistance;
	internal static delegate* unmanaged<ulong, float*, void> AudioSourceComponent_SetMaxDistance;
	internal static delegate* unmanaged<ulong, void> AudioSourceComponent_Play;
	internal static delegate* unmanaged<ulong, void> AudioSourceComponent_Pause;
	internal static delegate* unmanaged<ulong, void> AudioSourceComponent_Stop;
	internal static delegate* unmanaged<ulong, byte> AudioSourceComponent_IsPlaying;

	internal static delegate* unmanaged<ulong, byte*, void> AudioListenerComponent_GetActive;
	internal static delegate* unmanaged<ulong, byte*, void> AudioListenerComponent_SetActive;

	internal static delegate* unmanaged<ulong, int*, byte*> TextComponent_GetText;
	internal static delegate* unmanaged<ulong, byte*, int, void> TextComponent_SetText;
	internal static delegate* unmanaged<ulong, ulong*, void> TextComponent_GetFont;
	internal static delegate* unmanaged<ulong, ulong*, void> TextComponent_SetFont;
	internal static delegate* unmanaged<ulong, Color*, void> TextComponent_GetColor;
	internal static delegate* unmanaged<ulong, Color*, void> TextComponent_SetColor;
	internal static delegate* unmanaged<ulong, float*, void> TextComponent_GetFontSize;
	internal static delegate* unmanaged<ulong, float*, void> TextComponent_SetFontSize;
	internal static delegate* unmanaged<ulong, byte*, void> TextComponent_GetScreenSpace;
	internal static delegate* unmanaged<ulong, byte*, void> TextComponent_SetScreenSpace;
	internal static delegate* unmanaged<ulong, int*, void> TextComponent_GetAlignment;
	internal static delegate* unmanaged<ulong, int*, void> TextComponent_SetAlignment;
	internal static delegate* unmanaged<ulong, float*, void> TextComponent_GetLineSpacing;
	internal static delegate* unmanaged<ulong, float*, void> TextComponent_SetLineSpacing;

	internal static delegate* unmanaged<ulong, Color*, void> SpriteRendererComponent_GetColor;
	internal static delegate* unmanaged<ulong, Color*, void> SpriteRendererComponent_SetColor;
	internal static delegate* unmanaged<ulong, ulong*, void> SpriteRendererComponent_GetTexture;
	internal static delegate* unmanaged<ulong, ulong*, void> SpriteRendererComponent_SetTexture;
	internal static delegate* unmanaged<ulong, float*, void> SpriteRendererComponent_GetTiling;
	internal static delegate* unmanaged<ulong, float*, void> SpriteRendererComponent_SetTiling;
	internal static delegate* unmanaged<ulong, byte*, void> SpriteRendererComponent_GetScreenSpace;
	internal static delegate* unmanaged<ulong, byte*, void> SpriteRendererComponent_SetScreenSpace;

	internal static delegate* unmanaged<ulong, byte*, void> ParticleSystemComponent_GetEmitting;
	internal static delegate* unmanaged<ulong, byte*, void> ParticleSystemComponent_SetEmitting;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_GetRate;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_SetRate;
	internal static delegate* unmanaged<ulong, uint*, void> ParticleSystemComponent_GetBurst;
	internal static delegate* unmanaged<ulong, uint*, void> ParticleSystemComponent_SetBurst;
	internal static delegate* unmanaged<ulong, uint*, void> ParticleSystemComponent_GetMaxParticles;
	internal static delegate* unmanaged<ulong, uint*, void> ParticleSystemComponent_SetMaxParticles;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_GetLifetimeMin;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_SetLifetimeMin;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_GetLifetimeMax;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_SetLifetimeMax;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_GetSpeedMin;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_SetSpeedMin;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_GetSpeedMax;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_SetSpeedMax;
	internal static delegate* unmanaged<ulong, int*, void> ParticleSystemComponent_GetShape;
	internal static delegate* unmanaged<ulong, int*, void> ParticleSystemComponent_SetShape;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_GetConeAngle;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_SetConeAngle;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_GetRadius;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_SetRadius;
	internal static delegate* unmanaged<ulong, Vector3*, void> ParticleSystemComponent_GetBoxHalfExtents;
	internal static delegate* unmanaged<ulong, Vector3*, void> ParticleSystemComponent_SetBoxHalfExtents;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_GetGravityFactor;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_SetGravityFactor;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_GetDrag;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_SetDrag;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_GetStartSize;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_SetStartSize;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_GetEndSize;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_SetEndSize;
	internal static delegate* unmanaged<ulong, Color*, void> ParticleSystemComponent_GetStartColor;
	internal static delegate* unmanaged<ulong, Color*, void> ParticleSystemComponent_SetStartColor;
	internal static delegate* unmanaged<ulong, Color*, void> ParticleSystemComponent_GetEndColor;
	internal static delegate* unmanaged<ulong, Color*, void> ParticleSystemComponent_SetEndColor;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_GetIntensity;
	internal static delegate* unmanaged<ulong, float*, void> ParticleSystemComponent_SetIntensity;
	internal static delegate* unmanaged<ulong, ulong*, void> ParticleSystemComponent_GetTexture;
	internal static delegate* unmanaged<ulong, ulong*, void> ParticleSystemComponent_SetTexture;
	internal static delegate* unmanaged<ulong, int*, void> ParticleSystemComponent_GetBlendMode;
	internal static delegate* unmanaged<ulong, int*, void> ParticleSystemComponent_SetBlendMode;
	internal static delegate* unmanaged<ulong, byte*, void> ParticleSystemComponent_GetWorldSpace;
	internal static delegate* unmanaged<ulong, byte*, void> ParticleSystemComponent_SetWorldSpace;
	internal static delegate* unmanaged<ulong, byte*, void> ParticleSystemComponent_GetDestroyWhenFinished;
	internal static delegate* unmanaged<ulong, byte*, void> ParticleSystemComponent_SetDestroyWhenFinished;
	internal static delegate* unmanaged<ulong, uint, void> ParticleSystemComponent_Emit;
	internal static delegate* unmanaged<ulong, void> ParticleSystemComponent_Clear;
	internal static delegate* unmanaged<ulong, uint> ParticleSystemComponent_GetParticleCount;

	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_GetGlobal;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_SetGlobal;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_GetBlendDistance;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_SetBlendDistance;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_GetPriority;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_SetPriority;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_GetWeight;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_SetWeight;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_GetOverrideEV100;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_SetOverrideEV100;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_GetEV100;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_SetEV100;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_GetOverrideTonemapper;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_SetOverrideTonemapper;
	internal static delegate* unmanaged<ulong, int*, void> PostProcessComponent_GetTonemapper;
	internal static delegate* unmanaged<ulong, int*, void> PostProcessComponent_SetTonemapper;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_GetOverrideAmbientOcclusion;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_SetOverrideAmbientOcclusion;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_GetAmbientOcclusion;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_SetAmbientOcclusion;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_GetOverrideAmbientOcclusionIntensity;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_SetOverrideAmbientOcclusionIntensity;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_GetAmbientOcclusionIntensity;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_SetAmbientOcclusionIntensity;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_GetOverrideBloom;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_SetOverrideBloom;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_GetBloom;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_SetBloom;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_GetOverrideBloomIntensity;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_SetOverrideBloomIntensity;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_GetBloomIntensity;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_SetBloomIntensity;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_GetOverrideContrast;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_SetOverrideContrast;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_GetContrast;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_SetContrast;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_GetOverrideSaturation;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_SetOverrideSaturation;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_GetSaturation;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_SetSaturation;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_GetOverrideTemperature;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_SetOverrideTemperature;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_GetTemperature;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_SetTemperature;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_GetOverrideTint;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_SetOverrideTint;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_GetTint;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_SetTint;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_GetOverrideColorFilter;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_SetOverrideColorFilter;
	internal static delegate* unmanaged<ulong, Vector3*, void> PostProcessComponent_GetColorFilter;
	internal static delegate* unmanaged<ulong, Vector3*, void> PostProcessComponent_SetColorFilter;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_GetOverrideChromaticAberration;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_SetOverrideChromaticAberration;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_GetChromaticAberration;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_SetChromaticAberration;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_GetOverrideVignette;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_SetOverrideVignette;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_GetVignette;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_SetVignette;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_GetOverrideVignetteSmoothness;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_SetOverrideVignetteSmoothness;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_GetVignetteSmoothness;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_SetVignetteSmoothness;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_GetOverrideFilmGrain;
	internal static delegate* unmanaged<ulong, byte*, void> PostProcessComponent_SetOverrideFilmGrain;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_GetFilmGrain;
	internal static delegate* unmanaged<ulong, float*, void> PostProcessComponent_SetFilmGrain;

	internal static delegate* unmanaged<ulong, int*, byte*> ScriptComponent_GetClassName;
	internal static delegate* unmanaged<ulong, byte*, int, void> ScriptComponent_SetClassName;

	internal static delegate* unmanaged<ulong, int*, void> UICanvasComponent_GetScaleMode;
	internal static delegate* unmanaged<ulong, int*, void> UICanvasComponent_SetScaleMode;
	internal static delegate* unmanaged<ulong, Vector2*, void> UICanvasComponent_GetReferenceResolution;
	internal static delegate* unmanaged<ulong, Vector2*, void> UICanvasComponent_SetReferenceResolution;

	internal static delegate* unmanaged<ulong, Vector2*, void> UIRectComponent_GetAnchorMin;
	internal static delegate* unmanaged<ulong, Vector2*, void> UIRectComponent_SetAnchorMin;
	internal static delegate* unmanaged<ulong, Vector2*, void> UIRectComponent_GetAnchorMax;
	internal static delegate* unmanaged<ulong, Vector2*, void> UIRectComponent_SetAnchorMax;
	internal static delegate* unmanaged<ulong, Vector2*, void> UIRectComponent_GetPosition;
	internal static delegate* unmanaged<ulong, Vector2*, void> UIRectComponent_SetPosition;
	internal static delegate* unmanaged<ulong, Vector2*, void> UIRectComponent_GetSize;
	internal static delegate* unmanaged<ulong, Vector2*, void> UIRectComponent_SetSize;
	internal static delegate* unmanaged<ulong, Vector2*, void> UIRectComponent_GetPivot;
	internal static delegate* unmanaged<ulong, Vector2*, void> UIRectComponent_SetPivot;
	internal static delegate* unmanaged<ulong, byte*, void> UIRectComponent_GetVisible;
	internal static delegate* unmanaged<ulong, byte*, void> UIRectComponent_SetVisible;
	internal static delegate* unmanaged<ulong, float*, void> UIRectComponent_GetOpacity;
	internal static delegate* unmanaged<ulong, float*, void> UIRectComponent_SetOpacity;

	internal static delegate* unmanaged<ulong, Color*, void> UIImageComponent_GetColor;
	internal static delegate* unmanaged<ulong, Color*, void> UIImageComponent_SetColor;
	internal static delegate* unmanaged<ulong, ulong*, void> UIImageComponent_GetTexture;
	internal static delegate* unmanaged<ulong, ulong*, void> UIImageComponent_SetTexture;
	internal static delegate* unmanaged<ulong, byte*, void> UIImageComponent_GetPreserveAspect;
	internal static delegate* unmanaged<ulong, byte*, void> UIImageComponent_SetPreserveAspect;
	internal static delegate* unmanaged<ulong, float*, void> UIImageComponent_GetCornerRadius;
	internal static delegate* unmanaged<ulong, float*, void> UIImageComponent_SetCornerRadius;

	internal static delegate* unmanaged<ulong, int*, byte*> UITextComponent_GetText;
	internal static delegate* unmanaged<ulong, byte*, int, void> UITextComponent_SetText;
	internal static delegate* unmanaged<ulong, ulong*, void> UITextComponent_GetFont;
	internal static delegate* unmanaged<ulong, ulong*, void> UITextComponent_SetFont;
	internal static delegate* unmanaged<ulong, Color*, void> UITextComponent_GetColor;
	internal static delegate* unmanaged<ulong, Color*, void> UITextComponent_SetColor;
	internal static delegate* unmanaged<ulong, float*, void> UITextComponent_GetFontSize;
	internal static delegate* unmanaged<ulong, float*, void> UITextComponent_SetFontSize;
	internal static delegate* unmanaged<ulong, int*, void> UITextComponent_GetAlignment;
	internal static delegate* unmanaged<ulong, int*, void> UITextComponent_SetAlignment;
	internal static delegate* unmanaged<ulong, int*, void> UITextComponent_GetVerticalAlignment;
	internal static delegate* unmanaged<ulong, int*, void> UITextComponent_SetVerticalAlignment;
	internal static delegate* unmanaged<ulong, byte*, void> UITextComponent_GetWrap;
	internal static delegate* unmanaged<ulong, byte*, void> UITextComponent_SetWrap;
	internal static delegate* unmanaged<ulong, float*, void> UITextComponent_GetLineSpacing;
	internal static delegate* unmanaged<ulong, float*, void> UITextComponent_SetLineSpacing;

	internal static delegate* unmanaged<ulong, byte*, void> UIButtonComponent_GetInteractable;
	internal static delegate* unmanaged<ulong, byte*, void> UIButtonComponent_SetInteractable;
	internal static delegate* unmanaged<ulong, Color*, void> UIButtonComponent_GetNormalColor;
	internal static delegate* unmanaged<ulong, Color*, void> UIButtonComponent_SetNormalColor;
	internal static delegate* unmanaged<ulong, Color*, void> UIButtonComponent_GetHoveredColor;
	internal static delegate* unmanaged<ulong, Color*, void> UIButtonComponent_SetHoveredColor;
	internal static delegate* unmanaged<ulong, Color*, void> UIButtonComponent_GetPressedColor;
	internal static delegate* unmanaged<ulong, Color*, void> UIButtonComponent_SetPressedColor;
	internal static delegate* unmanaged<ulong, Color*, void> UIButtonComponent_GetDisabledColor;
	internal static delegate* unmanaged<ulong, Color*, void> UIButtonComponent_SetDisabledColor;
	internal static delegate* unmanaged<ulong, byte> UIButtonComponent_WasClicked;
	internal static delegate* unmanaged<ulong, int> UIButtonComponent_GetState;
	internal static delegate* unmanaged<ulong, void> UIButtonComponent_Click;
	internal static delegate* unmanaged<byte> Input_IsPointerOverUI;

	internal static delegate* unmanaged<double> Time_GetElapsed;
	internal static delegate* unmanaged<float> Time_GetFixedDeltaTime;
	internal static delegate* unmanaged<ulong> Time_GetFrameCount;
	internal static delegate* unmanaged<float> Time_GetTimeScale;
	internal static delegate* unmanaged<float, void> Time_SetTimeScale;

	internal static delegate* unmanaged<int, byte> Input_IsKeyDown;
	internal static delegate* unmanaged<int, byte> Input_IsKeyPressed;
	internal static delegate* unmanaged<int, byte> Input_IsKeyReleased;
	internal static delegate* unmanaged<int, byte> Input_IsMouseButtonDown;
	internal static delegate* unmanaged<int, byte> Input_IsMouseButtonPressed;
	internal static delegate* unmanaged<int, byte> Input_IsMouseButtonReleased;
	internal static delegate* unmanaged<Vector2*, void> Input_GetMousePosition;
	internal static delegate* unmanaged<Vector2*, void> Input_GetMouseDelta;
	internal static delegate* unmanaged<Vector2*, void> Input_GetMouseScrollDelta;
	internal static delegate* unmanaged<int> Input_GetCursorMode;
	internal static delegate* unmanaged<int, void> Input_SetCursorMode;
	internal static delegate* unmanaged<uint, byte> Input_IsGamepadConnected;
	internal static delegate* unmanaged<uint, int, float> Input_GetGamepadAxis;
	internal static delegate* unmanaged<uint, int, byte> Input_IsGamepadButtonDown;
	internal static delegate* unmanaged<uint, int, byte> Input_IsGamepadButtonPressed;
	internal static delegate* unmanaged<uint, int, byte> Input_IsGamepadButtonReleased;

	internal static delegate* unmanaged<Vector3*, void> Physics_GetGravity;
	internal static delegate* unmanaged<Vector3*, void> Physics_SetGravity;
	internal static delegate* unmanaged<Vector3*, Vector3*, float, uint, NativeRaycastHit*, byte> Physics_Raycast;

	internal static delegate* unmanaged<byte*, int, int, ulong> Assets_Load;
	internal static delegate* unmanaged<ulong> Material_Create;
	internal static delegate* unmanaged<ulong, ulong> Material_Clone;
	internal static delegate* unmanaged<ulong, NativeMaterialValues*, void> Material_GetValues;
	internal static delegate* unmanaged<ulong, NativeMaterialValues*, byte*, int, void> Material_SetValues;
	internal static delegate* unmanaged<ulong, byte> Material_IsRuntime;

	internal static delegate* unmanaged<byte> Application_IsEditor;
	internal static delegate* unmanaged<void> Application_Quit;
	internal static delegate* unmanaged<uint*, uint*, void> Application_GetWindowSize;
	internal static delegate* unmanaged<int*, byte*> SceneManager_GetCurrentSceneName;
	internal static delegate* unmanaged<byte*, int, byte> SceneManager_LoadScene;
	internal static delegate* unmanaged<Vector3*, Vector3*, Color*, float, void> Debug_DrawLine;
	internal static delegate* unmanaged<byte*, int, byte, byte*, int, void> TestReporter_Report;
	internal static delegate* unmanaged<void> TestReporter_Finish;
#pragma warning restore CS0649

	// Assigns every function pointer field from the native table. Returns null when each field found exactly one
	// function, otherwise the names that did not match.
	internal static string? Bind(Host.NativeBinding* bindings, int count)
	{
		Dictionary<string, nint> functions = new(StringComparer.Ordinal);
		List<string> duplicates = [];
		for (int index = 0; index < count; index++)
		{
			string name = NativeString.FromUtf8(bindings[index].Name, bindings[index].NameLength);
			if (!functions.TryAdd(name, (nint)bindings[index].Function))
			{
				duplicates.Add(name);
			}
		}

		List<string> missing = [];
		foreach (FieldInfo field in typeof(InternalCalls).GetFields(BindingFlags.Static | BindingFlags.NonPublic))
		{
			if (!field.FieldType.IsFunctionPointer)
			{
				continue;
			}
			if (functions.Remove(field.Name, out nint function) && function != 0)
			{
				field.SetValue(null, function);
			}
			else
			{
				missing.Add(field.Name);
			}
		}

		if (missing.Count == 0 && functions.Count == 0 && duplicates.Count == 0)
		{
			return null;
		}
		return $"The engine's script bindings do not match Strada.ScriptCore. Missing: [{string.Join(", ", missing)}]; "
			+ $"unknown: [{string.Join(", ", functions.Keys)}]; duplicated: [{string.Join(", ", duplicates)}]";
	}
}
