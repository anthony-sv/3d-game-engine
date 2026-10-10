using System.Runtime.InteropServices;

namespace Strada.Interop;

// The engine's raycast result (ScriptBindingsRuntime.cpp).
[StructLayout(LayoutKind.Sequential)]
internal struct NativeRaycastHit
{
	public ulong Entity;
	public Vector3 Point;
	public Vector3 Normal;
	public float Distance;
}
