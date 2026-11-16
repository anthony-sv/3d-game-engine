using System;
using System.Runtime.InteropServices;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The scripting API's view of the contact being handled (Strada.Collision, Strada.ContactPoint) without an engine:
// Collision_Read is served by a fake of the engine's binding. The engine's side is StradaTests' (Scene/SceneScriptingTests.cpp
// and SceneVisualScriptingTests.cpp: handlers read the contact events the scene dispatches). The fake binding is the
// process's, so the tests run alone.
[Collection(ProcessStaticsCollection.Name)]
public sealed unsafe class CollisionTests : IDisposable
{
	// The contact the fake engine reports as being handled; null for none.
	private static NativeCollision? s_Collision;
	private static NativeContactPoint[] s_Points = [];
	private static int s_Reads;

	private readonly delegate* unmanaged<NativeCollision*, NativeContactPoint*, byte> m_PreviousRead;

	public CollisionTests()
	{
		m_PreviousRead = InternalCalls.Collision_Read;
		InternalCalls.Collision_Read = &Read;
		s_Collision = null;
		s_Points = [];
		s_Reads = 0;
		DataAssets.Reset();
	}

	public void Dispose()
	{
		Collision.SetContinued(null);
		InternalCalls.Collision_Read = m_PreviousRead;
		s_Collision = null;
		s_Points = [];
		DataAssets.Reset();
	}

	// Collision_Read as ScriptBindingsRuntime.cpp answers: zeros and 0 outside a handler, the points only when asked.
	[UnmanagedCallersOnly]
	private static byte Read(NativeCollision* collision, NativeContactPoint* points)
	{
		s_Reads++;
		*collision = default;
		if (s_Collision is not { } handled)
		{
			return 0;
		}
		*collision = handled;
		if (points != null)
		{
			for (int index = 0; index < (int)handled.PointCount; index++)
			{
				points[index] = s_Points[index];
			}
		}
		return 1;
	}

	private static void Handle(ulong entity, ulong other, ulong step, params NativeContactPoint[] points)
	{
		s_Collision = new NativeCollision { Entity = entity, Other = other, Step = step, PointCount = (uint)points.Length };
		s_Points = points;
	}

	[Fact]
	public void OutsideAHandlerTheCollisionIsEmpty()
	{
		Assert.Null(Collision.Entity);
		Assert.Null(Collision.Other);
		Assert.Equal(0ul, Collision.Step);
		Assert.Empty(Collision.Points);
		Assert.False(Collision.TryGetStrongestPoint(out ContactPoint point));
		Assert.Equal(default, point);
		Assert.Equal(5, s_Reads);
	}

	[Fact]
	public void InsideAHandlerItGivesTheEntitiesTheStepAndEveryPoint()
	{
		Handle(5, 9, 42,
			new NativeContactPoint
			{
				Point = new Vector3(1.0f, 0.0f, 2.0f),
				Normal = Vector3.Up,
				RelativeVelocity = new Vector3(0.0f, -3.0f, 0.0f),
				Impulse = 4.5f,
				Material = 11,
				OtherMaterial = 12,
			},
			new NativeContactPoint { Point = new Vector3(-1.0f, 0.0f, 2.0f), Normal = Vector3.Up, Impulse = 1.5f, OtherMaterial = 12 });

		Assert.Equal(new Entity(5), Collision.Entity);
		Assert.Equal(new Entity(9), Collision.Other);
		Assert.Equal(42ul, Collision.Step);
		ContactPoint[] points = Collision.Points;
		Assert.Equal(2, points.Length);
		Assert.Equal(new Vector3(1.0f, 0.0f, 2.0f), points[0].Point);
		Assert.Equal(Vector3.Up, points[0].Normal);
		Assert.Equal(new Vector3(0.0f, -3.0f, 0.0f), points[0].RelativeVelocity);
		Assert.Equal(4.5f, points[0].Impulse);
		Assert.Equal(new AssetHandle(11), points[0].Material!.Handle);
		Assert.Equal(new AssetHandle(12), points[0].OtherMaterial!.Handle);
		Assert.Null(points[1].Material);
		// Points on one material share its reference, as raycast hits do.
		Assert.Same(points[0].OtherMaterial, points[1].OtherMaterial);
		Assert.Equal("Point (-1, 0, 2), Normal (0, 1, 0), Impulse 1.5", points[1].ToString());
		// Each read is a new array: changing one changes nothing the next read gives.
		points[0].Impulse = 0.0f;
		Assert.Equal(4.5f, Collision.Points[0].Impulse);
		Assert.NotSame(Collision.Points, Collision.Points);

		Assert.True(Collision.TryGetStrongestPoint(out ContactPoint strongest));
		Assert.Equal(4.5f, strongest.Impulse);
	}

