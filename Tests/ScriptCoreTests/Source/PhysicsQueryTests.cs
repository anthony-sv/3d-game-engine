using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Strada.Interop;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The scripting API's shape casts, overlaps and RaycastAll without an engine: Physics_ShapeCast, Physics_Overlap,
// Physics_RaycastAll and Physics_Raycast are served by fakes of the engine's bindings, so that the tests see what scripts send
// and what they get back. The engine's side is StradaTests' (Scene/ScenePhysicsQueryTests.cpp: scripts' queries give what the
// scene's give). The fakes are the process's, so the tests run alone.
[Collection(ProcessStaticsCollection.Name)]
public sealed unsafe class PhysicsQueryTests : IDisposable
{
	// What the fake engine was asked, call by call.
	private sealed record Request(string Binding, NativeQueryShape Shape, Vector3 Position, Quaternion Rotation, Vector3 Direction, float MaxDistance,
		uint LayerMask, byte IncludeTriggers, int Capacity);

	private static readonly List<Request> s_Requests = [];
	// Whether the fakes note what they are asked (which allocates); off while a test measures what the API allocates.
	private static bool s_Recording = true;
	// What the fake engine finds: the hits of a cast or ray (the first for a cast), and the entities of an overlap.
	private static NativeRaycastHit[] s_Hits = [];
	private static ulong[] s_Entities = [];

	private readonly delegate* unmanaged<NativeQueryShape*, Vector3*, Quaternion*, Vector3*, float, uint, byte, NativeRaycastHit*, byte> m_PreviousShapeCast;
	private readonly delegate* unmanaged<NativeQueryShape*, Vector3*, Quaternion*, uint, byte, ulong*, int, int> m_PreviousOverlap;
	private readonly delegate* unmanaged<Vector3*, Vector3*, float, uint, byte, NativeRaycastHit*, int, int> m_PreviousRaycastAll;
	private readonly delegate* unmanaged<Vector3*, Vector3*, float, uint, NativeRaycastHit*, byte> m_PreviousRaycast;

	public PhysicsQueryTests()
	{
		m_PreviousShapeCast = InternalCalls.Physics_ShapeCast;
		m_PreviousOverlap = InternalCalls.Physics_Overlap;
		m_PreviousRaycastAll = InternalCalls.Physics_RaycastAll;
		m_PreviousRaycast = InternalCalls.Physics_Raycast;
		InternalCalls.Physics_ShapeCast = &ShapeCast;
		InternalCalls.Physics_Overlap = &Overlap;
		InternalCalls.Physics_RaycastAll = &RaycastAll;
		InternalCalls.Physics_Raycast = &Raycast;
		s_Requests.Clear();
		s_Recording = true;
		s_Hits = [];
		s_Entities = [];
		DataAssets.Reset();
	}

	public void Dispose()
	{
		InternalCalls.Physics_ShapeCast = m_PreviousShapeCast;
		InternalCalls.Physics_Overlap = m_PreviousOverlap;
		InternalCalls.Physics_RaycastAll = m_PreviousRaycastAll;
		InternalCalls.Physics_Raycast = m_PreviousRaycast;
		s_Requests.Clear();
		DataAssets.Reset();
	}

	// The bindings as ScriptBindingsRuntime.cpp answers them: a cast and a ray give the first hit, the many-hit forms copy
	// at most capacity of what they find and say how many they copied.
	[UnmanagedCallersOnly]
	private static byte ShapeCast(NativeQueryShape* shape, Vector3* origin, Quaternion* rotation, Vector3* direction, float maxDistance,
		uint layerMask, byte includeTriggers, NativeRaycastHit* hit)
	{
		if (s_Recording)
		{
			s_Requests.Add(new Request("ShapeCast", *shape, *origin, *rotation, *direction, maxDistance, layerMask, includeTriggers, 1));
		}
		*hit = s_Hits.Length > 0 ? s_Hits[0] : default;
		return s_Hits.Length > 0 ? (byte)1 : (byte)0;
	}

