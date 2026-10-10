using Strada.Interop;

namespace Strada;

/// <summary>Makes the entity a physics body; its colliders give it shape. While the scene runs, velocities, forces and sleep act on the simulated body.</summary>
[NativeComponent("RigidBody")]
public sealed unsafe class RigidBodyComponent : Component
{
	private RigidBodyComponent()
	{
	}

	/// <summary>Static, dynamic or kinematic. Changing it rebuilds the body.</summary>
	public RigidBodyType Type
	{
		get => (RigidBodyType)NativeField.Get<int>(InternalCalls.RigidBodyComponent_GetType, Entity.ID);
		set => NativeField.Set<int>(InternalCalls.RigidBodyComponent_SetType, Entity.ID, (int)value);
	}

	/// <summary>Mass in kilograms. Changing it rebuilds the body.</summary>
	public float Mass
	{
		get => NativeField.Get<float>(InternalCalls.RigidBodyComponent_GetMass, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.RigidBodyComponent_SetMass, Entity.ID, value);
	}

	/// <summary>Physics layer index (0-15) of the project settings. Changing it rebuilds the body.</summary>
	public uint Layer
	{
		get => NativeField.Get<uint>(InternalCalls.RigidBodyComponent_GetLayer, Entity.ID);
		set => NativeField.Set<uint>(InternalCalls.RigidBodyComponent_SetLayer, Entity.ID, value);
	}

	/// <summary>Multiplier of the scene's gravity for this body.</summary>
	public float GravityFactor
	{
		get => NativeField.Get<float>(InternalCalls.RigidBodyComponent_GetGravityFactor, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.RigidBodyComponent_SetGravityFactor, Entity.ID, value);
	}

	/// <summary>Fraction of linear velocity lost per second.</summary>
	public float LinearDamping
	{
		get => NativeField.Get<float>(InternalCalls.RigidBodyComponent_GetLinearDamping, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.RigidBodyComponent_SetLinearDamping, Entity.ID, value);
	}

	/// <summary>Fraction of angular velocity lost per second.</summary>
	public float AngularDamping
	{
		get => NativeField.Get<float>(InternalCalls.RigidBodyComponent_GetAngularDamping, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.RigidBodyComponent_SetAngularDamping, Entity.ID, value);
	}

	/// <summary>Velocity in meters per second (the initial velocity while the scene is not simulated).</summary>
	public Vector3 LinearVelocity
	{
		get => NativeField.Get<Vector3>(InternalCalls.RigidBodyComponent_GetLinearVelocity, Entity.ID);
		set => NativeField.Set<Vector3>(InternalCalls.RigidBodyComponent_SetLinearVelocity, Entity.ID, value);
	}

	/// <summary>Angular velocity in radians per second (the initial one while the scene is not simulated).</summary>
	public Vector3 AngularVelocity
	{
		get => NativeField.Get<Vector3>(InternalCalls.RigidBodyComponent_GetAngularVelocity, Entity.ID);
		set => NativeField.Set<Vector3>(InternalCalls.RigidBodyComponent_SetAngularVelocity, Entity.ID, value);
	}

	/// <summary>Whether the simulated body is asleep (at rest and not simulated until something wakes it).</summary>
	public bool IsSleeping => InternalCalls.RigidBodyComponent_IsSleeping(Entity.ID) != 0;

	/// <summary>Applies a force (or impulse, acceleration or velocity change, see <see cref="ForceMode"/>) at the center of
	/// mass. Dynamic bodies only; wakes the body.</summary>
	public void AddForce(Vector3 force, ForceMode mode = ForceMode.Force)
	{
		InternalCalls.RigidBodyComponent_AddForce(Entity.ID, &force, (int)mode);
	}

	/// <summary>Applies a torque (or angular impulse, acceleration or velocity change). Dynamic bodies only; wakes the
	/// body.</summary>
	public void AddTorque(Vector3 torque, ForceMode mode = ForceMode.Force)
	{
		InternalCalls.RigidBodyComponent_AddTorque(Entity.ID, &torque, (int)mode);
	}

	/// <summary>Moves a kinematic body to a world position and rotation over the next physics step, pushing dynamic bodies
	/// on the way.</summary>
	public void MoveKinematic(Vector3 position, Quaternion rotation)
	{
		InternalCalls.RigidBodyComponent_MoveKinematic(Entity.ID, &position, &rotation);
	}

	/// <summary>Places the body at a world position and rotation at once, keeping its velocity.</summary>
	public void Teleport(Vector3 position, Quaternion rotation)
	{
		InternalCalls.RigidBodyComponent_Teleport(Entity.ID, &position, &rotation);
	}

	/// <summary>Wakes the simulated body up.</summary>
	public void WakeUp()
	{
		InternalCalls.RigidBodyComponent_WakeUp(Entity.ID);
	}
}
