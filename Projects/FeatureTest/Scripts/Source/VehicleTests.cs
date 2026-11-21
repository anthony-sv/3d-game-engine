using System;
using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>Vehicles. The scene's Bike (a dynamic chassis with a Vehicle on Vehicles/Bike.svehicle, whose gearbox is
/// Automatic, and a VehicleWheel entity for each wheel) stands on the ground at (-7, 0, 6), its front along -Z.
/// Features.sgraph rides it first, from its Event.FixedUpdate; once the graph's BikeRidden is set, this test rides it again
/// from OnFixedUpdate by its own count of steps (the throttle, a lean to the right, both brakes) and reads every member on
/// the way, then resets the stopped bike and reads it at once. Inputs set in a fixed step reach the physics step that
/// follows it, so the ride is the same at every frame rate.</summary>
public sealed class VehicleTests : FeatureTestScript
{
	// Game seconds the course may take, the graph's ride included.
	private const float TimeLimit = 20.0f;
	private const float RideThrottle = 0.4f;
	private const float TurnThrottle = 0.3f;
	private const float TurnLean = 0.5f;
	// The steps of each part of the ride, counted from its first.
	private const int ThrottleSteps = 60;
	private const int TurnSteps = 30;
	private const int BrakeSteps = 90;

	public Entity? Bike;
	public Entity? Ground;

	private enum Phase
	{
		WaitingForTheGraph,
		Riding,
		Done,
	}

	private Phase m_Phase = Phase.WaitingForTheGraph;
	private float m_Elapsed;
	// The fixed steps of this test's ride so far, and what it read at the ends of its parts.
	private int m_Steps;
	private float m_AppliedThrottle;
	private float m_SpeedAfterThrottle;
	private float m_EngineAfterThrottle;
	private int m_GearAfterThrottle;
	private int m_GearBeforeShift;
	private int m_GearAfterShift;
	private VehicleWheelState m_Front;
	private VehicleWheelState m_Rear;
	private Vector3 m_ForwardBeforeTurn;
	private Vector3 m_ForwardAfterTurn;
	private float m_LeanAfterTurn;
	private float m_AppliedLean;
	private float m_AppliedFrontBrake;
	private float m_AppliedRearBrake;
	private float m_SpeedAfterBraking;
	private bool m_ContactBeforeReset;
	private VehicleWheelState m_RearAfterReset;
	private int m_GearAfterReset = -1;
	private float m_EngineAfterReset;
	private float m_SpeedAfterReset = -1.0f;

	private VehicleComponent Vehicle => Bike!.GetComponent<VehicleComponent>()!;

