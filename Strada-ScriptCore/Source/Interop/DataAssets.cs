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
		InputActionsCache.Clear();
		PhysicsMaterialCache.Clear();
		AssetTypes.Reset();
	}
}

// The runtime's copies of input actions assets' action names, one per asset, shared by every reference (Strada.InputActions).
internal static unsafe class InputActionsCache
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

	internal static void Clear()
	{
		s_Entries.Clear();
	}

	// An input actions asset's action names as scripts read them, with the version they were read at.
	internal sealed class Entry
	{
		// The data asset epoch at which the engine was last asked; 0: never.
		private ulong m_Epoch;

		internal Entry(AssetHandle handle)
		{
			Handle = handle;
		}

		internal AssetHandle Handle { get; }

		// The asset's version the names were read at; 0 for none (not read, or the read failed).
		internal ulong Version { get; private set; }

		internal string[] Names { get; private set; } = [];

		// The names as of the current epoch: the engine is asked first when data assets may have changed since it last was.
		internal string[] GetNames()
		{
			if (m_Epoch != DataAssets.Epoch)
			{
				Refresh();
			}
			return Names;
		}

		private void Refresh()
		{
			m_Epoch = DataAssets.Epoch;
			ulong version = 0;
			int count = 0;
			switch ((PhysicsMaterialReadStatus)InternalCalls.InputActions_Read(Handle.ID, Version, &version, &count))
			{
				case PhysicsMaterialReadStatus.Unchanged:
					break;
				case PhysicsMaterialReadStatus.Read:
					string[] names = new string[System.Math.Max(count, 0)];
					for (int index = 0; index < names.Length; index++)
					{
						int length = 0;
						byte* name = InternalCalls.InputActions_GetActionName(Handle.ID, index, &length);
						names[index] = NativeString.FromUtf8(name, length);
					}
					Names = names;
					Version = version;
					break;
				default:
					// The engine logged why; the asset has no actions until data assets change again.
					Names = [];
					Version = 0;
					break;
			}
		}
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
