using System.Runtime.InteropServices;

namespace Strada.Interop;

// The engine's material parameters (ScriptBindingsAssets.cpp): 120 bytes without implicit padding.
[StructLayout(LayoutKind.Sequential)]
internal struct NativeMaterialValues
{
	public Color BaseColor;
	public Vector3 EmissiveColor;
	public float Metallic;
	public float Roughness;
	public float EmissiveIntensity;
	public float NormalStrength;
	public float OcclusionStrength;
	public Vector2 UVTiling;
	public Vector2 UVOffset;
	public ulong BaseColorTexture;
	public ulong NormalTexture;
	public ulong MetallicRoughnessTexture;
	public ulong OcclusionTexture;
	public ulong EmissiveTexture;
	public int AlphaMode;
	public float AlphaCutoff;
	public uint DoubleSided;
	public uint Reserved;
}
