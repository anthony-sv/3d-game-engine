using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>Counts the contacts of its entity: the falling box and ball of the scene carry one.</summary>
public sealed class CollisionProbe : Script
{
	public int CollisionsEntered;
	public int CollisionsExited;
	public int TriggersEntered;
	public int TriggersExited;
	public Entity? LastCollision;
	public Entity? LastTrigger;

	protected override void OnCollisionEnter(Entity other)
	{
		CollisionsEntered++;
		LastCollision = other;
	}

	protected override void OnCollisionExit(Entity other) => CollisionsExited++;

	protected override void OnTriggerEnter(Entity other)
	{
		TriggersEntered++;
		LastTrigger = other;
	}

	protected override void OnTriggerExit(Entity other) => TriggersExited++;
}

/// <summary>Rigid bodies, colliders, raycasts and contact callbacks. The scene sets the entities: a static ground whose
/// top is at y = 0, a box falling onto it, a ball falling through a trigger (Sensor), a capsule resting on the ground
/// (Pill), a kinematic platform and a static rock with a convex mesh collider.</summary>
public sealed class PhysicsTests : FeatureTestScript
{
	private const int FrameLimit = 600;

	public Entity? Ground;
	public Entity? FallingBox;
	public Entity? FallingBall;
	public Entity? Pill;
	public Entity? Platform;
	public Entity? Rock;
	public Entity? Sensor;

	private int m_FixedUpdates;
	private int m_OtherFixedSteps;
	private bool m_BoxLanded;
	private bool m_BoxTeleported;
	private bool m_BoxLeft;
	private bool m_BallPassed;

