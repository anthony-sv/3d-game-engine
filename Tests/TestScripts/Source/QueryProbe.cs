using System;

namespace Strada.Tests;

// Hovers its body (a dynamic sphere of radius 0.5) on what its physics queries find, from OnFixedUpdate, as vehicle logic
// does: a sphere cast down from just below the body holds it at HoverHeight, and box and capsule casts ahead and behind and
// what the overlaps around it count push it along X. Every query of the API runs each fixed update, and what each found
// (entities, points, normals, distances, counts) is folded into ResultHash, which ResultHashAtStep keeps as it is after
// fixed update HashStep: runs at any frame rate fold the same values.
public sealed class QueryProbe : Script
{
	public float HoverHeight = 1.0f;
	public int HashStep;
	public int Steps;
	public int Found;
	public ulong ResultHash = 14695981039346656037ul;
	public ulong ResultHashAtStep;

	private readonly RaycastHit[] m_Hits = new RaycastHit[4];
	private readonly Entity[] m_Entities = new Entity[8];

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		Steps++;
		RigidBodyComponent body = GetComponent<RigidBodyComponent>()!;
		Vector3 position = Translation;
		Vector3 force = Vector3.Zero;
		// Below the body, so that the cast never starts inside it.
		if (Physics.SphereCast(position + new Vector3(0.0f, -0.55f, 0.0f), 0.05f, Vector3.Down, 4.0f, out ShapeCastHit ground))
		{
			Fold(ground);
			force.Y = (40.0f * (HoverHeight - ground.Distance)) - (6.0f * body.LinearVelocity.Y) + 9.81f;
		}
		Quaternion turned = Quaternion.AngleAxis(30.0f, Vector3.Up);
		if (Physics.BoxCast(position + new Vector3(0.8f, 0.0f, 0.0f), new Vector3(0.2f), turned, Vector3.Right, 3.0f, out ShapeCastHit ahead,
			Physics.AllLayers, true))
		{
			Fold(ahead);
			force.X -= 4.0f / (ahead.Distance + 0.5f);
		}
		if (Physics.CapsuleCast(position - new Vector3(0.8f, 0.0f, 0.0f), 0.15f, 0.3f, turned, Vector3.Left, 3.0f, out ShapeCastHit behind))
		{
			Fold(behind);
			force.X += 4.0f / (behind.Distance + 0.5f);
		}
		int count = Physics.RaycastAll(position, Vector3.Down, 6.0f, m_Hits, Physics.AllLayers, true);
		Fold((uint)count);
		for (int index = 0; index < count; index++)
		{
			Fold(m_Hits[index].Entity.ID);
			Fold(m_Hits[index].Distance);
		}
		int around = FoldEntities(Physics.OverlapSphere(position, 2.0f, m_Entities, Physics.AllLayers, true));
		around += FoldEntities(Physics.OverlapBox(position, new Vector3(1.5f, 0.6f, 1.5f), turned, m_Entities));
		around += FoldEntities(Physics.OverlapCapsule(position, 0.6f, 1.0f, Quaternion.AngleAxis(90.0f, Vector3.Forward), m_Entities, 1u));
		force.X += 0.5f * (around % 3) - 0.5f;
		body.AddForce(force);
		if (Steps == HashStep)
		{
			ResultHashAtStep = ResultHash;
		}
	}

	private void Fold(ShapeCastHit hit)
	{
		Found++;
		Fold(hit.Entity!.ID);
		Fold(hit.Point.X);
		Fold(hit.Point.Y);
		Fold(hit.Point.Z);
		Fold(hit.Normal.X);
		Fold(hit.Normal.Y);
		Fold(hit.Normal.Z);
		Fold(hit.Distance);
	}

	private int FoldEntities(int count)
	{
		Fold((uint)count);
		for (int index = 0; index < count; index++)
		{
			Fold(m_Entities[index].ID);
		}
		return count;
	}

	private void Fold(float value) => Fold(BitConverter.SingleToUInt32Bits(value));

	private void Fold(ulong value)
	{
		// FNV-1a over the value's bytes.
		for (int shift = 0; shift < 64; shift += 8)
		{
			ResultHash = (ResultHash ^ ((value >> shift) & 0xFF)) * 1099511628211ul;
		}
	}
}

