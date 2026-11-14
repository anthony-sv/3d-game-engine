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
		AssetTypes.Reset();
	}
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
