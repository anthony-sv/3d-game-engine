using Strada.Interop;

namespace Strada;

/// <summary>A capsule collision shape along the entity's local Y axis.</summary>
[NativeComponent("CapsuleCollider")]
public sealed unsafe class CapsuleColliderComponent : Component
{
	private CapsuleColliderComponent()
	{
	}

	/// <summary>Radius of the capsule's cylinder and caps.</summary>
	public float Radius
	{
		get => NativeField.Get<float>(InternalCalls.CapsuleColliderComponent_GetRadius, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.CapsuleColliderComponent_SetRadius, Entity.ID, value);
	}

	/// <summary>Half the height of the cylinder between the caps.</summary>
	public float HalfHeight
	{
		get => NativeField.Get<float>(InternalCalls.CapsuleColliderComponent_GetHalfHeight, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.CapsuleColliderComponent_SetHalfHeight, Entity.ID, value);
	}

	/// <summary>Position of the capsule's center relative to the entity.</summary>
	public Vector3 Offset
	{
		get => NativeField.Get<Vector3>(InternalCalls.CapsuleColliderComponent_GetOffset, Entity.ID);
		set => NativeField.Set<Vector3>(InternalCalls.CapsuleColliderComponent_SetOffset, Entity.ID, value);
	}

	/// <summary>Whether the collider only reports overlaps instead of colliding.</summary>
	public bool IsTrigger
	{
		get => NativeField.GetBool(InternalCalls.CapsuleColliderComponent_GetIsTrigger, Entity.ID);
		set => NativeField.SetBool(InternalCalls.CapsuleColliderComponent_SetIsTrigger, Entity.ID, value);
	}

	/// <summary>Friction coefficient, used while the collider's Material (set in the editor) names no physics material:
	/// two colliders without materials combine theirs as the square root of the product.</summary>
	public float Friction
	{
		get => NativeField.Get<float>(InternalCalls.CapsuleColliderComponent_GetFriction, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.CapsuleColliderComponent_SetFriction, Entity.ID, value);
	}

	/// <summary>Bounciness from 0 to 1, used while the collider's Material (set in the editor) names no physics material:
	/// of two colliders without materials, the larger applies.</summary>
	public float Restitution
	{
		get => NativeField.Get<float>(InternalCalls.CapsuleColliderComponent_GetRestitution, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.CapsuleColliderComponent_SetRestitution, Entity.ID, value);
	}
}
