using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>Counts the contacts of its entity, and keeps what <see cref="Collision"/> said of the first collision that began,
/// the first trigger overlap that began and the first contact that ended: the falling box and ball of the scene carry
/// one.</summary>
public sealed class CollisionProbe : Script
{
	public int CollisionsEntered;
	public int CollisionsExited;
	public int TriggersEntered;
	public int TriggersExited;
	public Entity? LastCollision;
	public Entity? LastTrigger;

	private int m_FixedSteps;

	/// <summary>The first collision that began: Collision's entities and step, the physics steps run by then, and its
	/// points.</summary>
	internal (Entity? Entity, Entity? Other, ulong Step, int StepsRun, ContactPoint[] Points)? FirstCollision { get; private set; }

	/// <summary>The points of the first trigger overlap that began.</summary>
	internal ContactPoint[]? FirstTriggerPoints { get; private set; }

	/// <summary>The step and the points of the first contact that ended.</summary>
	internal (ulong Step, ContactPoint[] Points)? FirstExit { get; private set; }

	protected override void OnFixedUpdate(float fixedDeltaTime) => m_FixedSteps++;

	protected override void OnCollisionEnter(Entity other)
	{
		CollisionsEntered++;
		LastCollision = other;
		FirstCollision ??= (Collision.Entity, Collision.Other, Collision.Step, m_FixedSteps, Collision.Points);
	}

	protected override void OnCollisionExit(Entity other)
	{
		CollisionsExited++;
		FirstExit ??= (Collision.Step, Collision.Points);
	}

	protected override void OnTriggerEnter(Entity other)
	{
		TriggersEntered++;
		LastTrigger = other;
		FirstTriggerPoints ??= Collision.Points;
	}

	protected override void OnTriggerExit(Entity other)
	{
		TriggersExited++;
		FirstExit ??= (Collision.Step, Collision.Points);
	}
}

