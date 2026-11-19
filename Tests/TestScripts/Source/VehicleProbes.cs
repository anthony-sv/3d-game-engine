using System;
using System.Globalization;
using Strada;

namespace Strada.Tests;

// Rides its entity's vehicle from OnFixedUpdate by its own count of fixed steps, and reports what each step left in its
// name ("key=value;" pairs, numbers round-trip): the inputs it set the step before, its shifts and the reads. Its
// definition is Manual: step 1 shifts up from neutral, step 20 shifts up twice, step 30 down three times (past neutral),
// step 40 gives values out of their ranges, step 41 one that is not a number (refused), and GetWheelState(2) and (-1)
// throw.
public sealed class VehicleProbe : Script
{
	private int m_Steps;

	protected override void OnCreate()
	{
		VehicleComponent vehicle = GetComponent<VehicleComponent>()!;
		bool threw = false;
		foreach (int index in new[] { 2, -1 })
		{
			try
			{
				vehicle.GetWheelState(index);
			}
			catch (ArgumentOutOfRangeException)
			{
				threw = true;
			}
		}
		Name = string.Create(CultureInfo.InvariantCulture, $"Created;Threw={threw};Definition={vehicle.Definition?.GetWheelName(1)};");
	}

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		VehicleComponent vehicle = GetComponent<VehicleComponent>()!;
		m_Steps++;
		switch (m_Steps)
		{
			case 1:
				vehicle.ShiftUp();
				break;
			case 20:
				vehicle.ShiftUp();
				vehicle.ShiftUp();
				break;
			case 30:
				vehicle.ShiftDown();
				vehicle.ShiftDown();
				vehicle.ShiftDown();
				vehicle.ShiftDown();
				break;
			case 31:
				vehicle.ShiftUp();
				break;
		}
		if (m_Steps == 40)
		{
			vehicle.SetDriverInput(2.0f, -1.0f, 7.0f, -3.0f);
		}
		else if (m_Steps == 41)
		{
			vehicle.SetDriverInput(float.NaN, 0.0f, 0.0f, 0.0f);
		}
		else if (m_Steps < 40)
		{
			vehicle.SetDriverInput(0.5f, 0.0f, m_Steps % 2 == 0 ? 0.0f : 0.125f, m_Steps < 25 ? 0.0f : 0.25f);
		}
		else
		{
			vehicle.SetDriverInput(0.0f, 0.75f, 0.25f, 0.0f);
		}
		Report(vehicle);
	}

	private void Report(VehicleComponent vehicle)
	{
		VehicleWheelState front = vehicle.GetWheelState(0);
		VehicleWheelState rear = vehicle.GetWheelState(1);
		string reads = string.Create(CultureInfo.InvariantCulture,
			$"Step={m_Steps};Gear={vehicle.Gear};Engine={vehicle.EngineSpeed:R};Speed={vehicle.ForwardSpeed:R};Lean={vehicle.LeanAngle:R};");
		string inputs = string.Create(CultureInfo.InvariantCulture,
			$"Throttle={vehicle.Throttle:R};FrontBrake={vehicle.FrontBrake:R};RearBrake={vehicle.RearBrake:R};LeanInput={vehicle.Lean:R};");
		Name = reads + inputs + Describe("Front", front) + Describe("Rear", rear);
	}

	private static string Describe(string wheel, VehicleWheelState state) => string.Create(CultureInfo.InvariantCulture,
		$"{wheel}Contact={state.HasContact};{wheel}Ground={state.Ground?.Name};{wheel}Surface={state.Surface};{wheel}Material={(state.Material != null ? "set" : "none")};{wheel}Suspension={state.SuspensionLength:R};{wheel}Spin={state.Spin:R};{wheel}LongitudinalSlip={state.LongitudinalSlip:R};{wheel}LateralSlip={state.LateralSlip:R};{wheel}PointY={state.ContactPoint.Y:R};{wheel}NormalY={state.ContactNormal.Y:R};");
}

// Rides its entity's Manual bike from OnFixedUpdate as a function of its own count of fixed steps, its inputs changing at odd
// counts too (so that the two steps of a frame at 30 frames a second differ), and shifts on the way: a ride that must be the
// same at every frame rate.
public sealed class VehicleRider : Script
{
	private int m_Steps;

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		VehicleComponent vehicle = GetComponent<VehicleComponent>()!;
		m_Steps++;
		if (m_Steps == 1 || m_Steps == 45 || m_Steps == 91)
		{
			vehicle.ShiftUp();
		}
		if (m_Steps == 151)
		{
			vehicle.ShiftDown();
		}
		float throttle = m_Steps < 150 ? 0.4f + 0.05f * (m_Steps % 3) : 0.0f;
		float lean = m_Steps is > 60 and < 120 ? (m_Steps % 2 == 1 ? 0.3f : 0.2f) : 0.0f;
		float frontBrake = m_Steps >= 150 ? 0.6f + 0.1f * (m_Steps % 2) : 0.0f;
		float rearBrake = m_Steps >= 150 && m_Steps % 3 == 1 ? 0.3f : 0.0f;
		vehicle.SetDriverInput(throttle, frontBrake, rearBrake, lean);
	}
}
