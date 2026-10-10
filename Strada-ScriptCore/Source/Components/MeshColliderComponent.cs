using Strada.Interop;

namespace Strada;

/// <summary>A collision shape from a mesh: its convex hull, or its triangles (static and kinematic bodies).</summary>
[NativeComponent("MeshCollider")]
public sealed unsafe class MeshColliderComponent : Component
{
	private MeshColliderComponent()
	{
	}

	/// <summary>The collision mesh, or null for the entity's MeshComponent mesh.</summary>
	public Mesh? Mesh
	{
		get
		{
			AssetHandle handle = NativeField.GetAsset(InternalCalls.MeshColliderComponent_GetMesh, Entity.ID);
			return handle.IsValid ? new Mesh(handle) : null;
		}
		set => NativeField.SetAsset(InternalCalls.MeshColliderComponent_SetMesh, Entity.ID, value);
	}

	/// <summary>Whether the shape is the mesh's convex hull (required for dynamic bodies).</summary>
	public bool Convex
	{
		get => NativeField.GetBool(InternalCalls.MeshColliderComponent_GetConvex, Entity.ID);
		set => NativeField.SetBool(InternalCalls.MeshColliderComponent_SetConvex, Entity.ID, value);
	}

	/// <summary>Whether the collider only reports overlaps instead of colliding.</summary>
	public bool IsTrigger
	{
		get => NativeField.GetBool(InternalCalls.MeshColliderComponent_GetIsTrigger, Entity.ID);
		set => NativeField.SetBool(InternalCalls.MeshColliderComponent_SetIsTrigger, Entity.ID, value);
	}

	/// <summary>Friction coefficient (combined with the other collider's as their geometric mean).</summary>
	public float Friction
	{
		get => NativeField.Get<float>(InternalCalls.MeshColliderComponent_GetFriction, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.MeshColliderComponent_SetFriction, Entity.ID, value);
	}

	/// <summary>Bounciness from 0 to 1 (the larger of the two colliders' applies).</summary>
	public float Restitution
	{
		get => NativeField.Get<float>(InternalCalls.MeshColliderComponent_GetRestitution, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.MeshColliderComponent_SetRestitution, Entity.ID, value);
	}
}
