using System.Runtime.InteropServices;

namespace Strada.Interop;

// The shape a cast sweeps or an overlap tests (ScriptBindingsRuntime.cpp declares the same layout, without implicit padding):
// PhysicsQueryShape as the engine takes it from scripts.
[StructLayout(LayoutKind.Sequential)]
internal struct NativeQueryShape
{
	// PhysicsQueryShapeType: the values of the constants below.
	public int Type;
	// Spheres and capsules.
	public float Radius;
	// Capsules: half the length of the cylinder between the caps, along the shape's local Y axis.
	public float HalfHeight;
	// Boxes.
	public Vector3 HalfExtents;

	internal const int Sphere = 0;
	internal const int Box = 1;
	internal const int Capsule = 2;

	internal static NativeQueryShape MakeSphere(float radius) => new() { Type = Sphere, Radius = radius };

	internal static NativeQueryShape MakeBox(Vector3 halfExtents) => new() { Type = Box, HalfExtents = halfExtents };

	internal static NativeQueryShape MakeCapsule(float radius, float halfHeight) => new() { Type = Capsule, Radius = radius, HalfHeight = halfHeight };
}
