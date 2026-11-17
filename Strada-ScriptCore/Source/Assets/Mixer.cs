using System;
using Strada.Interop;
using Strada.VisualScripting;

namespace Strada;

/// <summary>A mixer asset (<c>.smixer</c>): the buses sounds play on, in a tree under Master, each with its volume, its mute
/// and its effects. The project names the mixer its games play through; a sound source names its bus
/// (<see cref="AudioSourceComponent.Bus"/>). Edit it in the editor; scripts read its buses, for example to list the volumes
/// a settings menu offers, and set the game's own values of a bus through <see cref="Audio"/>, which never change the asset.
/// Bus names match ignoring letter case, and the empty name is Master's. An edit made while the game plays shows on the next
/// frame. A mixer that cannot be loaded reads as a new one, Master alone (logged).</summary>
public sealed class Mixer : Asset
{
	// The runtime's copy of the asset's buses, shared by every reference to the asset; found on first use.
	private MixerCache.Entry? m_Entry;

	internal Mixer(AssetHandle handle)
		: base(handle)
	{
	}

	/// <summary>How many buses the mixer has, Master included (at least 1, at most 32).</summary>
	public int BusCount => GetValues().Buses.Length;

	/// <summary>The name of a bus by its place in the mixer's list (Master is the one without a parent); empty for an index
	/// outside the list.</summary>
	/// <param name="index">From 0 to <see cref="BusCount"/> - 1.</param>
	[PureNode]
	public string GetBusName(int index)
	{
		MixerBusValues[] buses = GetValues().Buses;
		return index >= 0 && index < buses.Length ? buses[index].Name : string.Empty;
	}

	/// <summary>Whether the mixer has a bus of that name.</summary>
	/// <param name="bus">The bus's name, in any letter case; empty for Master.</param>
	[PureNode]
	public bool HasBus(string bus) => Find(bus) != null;

	/// <summary>The name of the bus a bus feeds; empty for Master, and for a bus the mixer lacks.</summary>
	/// <param name="bus">The bus's name, in any letter case; empty for Master.</param>
	[PureNode]
	public string GetBusParent(string bus) => Find(bus)?.Parent ?? string.Empty;

	/// <summary>The mixer's own volume of a bus, in decibels (-80 is silence); 0 for a bus the mixer lacks. The game's volume
	/// comes after it (<see cref="Audio.GetBusVolume"/>).</summary>
	/// <param name="bus">The bus's name, in any letter case; empty for Master.</param>
	[PureNode]
	public float GetBusVolume(string bus) => Find(bus)?.VolumeDb ?? 0.0f;

	/// <summary>Whether the mixer mutes a bus; false for a bus the mixer lacks. The game's mute is
	/// <see cref="Audio.IsBusMuted"/>.</summary>
	/// <param name="bus">The bus's name, in any letter case; empty for Master.</param>
	[PureNode]
	public bool IsBusMuted(string bus) => Find(bus)?.Muted ?? false;

	private MixerBusValues? Find(string bus)
	{
		ArgumentNullException.ThrowIfNull(bus);
		return GetValues().Find(bus);
	}

	private MixerValues GetValues()
	{
		m_Entry ??= MixerCache.Find(Handle);
		return m_Entry.GetValues();
	}
}
