using System.Runtime.InteropServices;

namespace Strada.Interop;

// What Mixer_Read tells of a mixer asset besides its buses (ScriptBindingsAssets.cpp declares the same layout).
[StructLayout(LayoutKind.Sequential)]
internal struct NativeMixerHeader
{
	// The asset's version, never 0.
	public ulong Version;
	public int BusCount;
	public int Reserved;
}

// A bus as Mixer_Read copies it, its effects apart: the texts point into the asset's data, copied at once.
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeMixerBus
{
	public byte* Name;
	public byte* Parent;
	// The lengths in bytes of the UTF-8 texts.
	public int NameLength;
	public int ParentLength;
	public float VolumeDb;
	public byte Muted;
	public byte Reserved0;
	public byte Reserved1;
	public byte Reserved2;
}

// Mixer_Read's answers.
internal enum MixerReadStatus
{
	// The engine logged why; the header and the buses are as they were.
	Failed = 0,
	// The held version is the asset's: only the header's Version is written.
	Unchanged = 1,
	// The header and every bus are written.
	Read = 2,
	// The header is written but the buses do not fit: read again with room for BusCount buses.
	TooSmall = 3,
}