	[UnmanagedCallersOnly]
	private static int Overlap(NativeQueryShape* shape, Vector3* center, Quaternion* rotation, uint layerMask, byte includeTriggers, ulong* entities,
		int capacity)
	{
		if (s_Recording)
		{
			s_Requests.Add(new Request("Overlap", *shape, *center, *rotation, default, 0.0f, layerMask, includeTriggers, capacity));
		}
		int count = Math.Min(s_Entities.Length, capacity);
		s_Entities.AsSpan(0, count).CopyTo(new Span<ulong>(entities, count));
		return count;
	}

	[UnmanagedCallersOnly]
	private static int RaycastAll(Vector3* origin, Vector3* direction, float maxDistance, uint layerMask, byte includeTriggers, NativeRaycastHit* hits,
		int capacity)
	{
		if (s_Recording)
		{
			s_Requests.Add(new Request("RaycastAll", default, *origin, default, *direction, maxDistance, layerMask, includeTriggers, capacity));
		}
		int count = Math.Min(s_Hits.Length, capacity);
		s_Hits.AsSpan(0, count).CopyTo(new Span<NativeRaycastHit>(hits, count));
		return count;
	}

	[UnmanagedCallersOnly]
	private static byte Raycast(Vector3* origin, Vector3* direction, float maxDistance, uint layerMask, NativeRaycastHit* hit)
	{
		*hit = s_Hits.Length > 0 ? s_Hits[0] : default;
		return s_Hits.Length > 0 ? (byte)1 : (byte)0;
	}

	private static NativeRaycastHit Hit(ulong entity, float distance, ulong material = 0) => new()
	{
		Entity = entity,
		Point = new Vector3(distance, 1.0f, 2.0f),
		Normal = Vector3.Up,
		Distance = distance,
		Material = material,
	};

	[Fact]
	public void CastsSendTheirShapeAndArgumentsAndGiveTheHit()
	{
		s_Hits = [Hit(5, 2.5f, 7)];
		Quaternion turned = Quaternion.AngleAxis(45.0f, Vector3.Up);
		Vector3 origin = new(1.0f, 2.0f, 3.0f);

		Assert.True(Physics.SphereCast(origin, 0.25f, Vector3.Down, 10.0f, out ShapeCastHit sphere));
		Assert.True(Physics.BoxCast(origin, new Vector3(0.5f, 1.0f, 1.5f), turned, Vector3.Forward, 20.0f, out ShapeCastHit box, 1u << 2, true));
		Assert.True(Physics.CapsuleCast(origin, 0.3f, 0.6f, turned, Vector3.Right, 30.0f, out ShapeCastHit capsule, 6u));

		Assert.Equal(
		[
			new Request("ShapeCast", NativeQueryShape.MakeSphere(0.25f), origin, Quaternion.Identity, Vector3.Down, 10.0f, Physics.AllLayers, 0, 1),
			new Request("ShapeCast", NativeQueryShape.MakeBox(new Vector3(0.5f, 1.0f, 1.5f)), origin, turned, Vector3.Forward, 20.0f, 4u, 1, 1),
			new Request("ShapeCast", NativeQueryShape.MakeCapsule(0.3f, 0.6f), origin, turned, Vector3.Right, 30.0f, 6u, 0, 1),
		], s_Requests);
		foreach (ShapeCastHit hit in new[] { sphere, box, capsule })
		{
			Assert.Equal(new Entity(5), hit.Entity);
			Assert.Equal(new Vector3(2.5f, 1.0f, 2.0f), hit.Point);
			Assert.Equal(Vector3.Up, hit.Normal);
			Assert.Equal(2.5f, hit.Distance);
			Assert.Equal(new PhysicsMaterial(new AssetHandle(7)), hit.Material);
			// Hits on one material share one reference, as raycast hits do.
			Assert.Same(sphere.Material, hit.Material);
		}
		Assert.Equal("Entity 5, Point (2.5, 1, 2), Normal (0, 1, 0), Distance 2.5", sphere.ToString());
		Assert.Equal("Entity None, Point (0, 0, 0), Normal (0, 0, 0), Distance 0", default(ShapeCastHit).ToString());
	}

