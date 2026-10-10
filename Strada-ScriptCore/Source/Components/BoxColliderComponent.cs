using Strada.Interop;

namespace Strada;

/// <summary>A box collision shape of the entity's body.</summary>
[NativeComponent("BoxCollider")]
public sealed unsafe class BoxColliderComponent : Component
{
	private BoxColliderComponent()
	{
	}

	/// <summary>Half the box's size along each local axis.</summary>
	public Vector3 HalfExtents
	{
		get => NativeField.Get<Vector3>(InternalCalls.BoxColliderComponent_GetHalfExtents, Entity.ID);
		set => NativeField.Set<Vector3>(InternalCalls.BoxColliderComponent_SetHalfExtents, Entity.ID, value);
	}

	/// <summary>Position of the box's center relative to the entity.</summary>
	public Vector3 Offset
	{
		get => NativeField.Get<Vector3>(InternalCalls.BoxColliderComponent_GetOffset, Entity.ID);
		set => NativeField.Set<Vector3>(InternalCalls.BoxColliderComponent_SetOffset, Entity.ID, value);
	}

	/// <summary>Whether the collider only reports overlaps instead of colliding.</summary>
	public bool IsTrigger
	{
		get => NativeField.GetBool(InternalCalls.BoxColliderComponent_GetIsTrigger, Entity.ID);
		set => NativeField.SetBool(InternalCalls.BoxColliderComponent_SetIsTrigger, Entity.ID, value);
	}

	/// <summary>Friction coefficient (combined with the other collider's as their geometric mean).</summary>
	public float Friction
	{
		get => NativeField.Get<float>(InternalCalls.BoxColliderComponent_GetFriction, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.BoxColliderComponent_SetFriction, Entity.ID, value);
	}

	/// <summary>Bounciness from 0 to 1 (the larger of the two colliders' applies).</summary>
	public float Restitution
	{
		get => NativeField.Get<float>(InternalCalls.BoxColliderComponent_GetRestitution, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.BoxColliderComponent_SetRestitution, Entity.ID, value);
	}
}
