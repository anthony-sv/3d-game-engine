using Strada.Interop;

namespace Strada;

/// <summary>Position, rotation and scale of an entity relative to its parent (or the world at the root).</summary>
[NativeComponent("Transform")]
public sealed unsafe class TransformComponent : Component
{
	private TransformComponent()
	{
	}

	/// <summary>The position relative to the parent.</summary>
	public Vector3 Translation
	{
		get => NativeField.Get<Vector3>(InternalCalls.TransformComponent_GetTranslation, Entity.ID);
		set => NativeField.Set<Vector3>(InternalCalls.TransformComponent_SetTranslation, Entity.ID, value);
	}

	/// <summary>The rotation relative to the parent.</summary>
	public Quaternion Rotation
	{
		get => NativeField.Get<Quaternion>(InternalCalls.TransformComponent_GetRotation, Entity.ID);
		set => NativeField.Set<Quaternion>(InternalCalls.TransformComponent_SetRotation, Entity.ID, value);
	}

	/// <summary>The rotation relative to the parent as Euler angles in degrees (X, then Y, then Z), as the editor shows
	/// them.</summary>
	public Vector3 EulerAngles
	{
		get => NativeField.Get<Vector3>(InternalCalls.TransformComponent_GetEulerAngles, Entity.ID);
		set => NativeField.Set<Vector3>(InternalCalls.TransformComponent_SetEulerAngles, Entity.ID, value);
	}

	/// <summary>The scale relative to the parent.</summary>
	public Vector3 Scale
	{
		get => NativeField.Get<Vector3>(InternalCalls.TransformComponent_GetScale, Entity.ID);
		set => NativeField.Set<Vector3>(InternalCalls.TransformComponent_SetScale, Entity.ID, value);
	}

	/// <summary>The transform from the entity's space to world space.</summary>
	public Matrix4 WorldTransform => NativeField.Get<Matrix4>(InternalCalls.TransformComponent_GetWorldTransform, Entity.ID);

	/// <summary>The position in world space; setting it keeps the world rotation and scale.</summary>
	public Vector3 WorldTranslation
	{
		get => NativeField.Get<Vector3>(InternalCalls.TransformComponent_GetWorldTranslation, Entity.ID);
		set => NativeField.Set<Vector3>(InternalCalls.TransformComponent_SetWorldTranslation, Entity.ID, value);
	}

	/// <summary>The entity's forward direction (-Z) in world space.</summary>
	public Vector3 Forward => NativeField.Get<Vector3>(InternalCalls.TransformComponent_GetForward, Entity.ID);

	/// <summary>The entity's right direction (+X) in world space.</summary>
	public Vector3 Right => NativeField.Get<Vector3>(InternalCalls.TransformComponent_GetRight, Entity.ID);

	/// <summary>The entity's up direction (+Y) in world space.</summary>
	public Vector3 Up => NativeField.Get<Vector3>(InternalCalls.TransformComponent_GetUp, Entity.ID);
}
