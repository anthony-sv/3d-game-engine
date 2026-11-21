using System;
using System.Globalization;
using Strada.Interop;
using Strada.VisualScripting;

namespace Strada;

/// <summary>Makes the entity a motorcycle of its <see cref="Definition"/>: the entity needs a dynamic
/// <see cref="RigidBodyComponent"/> and a collider (its chassis), and the entities below it that draw its wheels a
/// VehicleWheel component each. While the scene simulates it, a rider drives it with <see cref="SetDriverInput"/> (the
/// throttle, the front and the rear brake, and the lean) and, in a Manual gearbox, <see cref="ShiftUp"/> and
/// <see cref="ShiftDown"/>, and <see cref="ResetVehicle"/> puts it back as it was made; its engine, gearbox, lean and wheels
/// read as its last physics step left them. Set the inputs in <see cref="Script.OnFixedUpdate"/> (or a graph's
/// Event.FixedUpdate): each physics step applies the latest given before it, so a ride set there as a function of the
/// simulation is the same at every frame rate. Without a simulated vehicle (the scene is not playing, the entity has no
/// dynamic body or no definition that loads) the reads are a vehicle at rest in neutral, and the inputs, shifts and resets
/// are logged and do nothing.</summary>
[NativeComponent("Vehicle")]
public sealed unsafe class VehicleComponent : Component
{
	private VehicleComponent()
	{
	}

	/// <summary>The vehicle definition (a <c>.svehicle</c> asset): its wheels, engine, gearbox and lean. Another definition makes
	/// the vehicle again; null leaves the entity without a vehicle.</summary>
	public Vehicle? Definition
	{
		get
		{
			AssetHandle handle = NativeField.GetAsset(InternalCalls.VehicleComponent_GetDefinition, Entity.ID);
			return handle.IsValid ? new Vehicle(handle) : null;
		}
		set => NativeField.SetAsset(InternalCalls.VehicleComponent_SetDefinition, Entity.ID, value);
	}

	/// <summary>The engine's speed in revolutions per minute, as the last physics step left it: from the definition's
	/// <see cref="Vehicle.MinRPM"/> (idle) to its <see cref="Vehicle.MaxRPM"/>.</summary>
	public float EngineSpeed => ReadState().EngineSpeed;

	/// <summary>The gear the gearbox is in: 0 is neutral, 1 the first gear, up to the definition's
	/// <see cref="Vehicle.GearCount"/>. A Manual gearbox starts in neutral.</summary>
	public int Gear => ReadState().Gear;

	/// <summary>The chassis's speed along its front in meters per second, as the last physics step left it: negative when
	/// it rolls backwards.</summary>
	public float ForwardSpeed => ReadState().ForwardSpeed;

	/// <summary>How far the chassis leans from the world's up around its front, in degrees: positive to its right, negative
	/// to its left.</summary>
	public float LeanAngle => ReadState().LeanAngle;

	/// <summary>The throttle the last physics step applied, from 0 to 1 (<see cref="SetDriverInput"/> gives the next
	/// steps').</summary>
	public float Throttle => ReadState().Throttle;

	/// <summary>The front brake the last physics step applied, from 0 to 1.</summary>
	public float FrontBrake => ReadState().FrontBrake;

	/// <summary>The rear brake the last physics step applied, from 0 to 1.</summary>
	public float RearBrake => ReadState().RearBrake;

	/// <summary>The lean the last physics step applied, from -1 (fully to the left) to 1 (fully to the right).</summary>
	public float Lean => ReadState().Lean;

	/// <summary>What the rider asks of the vehicle from its next physics step on, until the next call: each value is clamped
	/// to its range. The throttle opens the engine; each brake applies its torque at each wheel the definition gives it (the
	/// front brake is usually the front wheel's, the rear brake the rear wheel's); the lean steers, leans and turns the bike
	/// (positive to its right). The latest call before a step wins, from <see cref="Script.OnFixedUpdate"/> or
	/// <see cref="Script.OnUpdate"/>; a value that is not a finite number is a script error and the call does nothing.</summary>
	/// <param name="throttle">0 (closed) to 1 (fully open).</param>
	/// <param name="frontBrake">0 (off) to 1 (full).</param>
	/// <param name="rearBrake">0 (off) to 1 (full).</param>
	/// <param name="lean">-1 (fully to the left) to 1 (fully to the right).</param>
	public void SetDriverInput(float throttle, float frontBrake, float rearBrake, float lean)
	{
		InternalCalls.VehicleComponent_SetDriverInput(Entity.ID, throttle, frontBrake, rearBrake, lean);
	}

	/// <summary>Asks a Manual gearbox for the next gear up at the next physics step (requests before a step add up); the
	/// clutch opens for the definition's switch time and then closes. Nothing happens in top gear, nor in an Automatic
	/// gearbox, which shifts by itself.</summary>
	public void ShiftUp()
	{
		InternalCalls.VehicleComponent_Shift(Entity.ID, 1);
	}

	/// <summary>Asks a Manual gearbox for the next gear down at the next physics step, down to neutral; nothing happens in
	/// neutral, nor in an Automatic gearbox.</summary>
	public void ShiftDown()
	{
		InternalCalls.VehicleComponent_Shift(Entity.ID, -1);
	}

