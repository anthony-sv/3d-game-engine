using System.Runtime.InteropServices;

namespace Strada.Interop;

// What Curve_Read tells of a curve asset besides its keys (ScriptBindingsAssets.cpp declares the same layout).
[StructLayout(LayoutKind.Sequential)]
internal struct NativeCurveHeader
{
	// The asset's version, never 0.
	public ulong Version;
	public int KeyCount;
	public int PreExtrapolation;
	public int PostExtrapolation;
	public int Reserved;
}

// A curve key as Curve_Read copies it: unset slopes are NaN (automatic).
[StructLayout(LayoutKind.Sequential)]
internal struct NativeCurveKey
{
	public float Time;
	public float Value;
	public float In;
	public float Out;
	public int Interpolation;
}

// Curve_Read's answers.
internal enum CurveReadStatus
{
	// The engine logged why; the header and the keys are as they were.
	Failed = 0,
	// The held version is the asset's: only the header's Version is written.
	Unchanged = 1,
	// The header and every key are written.
	Read = 2,
	// The header is written but the keys do not fit: read again with room for KeyCount keys.
	TooSmall = 3,
}
