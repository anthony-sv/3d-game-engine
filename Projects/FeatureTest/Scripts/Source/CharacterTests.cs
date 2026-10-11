using System;
using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>Character controllers. The scene's Character (a capsule with a CollisionProbe) stands on the ground, and the
/// Gate, a trigger, stands across its way along +X: it walks through the gate, jumps, lands and is teleported back.</summary>
public sealed class CharacterTests : FeatureTestScript
{
	private const int FrameLimit = 600;
	private const float WalkSpeed = 2.0f;
	private const float WalkDistance = 2.5f;
	private const float JumpSpeed = 4.0f;

	public Entity? Character;
	public Entity? Gate;

	private enum Phase
	{
		Standing,
		Walking,
		Jumping,
		Teleported,
	}

	private Phase m_Phase = Phase.Standing;
	private Vector3 m_Start;
	private bool m_Jumped;
	private bool m_Airborne;
	private float m_Peak;

	private CharacterControllerComponent Controller => Character!.GetComponent<CharacterControllerComponent>()!;

	protected override void Start()
	{
		Check("character controller fields", () =>
		{
			CharacterControllerComponent controller = Controller;
			Assert.AreEqual(0.5f, controller.Radius);
			controller.Radius = 0.5f;
			Assert.AreEqual(2.0f, controller.Height);
			controller.Height = 2.0f;
			Assert.AreEqual(Vector3.Zero, controller.Offset);
			controller.Offset = Vector3.Zero;
			Assert.AreEqual(0.3f, controller.StepHeight);
			controller.StepHeight = 0.3f;
			Assert.AreEqual(45.0f, controller.MaxSlopeAngle);
			controller.MaxSlopeAngle = 45.0f;
			Assert.AreEqual(70.0f, controller.Mass);
			controller.Mass = 70.0f;
			Assert.AreEqual(200.0f, controller.PushForce);
			controller.PushForce = 200.0f;
			Assert.AreEqual(1.0f, controller.GravityFactor);
			controller.GravityFactor = 1.0f;
			Assert.AreEqual(0u, controller.Layer);
			controller.Layer = 0;
		});
	}

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		CharacterControllerComponent controller = Controller;
		if (m_Phase == Phase.Walking)
		{
			controller.Move(new Vector3(WalkSpeed, 0.0f, 0.0f));
		}
		else if (m_Phase == Phase.Jumping && !m_Jumped && controller.IsGrounded)
		{
			controller.Velocity = new Vector3(0.0f, JumpSpeed, 0.0f);
			m_Jumped = true;
		}
	}

	protected override bool Step(int frame, float deltaTime)
	{
		CharacterControllerComponent controller = Controller;
		CollisionProbe probe = Character!.As<CollisionProbe>()!;
		if (frame >= FrameLimit)
		{
			Check("the character finished its course", () => Assert.Fail($"stuck {m_Phase} at {Character.Translation}"));
			return true;
		}

		switch (m_Phase)
		{
			case Phase.Standing:
				Check("characters stand on the ground", () =>
				{
					Assert.IsTrue(controller.IsGrounded);
					Assert.AreApproximatelyEqual(Vector3.Up, controller.GroundNormal, 1e-3f);
					Assert.AreEqual(Vector3.Zero, controller.Velocity);
				});
				m_Start = Character.Translation;
				m_Phase = Phase.Walking;
				return false;
			case Phase.Walking:
				if (Character.Translation.X < m_Start.X + WalkDistance)
				{
					return false;
				}
				Check("characters walk where they are moved, through triggers that notice them", () =>
				{
					Assert.AreApproximatelyEqual(m_Start.Y, Character.Translation.Y, 0.01f);
					Assert.AreApproximatelyEqual(m_Start.Z, Character.Translation.Z, 0.01f);
					Assert.IsTrue(controller.IsGrounded);
					Assert.IsTrue(probe.LastTrigger == Gate, probe.LastTrigger?.Name ?? "no trigger");
					Assert.IsTrue(probe.TriggersEntered > 0 && probe.TriggersExited > 0,
						$"entered {probe.TriggersEntered}, left {probe.TriggersExited}");
				});
				m_Phase = Phase.Jumping;
				return false;
			case Phase.Jumping:
				m_Peak = MathF.Max(m_Peak, Character.Translation.Y);
				if (m_Jumped && !controller.IsGrounded && controller.Velocity.Y > 0.0f)
				{
					m_Airborne = true;
				}
				if (!m_Airborne || !controller.IsGrounded)
				{
					return false;
				}
				Check("characters jump and land", () =>
				{
					Assert.IsTrue(m_Peak > m_Start.Y + 0.5f, $"peak {m_Peak}");
					Assert.AreApproximatelyEqual(m_Start.Y, Character.Translation.Y, 0.01f);
					Assert.AreEqual(Vector3.Zero, controller.Velocity);
				});
				controller.Teleport(m_Start);
				Check("teleported characters are where they were sent, at rest", () =>
				{
					Assert.AreApproximatelyEqual(m_Start, Character.Translation, 1e-4f);
					Assert.AreEqual(Vector3.Zero, controller.Velocity);
				});
				m_Phase = Phase.Teleported;
				return false;
			default:
				Check("teleported characters stand where they were sent", () =>
				{
					Assert.IsTrue(controller.IsGrounded);
					Assert.AreApproximatelyEqual(m_Start, Character.Translation, 0.01f);
				});
				return true;
		}
	}
}
