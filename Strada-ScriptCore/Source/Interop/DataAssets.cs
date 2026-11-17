using System;
using System.Collections.Generic;

namespace Strada.Interop;

// The scripting runtime's copies of data assets (Docs/Architecture.md section 6.4, "Scripts: the managed pattern"). The data
// stays the engine's; a class that reads it per call (a curve evaluated per sample, which must allocate nothing and cross no
// boundary) keeps a copy with the asset's version. The engine says when data assets may have changed (Host.InvalidateDataAssets,
// before scripts run, when its epoch moved): Epoch moves, and each copy asks the engine again on its next use, which copies
// the data only when the asset's version differs. A use otherwise costs a comparison of two numbers. Main thread only.
internal static class DataAssets
{
	// Starts above the 0 of copies never read.
	internal static ulong Epoch { get; private set; } = 1;

	// Data assets may have changed: every copy asks again on its next use.
	internal static void Invalidate()
	{
		Epoch++;
	}

	// The engine binds the scripting runtime again, or lets it go (Host.Initialize and Shutdown): the copies and the data
	// asset types' values are forgotten, and references kept from before read again.
	internal static void Reset()
	{
		Epoch++;
		CurveCache.Clear();
		MixerCache.Clear();
		PhysicsMaterialCache.Clear();
		AssetTypes.Reset();
	}
}

// The runtime's copies of physics material assets, one per asset, shared by every reference (Strada.PhysicsMaterial).
internal static unsafe class PhysicsMaterialCache
{
	private static readonly Dictionary<ulong, Entry> s_Entries = [];

	internal static int EntryCount => s_Entries.Count;

	internal static Entry Find(AssetHandle handle)
	{
		if (!s_Entries.TryGetValue(handle.ID, out Entry? entry))
		{
			entry = new Entry(handle);
			s_Entries.Add(handle.ID, entry);
		}
		return entry;
	}

	// The reference of the material with this handle that raycast hits share, so that a hit allocates nothing once its
	// material has been met; null for 0 (no material).
	internal static PhysicsMaterial? GetShared(ulong handle)
	{
		if (handle == 0)
		{
			return null;
		}
		Entry entry = Find(new AssetHandle(handle));
		return entry.Reference ??= new PhysicsMaterial(entry);
	}

	internal static void Clear()
	{
		s_Entries.Clear();
	}

	// A physics material asset's values as scripts read them, with the version they were read at.
	internal sealed class Entry
	{
		// The data asset epoch at which the engine was last asked; 0: never.
		private ulong m_Epoch;

		internal Entry(AssetHandle handle)
		{
			Handle = handle;
		}

		internal AssetHandle Handle { get; }

		// The asset's version the values were read at; 0 for none (not read, or the read failed).
		internal ulong Version { get; private set; }

		// The reference GetShared hands out, made on first use.
		internal PhysicsMaterial? Reference { get; set; }

		internal PhysicsMaterialValues Values { get; private set; } = PhysicsMaterialValues.Default;

		// The values as of the current epoch: the engine is asked first when data assets may have changed since it last was.
		internal PhysicsMaterialValues GetValues()
		{
			if (m_Epoch != DataAssets.Epoch)
			{
				Refresh();
			}
			return Values;
		}

		private void Refresh()
		{
			m_Epoch = DataAssets.Epoch;
			NativePhysicsMaterial values = default;
			byte* surface = null;
			switch ((PhysicsMaterialReadStatus)InternalCalls.PhysicsMaterial_Read(Handle.ID, Version, &values, &surface))
			{
				case PhysicsMaterialReadStatus.Unchanged:
					break;
				case PhysicsMaterialReadStatus.Read:
					Values = new PhysicsMaterialValues(values, NativeString.FromUtf8(surface, values.SurfaceLength));
					Version = values.Version;
					break;
				default:
					// The engine logged why; the material reads as a new one's until data assets change again.
					Values = PhysicsMaterialValues.Default;
					Version = 0;
					break;
			}
		}
	}
}

// One version of a physics material's values, made when the engine copies them, so that reading a member allocates
// nothing.
internal sealed class PhysicsMaterialValues
{
	// A new material's values (PhysicsMaterial.h's defaults): what a material that cannot be loaded reads as.
	internal static readonly PhysicsMaterialValues Default = new();

	private PhysicsMaterialValues()
	{
		Friction = 0.6f;
		FrictionCombine = PhysicsMaterialCombine.GeometricMean;
		RestitutionCombine = PhysicsMaterialCombine.Maximum;
		Surface = string.Empty;
	}