	/// <summary>Puts the vehicle back as it was made, at rest where its body stands: the engine at idle, the gearbox in
	/// neutral with no shift under way or asked for, the wheels still and straight, the lean as new, and the chassis's
	/// velocity and angular velocity at zero. The inputs last given stay, so the next physics step applies them; give others
	/// after this call to change them. It takes effect at once, before the next physics step, and keeps where the body
	/// stands: to put the vehicle somewhere else as well, Teleport its <see cref="RigidBodyComponent"/> in the same frame,
	/// before or after. Without a simulated vehicle it is logged and does nothing.</summary>
	public void ResetVehicle()
	{
		InternalCalls.VehicleComponent_ResetVehicle(Entity.ID);
	}

	/// <summary>A wheel as the last physics step left it: what it stands on, its suspension, its spin and its slips. Without
	/// a simulated vehicle, a wheel without contact.</summary>
	/// <param name="index">0 for the front wheel, 1 for the rear wheel.</param>
	/// <exception cref="ArgumentOutOfRangeException">The index is neither 0 nor 1.</exception>
	[PureNode]
	public VehicleWheelState GetWheelState(int index)
	{
		if (index < 0 || index >= NativeVehicleState.WheelCount)
		{
			throw new ArgumentOutOfRangeException(nameof(index), index, "a vehicle's wheels are 0 (the front) and 1 (the rear)");
		}
		NativeVehicleState state = ReadState();
		NativeVehicleWheel wheel = index == 0 ? state.Front : state.Rear;
		return new VehicleWheelState
		{
			HasContact = wheel.HasContact != 0,
			Ground = wheel.HasContact != 0 ? ScriptRegistry.GetEntity(wheel.Ground) : null,
			Material = PhysicsMaterialCache.GetShared(wheel.Material),
			ContactPoint = wheel.ContactPoint,
			ContactNormal = wheel.ContactNormal,
			SuspensionLength = wheel.SuspensionLength,
			Spin = wheel.Spin,
			LongitudinalSlip = wheel.LongitudinalSlip,
			LateralSlip = wheel.LateralSlip,
		};
	}

	private NativeVehicleState ReadState()
	{
		NativeVehicleState state = default;
		InternalCalls.VehicleComponent_GetState(Entity.ID, &state);
		return state;
	}
}

/// <summary>A wheel of a vehicle as its last physics step left it (<see cref="VehicleComponent.GetWheelState"/>), a struct
/// of public fields that graphs Make and Break.</summary>
public struct VehicleWheelState
{
	/// <summary>Whether the wheel touches the ground: its suspension then carries the chassis and its tyre grips.</summary>
	public bool HasContact;

	/// <summary>The entity the wheel stands on (its script instance when it has one); null without contact.</summary>
	public Entity? Ground;

	/// <summary>The physics material of the surface under the wheel; null without contact and on a surface without one,
	/// whose tyre grip is 1 (see <see cref="Surface"/>).</summary>
	public PhysicsMaterial? Material;

	/// <summary>Where the tyre touches the ground, in world space; zero without contact.</summary>
	public Vector3 ContactPoint;

	/// <summary>The ground's normal where the tyre touches it, unit length; zero without contact.</summary>
	public Vector3 ContactNormal;

	/// <summary>Meters from where the suspension is attached to the wheel's center, between the definition's shortest and
	/// longest suspension lengths: short when compressed.</summary>
	public float SuspensionLength;

	/// <summary>How fast the wheel turns around its axle, in radians per second: positive when it rolls forward.</summary>
	public float Spin;

	/// <summary>The size of the longitudinal slip ratio: how much faster or slower the tyre's tread moves than the ground
	/// goes by under it, as a share of the ground's speed. It is 0 when the tyre rolls with the ground and grows as the tyre
	/// spins or slides, either way: 1 for a locked wheel, and large for a wheel that turns while the bike barely moves. It is
	/// never negative, so it does not tell spinning from braking: compare <see cref="Spin"/> times the wheel's radius with
	/// <see cref="VehicleComponent.ForwardSpeed"/> for that (faster when the tyre spins, slower when it brakes). 0 without
	/// contact.</summary>
	public float LongitudinalSlip;

	/// <summary>The slip angle in degrees, between where the tyre points and where it moves over the ground: from 0 to 90,
	/// whichever side the tyre slides to (never negative); 0 without contact or while the tyre barely moves.</summary>
	public float LateralSlip;

	/// <summary>The name of the surface under the wheel (its material's <see cref="PhysicsMaterial.Surface"/>, such as
	/// Asphalt or Gravel); empty without contact or a material.</summary>
	public readonly string Surface => Material?.Surface ?? string.Empty;

	/// <summary>The contact, the surface, the suspension length, the spin and the slips, as logs and the visual script
	/// debugger show them.</summary>
	public override readonly string ToString() => string.Create(CultureInfo.InvariantCulture,
		$"{(HasContact ? "Contact" : "No contact")}, Surface {Surface}, Suspension {SuspensionLength}, Spin {Spin}, Slips {LongitudinalSlip} {LateralSlip}");
}
