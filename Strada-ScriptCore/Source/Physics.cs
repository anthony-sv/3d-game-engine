using System;
using System.Buffers;
using System.Globalization;
using System.Runtime.InteropServices;
using Strada.Interop;
using Strada.VisualScripting;

namespace Strada;

/// <summary>A half-line from an origin along a direction.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct Ray
{
	/// <summary>Where the ray starts.</summary>
	public Vector3 Origin;
	/// <summary>The direction it goes (unit length for rays from the engine).</summary>
	public Vector3 Direction;

	/// <summary>Creates a ray.</summary>
	public Ray(Vector3 origin, Vector3 direction)
	{
		Origin = origin;
		Direction = direction;
	}

	/// <summary>The point <paramref name="distance"/> along the ray (in units of the direction's length).</summary>
	[PureNode]
	public readonly Vector3 GetPoint(float distance) => Origin + (Direction * distance);
}

/// <summary>What a raycast hit.</summary>
public readonly struct RaycastHit
{
	internal RaycastHit(Entity entity, Vector3 point, Vector3 normal, float distance, PhysicsMaterial? material)
	{
		Entity = entity;
		Point = point;
		Normal = normal;
		Distance = distance;
		Material = material;
	}

	/// <summary>The entity of the collider hit (its script instance when it has one).</summary>
	public Entity Entity { get; }

	/// <summary>The world-space point hit.</summary>
	public Vector3 Point { get; }

	/// <summary>The surface normal at the point.</summary>
	public Vector3 Normal { get; }

	/// <summary>Distance from the ray's origin to the point.</summary>
	public float Distance { get; }

	/// <summary>The physics material of the surface hit: the collider's Material, or for a triangle of a mesh collider its
	/// submesh's entry in the collider's Materials (the collider's Material when that entry is empty); null when the
	/// surface has none and its collider's own Friction and Restitution apply. Hits on one material share one
	/// reference.</summary>
	public PhysicsMaterial? Material { get; }
}

/// <summary>What a sphere, box or capsule cast touched first (<see cref="Physics.SphereCast"/>, <see cref="Physics.BoxCast"/>,
/// <see cref="Physics.CapsuleCast"/>).</summary>
public struct ShapeCastHit
{
	/// <summary>The entity of the collider touched (its script instance when it has one); null in a hit made by hand that names
	/// none.</summary>
	public Entity? Entity;

	/// <summary>The world-space point where the shape touches the collider, on the collider's surface; for a collider the shape
	/// overlapped where the cast started, the deepest point of the overlap.</summary>
	public Vector3 Point;

	/// <summary>Unit length, out of the collider towards the shape: the way that pushes the shape off it. On a face it is the
	/// face's normal; on an edge or a corner it points from there towards the shape.</summary>
	public Vector3 Normal;

	/// <summary>How far the shape travelled along the cast's direction until it touched, in meters: its center is then at the
	/// origin plus the unit direction times this. 0 for a collider the shape overlapped where the cast started.</summary>
	public float Distance;

	/// <summary>The physics material of the surface touched, as <see cref="RaycastHit.Material"/>: null when the surface has
	/// none and its collider's own Friction and Restitution apply.</summary>
	public PhysicsMaterial? Material;

	/// <summary>The entity, point, normal and distance, as logs and the visual script debugger show them.</summary>
	public override readonly string ToString() => string.Create(CultureInfo.InvariantCulture,
		$"Entity {(Entity is null ? "None" : Entity.ID)}, Point {Point}, Normal {Normal}, Distance {Distance}");
}

/// <summary>Queries and settings of the running scene's physics. Queries read the world as its last physics step left it: in
/// <see cref="Script.OnFixedUpdate"/> that is where bodies are; in <see cref="Script.OnUpdate"/> a body drawn between its last
/// two steps is found up to a step ahead of where it is drawn, and an entity moved since the last step (by a script, or with
/// its parent) is found where it was until the next step takes its body there. A body made or changed since the last step is
/// found where its entity is. A query sees the bodies and characters on the layers of its layer mask (bit n is layer n), each
/// entity at most once, and trigger colliders only when it includes them. Directions need not be unit length. Values that
/// are not finite, and sizes below 0 or above a million meters, are script errors and find nothing; a zero direction or a
/// distance that is not positive finds nothing.</summary>
public static unsafe class Physics
{
	// The most hits and entities the many-hit queries gather on the stack before they copy them out; larger buffers borrow
	// pooled ones, so no query allocates a buffer of its own.
	private const int StackHits = 32;
	private const int StackEntities = 128;

	/// <summary>Every physics layer.</summary>
	public const uint AllLayers = 0xFFFFFFFF;

	/// <summary>The acceleration of gravity in meters per second squared.</summary>
	public static Vector3 Gravity
	{
		get
		{
			Vector3 gravity;
			InternalCalls.Physics_GetGravity(&gravity);
			return gravity;
		}
		set => InternalCalls.Physics_SetGravity(&value);
	}

