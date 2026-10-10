using System.Runtime.InteropServices;
using Strada.Interop;

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
	public readonly Vector3 GetPoint(float distance) => Origin + (Direction * distance);
}

/// <summary>What a raycast hit.</summary>
public readonly struct RaycastHit
{
	internal RaycastHit(Entity entity, Vector3 point, Vector3 normal, float distance)
	{
		Entity = entity;
		Point = point;
		Normal = normal;
		Distance = distance;
	}

	/// <summary>The entity of the collider hit (its script instance when it has one).</summary>
	public Entity Entity { get; }

	/// <summary>The world-space point hit.</summary>
	public Vector3 Point { get; }

	/// <summary>The surface normal at the point.</summary>
	public Vector3 Normal { get; }

	/// <summary>Distance from the ray's origin to the point.</summary>
	public float Distance { get; }
}

/// <summary>Queries and settings of the running scene's physics.</summary>
public static unsafe class Physics
{
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
		hit = new RaycastHit(ScriptRegistry.GetEntity(result.Entity), result.Point, result.Normal, result.Distance);
		return true;
	}

	/// <summary>The closest solid collider along a ray (see <see cref="Raycast(Vector3, Vector3, float, out RaycastHit, uint)"/>).</summary>
	public static bool Raycast(Ray ray, float maxDistance, out RaycastHit hit, uint layerMask = AllLayers) =>
		Raycast(ray.Origin, ray.Direction, maxDistance, out hit, layerMask);
}