	protected override void Start()
	{
		Check("a vehicle's definition reads its gearbox, engine, lean and wheels", () =>
		{
			VehicleComponent vehicle = Vehicle;
			Vehicle? definition = vehicle.Definition;
			Assert.IsNotNull(definition, "the Bike names no definition");
			Assert.AreEqual(Assets.Load<Vehicle>("Vehicles/Bike.svehicle"), definition);
			vehicle.Definition = definition;
			Assert.AreEqual(VehicleTransmissionMode.Automatic, definition.TransmissionMode);
			Assert.AreNotEqual(VehicleTransmissionMode.Manual, definition.TransmissionMode);
			Assert.AreEqual(6, definition.GearCount);
			Assert.AreApproximatelyEqual(2.6f, definition.GetGearRatio(1));
			Assert.AreApproximatelyEqual(1.0f, definition.GetGearRatio(6));
			Assert.AreEqual(0.0f, definition.GetGearRatio(7));
			Assert.AreApproximatelyEqual(5.2f, definition.FinalDrive);
			Assert.AreEqual(120.0f, definition.MaxTorque);
			Assert.AreEqual(1500.0f, definition.MinRPM);
			Assert.AreEqual(12000.0f, definition.MaxRPM);
			Assert.AreEqual(10500.0f, definition.ShiftUpRPM);
			Assert.AreEqual(4000.0f, definition.ShiftDownRPM);
			Assert.AreEqual(48.0f, definition.MaxLeanAngle);
			Assert.AreEqual("Front", definition.GetWheelName(0));
			Assert.AreEqual("Rear", definition.GetWheelName(1));
			Assert.AreEqual("", definition.GetWheelName(2));
			Assert.Throws<ArgumentOutOfRangeException>(() => vehicle.GetWheelState(2));
		});
	}

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		if (m_Phase != Phase.Riding)
		{
			return;
		}
		VehicleComponent vehicle = Vehicle;
		m_Steps++;
		// What the step before this one left: the inputs this test gave the steps before.
		if (m_Steps == 2)
		{
			m_AppliedThrottle = vehicle.Throttle;
		}
		else if (m_Steps == ThrottleSteps + 1)
		{
			m_SpeedAfterThrottle = vehicle.ForwardSpeed;
			m_EngineAfterThrottle = vehicle.EngineSpeed;
			m_GearAfterThrottle = vehicle.Gear;
			m_Front = vehicle.GetWheelState(0);
			m_Rear = vehicle.GetWheelState(1);
			m_ForwardBeforeTurn = Bike!.Transform.Forward;
			// An Automatic gearbox shifts by itself: a request changes nothing.
			m_GearBeforeShift = vehicle.Gear;
			vehicle.ShiftUp();
		}
		else if (m_Steps == ThrottleSteps + 2)
		{
			m_GearAfterShift = vehicle.Gear;
		}
		else if (m_Steps == ThrottleSteps + TurnSteps + 1)
		{
			m_ForwardAfterTurn = Bike!.Transform.Forward;
			m_LeanAfterTurn = vehicle.LeanAngle;
			m_AppliedLean = vehicle.Lean;
		}
		else if (m_Steps == ThrottleSteps + TurnSteps + 2)
		{
			m_AppliedFrontBrake = vehicle.FrontBrake;
			m_AppliedRearBrake = vehicle.RearBrake;
		}
		else if (m_Steps == ThrottleSteps + TurnSteps + BrakeSteps + 1)
		{
			m_SpeedAfterBraking = vehicle.ForwardSpeed;
			vehicle.SetDriverInput(0.0f, 0.0f, 0.0f, 0.0f);
			vehicle.ShiftDown();
			// Stopped, the ride's last read taken: put back as it was made, where it stands, and read before any step.
			m_ContactBeforeReset = vehicle.GetWheelState(1).HasContact;
			vehicle.ResetVehicle();
			m_GearAfterReset = vehicle.Gear;
			m_EngineAfterReset = vehicle.EngineSpeed;
			m_SpeedAfterReset = vehicle.ForwardSpeed;
			m_RearAfterReset = vehicle.GetWheelState(1);
			m_Phase = Phase.Done;
			return;
		}