	/// <summary>The closest solid collider along a ray, on the layers of <paramref name="layerMask"/> (bit n is layer n).
	/// Triggers and colliders containing the origin are ignored.</summary>
	public static bool Raycast(Vector3 origin, Vector3 direction, float maxDistance, out RaycastHit hit, uint layerMask = AllLayers)
	{
		NativeRaycastHit result;
		if (InternalCalls.Physics_Raycast(&origin, &direction, maxDistance, layerMask, &result) == 0)
		{
			hit = default;
			return false;
		}
		hit = new RaycastHit(ScriptRegistry.GetEntity(result.Entity), result.Point, result.Normal, result.Distance,
			PhysicsMaterialCache.GetShared(result.Material));
		return true;
	}

	/// <summary>The closest solid collider along a ray (see <see cref="Raycast(Vector3, Vector3, float, out RaycastHit, uint)"/>).</summary>
	public static bool Raycast(Ray ray, float maxDistance, out RaycastHit hit, uint layerMask = AllLayers) =>
		Raycast(ray.Origin, ray.Direction, maxDistance, out hit, layerMask);

	/// <summary>Every entity along a ray, each at its nearest hit, nearest first (at equal distances by entity ID), written into
	/// <paramref name="hits"/>; returns how many were written. A buffer smaller than the hits keeps the nearest. As with
	/// <see cref="Raycast(Vector3, Vector3, float, out RaycastHit, uint)"/>, a ray starting inside a convex collider passes out of
	/// it, and triangles are hit from their front only; triggers count when <paramref name="includeTriggers"/> is true. Allocates
	/// nothing but the hits' <see cref="RaycastHit.Entity"/> of entities without a script, as a raycast does. Graphs have no
	/// node of it: they hold arrays as values, which a node may not write into.</summary>
	public static int RaycastAll(Vector3 origin, Vector3 direction, float maxDistance, Span<RaycastHit> hits, uint layerMask = AllLayers,
		bool includeTriggers = false)
	{
		NativeRaycastHit[]? rented = hits.Length > StackHits ? ArrayPool<NativeRaycastHit>.Shared.Rent(hits.Length) : null;
		Span<NativeRaycastHit> found = rented != null ? rented.AsSpan(0, hits.Length) : stackalloc NativeRaycastHit[hits.Length];
		try
		{
			int count;
			fixed (NativeRaycastHit* buffer = found)
			{
				count = InternalCalls.Physics_RaycastAll(&origin, &direction, maxDistance, layerMask, includeTriggers ? (byte)1 : (byte)0, buffer,
					found.Length);
			}
			count = Math.Clamp(count, 0, hits.Length);
			for (int index = 0; index < count; index++)
			{
				hits[index] = ToRaycastHit(found[index]);
			}
			return count;
		}
		finally
		{
			if (rented != null)
			{
				ArrayPool<NativeRaycastHit>.Shared.Return(rented);
			}
		}
	}

	/// <summary>The first entity a sphere of <paramref name="radius"/> touches as it moves from <paramref name="origin"/> (its
	/// center) along <paramref name="direction"/> for at most <paramref name="maxDistance"/>: of those it touches, the nearest
	/// (at equal distances by entity ID). A collider the sphere overlaps where it starts is touched at distance 0, whichever
	/// way it moves, and so is the caster's own collider when the cast starts inside it: leave its layer out of
	/// <paramref name="layerMask"/>, or start outside it. Triangles are touched from their front only, as by rays.</summary>
	public static bool SphereCast(Vector3 origin, float radius, Vector3 direction, float maxDistance, out ShapeCastHit hit,
		uint layerMask = AllLayers, bool includeTriggers = false)
	{
		NativeQueryShape shape = NativeQueryShape.MakeSphere(radius);
		return Cast(&shape, origin, Quaternion.Identity, direction, maxDistance, out hit, layerMask, includeTriggers);
	}

	/// <summary>The first entity a box of <paramref name="halfExtents"/>, turned by <paramref name="rotation"/>, touches as it
	/// moves from <paramref name="origin"/> (its center) along <paramref name="direction"/> for at most
	/// <paramref name="maxDistance"/> without turning (see <see cref="SphereCast"/>). The box keeps its sharp edges and
	/// corners.</summary>
	public static bool BoxCast(Vector3 origin, Vector3 halfExtents, Quaternion rotation, Vector3 direction, float maxDistance,
		out ShapeCastHit hit, uint layerMask = AllLayers, bool includeTriggers = false)
	{
		NativeQueryShape shape = NativeQueryShape.MakeBox(halfExtents);
		return Cast(&shape, origin, rotation, direction, maxDistance, out hit, layerMask, includeTriggers);
	}

