using System;
using Strada;
using Strada.Testing;

namespace FeatureTest;

/// <summary>Shape casts, overlaps and RaycastAll against closed forms. It makes its own static row at y = 1 along +X from
/// (-60, 1, -60), away from everything else: a trigger whose near face is at x = -58.5, a box at -56.5 (Near), a box on the
/// Movers layer at -54.5 (Mover) and a sphere at -51 (Far), each 1 m deep; and it casts onto the static Physics Material Probe
/// at (60, 0.5, 60), whose topmost collider (a capsule of Rubber) ends at y = 5. Bodies made this frame are found at once:
/// a query applies the physics changes made since the last step first.</summary>
public sealed class PhysicsQueryTests : FeatureTestScript
{
	private const float Tolerance = 1e-3f;
	private const uint MoversLayer = 1u << 1;

	private static readonly Vector3 s_RowStart = new(-60.0f, 1.0f, -60.0f);
	private static readonly Vector3 s_AlongX = Vector3.Right;

	private Entity? m_Trigger;
	private Entity? m_Near;
	private Entity? m_Mover;
	private Entity? m_Far;

	protected override void Start()
	{
		m_Trigger = MakeBox("Query Trigger", -58.0f, trigger: true, layer: 0);
		m_Near = MakeBox("Query Near", -56.0f, trigger: false, layer: 0);
		m_Mover = MakeBox("Query Mover", -54.0f, trigger: false, layer: 1);
		m_Far = Entity.Create("Query Far");
		m_Far.Translation = new Vector3(-50.0f, 1.0f, -60.0f);
		m_Far.AddComponent<SphereColliderComponent>().Radius = 1.0f;
	}

	protected override bool Step(int frame, float deltaTime)
	{
		Check("a sphere cast touches the first solid collider in its way", SphereCastChecks);
		Check("box and capsule casts touch with their turned shapes", TurnedCastChecks);
		Check("casts see triggers and layers as their filter says, and name the material they touch", FilterChecks);
		Check("RaycastAll gives every entity along a ray, nearest first", RaycastAllChecks);
		Check("overlaps give every entity they reach, by entity ID", OverlapChecks);
		m_Trigger!.Destroy();
		m_Near!.Destroy();
		m_Mover!.Destroy();
		m_Far!.Destroy();
		return true;
	}

	private static Entity MakeBox(string name, float x, bool trigger, uint layer)
	{
		Entity box = Entity.Create(name);
		box.Translation = new Vector3(x, 1.0f, -60.0f);
		if (layer != 0)
		{
			RigidBodyComponent body = box.AddComponent<RigidBodyComponent>();
			body.Type = RigidBodyType.Static;
			body.Layer = layer;
		}
		BoxColliderComponent collider = box.AddComponent<BoxColliderComponent>();
		collider.HalfExtents = new Vector3(0.5f, 1.0f, 2.0f);
		collider.IsTrigger = trigger;
		return box;
	}

	private void SphereCastChecks()
	{
		// A sphere of 0.25 m from x = -60 touches Near's face at -56.5 when its center is 3.25 m along.
		Assert.IsTrue(Physics.SphereCast(s_RowStart, 0.25f, s_AlongX, 20.0f, out ShapeCastHit hit), "the sphere cast touches nothing");
		ExpectHit(hit, m_Near, new Vector3(-56.5f, 1.0f, -60.0f), new Vector3(-1.0f, 0.0f, 0.0f), 3.25f);
		Assert.IsNull(hit.Material, "Near has no physics material");
		// Too short, it touches nothing, and the hit is empty.
		Assert.IsFalse(Physics.SphereCast(s_RowStart, 0.25f, s_AlongX, 3.0f, out ShapeCastHit missed), "a cast of 3 m reaches Near");
		Assert.IsNull(missed.Entity);
		Assert.AreEqual(0.0f, missed.Distance);
		// From inside Near, it touches Near at once, whichever way it goes.
		Assert.IsTrue(Physics.SphereCast(new Vector3(-56.0f, 1.0f, -60.0f), 0.25f, -s_AlongX, 20.0f, out ShapeCastHit inside));
		Assert.AreEqual(m_Near, inside.Entity);
		Assert.AreEqual(0.0f, inside.Distance);
		// A direction need not be unit length: the distance is in meters along it.
		Assert.IsTrue(Physics.SphereCast(s_RowStart, 0.25f, s_AlongX * 7.0f, 20.0f, out ShapeCastHit scaled));
		Assert.AreApproximatelyEqual(3.25f, scaled.Distance, Tolerance);
	}