	internal PhysicsMaterialValues(NativePhysicsMaterial values, string surface)
	{
		Friction = values.Friction;
		Restitution = values.Restitution;
		FrictionCombine = (PhysicsMaterialCombine)values.FrictionCombine;
		RestitutionCombine = (PhysicsMaterialCombine)values.RestitutionCombine;
		RollingResistance = values.RollingResistance;
		Surface = surface;
		ImpactSound = values.ImpactSound != 0 ? new AudioClip(new AssetHandle(values.ImpactSound)) : null;
		ImpactParticles = values.ImpactParticles != 0 ? new Prefab(new AssetHandle(values.ImpactParticles)) : null;
	}

	internal float Friction { get; }
	internal float Restitution { get; }
	internal PhysicsMaterialCombine FrictionCombine { get; }
	internal PhysicsMaterialCombine RestitutionCombine { get; }
	internal float RollingResistance { get; }
	internal string Surface { get; }
	internal AudioClip? ImpactSound { get; }
	internal Prefab? ImpactParticles { get; }
}

// The runtime's copies of mixer assets, one per asset, shared by every reference (Strada.Mixer).
internal static unsafe class MixerCache
{
	private static readonly Dictionary<ulong, Entry> s_Entries = [];
	// A mixer that loads has at most this many buses (the engine's MaxMixerBuses).
	private const int MaxBuses = 32;
	// Room for the buses a read copies, grown when an asset has more.
	private static NativeMixerBus[] s_Buses = new NativeMixerBus[MaxBuses];

	internal static int EntryCount => s_Entries.Count;

	internal static Entry Find(AssetHandle handle)
	{
		if (!s_Entries.TryGetValue(handle.ID, out Entry? entry))
		{
			entry = new Entry(handle);
			s_Entries.Add(handle.ID, entry);
		}
		return entry;
	}

	internal static void Clear()
	{
		s_Entries.Clear();
		s_Buses = new NativeMixerBus[MaxBuses];
	}

	// A mixer asset's buses as scripts read them, with the version they were read at.
	internal sealed class Entry
	{
		// The data asset epoch at which the engine was last asked; 0: never.
		private ulong m_Epoch;

		internal Entry(AssetHandle handle)
		{
			Handle = handle;
		}

		internal AssetHandle Handle { get; }

		// The asset's version the buses were read at; 0 for none (not read, or the read failed).
		internal ulong Version { get; private set; }

		internal MixerValues Values { get; private set; } = MixerValues.Default;

		// The buses as of the current epoch: the engine is asked first when data assets may have changed since it last was.
		internal MixerValues GetValues()
		{
			if (m_Epoch != DataAssets.Epoch)
			{
				Refresh();
			}
			return Values;
		}

		private void Refresh()
		{
			m_Epoch = DataAssets.Epoch;
			NativeMixerHeader header = default;
			MixerReadStatus status = Read(&header);
			if (status == MixerReadStatus.TooSmall)
			{
				s_Buses = new NativeMixerBus[header.BusCount];
				status = Read(&header);
			}
			switch (status)
			{
				case MixerReadStatus.Unchanged:
					break;
				case MixerReadStatus.Read:
					Values = new MixerValues(s_Buses.AsSpan(0, header.BusCount));
					Version = header.Version;
					break;
				default:
					// The engine logged why; the mixer reads as a new one until data assets change again.
					Values = MixerValues.Default;
					Version = 0;
					break;
			}
		}

		private MixerReadStatus Read(NativeMixerHeader* header)
		{
			fixed (NativeMixerBus* buses = s_Buses)
			{
				return (MixerReadStatus)InternalCalls.Mixer_Read(Handle.ID, Version, header, buses, s_Buses.Length);
			}
		}
	}
}

// One version of a mixer's buses, made when the engine copies them, so that reading a member allocates nothing.
internal sealed class MixerValues
{
	// A new mixer's buses (Mixer.h's default): what a mixer that cannot be loaded reads as.
	internal static readonly MixerValues Default = new(new[] { new MixerBusValues("Master", string.Empty, 0.0f, false) });

	private MixerValues(MixerBusValues[] buses)
	{
		Buses = buses;
	}

	internal unsafe MixerValues(ReadOnlySpan<NativeMixerBus> buses)
	{
		Buses = new MixerBusValues[buses.Length];
		for (int index = 0; index < buses.Length; index++)
		{
			NativeMixerBus bus = buses[index];
			Buses[index] = new MixerBusValues(NativeString.FromUtf8(bus.Name, bus.NameLength), NativeString.FromUtf8(bus.Parent, bus.ParentLength),
				bus.VolumeDb, bus.Muted != 0);
		}
	}