	[Fact]
	public void ACastThatTouchesNothingGivesNoHit()
	{
		Assert.False(Physics.SphereCast(Vector3.Zero, 0.5f, Vector3.Down, 10.0f, out ShapeCastHit hit));
		Assert.Null(hit.Entity);
		Assert.Null(hit.Material);
		Assert.Equal(0.0f, hit.Distance);
		Assert.False(Physics.BoxCast(Vector3.Zero, Vector3.One, Quaternion.Identity, Vector3.Down, 10.0f, out _));
		Assert.False(Physics.CapsuleCast(Vector3.Zero, 0.5f, 0.5f, Quaternion.Identity, Vector3.Down, 10.0f, out _));
	}

	[Fact]
	public void RaycastAllFillsTheBufferWithTheFirstHitsAndCountsThem()
	{
		s_Hits = [Hit(11, 1.0f), Hit(12, 2.0f, 7), Hit(13, 3.0f)];
		RaycastHit[] hits = new RaycastHit[8];
		Assert.Equal(3, Physics.RaycastAll(Vector3.Zero, Vector3.Forward, 50.0f, hits, 1u << 1, true));
		Assert.Equal([11ul, 12ul, 13ul, 0ul], new[] { hits[0].Entity.ID, hits[1].Entity.ID, hits[2].Entity.ID, hits[3].Entity?.ID ?? 0ul });
		Assert.Equal(2.0f, hits[1].Distance);
		Assert.Equal(new Vector3(2.0f, 1.0f, 2.0f), hits[1].Point);
		Assert.Equal(new PhysicsMaterial(new AssetHandle(7)), hits[1].Material);
		Assert.Null(hits[0].Material);
		Assert.Equal(new Request("RaycastAll", default, Vector3.Zero, default, Vector3.Forward, 50.0f, 2u, 1, 8), s_Requests[^1]);

		// A buffer smaller than the hits gets the first of them: the engine is asked for no more than it holds.
		RaycastHit[] two = new RaycastHit[2];
		Assert.Equal(2, Physics.RaycastAll(Vector3.Zero, Vector3.Forward, 50.0f, two));
		Assert.Equal([11ul, 12ul], new[] { two[0].Entity.ID, two[1].Entity.ID });
		Assert.Equal(2, s_Requests[^1].Capacity);
		Assert.Equal((Physics.AllLayers, (byte)0), (s_Requests[^1].LayerMask, s_Requests[^1].IncludeTriggers));

		// A slice of a buffer is filled where it lies; an empty buffer gets nothing, but the query is still checked.
		RaycastHit[] wide = new RaycastHit[4];
		Assert.Equal(1, Physics.RaycastAll(Vector3.Zero, Vector3.Forward, 50.0f, wide.AsSpan(2, 1)));
		Assert.Equal(11ul, wide[2].Entity.ID);
		Assert.Null(wide[1].Entity);
		Assert.Equal(0, Physics.RaycastAll(Vector3.Zero, Vector3.Forward, 50.0f, []));
		Assert.Equal(0, s_Requests[^1].Capacity);
	}

