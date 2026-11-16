namespace Strada.Tests;

// Keeps what Strada.Collision says in its entity's contact handlers: the first collision that begins (its entities, step and
// points: the first and the last of them in full), the first trigger overlap that begins and ends, and whether Collision
// was empty in every OnUpdate and OnFixedUpdate, for the scene tests to compare with the scene's contact events.
public sealed class ContactReader : Script
{
	public int CollisionsEntered;
	public Entity? EnterEntity;
	public Entity? EnterOther;
	public ulong EnterStep;
	public int EnterPointCount;
	public Vector3 FirstPoint;
	public Vector3 FirstNormal;
	public float FirstImpulse;
	public Vector3 FirstRelativeVelocity;
	public PhysicsMaterial? FirstMaterial;
	public PhysicsMaterial? FirstOtherMaterial;
	public Vector3 LastPoint;
	public float LastImpulse;

	public int TriggersEntered;
	public ulong TriggerEnterStep;
	public int TriggerEnterPointCount;
	public float TriggerEnterImpulse = -1.0f;
	public int TriggersExited;
	public ulong TriggerExitStep;
	public int TriggerExitPointCount = -1;
	public Entity? TriggerExitOther;

	public bool EmptyOutsideHandlers = true;

	protected override void OnUpdate(float deltaTime) => CheckEmpty();

	protected override void OnFixedUpdate(float fixedDeltaTime) => CheckEmpty();

	protected override void OnCollisionEnter(Entity other)
	{
		if (CollisionsEntered++ > 0)
		{
			return;
		}
		EnterEntity = Collision.Entity;
		EnterOther = Collision.Other;
		EnterStep = Collision.Step;
		ContactPoint[] points = Collision.Points;
		EnterPointCount = points.Length;
		if (points.Length == 0)
		{
			return;
		}
		FirstPoint = points[0].Point;
		FirstNormal = points[0].Normal;
		FirstImpulse = points[0].Impulse;
		FirstRelativeVelocity = points[0].RelativeVelocity;
		FirstMaterial = points[0].Material;
		FirstOtherMaterial = points[0].OtherMaterial;
		LastPoint = points[^1].Point;
		LastImpulse = points[^1].Impulse;
	}

	protected override void OnTriggerEnter(Entity other)
	{
		if (TriggersEntered++ > 0)
		{
			return;
		}
		TriggerEnterStep = Collision.Step;
		ContactPoint[] points = Collision.Points;
		TriggerEnterPointCount = points.Length;
		TriggerEnterImpulse = points.Length > 0 ? points[0].Impulse : -1.0f;
	}

	protected override void OnTriggerExit(Entity other)
	{
		if (TriggersExited++ > 0)
		{
			return;
		}
		TriggerExitStep = Collision.Step;
		TriggerExitPointCount = Collision.Points.Length;
		TriggerExitOther = Collision.Other;
	}

	private void CheckEmpty()
	{
		if (Collision.Entity != null || Collision.Other != null || Collision.Step != 0 || Collision.Points.Length != 0)
		{
			EmptyOutsideHandlers = false;
		}
	}
}