	private void TurnedCastChecks()
	{
		// A cube of half extent 0.25 turned 45 degrees about Y leads with an edge 0.25 * sqrt(2) ahead of its center.
		Quaternion turned = Quaternion.AngleAxis(45.0f, Vector3.Up);
		Assert.IsTrue(Physics.BoxCast(s_RowStart, new Vector3(0.25f), turned, s_AlongX, 20.0f, out ShapeCastHit box), "the box cast touches nothing");
		Assert.AreEqual(m_Near, box.Entity);
		Assert.AreApproximatelyEqual(3.5f - (0.25f * MathF.Sqrt(2.0f)), box.Distance, Tolerance);
		Assert.AreApproximatelyEqual(-56.5f, box.Point.X, Tolerance);
		Assert.AreApproximatelyEqual(new Vector3(-1.0f, 0.0f, 0.0f), box.Normal, Tolerance);
		// Unturned, its face leads by 0.25.
		Assert.IsTrue(Physics.BoxCast(s_RowStart, new Vector3(0.25f), Quaternion.Identity, s_AlongX, 20.0f, out ShapeCastHit square));
		Assert.AreApproximatelyEqual(3.25f, square.Distance, Tolerance);
		// A capsule of radius 0.25 and half height 0.5 lying along X (turned 90 degrees about Z) leads by 0.75; upright, by 0.25.
		Quaternion lying = Quaternion.AngleAxis(90.0f, Vector3.Forward);
		Assert.IsTrue(Physics.CapsuleCast(s_RowStart, 0.25f, 0.5f, lying, s_AlongX, 20.0f, out ShapeCastHit capsule), "the capsule cast touches nothing");
		ExpectHit(capsule, m_Near, new Vector3(-56.5f, 1.0f, -60.0f), new Vector3(-1.0f, 0.0f, 0.0f), 2.75f);
		Assert.IsTrue(Physics.CapsuleCast(s_RowStart, 0.25f, 0.5f, Quaternion.Identity, s_AlongX, 20.0f, out ShapeCastHit upright));
		Assert.AreApproximatelyEqual(3.25f, upright.Distance, Tolerance);
	}

	private void FilterChecks()
	{
		// With triggers, the trigger's face at -58.5 comes first.
		Assert.IsTrue(Physics.SphereCast(s_RowStart, 0.25f, s_AlongX, 20.0f, out ShapeCastHit trigger, Physics.AllLayers, true));
		ExpectHit(trigger, m_Trigger, new Vector3(-58.5f, 1.0f, -60.0f), new Vector3(-1.0f, 0.0f, 0.0f), 1.25f);
		// On the Movers layer alone, Mover's face at -54.5 is the first.
		Assert.IsTrue(Physics.BoxCast(s_RowStart, new Vector3(0.25f), Quaternion.Identity, s_AlongX, 20.0f, out ShapeCastHit mover, MoversLayer));
		ExpectHit(mover, m_Mover, new Vector3(-54.5f, 1.0f, -60.0f), new Vector3(-1.0f, 0.0f, 0.0f), 5.25f);
		Assert.IsFalse(Physics.CapsuleCast(s_RowStart, 0.25f, 0.5f, Quaternion.Identity, s_AlongX, 20.0f, out _, 0u), "a mask of no layers sees something");

		// Down onto the Physics Material Probe: its capsule of Rubber ends at y = 5, so a sphere of 0.25 from y = 10 touches it
		// 4.75 m down, a box of half height 0.3 4.7 m down and an upright capsule reaching 0.5 below its center 4.5 m down.
		Entity probe = Entity.FindByName("Physics Material Probe")!;
		PhysicsMaterial rubber = Assets.Load<PhysicsMaterial>("PhysicsMaterials/Rubber.sphysmat")!;
		Vector3 above = new(60.0f, 10.0f, 60.0f);
		Assert.IsTrue(Physics.SphereCast(above, 0.25f, Vector3.Down, 20.0f, out ShapeCastHit sphere));
		ExpectHit(sphere, probe, new Vector3(60.0f, 5.0f, 60.0f), Vector3.Up, 4.75f);
		Assert.IsTrue(Physics.BoxCast(above, new Vector3(0.2f, 0.3f, 0.2f), Quaternion.Identity, Vector3.Down, 20.0f, out ShapeCastHit box));
		ExpectHit(box, probe, new Vector3(60.0f, 5.0f, 60.0f), Vector3.Up, 4.7f);
		Assert.IsTrue(Physics.CapsuleCast(above, 0.2f, 0.3f, Quaternion.Identity, Vector3.Down, 20.0f, out ShapeCastHit capsule));
		ExpectHit(capsule, probe, new Vector3(60.0f, 5.0f, 60.0f), Vector3.Up, 4.5f);
		foreach (ShapeCastHit hit in new[] { sphere, box, capsule })
		{
			Assert.AreEqual(rubber, hit.Material, "the cast does not name the probe's material");
			Assert.AreEqual("Rubber", hit.Material!.Surface);
		}
	}

