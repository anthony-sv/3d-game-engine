using System;

namespace Strada;

/// <summary>Base class of references to the project's assets. References compare equal when their handles do.</summary>
public abstract class Asset : IEquatable<Asset>
{
	// Only the engine's asset types derive from Asset.
	private protected Asset(AssetHandle handle)
	{
		Handle = handle;
	}

	/// <summary>The asset's handle.</summary>
	public AssetHandle Handle { get; }

	/// <summary>Whether both refer to the same asset (or both are null).</summary>
	public static bool operator ==(Asset? a, Asset? b) => a is null ? b is null : a.Equals(b);

	/// <summary>Whether they refer to different assets.</summary>
	public static bool operator !=(Asset? a, Asset? b) => !(a == b);

	/// <summary>Whether <paramref name="other"/> refers to the same asset (of the same type).</summary>
	public bool Equals(Asset? other) => other is not null && other.GetType() == GetType() && other.Handle == Handle;

	/// <inheritdoc/>
	public override bool Equals(object? obj) => obj is Asset other && Equals(other);

	/// <inheritdoc/>
	public override int GetHashCode() => Handle.GetHashCode();

	/// <summary>"Type(handle)".</summary>
	public override string ToString() => $"{GetType().Name}({Handle})";
}

/// <summary>A mesh asset (imported model or built-in shape).</summary>
public sealed class Mesh : Asset
{
	internal Mesh(AssetHandle handle)
		: base(handle)
	{
	}
}

/// <summary>A material asset: the surface of meshes.</summary>
public sealed class Material : Asset
{
	internal Material(AssetHandle handle)
		: base(handle)
	{
	}
}

/// <summary>A texture asset.</summary>
public sealed class Texture : Asset
{
	internal Texture(AssetHandle handle)
		: base(handle)
	{
	}
}

/// <summary>An audio clip asset (WAV, FLAC, MP3 or Ogg Vorbis).</summary>
public sealed class AudioClip : Asset
{
	internal AudioClip(AssetHandle handle)
		: base(handle)
	{
	}
}

/// <summary>A font asset for text.</summary>
public sealed class Font : Asset
{
	internal Font(AssetHandle handle)
		: base(handle)
	{
	}
}

/// <summary>An HDR environment map: the sky and image-based lighting of a sky light.</summary>
public sealed class EnvironmentMap : Asset
{
	internal EnvironmentMap(AssetHandle handle)
		: base(handle)
	{
	}
}

/// <summary>A prefab asset: entities saved for instantiation.</summary>
public sealed class Prefab : Asset
{
	internal Prefab(AssetHandle handle)
		: base(handle)
	{
	}
}