// Runs each query of the API once, in its first fixed update, and keeps what it found, for the scene tests to compare with
// the scene's own queries: casts from Origin along Direction (a sphere of Radius, a box of HalfExtents turned TurnDegrees
// about Y, a capsule of Radius and HalfHeight turned likewise), RaycastAll from Origin along Direction into a buffer of
// Capacity, and overlaps at Center (a box of HalfExtents, a sphere of Radius and a capsule of Radius and HalfHeight, all
// turned likewise), all with triggers. Then it calls the queries with values they refuse, which must find nothing.
public sealed class QueryReader : Script
{
	public Vector3 Origin;
	public Vector3 Direction = Vector3.Right;
	public float MaxDistance = 20.0f;
	public Vector3 Center;
	public float Radius = 0.25f;
	public float HalfHeight = 0.5f;
	public Vector3 HalfExtents = new(0.25f);
	public float TurnDegrees;
	public int Capacity = 8;

	public bool Read;
	public Entity? SphereEntity;
	public Vector3 SpherePoint;
	public Vector3 SphereNormal;
	public float SphereDistance;
	public PhysicsMaterial? SphereMaterial;
	public Entity? BoxEntity;
	public Vector3 BoxPoint;
	public float BoxDistance;
	public Entity? CapsuleEntity;
	public Vector3 CapsuleNormal;
	public float CapsuleDistance;
	public int AllCount;
	public Entity? AllFirst;
	public Entity? AllLast;
	public float AllLastDistance;
	public PhysicsMaterial? AllLastMaterial;
	public int BoxOverlaps;
	public Entity? BoxOverlapFirst;
	public Entity? BoxOverlapLast;
	public int SphereOverlaps;
	public int CapsuleOverlaps;
	public int RefusedFound = -1;

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		if (Read)
		{
			return;
		}
		Read = true;
		Quaternion turned = Quaternion.AngleAxis(TurnDegrees, Vector3.Up);
		if (Physics.SphereCast(Origin, Radius, Direction, MaxDistance, out ShapeCastHit sphere, Physics.AllLayers, true))
		{
			SphereEntity = sphere.Entity;
			SpherePoint = sphere.Point;
			SphereNormal = sphere.Normal;
			SphereDistance = sphere.Distance;
			SphereMaterial = sphere.Material;
		}
		if (Physics.BoxCast(Origin, HalfExtents, turned, Direction, MaxDistance, out ShapeCastHit box, Physics.AllLayers, true))
		{
			BoxEntity = box.Entity;
			BoxPoint = box.Point;
			BoxDistance = box.Distance;
		}
		if (Physics.CapsuleCast(Origin, Radius, HalfHeight, turned, Direction, MaxDistance, out ShapeCastHit capsule, Physics.AllLayers, true))
		{
			CapsuleEntity = capsule.Entity;
			CapsuleNormal = capsule.Normal;
			CapsuleDistance = capsule.Distance;
		}
		RaycastHit[] hits = new RaycastHit[Capacity];
		AllCount = Physics.RaycastAll(Origin, Direction, MaxDistance, hits, Physics.AllLayers, true);
		if (AllCount > 0)
		{
			AllFirst = hits[0].Entity;
			AllLast = hits[AllCount - 1].Entity;
			AllLastDistance = hits[AllCount - 1].Distance;
			AllLastMaterial = hits[AllCount - 1].Material;
		}
		Entity[] entities = new Entity[Capacity];
		BoxOverlaps = Physics.OverlapBox(Center, HalfExtents, turned, entities, Physics.AllLayers, true);
		if (BoxOverlaps > 0)
		{
			BoxOverlapFirst = entities[0];
			BoxOverlapLast = entities[BoxOverlaps - 1];
		}
		SphereOverlaps = Physics.OverlapSphere(Center, Radius, entities, Physics.AllLayers, true);
		CapsuleOverlaps = Physics.OverlapCapsule(Center, Radius, HalfHeight, turned, entities, Physics.AllLayers, true);

		// Values the queries refuse, each a script error: they find nothing.
		RefusedFound = 0;
		RefusedFound += Physics.SphereCast(Origin, -1.0f, Direction, MaxDistance, out _) ? 1 : 0;
		RefusedFound += Physics.BoxCast(new Vector3(float.NaN, 0.0f, 0.0f), HalfExtents, turned, Direction, MaxDistance, out _) ? 1 : 0;
		RefusedFound += Physics.CapsuleCast(Origin, Radius, float.PositiveInfinity, turned, Direction, MaxDistance, out _) ? 1 : 0;
		RefusedFound += Physics.RaycastAll(Origin, Direction, float.NaN, hits);
		RefusedFound += Physics.OverlapBox(Center, new Vector3(2.0e6f, 1.0f, 1.0f), turned, entities);
		RefusedFound += Physics.OverlapCapsule(Center, Radius, HalfHeight, new Quaternion(float.NaN, 0.0f, 0.0f, 1.0f), entities);
	}
}
