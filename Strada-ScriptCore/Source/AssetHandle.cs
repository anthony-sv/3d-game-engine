using System;
using System.Globalization;

namespace Strada;

/// <summary>Identifies an asset of the project; the default value refers to no asset.</summary>
public readonly struct AssetHandle : IEquatable<AssetHandle>
{
	/// <summary>Wraps an asset identifier.</summary>
	public AssetHandle(ulong id)
	{
		ID = id;
	}

	/// <summary>The asset's identifier (0 for no asset).</summary>
	public ulong ID { get; }

	/// <summary>Whether the handle refers to an asset (the asset itself may still be missing).</summary>
	public bool IsValid => ID != 0;

	/// <summary>The handle that refers to no asset.</summary>
	public static AssetHandle Invalid => default;

	/// <summary>Identifier equality.</summary>
	public static bool operator ==(AssetHandle a, AssetHandle b) => a.ID == b.ID;

	/// <summary>Identifier inequality.</summary>
	public static bool operator !=(AssetHandle a, AssetHandle b) => a.ID != b.ID;

	/// <summary>Identifier equality.</summary>
	public bool Equals(AssetHandle other) => ID == other.ID;

	/// <inheritdoc/>
	public override bool Equals(object? obj) => obj is AssetHandle other && Equals(other);

	/// <inheritdoc/>
	public override int GetHashCode() => ID.GetHashCode();

	/// <summary>The identifier in decimal.</summary>
	public override string ToString() => ID.ToString(CultureInfo.InvariantCulture);
}
