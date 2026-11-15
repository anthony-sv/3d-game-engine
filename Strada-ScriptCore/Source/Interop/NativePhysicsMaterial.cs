using System.Runtime.InteropServices;

namespace Strada.Interop;

// What PhysicsMaterial_Read copies of a physics material asset, its surface's text apart (ScriptBindingsAssets.cpp declares
// the same layout).
[StructLayout(LayoutKind.Sequential)]
internal struct NativePhysicsMaterial
{
	// The asset's version, never 0.
	public ulong Version;
	public ulong ImpactSound;
	public ulong ImpactParticles;
	public float Friction;
	public float Restitution;
	public float RollingResistance;
	public int FrictionCombine;
	public int RestitutionCombine;
	// The length in bytes of the surface's UTF-8 text.
	public int SurfaceLength;
}

// PhysicsMaterial_Read's answers.
internal enum PhysicsMaterialReadStatus
{
	// The engine logged why; the values and the surface are as they were.
	Failed = 0,
	// The held version is the asset's: only the values' Version is written.
	Unchanged = 1,
	// The values and the surface's text are written.
	Read = 2,
}
