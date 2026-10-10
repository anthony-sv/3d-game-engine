using Strada.Interop;

namespace Strada;

/// <summary>Renders a mesh asset with its materials.</summary>
[NativeComponent("Mesh")]
public sealed unsafe class MeshComponent : Component
{
	private MeshComponent()
	{
	}

	/// <summary>The rendered mesh, or null.</summary>
	public Mesh? Mesh
	{
		get
		{
			AssetHandle handle = NativeField.GetAsset(InternalCalls.MeshComponent_GetMesh, Entity.ID);
			return handle.IsValid ? new Mesh(handle) : null;
		}
		set => NativeField.SetAsset(InternalCalls.MeshComponent_SetMesh, Entity.ID, value);
	}

	/// <summary>Whether the mesh casts shadows.</summary>
	public bool CastShadows
	{
		get => NativeField.GetBool(InternalCalls.MeshComponent_GetCastShadows, Entity.ID);
		set => NativeField.SetBool(InternalCalls.MeshComponent_SetCastShadows, Entity.ID, value);
	}

	/// <summary>Whether the mesh is rendered.</summary>
	public bool Visible
	{
		get => NativeField.GetBool(InternalCalls.MeshComponent_GetVisible, Entity.ID);
		set => NativeField.SetBool(InternalCalls.MeshComponent_SetVisible, Entity.ID, value);
	}

	/// <summary>The number of material overrides (submeshes without one use the mesh's own material).</summary>
	public int MaterialCount => InternalCalls.MeshComponent_GetMaterialCount(Entity.ID);

	/// <summary>The material override of a submesh, or null when it uses the mesh's own material.</summary>
	public Material? GetMaterial(int index)
	{
		ulong material = InternalCalls.MeshComponent_GetMaterial(Entity.ID, index);
		return material != 0 ? new Material(new AssetHandle(material)) : null;
	}

	/// <summary>Overrides the material of a submesh; null restores the mesh's own material.</summary>
	public void SetMaterial(int index, Material? material)
	{
		InternalCalls.MeshComponent_SetMaterial(Entity.ID, index, material?.Handle.ID ?? 0);
	}
}
