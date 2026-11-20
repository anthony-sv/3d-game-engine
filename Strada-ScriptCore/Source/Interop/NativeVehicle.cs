using System.Runtime.InteropServices;

namespace Strada.Interop;

// A wheel as VehicleComponent_GetState copies it (ScriptBindingsComponents.cpp declares the same layout): the slip angle in
// degrees, the entity and the material 0 for none.
[StructLayout(LayoutKind.Sequential)]
internal struct NativeVehicleWheel
{
	public ulong Ground;
	public ulong Material;
	public Vector3 ContactPoint;
	public Vector3 ContactNormal;
	public float SuspensionLength;
	public float Spin;
	public float LongitudinalSlip;
	public float LateralSlip;
	public uint HasContact;
	public uint Reserved;
}

// What VehicleComponent_GetState copies of a vehicle as its last step left it (ScriptBindingsComponents.cpp declares the
// same layout): the inputs it applied, its engine, gearbox and lean (degrees, positive to its right), and its two wheels.
// All zero without a simulated vehicle.
[StructLayout(LayoutKind.Sequential)]
internal struct NativeVehicleState
{
	// A vehicle's wheels: the front, then the rear (the engine's VehicleWheelCount).
	internal const int WheelCount = 2;

	public float Throttle;
	public float FrontBrake;
	public float RearBrake;
	public float Lean;
	public float EngineSpeed;
	public float ForwardSpeed;
	public float LeanAngle;
	public int Gear;
	public NativeVehicleWheel Front;
	public NativeVehicleWheel Rear;
}

// What Vehicle_Read copies of a vehicle definition (ScriptBindingsAssets.cpp declares the same layout): its gears' ratios
// apart, its wheels' names pointing into the asset's data, which the caller copies at once.
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeVehicle
{
	// The asset's version, never 0.
	public ulong Version;
	public byte* FrontName;
	public byte* RearName;
	// The lengths in bytes of the UTF-8 names.
	public int FrontNameLength;
	public int RearNameLength;
	public float MaxTorque;
	public float MinRPM;
	public float MaxRPM;
	public float ShiftUpRPM;
	public float ShiftDownRPM;
	public float FinalDrive;
	public float MaxLeanAngle;
	public int TransmissionMode;
	public int GearCount;
	public int Reserved;
}

// Vehicle_Read's answers.
internal enum VehicleReadStatus
{
	// The engine logged why; the values and the ratios are as they were.
	Failed = 0,
	// The held version is the asset's: only the values' Version is written.
	Unchanged = 1,
	// The values and the gears' ratios (GearCount of them) are written.
	Read = 2,
}