	private void RaycastAllChecks()
	{
		// Every entity along the row, each where the ray enters it.
		RaycastHit[] hits = new RaycastHit[8];
		Assert.AreEqual(4, Physics.RaycastAll(s_RowStart, s_AlongX, 20.0f, hits, Physics.AllLayers, true));
		ExpectRayHits(hits, [(m_Trigger, 1.5f), (m_Near, 3.5f), (m_Mover, 5.5f), (m_Far, 9.0f)]);
		Assert.AreApproximatelyEqual(new Vector3(-58.5f, 1.0f, -60.0f), hits[0].Point, Tolerance);
		Assert.AreApproximatelyEqual(new Vector3(-1.0f, 0.0f, 0.0f), hits[0].Normal, Tolerance);
		// Without triggers, and on the Default layer alone.
		Assert.AreEqual(3, Physics.RaycastAll(s_RowStart, s_AlongX, 20.0f, hits));
		ExpectRayHits(hits, [(m_Near, 3.5f), (m_Mover, 5.5f), (m_Far, 9.0f)]);
		Assert.AreEqual(2, Physics.RaycastAll(s_RowStart, s_AlongX, 20.0f, hits, 1u));
		ExpectRayHits(hits, [(m_Near, 3.5f), (m_Far, 9.0f)]);
		// A buffer smaller than the hits keeps the nearest; one larger is filled no further than the hits.
		RaycastHit[] two = new RaycastHit[2];
		Assert.AreEqual(2, Physics.RaycastAll(s_RowStart, s_AlongX, 20.0f, two, Physics.AllLayers, true));
		ExpectRayHits(two, [(m_Trigger, 1.5f), (m_Near, 3.5f)]);
		// Short of Near, the ray hits nothing.
		Assert.AreEqual(0, Physics.RaycastAll(s_RowStart, s_AlongX, 3.0f, hits));
	}

	private void OverlapChecks()
	{
		// A box from x = -57 to -53 reaches Near and Mover; the trigger only when triggers count, and it ends at -57.5.
		Entity[] entities = new Entity[4];
		Vector3 middle = new(-55.0f, 1.0f, -60.0f);
		Assert.AreEqual(2, Physics.OverlapBox(middle, new Vector3(2.0f, 0.5f, 0.5f), Quaternion.Identity, entities));
		ExpectByID(entities, 2, m_Near!, m_Mover!);
		Assert.AreEqual(3, Physics.OverlapBox(middle, new Vector3(3.0f, 0.5f, 0.5f), Quaternion.Identity, entities, Physics.AllLayers, true));
		ExpectByID(entities, 3, m_Trigger!, m_Near!, m_Mover!);
		// A capsule lying along X over the same span, with the Default layer alone, and with room for one.
		Quaternion lying = Quaternion.AngleAxis(90.0f, Vector3.Forward);
		Assert.AreEqual(1, Physics.OverlapCapsule(middle, 0.25f, 1.75f, lying, entities, 1u));
		Assert.AreEqual(m_Near, entities[0]);
		Entity[] one = new Entity[1];
		Assert.AreEqual(1, Physics.OverlapCapsule(middle, 0.25f, 1.75f, lying, one));
		Assert.AreEqual(Math.Min(m_Near!.ID, m_Mover!.ID), one[0].ID, "a buffer of one keeps the smallest ID");
		// A sphere inside the trigger overlaps it when triggers count, and nothing otherwise.
		Vector3 inTrigger = new(-58.0f, 1.0f, -60.0f);
		Assert.AreEqual(0, Physics.OverlapSphere(inTrigger, 0.25f, entities));
		Assert.AreEqual(1, Physics.OverlapSphere(inTrigger, 0.25f, entities, Physics.AllLayers, true));
		Assert.AreEqual(m_Trigger, entities[0]);
	}

	private static void ExpectHit(ShapeCastHit hit, Entity? entity, Vector3 point, Vector3 normal, float distance)
	{
		Assert.AreEqual(entity, hit.Entity, $"the cast touched {hit}");
		Assert.AreApproximatelyEqual(point, hit.Point, Tolerance, $"the cast touched {hit}");
		Assert.AreApproximatelyEqual(normal, hit.Normal, Tolerance, $"the cast touched {hit}");
		Assert.AreApproximatelyEqual(distance, hit.Distance, Tolerance, $"the cast touched {hit}");
	}

	private static void ExpectRayHits(RaycastHit[] hits, (Entity? Entity, float Distance)[] expected)
	{
		for (int index = 0; index < expected.Length; index++)
		{
			Assert.AreEqual(expected[index].Entity, hits[index].Entity, $"hit {index}");
			Assert.AreApproximatelyEqual(expected[index].Distance, hits[index].Distance, Tolerance, $"hit {index}");
		}
	}

	private static void ExpectByID(Entity[] entities, int count, params Entity[] expected)
	{
		Array.Sort(expected, static (a, b) => a.ID.CompareTo(b.ID));
		for (int index = 0; index < count; index++)
		{
			Assert.AreEqual(expected[index], entities[index], $"entity {index}");
		}
	}
}
