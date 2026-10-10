using System;

namespace Strada.Interop;

// The engine's AssetType (Asset.h).
internal enum NativeAssetType
{
	None = 0,
	Scene,
	Prefab,
	Mesh,
	Material,
	Texture,
	Environment,
	AudioClip,
	Font,
}

// The engine's type of each asset class, and references of a class made from handles.
internal static class AssetTypes
{
	internal static NativeAssetType GetNativeType(Type type) => type switch
	{
		_ when type == typeof(Prefab) => NativeAssetType.Prefab,
		_ when type == typeof(Mesh) => NativeAssetType.Mesh,
		_ when type == typeof(Material) => NativeAssetType.Material,
		_ when type == typeof(Texture) => NativeAssetType.Texture,
		_ when type == typeof(EnvironmentMap) => NativeAssetType.Environment,
		_ when type == typeof(AudioClip) => NativeAssetType.AudioClip,
		_ when type == typeof(Font) => NativeAssetType.Font,
		_ => NativeAssetType.None,
	};

	internal static Asset Create(NativeAssetType type, AssetHandle handle) => type switch
	{
		NativeAssetType.Prefab => new Prefab(handle),
		NativeAssetType.Mesh => new Mesh(handle),
		NativeAssetType.Material => new Material(handle),
		NativeAssetType.Texture => new Texture(handle),
		NativeAssetType.Environment => new EnvironmentMap(handle),
		NativeAssetType.AudioClip => new AudioClip(handle),
		NativeAssetType.Font => new Font(handle),
		_ => throw new ArgumentOutOfRangeException(nameof(type), type, "not an asset class"),
	};
}
