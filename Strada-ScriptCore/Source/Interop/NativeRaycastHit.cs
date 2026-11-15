using System.Runtime.InteropServices;

namespace Strada.Interop;

// The engine's raycast result (ScriptBindingsRuntime.cpp declares the same layout, without implicit padding).
[StructLayout(LayoutKind.Sequential)]
internal struct NativeRaycastHit
{
	public ulong Entity;
	public Vector3 Point;
	public Vector3 Normal;
	public float Distance;
	public uint Reserved;
	// The handle of the physics material of the surface hit; 0 for none.
	public ulong Material;
}