/// <summary>Rigid bodies, colliders, raycasts, physics materials, contact callbacks and what they read of their contacts. The
/// scene sets the entities: a static ground whose top is at y = 0, a box falling onto it, a ball of Rubber falling through a
/// trigger (Sensor) onto the Contact Pad (a static slab of Rubber on the ground under it, whose top is at y = 0.2), a capsule
/// resting on the ground (Pill), a kinematic platform and a static rock with a convex mesh collider; and the physics
/// material PhysicsMaterials/Rubber.sphysmat (Rubber), which every collider of the static Physics Material Probe at
/// (60, 0.5, 60) uses.</summary>
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
	public PhysicsMaterial? Rubber;

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
		if (frame == 1)
		{
			Check("outside a contact handler no contact is being handled", () =>
			{
				Assert.IsNull(Collision.Entity);
				Assert.IsNull(Collision.Other);
				Assert.AreEqual(0ul, Collision.Step);
				Assert.AreEqual(0, Collision.Points.Length);
			});
		}
		RigidBodyComponent pill = Pill!.GetComponent<RigidBodyComponent>()!;
		RigidBodyComponent platform = Platform!.GetComponent<RigidBodyComponent>()!;
		if (frame == 1)
		{
			// Bodies exist once the scene runs.
			Check("raycasts hit what is in their way", RaycastChecks);
			Check("physics materials read their fields, and raycasts name the material they hit", PhysicsMaterialChecks);
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

		bool done = m_BoxLeft && m_BallPassed && ball.CollisionsEntered > 0;
		if (done || frame >= FrameLimit)
		{
			Check("every contact was reported", () =>
			{
				Assert.IsTrue(m_BoxLanded, "the box never landed");
				Assert.IsTrue(m_BoxLeft, "the box never left the ground");
				Assert.IsTrue(m_BallPassed, $"the ball entered the trigger {ball.TriggersEntered} and left it {ball.TriggersExited} times");
				Assert.IsTrue(ball.CollisionsEntered > 0, "the ball never landed");
			});
			Check("contact handlers read where and how hard bodies touch, and which surfaces", () => ContactChecks(box, ball));
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
		// Drawn between steps by default; the platform is drawn where its steps leave it, as the scene says.
		Assert.AreEqual(RigidBodyInterpolation.Interpolate, pill.Interpolation);
		pill.Interpolation = RigidBodyInterpolation.None;
		Assert.AreEqual(RigidBodyInterpolation.None, pill.Interpolation);
		pill.Interpolation = RigidBodyInterpolation.Interpolate;
		Assert.AreEqual(RigidBodyInterpolation.Interpolate, pill.Interpolation);
		Assert.AreEqual(RigidBodyInterpolation.None, Platform!.GetComponent<RigidBodyComponent>()!.Interpolation);
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

	// The box lands flat on the ground, which has no material; the ball lands on the Contact Pad, both of Rubber, after
	// passing through the trigger.
	private void ContactChecks(CollisionProbe box, CollisionProbe ball)
	{
		Assert.IsTrue(box.FirstCollision is not null, "the box saw no collision");
		(Entity? boxEntity, Entity? boxOther, ulong boxStep, int boxSteps, ContactPoint[] boxPoints) = box.FirstCollision!.Value;
		Assert.IsTrue(boxEntity == FallingBox, boxEntity?.Name ?? "nothing");
		Assert.IsTrue(boxOther == Ground, boxOther?.Name ?? "nothing");
		Assert.IsTrue(boxStep > 0 && boxStep <= (ulong)boxSteps, $"step {boxStep} with {boxSteps} steps run");
		Assert.IsTrue(boxPoints.Length is > 0 and <= 4, $"{boxPoints.Length} points");
		for (int index = 0; index < boxPoints.Length; index++)
		{
			ContactPoint point = boxPoints[index];
			Assert.AreApproximatelyEqual(0.0f, point.Point.Y, 0.05f);
			Assert.AreApproximatelyEqual(Vector3.Up, point.Normal, 1e-3f);
			Assert.IsTrue(point.Impulse > 0.0f, point.ToString());
			Assert.IsNull(point.Material);
			Assert.IsNull(point.OtherMaterial);
			// Strongest first.
			Assert.IsTrue(index == 0 || boxPoints[index - 1].Impulse >= point.Impulse, $"point {index} is stronger than the one before");
		}

		Assert.IsTrue(ball.FirstCollision is not null, "the ball saw no collision");
		(Entity? ballEntity, Entity? ballOther, ulong ballStep, _, ContactPoint[] ballPoints) = ball.FirstCollision!.Value;
		Assert.IsTrue(ballEntity == FallingBall, ballEntity?.Name ?? "nothing");
		Assert.AreEqual("Contact Pad", ballOther?.Name);
		Assert.IsTrue(ballStep > 0);
		Assert.AreEqual(1, ballPoints.Length);
		ContactPoint landing = ballPoints[0];
		Assert.AreApproximatelyEqual(new Vector3(-3.0f, 0.2f, 0.0f), landing.Point, 0.05f);
		Assert.AreApproximatelyEqual(Vector3.Up, landing.Normal, 1e-3f);
		Assert.IsTrue(landing.Impulse > 0.0f, landing.ToString());
		// It was falling: this side moves against the normal.
		Assert.IsTrue(landing.RelativeVelocity.Y < -1.0f, landing.RelativeVelocity.ToString());
		Assert.AreEqual(Rubber, landing.Material);
		Assert.AreEqual(Rubber, landing.OtherMaterial);
		Assert.AreEqual("Rubber", landing.OtherMaterial!.Surface);

		// Triggers touch without an impulse; a contact that ends has no points.
		ContactPoint[] trigger = ball.FirstTriggerPoints ?? [];
		Assert.IsTrue(trigger.Length > 0, "the trigger overlap had no points");
		Assert.IsTrue(System.Array.TrueForAll(trigger, static point => point.Impulse == 0.0f), "a trigger point has an impulse");
		Assert.IsTrue(ball.FirstExit is { Step: > 0, Points.Length: 0 }, "the ball's first contact to end had points");
		Assert.IsTrue(box.FirstExit is { Points.Length: 0 }, "the box's first contact to end had points");
		Assert.IsTrue(ball.FirstExit!.Value.Step < ballStep, "the ball landed before it left the trigger");

		// Make and Break's struct: fields that can be set, as graphs make it.
		ContactPoint made = new()
		{
			Point = Vector3.One,
			Normal = Vector3.Up,
			Impulse = 2.0f,
			RelativeVelocity = Vector3.Down,
			Material = Rubber,
			OtherMaterial = null,
		};
		Assert.AreEqual("Point (1, 1, 1), Normal (0, 1, 0), Impulse 2", made.ToString());
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

	// PhysicsMaterials/Rubber.sphysmat sets every field away from its default.
	private void PhysicsMaterialChecks()
	{
		PhysicsMaterial rubber = Rubber!;
		Assert.AreEqual(rubber, Assets.Load<PhysicsMaterial>("PhysicsMaterials/Rubber.sphysmat"));
		Assert.AreEqual(1.1f, rubber.Friction);
		Assert.AreEqual(0.35f, rubber.Restitution);
		Assert.AreEqual(PhysicsMaterialCombine.Maximum, rubber.FrictionCombine);
		Assert.AreEqual(PhysicsMaterialCombine.Average, rubber.RestitutionCombine);
		Assert.AreEqual(0.015f, rubber.RollingResistance);
		Assert.AreEqual("Rubber", rubber.Surface);
		Assert.AreEqual(Assets.Load<AudioClip>("Audio/Beep.wav"), rubber.ImpactSound);
		Assert.AreEqual(Assets.Load<Prefab>("Prefabs/Crate.sprefab"), rubber.ImpactParticles);

		// The probe's capsule is on top: a ray from above names its material, and a ray onto the ground, which has none, names
		// none.
		Assert.IsTrue(Physics.Raycast(new Vector3(60.0f, 10.0f, 60.0f), Vector3.Down, 20.0f, out RaycastHit probe));
		Assert.AreEqual("Physics Material Probe", probe.Entity.Name);
		Assert.AreEqual(rubber, probe.Material);
		Assert.AreEqual("Rubber", probe.Material!.Surface);
		// From the side, the box and the triangles of the mesh collider name it too.
		Assert.IsTrue(Physics.Raycast(new Vector3(55.0f, 0.5f, 60.0f), Vector3.Right, 10.0f, out RaycastHit side));
		Assert.AreApproximatelyEqual(4.5f, side.Distance, 1e-3f);
		Assert.AreEqual(rubber, side.Material);
		Assert.IsTrue(Physics.Raycast(new Vector3(5.0f, 10.0f, 5.0f), Vector3.Down, 50.0f, out RaycastHit ground));
		Assert.IsTrue(ground.Entity == Ground, ground.Entity.Name);
		Assert.IsNull(ground.Material);
	}
}
