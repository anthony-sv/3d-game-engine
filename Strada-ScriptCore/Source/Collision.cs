using System;
using System.Globalization;
using Strada.Interop;

namespace Strada;

/// <summary>Where and how two colliders touch as their contact begins, seen from one of them: the entity whose handler
/// reads it (<see cref="Collision.Entity"/>), against <see cref="Collision.Other"/>.</summary>
public struct ContactPoint
{
	/// <summary>The world-space point where they touch: halfway between the two surfaces (a contact found just before
	/// they touch has it in the gap between them).</summary>
	public Vector3 Point;

	/// <summary>The contact's normal, unit length, from the other collider towards this one: the way this entity is
	/// pushed.</summary>
	public Vector3 Normal;

	/// <summary>The impulse along the normal at this point (newton seconds) that stops the two bodies' approach and, with
	/// their restitution, bounces them apart: how hard the impact is. Estimated by the physics engine from the two bodies
	/// alone, for the points that share this point's normal, as if they alone stopped the bodies: when a contact begins on
	/// surfaces at an angle at once (a ball landing in a groove), each surface's points are not shared out with the others',
	/// so they read too high when the surfaces' normals are less than 90 degrees apart and too low when more. Inaccurate as
	/// well when more than two bodies collide at once; 0 for a trigger and for a contact without a dynamic body.</summary>
	public float Impulse;

	/// <summary>This entity's velocity at the point minus the other's (meters per second) as the contact began: they
	/// approach when it points against <see cref="Normal"/>.</summary>
	public Vector3 RelativeVelocity;

	/// <summary>The physics material of this side's surface at the point (its collider's Material, or for a triangle of a
	/// mesh collider its submesh's); null when the collider's own Friction and Restitution apply.</summary>
	public PhysicsMaterial? Material;

	/// <summary>The physics material of the other side's surface at the point (see <see cref="Material"/>); its
	/// <see cref="PhysicsMaterial.Surface"/> tells what was hit.</summary>
	public PhysicsMaterial? OtherMaterial;

	/// <summary>The point, the normal and the impulse, as logs and the visual script debugger show them.</summary>
	public override readonly string ToString() =>
		string.Create(CultureInfo.InvariantCulture, $"Point {Point}, Normal {Normal}, Impulse {Impulse}");
}

/// <summary>The contact event being handled: readable inside a script's <see cref="Script.OnCollisionEnter"/>,
/// <see cref="Script.OnCollisionExit"/>, <see cref="Script.OnTriggerEnter"/> and <see cref="Script.OnTriggerExit"/>, and
/// while a graph's contact event runs, until its first wait (a flow the visual script debugger stopped continues with the
/// contact it read). Physics reports contacts once per frame, after the frame's
/// physics steps, so <see cref="Step"/> says which step a contact happened in. Outside a handler <see cref="Entity"/> and
/// <see cref="Other"/> are null, <see cref="Step"/> is 0 and <see cref="Points"/> is empty.</summary>
public static unsafe class Collision
{
	// The contact that reads give in place of the engine's while a stopped graph execution continues (SetContinued); null
	// for the engine's.
	private static HandledContact? s_Continued;

	/// <summary>The entity whose handler runs (its script instance when it has one): the script's own entity.</summary>
	public static Entity? Entity
	{
		get
		{
			NativeCollision collision;
			return Read(&collision, null) ? ScriptRegistry.GetEntity(collision.Entity) : null;
		}
	}

	/// <summary>The other entity of the contact (its script instance when it has one): the handler's argument.</summary>
	public static Entity? Other
	{
		get
		{
			NativeCollision collision;
			return Read(&collision, null) ? ScriptRegistry.GetEntity(collision.Other) : null;
		}
	}

	/// <summary>The physics step the contact began or ended in, counting the scene's physics steps from 1 (a contact that
	/// ends because an entity was destroyed between steps has the number of the step before).</summary>
	public static ulong Step
	{
		get
		{
			NativeCollision collision;
			return Read(&collision, null) ? collision.Step : 0;
		}
	}

