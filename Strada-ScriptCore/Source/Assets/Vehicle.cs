using Strada.Interop;
using Strada.VisualScripting;

namespace Strada;

/// <summary>How a vehicle's gearbox changes gear.</summary>
public enum VehicleTransmissionMode
{
	/// <summary>By itself: up past <see cref="Vehicle.ShiftUpRPM"/>, down below <see cref="Vehicle.ShiftDownRPM"/>; from
	/// neutral it takes the first gear when the throttle opens.</summary>
	Automatic = 0,
	/// <summary>When the rider asks (<see cref="VehicleComponent.ShiftUp"/>, <see cref="VehicleComponent.ShiftDown"/>), one
	/// gear per request; it starts in neutral.</summary>
	Manual = 1,
}

/// <summary>A vehicle definition asset (<c>.svehicle</c>): a motorcycle's two wheels (the front steers, the engine drives
/// the rear), its engine, its gearbox and final drive, and how it leans. A <see cref="VehicleComponent"/> names the
/// definition its entity rides with. Edit it in the editor; scripts read it, for example the engine's speeds a rev counter
/// shows or the gears a gear indicator counts, and an edit made while the game plays reaches the vehicles and the scripts on
/// the next frame. A definition that cannot be loaded reads as a new one's values (logged).</summary>
public sealed class Vehicle : Asset
{
	// The runtime's copy of the asset's values, shared by every reference to the asset; found on first use.
	private VehicleCache.Entry? m_Entry;

	internal Vehicle(AssetHandle handle)
		: base(handle)
	{
	}

	/// <summary>Whether the gearbox shifts by itself or when the rider asks.</summary>
	public VehicleTransmissionMode TransmissionMode => GetValues().TransmissionMode;

	/// <summary>How many forward gears the gearbox has (1 to 16); there is no reverse gear.</summary>
	public int GearCount => GetValues().GearRatios.Length;

	/// <summary>A forward gear's ratio between the engine and the gearbox's output (the final drive comes after it); 0 for a
	/// gear the gearbox lacks.</summary>
	/// <param name="gear">From 1 (the first gear) to <see cref="GearCount"/>.</param>
	[PureNode]
	public float GetGearRatio(int gear)
	{
		float[] ratios = GetValues().GearRatios;
		return gear >= 1 && gear <= ratios.Length ? ratios[gear - 1] : 0.0f;
	}

	/// <summary>The ratio from the gearbox's output to the rear wheel: the primary and final drives together.</summary>
	public float FinalDrive => GetValues().FinalDrive;

	/// <summary>The largest torque the engine gives, in newton meters, at full throttle.</summary>
	public float MaxTorque => GetValues().MaxTorque;

	/// <summary>The engine's lowest speed in revolutions per minute: it idles there.</summary>
	public float MinRPM => GetValues().MinRPM;

	/// <summary>The engine's highest speed in revolutions per minute: it never turns faster (the red line).</summary>
	public float MaxRPM => GetValues().MaxRPM;

	/// <summary>The engine speed in revolutions per minute above which an Automatic gearbox shifts up.</summary>
	public float ShiftUpRPM => GetValues().ShiftUpRPM;

	/// <summary>The engine speed in revolutions per minute below which an Automatic gearbox shifts down.</summary>
	public float ShiftDownRPM => GetValues().ShiftDownRPM;

	/// <summary>The farthest the bike leans in turns, in degrees.</summary>
	public float MaxLeanAngle => GetValues().MaxLeanAngle;

	/// <summary>A wheel's name, which a VehicleWheel component names to draw it; empty for an index other than 0 and 1.</summary>
	/// <param name="index">0 for the front wheel, 1 for the rear wheel.</param>
	[PureNode]
	public string GetWheelName(int index) => index switch
	{
		0 => GetValues().FrontName,
		1 => GetValues().RearName,
		_ => string.Empty,
	};

	private VehicleValues GetValues()
	{
		m_Entry ??= VehicleCache.Find(Handle);
		return m_Entry.GetValues();
	}
}
