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
	/// alone, so inaccurate when more than two collide at once; 0 for a trigger and for a contact without a dynamic
	/// body.</summary>
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
/// while a graph's contact event runs, until its first wait. Physics reports contacts once per frame, after the frame's
/// physics steps, so <see cref="Step"/> says which step a contact happened in. Outside a handler <see cref="Entity"/> and
/// <see cref="Other"/> are null, <see cref="Step"/> is 0 and <see cref="Points"/> is empty.</summary>
public static unsafe class Collision
{
	/// <summary>The entity whose handler runs (its script instance when it has one): the script's own entity.</summary>
	public static Entity? Entity
	{
		get
		{
			NativeCollision collision;
			return InternalCalls.Collision_Read(&collision, null) != 0 ? ScriptRegistry.GetEntity(collision.Entity) : null;
		}
	}

	/// <summary>The other entity of the contact (its script instance when it has one): the handler's argument.</summary>
	public static Entity? Other
	{
		get
		{
			NativeCollision collision;
			return InternalCalls.Collision_Read(&collision, null) != 0 ? ScriptRegistry.GetEntity(collision.Other) : null;
		}
	}

	/// <summary>The physics step the contact began or ended in, counting the scene's physics steps from 1 (a contact that
	/// ends because an entity was destroyed between steps has the number of the step before).</summary>
	public static ulong Step
	{
		get
		{
			NativeCollision collision;
			return InternalCalls.Collision_Read(&collision, null) != 0 ? collision.Step : 0;
		}
	}

	/// <summary>Where the colliders touch as the contact begins, seen from <see cref="Entity"/>: up to four points, the
	/// strongest first (by <see cref="ContactPoint.Impulse"/>, then by position). Empty when a contact ends, for a contact
	/// sleeping bodies resume on waking (which raises no event), and outside a handler. A new array on each
	/// read.</summary>
	public static ContactPoint[] Points
	{
		get
		{
			NativeCollision collision;
			NativeContactPoint* points = stackalloc NativeContactPoint[NativeContactPoint.MaxCount];
			if (InternalCalls.Collision_Read(&collision, points) == 0 || collision.PointCount == 0)
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
		if (InternalCalls.Collision_Read(&collision, points) == 0 || collision.PointCount == 0)
		{
			point = default;
			return false;
		}
		point = FromNative(points[0]);
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