		if (m_Steps <= ThrottleSteps)
		{
			vehicle.SetDriverInput(RideThrottle, 0.0f, 0.0f, 0.0f);
		}
		else if (m_Steps <= ThrottleSteps + TurnSteps)
		{
			vehicle.SetDriverInput(TurnThrottle, 0.0f, 0.0f, TurnLean);
		}
		else
		{
			vehicle.SetDriverInput(0.0f, 1.0f, 1.0f, 0.0f);
		}
	}

	protected override bool Step(int frame, float deltaTime)
	{
		m_Elapsed += deltaTime;
		if (m_Phase == Phase.WaitingForTheGraph)
		{
			VisualScriptComponent? graph = Entity.FindByName("Visual Script Tests")?.GetComponent<VisualScriptComponent>();
			if (graph != null && graph.HasVariable("BikeRidden") && graph.GetVariable<bool>("BikeRidden"))
			{
				m_Phase = Phase.Riding;
			}
		}
		if (m_Phase != Phase.Done)
		{
			if (m_Elapsed < TimeLimit)
			{
				return false;
			}
			Check("the vehicle finished its ride", () => Assert.Fail($"stuck {m_Phase} after {m_Steps} steps"));
			return true;
		}

		Check("the throttle reaches the next physics step, takes the first gear and speeds the bike up", () =>
		{
			Assert.AreEqual(RideThrottle, m_AppliedThrottle);
			Assert.IsTrue(m_SpeedAfterThrottle > 1.0f, $"{m_SpeedAfterThrottle} m/s");
			Assert.IsTrue(m_EngineAfterThrottle > 1500.0f, $"{m_EngineAfterThrottle} rpm");
			Assert.IsTrue(m_GearAfterThrottle >= 1, $"gear {m_GearAfterThrottle}");
		});
		Check("an Automatic gearbox ignores the rider's shifts", () => Assert.AreEqual(m_GearBeforeShift, m_GearAfterShift));
		Check("both wheels stand on the ground, their suspensions between their lengths, the rear wheel turning", () =>
		{
			foreach (VehicleWheelState wheel in new[] { m_Front, m_Rear })
			{
				Assert.IsTrue(wheel.HasContact, wheel.ToString());
				Assert.IsTrue(wheel.Ground != null && wheel.Ground.ID == Ground!.ID, wheel.Ground?.Name ?? "no ground");
				Assert.IsNull(wheel.Material, "the ground has no physics material");
				Assert.AreEqual("", wheel.Surface);
				Assert.IsTrue(wheel.SuspensionLength >= 0.25f && wheel.SuspensionLength <= 0.45f, $"{wheel.SuspensionLength} m");
				Assert.IsTrue(wheel.ContactNormal.Y > 0.99f, $"normal {wheel.ContactNormal}");
				Assert.AreApproximatelyEqual(0.0f, wheel.ContactPoint.Y, 0.05f, $"contact {wheel.ContactPoint}");
				Assert.IsTrue(MathF.Abs(wheel.LateralSlip) < 10.0f && float.IsFinite(wheel.LongitudinalSlip), wheel.ToString());
			}
			// The engine drives the rear wheel: it turns forward, and under the throttle its tyre slips. A slip ratio is a
			// size, never negative (it grows whether a tyre spins or brakes), so this shows a slip, not its direction; and at
			// this pace the rear tyre's slip alternates from one step to the next between a large one and a small one, so
			// the reading is only required above 0.
			Assert.IsTrue(m_Rear.Spin > 1.0f, $"the rear wheel turns at {m_Rear.Spin} rad/s");
			Assert.IsTrue(m_Rear.LongitudinalSlip > 0.0f && m_Front.LongitudinalSlip >= 0.0f, $"rear {m_Rear}; front {m_Front}");
		});
		Check("a lean to the right leans the bike and turns it to its right", () =>
		{
			Assert.AreEqual(TurnLean, m_AppliedLean);
			Assert.IsTrue(m_LeanAfterTurn > 0.25f, $"lean {m_LeanAfterTurn} degrees");
			// Turning right from -Z swings the front towards +X.
			Assert.IsTrue(m_ForwardAfterTurn.X > m_ForwardBeforeTurn.X + 0.01f, $"{m_ForwardBeforeTurn} then {m_ForwardAfterTurn}");
		});
		Check("both brakes stop the bike", () =>
		{
			Assert.AreEqual(1.0f, m_AppliedFrontBrake);
			Assert.AreEqual(1.0f, m_AppliedRearBrake);
			Assert.IsTrue(MathF.Abs(m_SpeedAfterBraking) < 0.3f, $"{m_SpeedAfterBraking} m/s");
		});
		Check("a reset puts the vehicle back as it was made, at rest where it stands", () =>
		{
			// In neutral at idle, the chassis at rest, the wheels still and without contact until the next step tests them.
			Assert.AreEqual(0, m_GearAfterReset);
			Assert.AreEqual(1500.0f, m_EngineAfterReset);
			Assert.AreEqual(0.0f, m_SpeedAfterReset);
			Assert.IsTrue(m_ContactBeforeReset, "the stopped bike stood on the ground");
			Assert.IsFalse(m_RearAfterReset.HasContact, m_RearAfterReset.ToString());
			Assert.AreEqual(0.0f, m_RearAfterReset.Spin);
		});
		return true;
	}
}
