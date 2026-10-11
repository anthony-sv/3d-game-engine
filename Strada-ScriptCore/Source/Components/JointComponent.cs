using Strada.Interop;

namespace Strada;

/// <summary>Joins this entity's body to another entity's body, or to the world, while the scene runs: welded
/// (<see cref="JointType.Fixed"/>), turning around an axis (<see cref="JointType.Hinge"/>, with limits and a motor), turning
/// freely (<see cref="JointType.Ball"/>), kept within distances (<see cref="JointType.Distance"/>) or sliding along an axis
/// (<see cref="JointType.Slider"/>). Both entities need a rigid body with a collider. Changing a property makes the joint
/// again from where the bodies are.</summary>
[NativeComponent("Joint")]
public sealed unsafe class JointComponent : Component
{
	private JointComponent()
	{
	}

	/// <summary>How the bodies are joined.</summary>
	public JointType Type
	{
		get => (JointType)NativeField.Get<int>(InternalCalls.JointComponent_GetType, Entity.ID);
		set => NativeField.Set<int>(InternalCalls.JointComponent_SetType, Entity.ID, (int)value);
	}

	/// <summary>The entity whose body this one is joined to, or null for the world.</summary>
	public Entity? ConnectedEntity
	{
		get
		{
			ulong id = NativeField.Get<ulong>(InternalCalls.JointComponent_GetConnectedEntity, Entity.ID);
			return id != 0 ? new Entity(id) : null;
		}
		set => NativeField.Set<ulong>(InternalCalls.JointComponent_SetConnectedEntity, Entity.ID, value?.ID ?? 0);
	}

	/// <summary>Where the joint is, in this entity's local space.</summary>
	public Vector3 Anchor
	{
		get => NativeField.Get<Vector3>(InternalCalls.JointComponent_GetAnchor, Entity.ID);
		set => NativeField.Set<Vector3>(InternalCalls.JointComponent_SetAnchor, Entity.ID, value);
	}

	/// <summary>Distance joints: the far point, in the connected entity's local space (world space without one).</summary>
	public Vector3 ConnectedAnchor
	{
		get => NativeField.Get<Vector3>(InternalCalls.JointComponent_GetConnectedAnchor, Entity.ID);
		set => NativeField.Set<Vector3>(InternalCalls.JointComponent_SetConnectedAnchor, Entity.ID, value);
	}

	/// <summary>The hinge or slider axis in this entity's local space.</summary>
	public Vector3 Axis
	{
		get => NativeField.Get<Vector3>(InternalCalls.JointComponent_GetAxis, Entity.ID);
		set => NativeField.Set<Vector3>(InternalCalls.JointComponent_SetAxis, Entity.ID, value);
	}

	/// <summary>Whether a hinge's angle or a slider's travel is limited.</summary>
	public bool LimitsEnabled
	{
		get => NativeField.GetBool(InternalCalls.JointComponent_GetLimitsEnabled, Entity.ID);
		set => NativeField.SetBool(InternalCalls.JointComponent_SetLimitsEnabled, Entity.ID, value);
	}

	/// <summary>Hinges: the lowest angle in degrees (down to -180); sliders: meters (0 is the start).</summary>
	public float MinLimit
	{
		get => NativeField.Get<float>(InternalCalls.JointComponent_GetMinLimit, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.JointComponent_SetMinLimit, Entity.ID, value);
	}

	/// <summary>Hinges: the highest angle in degrees (up to 180); sliders: meters.</summary>
	public float MaxLimit
	{
		get => NativeField.Get<float>(InternalCalls.JointComponent_GetMaxLimit, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.JointComponent_SetMaxLimit, Entity.ID, value);
	}

	/// <summary>Distance joints: the shortest distance; -1 keeps the starting distance.</summary>
	public float MinDistance
	{
		get => NativeField.Get<float>(InternalCalls.JointComponent_GetMinDistance, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.JointComponent_SetMinDistance, Entity.ID, value);
	}

	/// <summary>Distance joints: the longest distance; -1 keeps the starting distance.</summary>
	public float MaxDistance
	{
		get => NativeField.Get<float>(InternalCalls.JointComponent_GetMaxDistance, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.JointComponent_SetMaxDistance, Entity.ID, value);
	}

	/// <summary>Whether a motor drives the hinge or slider at <see cref="MotorSpeed"/>.</summary>
	public bool MotorEnabled
	{
		get => NativeField.GetBool(InternalCalls.JointComponent_GetMotorEnabled, Entity.ID);
		set => NativeField.SetBool(InternalCalls.JointComponent_SetMotorEnabled, Entity.ID, value);
	}

	/// <summary>Hinges: degrees per second; sliders: meters per second.</summary>
	public float MotorSpeed
	{
		get => NativeField.Get<float>(InternalCalls.JointComponent_GetMotorSpeed, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.JointComponent_SetMotorSpeed, Entity.ID, value);
	}

	/// <summary>The most torque (hinges, newton meters) or force (sliders, newtons) the motor uses.</summary>
	public float MotorMaxForce
	{
		get => NativeField.Get<float>(InternalCalls.JointComponent_GetMotorMaxForce, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.JointComponent_SetMotorMaxForce, Entity.ID, value);
	}
}