	[Fact]
	public void OverlapsSendTheirShapeAndFillTheBufferWithTheFirstEntities()
	{
		s_Entities = [21, 22, 23];
		Quaternion turned = Quaternion.AngleAxis(30.0f, Vector3.Right);
		Vector3 center = new(4.0f, 5.0f, 6.0f);
		Entity[] entities = new Entity[3];

		Assert.Equal(3, Physics.OverlapSphere(center, 1.5f, entities));
		Assert.Equal([new Entity(21), new Entity(22), new Entity(23)], entities);
		Entity[] one = new Entity[1];
		Assert.Equal(1, Physics.OverlapBox(center, new Vector3(1.0f, 2.0f, 3.0f), turned, one, 1u, true));
		Assert.Equal(new Entity(21), one[0]);
		Assert.Equal(2, Physics.OverlapCapsule(center, 0.5f, 1.0f, turned, entities.AsSpan(1), 3u));
		Assert.Equal([new Entity(21), new Entity(21), new Entity(22)], entities);

		Assert.Equal(
		[
			new Request("Overlap", NativeQueryShape.MakeSphere(1.5f), center, Quaternion.Identity, default, 0.0f, Physics.AllLayers, 0, 3),
			new Request("Overlap", NativeQueryShape.MakeBox(new Vector3(1.0f, 2.0f, 3.0f)), center, turned, default, 0.0f, 1u, 1, 1),
			new Request("Overlap", NativeQueryShape.MakeCapsule(0.5f, 1.0f), center, turned, default, 0.0f, 3u, 0, 2),
		], s_Requests);
		s_Entities = [];
		Assert.Equal(0, Physics.OverlapSphere(center, 1.5f, entities));
	}

	[Fact]
	public void BuffersLargerThanTheStackScratchAreFilledAlike()
	{
		s_Hits = new NativeRaycastHit[300];
		s_Entities = new ulong[300];
		for (int index = 0; index < 300; index++)
		{
			s_Hits[index] = Hit((ulong)index + 1, index);
			s_Entities[index] = (ulong)index + 1;
		}
		RaycastHit[] hits = new RaycastHit[200];
		Assert.Equal(200, Physics.RaycastAll(Vector3.Zero, Vector3.Forward, 500.0f, hits));
		Entity[] entities = new Entity[250];
		Assert.Equal(250, Physics.OverlapBox(Vector3.Zero, Vector3.One, Quaternion.Identity, entities));
		for (int index = 0; index < 200; index++)
		{
			Assert.Equal((ulong)index + 1, hits[index].Entity.ID);
			Assert.Equal(index, hits[index].Distance);
		}
		for (int index = 0; index < 250; index++)
		{
			Assert.Equal((ulong)index + 1, entities[index].ID);
		}
	}