	// A contact that ends, or one sleeping bodies resume on waking, has no points.
	[Fact]
	public void AContactWithoutPointsGivesAnEmptyArray()
	{
		Handle(5, 9, 7);
		Assert.Equal(new Entity(9), Collision.Other);
		Assert.Equal(7ul, Collision.Step);
		Assert.Empty(Collision.Points);
		Assert.False(Collision.TryGetStrongestPoint(out _));
	}

	// The engine environment gives a graph's Event.CollisionEnter the strongest point and its other side's surface name ("" for
	// none, also when the material cannot be read).
	[Fact]
	public void TheEngineEnvironmentGivesTheStrongestPointAndTheOtherSurface()
	{
		EngineEnvironment environment = new();
		Assert.False(environment.TryGetContactPoint(out ContactPoint none, out string noSurface));
		Assert.Equal(default, none);
		Assert.Equal(string.Empty, noSurface);

		Handle(5, 9, 3, new NativeContactPoint { Point = new Vector3(0.0f, 1.0f, 0.0f), Normal = Vector3.Up, Impulse = 2.0f });
		Assert.True(environment.TryGetContactPoint(out ContactPoint point, out string surface));
		Assert.Equal(2.0f, point.Impulse);
		Assert.Equal(string.Empty, surface);
	}

	// What a graph execution the debugger stopped inside a contact flow continues with: a copy of the contact being handled
	// (none outside a handler), which reads give in place of the engine's, without asking it, until it is put back.
	[Fact]
	public void ASavedContactIsReadInPlaceOfTheEnginesWhileItContinues()
	{
		Assert.Null(Collision.SaveHandled());
		Handle(5, 9, 42,
			new NativeContactPoint { Point = new Vector3(1.0f, 0.0f, 0.0f), Normal = Vector3.Up, Impulse = 3.0f, OtherMaterial = 12 },
			new NativeContactPoint { Point = new Vector3(-1.0f, 0.0f, 0.0f), Normal = Vector3.Up, Impulse = 1.0f });
		EngineEnvironment environment = new();
		HandledContact? saved = environment.SaveContact();
		Assert.NotNull(saved);
		Assert.Equal(2u, saved.Collision.PointCount);
		Assert.Equal(2, saved.Points.Length);

		// The handler returned.
		s_Collision = null;
		s_Points = [];
		int reads = s_Reads;
		Assert.Null(environment.RestoreContact(saved));
		Assert.Same(saved, Collision.SaveHandled());
		Assert.Equal(new Entity(5), Collision.Entity);
		Assert.Equal(new Entity(9), Collision.Other);
		Assert.Equal(42ul, Collision.Step);
		ContactPoint[] points = Collision.Points;
		Assert.Equal(2, points.Length);
		Assert.Equal(3.0f, points[0].Impulse);
		Assert.Equal(1.0f, points[1].Impulse);
		Assert.Equal(new AssetHandle(12), points[0].OtherMaterial!.Handle);
		Assert.True(Collision.TryGetStrongestPoint(out ContactPoint strongest));
		Assert.Equal(3.0f, strongest.Impulse);
		Assert.Equal(reads, s_Reads);

		// Put back: the engine's again, which handles none now.
		Assert.Same(saved, environment.RestoreContact(null));
		Assert.Null(Collision.Entity);
		Assert.Empty(Collision.Points);
		Assert.Equal(reads + 2, s_Reads);
	}
}