	protected override void Start()
	{
		Check("physics settings", () =>
		{
			Assert.AreApproximatelyEqual(new Vector3(0.0f, -9.81f, 0.0f), Physics.Gravity, 1e-4f);
			Physics.Gravity = new Vector3(0.0f, -9.81f, 0.0f);
			Assert.AreEqual(uint.MaxValue, Physics.AllLayers);
		});
		Check("rigid bodies", RigidBodyChecks);
		Check("colliders", ColliderChecks);
	}

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		m_FixedUpdates++;
		if (fixedDeltaTime != Time.FixedDeltaTime)
		{
			m_OtherFixedSteps++;
		}
	}

	protected override bool Step(int frame, float deltaTime)
	{
		RigidBodyComponent pill = Pill!.GetComponent<RigidBodyComponent>()!;
		RigidBodyComponent platform = Platform!.GetComponent<RigidBodyComponent>()!;
		if (frame == 1)
		{
			// Bodies exist once the scene runs.
			Check("raycasts hit what is in their way", RaycastChecks);
			pill.AddForce(new Vector3(0.0f, 0.0f, -5.0f), ForceMode.Impulse);
			pill.AddTorque(new Vector3(0.0f, 2.0f, 0.0f), ForceMode.VelocityChange);
			platform.MoveKinematic(new Vector3(0.0f, 0.25f, -5.0f), Quaternion.Identity);
			return false;
		}
		if (frame == 2)
		{
			Check("forces and torques move bodies", () =>
			{
				Assert.IsTrue(pill.LinearVelocity.Z < -1.0f, pill.LinearVelocity.ToString());
				Assert.IsTrue(pill.AngularVelocity.Y > 1.0f, pill.AngularVelocity.ToString());
				pill.LinearVelocity = Vector3.Zero;
				Assert.AreEqual(Vector3.Zero, pill.LinearVelocity);
				pill.AngularVelocity = Vector3.Zero;
				Assert.AreEqual(Vector3.Zero, pill.AngularVelocity);
				pill.WakeUp();
				Assert.IsFalse(pill.IsSleeping);
			});
			Check("kinematic bodies move to their targets", () =>
				Assert.AreApproximatelyEqual(new Vector3(0.0f, 0.25f, -5.0f), Platform.Translation, 1e-3f));
			return false;
		}

		CollisionProbe box = FallingBox!.As<CollisionProbe>()!;
		CollisionProbe ball = FallingBall!.As<CollisionProbe>()!;
		RigidBodyComponent boxBody = FallingBox.GetComponent<RigidBodyComponent>()!;
		if (!m_BoxLanded && box.CollisionsEntered > 0)
		{
			m_BoxLanded = true;
			Check("bodies collide with what they land on", () =>
			{
				Assert.IsTrue(box.LastCollision == Ground, box.LastCollision?.Name ?? "nothing");
				Assert.IsTrue(FallingBox.Translation.Y < 3.0f);
			});
			boxBody.Teleport(new Vector3(3.0f, 4.0f, 0.0f), Quaternion.Identity);
		}
		else if (m_BoxLanded && !m_BoxTeleported)
		{
			m_BoxTeleported = true;
			Check("teleported bodies are where they were sent", () =>
				Assert.IsTrue(FallingBox.Translation.Y > 3.0f, FallingBox.Translation.ToString()));
		}
		if (m_BoxTeleported && !m_BoxLeft && box.CollisionsExited > 0)
		{
			m_BoxLeft = true;
		}
		if (!m_BallPassed && ball.TriggersEntered > 0 && ball.TriggersExited > 0)
		{
			m_BallPassed = true;
			Check("bodies pass through triggers", () =>
			{
				Assert.IsTrue(ball.LastTrigger == Sensor, ball.LastTrigger?.Name ?? "nothing");
				Assert.IsFalse(ball.LastCollision == Sensor, "the trigger was reported as a collision");
			});
		}

		bool done = m_BoxLeft && m_BallPassed;
		if (done || frame >= FrameLimit)
		{
			Check("every contact was reported", () =>
			{
				Assert.IsTrue(m_BoxLanded, "the box never landed");
				Assert.IsTrue(m_BoxLeft, "the box never left the ground");
				Assert.IsTrue(m_BallPassed, $"the ball entered the trigger {ball.TriggersEntered} and left it {ball.TriggersExited} times");
			});
			Check("fixed updates step by the fixed time step", () =>
			{
				Assert.IsTrue(m_FixedUpdates > 0);
				Assert.AreEqual(0, m_OtherFixedSteps, $"{m_OtherFixedSteps} of {m_FixedUpdates} fixed updates did not step by {Time.FixedDeltaTime}");
			});
			return true;
		}
		return false;
	}

	private void RigidBodyChecks()
	{
		RigidBodyComponent pill = Pill!.GetComponent<RigidBodyComponent>()!;
		Assert.AreEqual(RigidBodyType.Dynamic, pill.Type);
		pill.Type = RigidBodyType.Dynamic;
		pill.Mass = 2.0f;
		Assert.AreEqual(2.0f, pill.Mass);
		pill.Layer = 1;
		Assert.AreEqual(1u, pill.Layer);
		pill.GravityFactor = 1.0f;
		Assert.AreEqual(1.0f, pill.GravityFactor);
		pill.LinearDamping = 0.1f;
		Assert.AreEqual(0.1f, pill.LinearDamping);
		pill.AngularDamping = 0.2f;
		Assert.AreEqual(0.2f, pill.AngularDamping);
		Assert.AreEqual(RigidBodyType.Kinematic, Platform!.GetComponent<RigidBodyComponent>()!.Type);
		Assert.AreEqual(RigidBodyType.Static, Ground!.GetComponent<RigidBodyComponent>()!.Type);
	}

	private void ColliderChecks()
	{
		BoxColliderComponent sensor = Sensor!.GetComponent<BoxColliderComponent>()!;
		Assert.IsTrue(sensor.IsTrigger);
		Assert.AreEqual(new Vector3(1.0f, 0.5f, 1.0f), sensor.HalfExtents);
		sensor.HalfExtents = new Vector3(1.0f, 0.5f, 1.0f);
		sensor.Offset = Vector3.Zero;
		Assert.AreEqual(Vector3.Zero, sensor.Offset);
		sensor.IsTrigger = true;
		sensor.Friction = 0.5f;
		Assert.AreEqual(0.5f, sensor.Friction);
		sensor.Restitution = 0.0f;
		Assert.AreEqual(0.0f, sensor.Restitution);

		SphereColliderComponent ball = FallingBall!.GetComponent<SphereColliderComponent>()!;
		Assert.AreEqual(0.5f, ball.Radius);
		ball.Radius = 0.5f;
		ball.Offset = Vector3.Zero;
		Assert.AreEqual(Vector3.Zero, ball.Offset);
		ball.IsTrigger = false;
		Assert.IsFalse(ball.IsTrigger);
		ball.Friction = 0.4f;
		Assert.AreEqual(0.4f, ball.Friction);
		ball.Restitution = 0.1f;
		Assert.AreEqual(0.1f, ball.Restitution);

		CapsuleColliderComponent capsule = Pill!.GetComponent<CapsuleColliderComponent>()!;
		Assert.AreEqual(0.5f, capsule.Radius);
		capsule.Radius = 0.5f;
		Assert.AreEqual(0.5f, capsule.HalfHeight);
		capsule.HalfHeight = 0.5f;
		capsule.Offset = Vector3.Zero;
		Assert.AreEqual(Vector3.Zero, capsule.Offset);
		capsule.IsTrigger = false;
		Assert.IsFalse(capsule.IsTrigger);
		capsule.Friction = 0.6f;
		Assert.AreEqual(0.6f, capsule.Friction);
		capsule.Restitution = 0.0f;
		Assert.AreEqual(0.0f, capsule.Restitution);

		MeshColliderComponent rock = Rock!.GetComponent<MeshColliderComponent>()!;
		Mesh? cube = Assets.Load<Mesh>("builtin://Cube");
		Assert.AreEqual(cube, rock.Mesh);
		rock.Mesh = cube;
		Assert.IsTrue(rock.Convex);
		rock.Convex = true;
		rock.IsTrigger = false;
		Assert.IsFalse(rock.IsTrigger);
		rock.Friction = 0.7f;
		Assert.AreEqual(0.7f, rock.Friction);
		rock.Restitution = 0.2f;
		Assert.AreEqual(0.2f, rock.Restitution);
	}

	private void RaycastChecks()
	{
		Assert.IsTrue(Physics.Raycast(new Vector3(5.0f, 10.0f, 5.0f), Vector3.Down, 50.0f, out RaycastHit hit));
		Assert.IsTrue(hit.Entity == Ground, hit.Entity.Name);
		Assert.AreApproximatelyEqual(new Vector3(5.0f, 0.0f, 5.0f), hit.Point, 1e-3f);
		Assert.AreApproximatelyEqual(Vector3.Up, hit.Normal, 1e-3f);
		Assert.AreApproximatelyEqual(10.0f, hit.Distance, 1e-3f);

		Ray ray = new(new Vector3(0.0f, 10.0f, 0.0f), Vector3.Down);
		ray.Origin = new Vector3(5.0f, 10.0f, 5.0f);
		ray.Direction = Vector3.Down;
		Assert.AreEqual(new Vector3(5.0f, 7.0f, 5.0f), ray.GetPoint(3.0f));
		Assert.IsTrue(Physics.Raycast(ray, 50.0f, out RaycastHit again));
		Assert.AreApproximatelyEqual(hit.Distance, again.Distance, 1e-4f);
		// Too short, or only looking for bodies on the Movers layer: the ground is on Default.
		Assert.IsFalse(Physics.Raycast(ray, 5.0f, out _));
		Assert.IsFalse(Physics.Raycast(ray, 50.0f, out _, 1u << 1));
	}
}
