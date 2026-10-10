using Strada.Interop;

namespace Strada;

/// <summary>Position, rotation and scale of an entity relative to its parent.</summary>
[NativeComponent("Transform")]
public sealed unsafe class TransformComponent : Component
{
	private TransformComponent()
	{
	}

	/// <summary>The position relative to the parent.</summary>
	public Vector3 Translation
	{
		get
		{
			Vector3 value;
			InternalCalls.TransformComponent_GetTranslation(Entity.ID, &value);
			return value;
		}
		set => InternalCalls.TransformComponent_SetTranslation(Entity.ID, &value);
	}

	/// <summary>The rotation relative to the parent.</summary>
	public Quaternion Rotation
	{
		get
		{
			Quaternion value;
			InternalCalls.TransformComponent_GetRotation(Entity.ID, &value);
			return value;
		}
		set => InternalCalls.TransformComponent_SetRotation(Entity.ID, &value);
	}

	/// <summary>The scale relative to the parent.</summary>
	public Vector3 Scale
	{
		get
		{
			Vector3 value;
			InternalCalls.TransformComponent_GetScale(Entity.ID, &value);
			return value;
		}
		set => InternalCalls.TransformComponent_SetScale(Entity.ID, &value);
	}
}