	/// <summary>The first entity a capsule touches as it moves from <paramref name="origin"/> (its center) along
	/// <paramref name="direction"/> for at most <paramref name="maxDistance"/> without turning (see <see cref="SphereCast"/>).
	/// The capsule lies along its local Y axis, turned by <paramref name="rotation"/>, as a capsule collider: caps of
	/// <paramref name="radius"/> at either end of a cylinder whose half length is <paramref name="halfHeight"/>.</summary>
	public static bool CapsuleCast(Vector3 origin, float radius, float halfHeight, Quaternion rotation, Vector3 direction, float maxDistance,
		out ShapeCastHit hit, uint layerMask = AllLayers, bool includeTriggers = false)
	{
		NativeQueryShape shape = NativeQueryShape.MakeCapsule(radius, halfHeight);
		return Cast(&shape, origin, rotation, direction, maxDistance, out hit, layerMask, includeTriggers);
	}

	/// <summary>Every entity with a collider that a sphere of <paramref name="radius"/> at <paramref name="center"/> overlaps,
	/// by entity ID from the smallest, written into <paramref name="entities"/>; returns how many were written. A buffer smaller
	/// than the overlaps keeps the smallest IDs. A triangle counts from either side; a shape wholly inside a closed triangle mesh
	/// does not meet its surface and does not overlap it. Allocates nothing but the <see cref="Entity"/> of entities without a
	/// script. Graphs have no node of it: they hold arrays as values, which a node may not write into.</summary>
	public static int OverlapSphere(Vector3 center, float radius, Span<Entity> entities, uint layerMask = AllLayers, bool includeTriggers = false)
	{
		NativeQueryShape shape = NativeQueryShape.MakeSphere(radius);
		return Overlap(&shape, center, Quaternion.Identity, entities, layerMask, includeTriggers);
	}

	/// <summary>Every entity with a collider that a box of <paramref name="halfExtents"/> at <paramref name="center"/>, turned by
	/// <paramref name="rotation"/>, overlaps (see <see cref="OverlapSphere"/>). Graphs have no node of it.</summary>
	public static int OverlapBox(Vector3 center, Vector3 halfExtents, Quaternion rotation, Span<Entity> entities, uint layerMask = AllLayers,
		bool includeTriggers = false)
	{
		NativeQueryShape shape = NativeQueryShape.MakeBox(halfExtents);
		return Overlap(&shape, center, rotation, entities, layerMask, includeTriggers);
	}

	/// <summary>Every entity with a collider that a capsule at <paramref name="center"/> overlaps (see
	/// <see cref="OverlapSphere"/>; the capsule as in <see cref="CapsuleCast"/>). Graphs have no node of it.</summary>
	public static int OverlapCapsule(Vector3 center, float radius, float halfHeight, Quaternion rotation, Span<Entity> entities,
		uint layerMask = AllLayers, bool includeTriggers = false)
	{
		NativeQueryShape shape = NativeQueryShape.MakeCapsule(radius, halfHeight);
		return Overlap(&shape, center, rotation, entities, layerMask, includeTriggers);
	}

	private static bool Cast(NativeQueryShape* shape, Vector3 origin, Quaternion rotation, Vector3 direction, float maxDistance,
		out ShapeCastHit hit, uint layerMask, bool includeTriggers)
	{
		NativeRaycastHit result;
		if (InternalCalls.Physics_ShapeCast(shape, &origin, &rotation, &direction, maxDistance, layerMask, includeTriggers ? (byte)1 : (byte)0,
			&result) == 0)
		{
			hit = default;
			return false;
		}
		hit = new ShapeCastHit
		{
			Entity = ScriptRegistry.GetEntity(result.Entity),
			Point = result.Point,
			Normal = result.Normal,
			Distance = result.Distance,
			Material = PhysicsMaterialCache.GetShared(result.Material),
		};
		return true;
	}

	private static int Overlap(NativeQueryShape* shape, Vector3 center, Quaternion rotation, Span<Entity> entities, uint layerMask,
		bool includeTriggers)
	{
		ulong[]? rented = entities.Length > StackEntities ? ArrayPool<ulong>.Shared.Rent(entities.Length) : null;
		Span<ulong> found = rented != null ? rented.AsSpan(0, entities.Length) : stackalloc ulong[entities.Length];
		try
		{
			int count;
			fixed (ulong* buffer = found)
			{
				count = InternalCalls.Physics_Overlap(shape, &center, &rotation, layerMask, includeTriggers ? (byte)1 : (byte)0, buffer,
					found.Length);
			}
			count = Math.Clamp(count, 0, entities.Length);
			for (int index = 0; index < count; index++)
			{
				entities[index] = ScriptRegistry.GetEntity(found[index]);
			}
			return count;
		}
		finally
		{
			if (rented != null)
			{
				ArrayPool<ulong>.Shared.Return(rented);
			}
		}
	}

	private static RaycastHit ToRaycastHit(NativeRaycastHit native) => new(ScriptRegistry.GetEntity(native.Entity), native.Point, native.Normal,
		native.Distance, PhysicsMaterialCache.GetShared(native.Material));
}