	/// <summary>Where the colliders touch as the contact begins, seen from <see cref="Entity"/>: up to four points, the
	/// strongest first (by <see cref="ContactPoint.Impulse"/>, then by position, normal, relative velocity and
	/// materials). Empty when a contact ends, for a contact
	/// sleeping bodies resume on waking (which raises no event), and outside a handler. A new array on each
	/// read.</summary>
	public static ContactPoint[] Points
	{
		get
		{
			NativeCollision collision;
			NativeContactPoint* points = stackalloc NativeContactPoint[NativeContactPoint.MaxCount];
			if (!Read(&collision, points) || collision.PointCount == 0)
			{
				return [];
			}
			ContactPoint[] result = new ContactPoint[Math.Min((int)collision.PointCount, NativeContactPoint.MaxCount)];
			for (int index = 0; index < result.Length; index++)
			{
				result[index] = FromNative(points[index]);
			}
			return result;
		}
	}

	// The strongest point of the contact being handled, without an array: what a graph's Event.CollisionEnter gives. False
	// outside a handler and for a contact without points.
	internal static bool TryGetStrongestPoint(out ContactPoint point)
	{
		NativeCollision collision;
		NativeContactPoint* points = stackalloc NativeContactPoint[NativeContactPoint.MaxCount];
		if (!Read(&collision, points) || collision.PointCount == 0)
		{
			point = default;
			return false;
		}
		point = FromNative(points[0]);
		return true;
	}

	// A copy of the contact being handled (the continued one while a stopped execution continues); null outside a handler.
	// What a graph execution saves when the debugger stops it, so that the stop changes nothing its flow reads.
	internal static HandledContact? SaveHandled()
	{
		if (s_Continued != null)
		{
			return s_Continued;
		}
		NativeCollision collision;
		NativeContactPoint* points = stackalloc NativeContactPoint[NativeContactPoint.MaxCount];
		if (InternalCalls.Collision_Read(&collision, points) == 0)
		{
			return null;
		}
		int count = Math.Min((int)collision.PointCount, NativeContactPoint.MaxCount);
		collision.PointCount = (uint)count;
		return new HandledContact(collision, new ReadOnlySpan<NativeContactPoint>(points, count).ToArray());
	}

	// Makes reads give the contact in place of the engine's (null: the engine's again) while a stopped execution continues;
	// answers the one it replaces, which the caller puts back.
	internal static HandledContact? SetContinued(HandledContact? contact)
	{
		HandledContact? previous = s_Continued;
		s_Continued = contact;
		return previous;
	}

	// Collision_Read, or the continued contact: false outside a handler. points, when given, has room for
	// NativeContactPoint.MaxCount.
	private static bool Read(NativeCollision* collision, NativeContactPoint* points)
	{
		if (s_Continued is not { } continued)
		{
			return InternalCalls.Collision_Read(collision, points) != 0;
		}
		*collision = continued.Collision;
		if (points != null)
		{
			continued.Points.AsSpan().CopyTo(new Span<NativeContactPoint>(points, NativeContactPoint.MaxCount));
		}
		return true;
	}

	private static ContactPoint FromNative(NativeContactPoint native) => new()
	{
		Point = native.Point,
		Normal = native.Normal,
		Impulse = native.Impulse,
		RelativeVelocity = native.RelativeVelocity,
		Material = PhysicsMaterialCache.GetShared(native.Material),
		OtherMaterial = PhysicsMaterialCache.GetShared(native.OtherMaterial),
	};
}

// A copy of a contact event being handled, as Collision_Read gives it: what a graph execution the debugger stopped inside a
// contact flow continues with.
internal sealed class HandledContact(NativeCollision collision, NativeContactPoint[] points)
{
	// Its PointCount is the length of Points.
	internal NativeCollision Collision { get; } = collision;

	internal NativeContactPoint[] Points { get; } = points;
}