	internal MixerBusValues[] Buses { get; }

	// The bus of that name, its ASCII letters matched in any case as the engine matches them (MakeMixerBusKey), the empty
	// name being Master's; null for none.
	internal MixerBusValues? Find(string name)
	{
		ReadOnlySpan<char> wanted = name.Length > 0 ? name : "Master";
		foreach (MixerBusValues bus in Buses)
		{
			if (IsSameBusName(bus.Name, wanted))
			{
				return bus;
			}
		}
		return null;
	}

	private static bool IsSameBusName(ReadOnlySpan<char> first, ReadOnlySpan<char> second)
	{
		if (first.Length != second.Length)
		{
			return false;
		}
		for (int index = 0; index < first.Length; index++)
		{
			if (ToAsciiLower(first[index]) != ToAsciiLower(second[index]))
			{
				return false;
			}
		}
		return true;
	}

	private static char ToAsciiLower(char character) => character is >= 'A' and <= 'Z' ? (char)(character + ('a' - 'A')) : character;
}

// One bus of a mixer as scripts read it.
internal sealed record MixerBusValues(string Name, string Parent, float VolumeDb, bool Muted);

// The runtime's copies of curve assets, one per asset, shared by every reference (Strada.Curve).
internal static unsafe class CurveCache
{
	private static readonly Dictionary<ulong, Entry> s_Entries = [];
	// Room for the keys a read copies, grown when an asset has more (at most the engine's MaxCurveKeys).
	private static NativeCurveKey[] s_Keys = new NativeCurveKey[64];

	internal static int EntryCount => s_Entries.Count;

	internal static Entry Find(AssetHandle handle)
	{
		if (!s_Entries.TryGetValue(handle.ID, out Entry? entry))
		{
			entry = new Entry(handle);
			s_Entries.Add(handle.ID, entry);
		}
		return entry;
	}

	internal static void Clear()
	{
		s_Entries.Clear();
	}

	// A curve asset's data as the runtime evaluates it, with the version it was read at.
	internal sealed class Entry
	{
		// The data asset epoch at which the engine was last asked; 0: never.
		private ulong m_Epoch;

		internal Entry(AssetHandle handle)
		{
			Handle = handle;
		}

		internal AssetHandle Handle { get; }

		// The asset's version the definition was read at; 0 for none (not read, or the read failed).
		internal ulong Version { get; private set; }

		// The data as of the current epoch: the engine is asked first when data assets may have changed since it last was.
		internal CurveDefinition GetDefinition()
		{
			if (m_Epoch != DataAssets.Epoch)
			{
				Refresh();
			}
			return Definition;
		}

		internal CurveDefinition Definition { get; private set; } = CurveDefinition.Empty;

		private void Refresh()
		{
			m_Epoch = DataAssets.Epoch;
			NativeCurveHeader header = default;
			CurveReadStatus status = Read(&header);
			if (status == CurveReadStatus.TooSmall)
			{
				s_Keys = new NativeCurveKey[header.KeyCount];
				status = Read(&header);
			}
			switch (status)
			{
				case CurveReadStatus.Unchanged:
					break;
				case CurveReadStatus.Read:
					Definition = ToDefinition(header, s_Keys.AsSpan(0, header.KeyCount));
					Version = header.Version;
					break;
				default:
					// The engine logged why; the curve is 0 until data assets change again.
					Definition = CurveDefinition.Empty;
					Version = 0;
					break;
			}
		}

		private CurveReadStatus Read(NativeCurveHeader* header)
		{
			fixed (NativeCurveKey* keys = s_Keys)
			{
				return (CurveReadStatus)InternalCalls.Curve_Read(Handle.ID, Version, header, keys, s_Keys.Length);
			}
		}

		private static CurveDefinition ToDefinition(NativeCurveHeader header, ReadOnlySpan<NativeCurveKey> keys)
		{
			int count = keys.Length;
			double[] times = new double[count];
			double[] values = new double[count];
			double[] inSlopes = new double[count];
			double[] outSlopes = new double[count];
			CurveInterpolation[] interpolations = new CurveInterpolation[count];
			for (int index = 0; index < count; index++)
			{
				NativeCurveKey key = keys[index];
				times[index] = key.Time;
				values[index] = key.Value;
				inSlopes[index] = key.In;
				outSlopes[index] = key.Out;
				interpolations[index] = (CurveInterpolation)key.Interpolation;
			}
			return new CurveDefinition(times, values, inSlopes, outSlopes, interpolations, (CurveExtrapolation)header.PreExtrapolation,
				(CurveExtrapolation)header.PostExtrapolation);
		}
	}
}