	[Fact]
	public void QueriesAllocateNothingOnceTheirEntitiesHaveBeenMet()
	{
		ScriptRegistry.DestroyAllInstances();
		s_Hits = [Hit(31, 1.0f, 7), Hit(32, 2.0f, 7), Hit(33, 3.0f, 7)];
		s_Entities = [31, 32, 33];
		RaycastHit[] small = new RaycastHit[4];
		RaycastHit[] large = new RaycastHit[100];
		Entity[] entities = new Entity[3];
		Entity[] many = new Entity[200];
		// Every path once, so that what is made once (the shared material, the pooled buffers, the entities' Entity) is made.
		Physics.Raycast(Vector3.Zero, Vector3.Forward, 10.0f, out _);
		Physics.SphereCast(Vector3.Zero, 0.5f, Vector3.Forward, 10.0f, out _);
		Physics.RaycastAll(Vector3.Zero, Vector3.Forward, 10.0f, small);
		Physics.RaycastAll(Vector3.Zero, Vector3.Forward, 10.0f, large);
		Physics.OverlapSphere(Vector3.Zero, 1.0f, many);
		s_Recording = false;

		// The single-hit forms, Raycast among them, allocate nothing: their entities have no script instance, and the Entity
		// made for each at its first hit is kept.
		long before = GC.GetAllocatedBytesForCurrentThread();
		Physics.Raycast(Vector3.Zero, Vector3.Forward, 10.0f, out RaycastHit ray);
		Physics.SphereCast(Vector3.Zero, 0.5f, Vector3.Forward, 10.0f, out ShapeCastHit sphere);
		Physics.BoxCast(Vector3.Zero, Vector3.One, Quaternion.Identity, Vector3.Forward, 10.0f, out ShapeCastHit box);
		Physics.CapsuleCast(Vector3.Zero, 0.5f, 0.5f, Quaternion.Identity, Vector3.Forward, 10.0f, out ShapeCastHit capsule);
		Assert.Equal(0, GC.GetAllocatedBytesForCurrentThread() - before);
		Assert.Same(ray.Entity, sphere.Entity);
		Assert.Same(sphere.Entity, box.Entity);
		Assert.Same(box.Entity, capsule.Entity);
		// Nor do the many-hit forms, whatever the size of the buffer.
		before = GC.GetAllocatedBytesForCurrentThread();
		Physics.RaycastAll(Vector3.Zero, Vector3.Forward, 10.0f, small);
		Physics.RaycastAll(Vector3.Zero, Vector3.Forward, 10.0f, large);
		Physics.OverlapSphere(Vector3.Zero, 1.0f, entities);
		Physics.OverlapSphere(Vector3.Zero, 1.0f, many);
		Assert.Equal(0, GC.GetAllocatedBytesForCurrentThread() - before);
		Assert.Same(small[1].Entity, entities[1]);

		// An entity met for the first time costs its Entity once.
		s_Hits = [Hit(34, 1.0f, 7)];
		before = GC.GetAllocatedBytesForCurrentThread();
		Physics.SphereCast(Vector3.Zero, 0.5f, Vector3.Forward, 10.0f, out ShapeCastHit first);
		Assert.True(GC.GetAllocatedBytesForCurrentThread() - before > 0);
		before = GC.GetAllocatedBytesForCurrentThread();
		Physics.SphereCast(Vector3.Zero, 0.5f, Vector3.Forward, 10.0f, out ShapeCastHit again);
		Assert.Equal(0, GC.GetAllocatedBytesForCurrentThread() - before);
		Assert.Same(first.Entity, again.Entity);
		Assert.Equal(new Entity(34), again.Entity);
		ScriptRegistry.DestroyAllInstances();
	}

	[Fact]
	public void TheKeptEntitiesGoWithTheScenesInstancesAndStartOverWhenThereAreTooMany()
	{
		ScriptRegistry.DestroyAllInstances();
		s_Hits = [Hit(41, 1.0f)];
		Physics.SphereCast(Vector3.Zero, 0.5f, Vector3.Forward, 10.0f, out ShapeCastHit before);
		Assert.Equal(1, ScriptRegistry.KeptEntityCount);
		// The scene's instances go when it stops: the next hit makes the entity's Entity again, equal but not the same.
		ScriptRegistry.DestroyAllInstances();
		Assert.Equal(0, ScriptRegistry.KeptEntityCount);
		Physics.SphereCast(Vector3.Zero, 0.5f, Vector3.Forward, 10.0f, out ShapeCastHit after);
		Assert.Equal(before.Entity, after.Entity);
		Assert.NotSame(before.Entity, after.Entity);

		// Past MaxKeptEntities entities the cache starts over, so it never grows with every entity a run makes.
		ScriptRegistry.DestroyAllInstances();
		for (ulong id = 1000; id < 1000 + (ulong)ScriptRegistry.MaxKeptEntities + 10; id++)
		{
			s_Hits = [Hit(id, 1.0f)];
			Physics.SphereCast(Vector3.Zero, 0.5f, Vector3.Forward, 10.0f, out ShapeCastHit hit);
			Assert.Equal(id, hit.Entity!.ID);
			Assert.True(ScriptRegistry.KeptEntityCount <= ScriptRegistry.MaxKeptEntities);
		}
		Assert.Equal(10, ScriptRegistry.KeptEntityCount);
		ScriptRegistry.DestroyAllInstances();
	}
}
