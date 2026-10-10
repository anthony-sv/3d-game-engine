using Strada.Interop;

namespace Strada;

/// <summary>A sphere collision shape of the entity's body.</summary>
[NativeComponent("SphereCollider")]
public sealed unsafe class SphereColliderComponent : Component
{
	private SphereColliderComponent()
	{
	}

	/// <summary>The sphere's radius.</summary>
	public float Radius
	{
		get => NativeField.Get<float>(InternalCalls.SphereColliderComponent_GetRadius, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.SphereColliderComponent_SetRadius, Entity.ID, value);
	}

	/// <summary>Position of the sphere's center relative to the entity.</summary>
	public Vector3 Offset
	{
		get => NativeField.Get<Vector3>(InternalCalls.SphereColliderComponent_GetOffset, Entity.ID);
		set => NativeField.Set<Vector3>(InternalCalls.SphereColliderComponent_SetOffset, Entity.ID, value);
	}

	/// <summary>Whether the collider only reports overlaps instead of colliding.</summary>
	public bool IsTrigger
	{
		get => NativeField.GetBool(InternalCalls.SphereColliderComponent_GetIsTrigger, Entity.ID);
		set => NativeField.SetBool(InternalCalls.SphereColliderComponent_SetIsTrigger, Entity.ID, value);
	}

	/// <summary>Friction coefficient (combined with the other collider's as their geometric mean).</summary>
	public float Friction
	{
		get => NativeField.Get<float>(InternalCalls.SphereColliderComponent_GetFriction, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.SphereColliderComponent_SetFriction, Entity.ID, value);
	}

	/// <summary>Bounciness from 0 to 1 (the larger of the two colliders' applies).</summary>
	public float Restitution
	{
		get => NativeField.Get<float>(InternalCalls.SphereColliderComponent_GetRestitution, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.SphereColliderComponent_SetRestitution, Entity.ID, value);
	}
}
