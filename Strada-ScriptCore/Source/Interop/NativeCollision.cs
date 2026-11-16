using System.Runtime.InteropServices;

namespace Strada.Interop;

// The contact event a handler runs for, as Collision_Read fills it (ScriptBindingsRuntime.cpp declares the same layout,
// without implicit padding).
[StructLayout(LayoutKind.Sequential)]
internal struct NativeCollision
{
	// The entity whose handler runs, and the other entity of the contact.
	public ulong Entity;
	public ulong Other;
	// The physics world's step the contact began or ended in.
	public ulong Step;
	// Of Points, at most NativeContactPoint.MaxCount.
	public uint PointCount;
	public uint Reserved;
}

// A contact point seen from the entity whose handler runs (ScriptBindingsRuntime.cpp declares the same layout, without
// implicit padding).
[StructLayout(LayoutKind.Sequential)]
internal struct NativeContactPoint
{
	// The most points an event carries: a buffer Collision_Read fills holds this many.
	public const int MaxCount = 4;

	public Vector3 Point;
	public Vector3 Normal;
	public Vector3 RelativeVelocity;
	public float Impulse;
	// The handles of the physics materials of this side's surface and the other's; 0 for none.
	public ulong Material;
	public ulong OtherMaterial;
}
