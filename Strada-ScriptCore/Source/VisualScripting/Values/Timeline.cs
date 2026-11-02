using System;
using System.Collections.Generic;

namespace Strada.VisualScripting;

// How a curve key's segment to the next key interpolates (spec-slice3 section 3.8.2).
internal enum TimelineInterpolation : byte
{
	Cubic,
	Linear,
	Constant,
}

// A track of a Timeline setting (spec-slice3 section 3.8.2): a curve of float, vector or colour values, which a data output
// of its type follows, or an event track (Type null), whose keys fire its exec output. Times are in seconds, values per
// component, all in double; a key's tangents are resolved (auto tangents computed) when the track is made. Immutable.
internal sealed class TimelineTrack
{
	internal TimelineTrack(string name, VisualType? type, double[] times, double[] values, double[] inTangents, double[] outTangents,
		TimelineInterpolation[] interpolations)
	{
		Name = name;
		Type = type;
		Components = type == null ? 0 : ComponentsOf(type);
		Times = times;
		Values = values;
		Interpolations = interpolations;
		InTangents = Resolve(inTangents);
		OutTangents = Resolve(outTangents);
	}

	internal string Name { get; }

	// The curve's type (float, Strada.Vector2/3/4, Strada.Color); null for an event track.
	internal VisualType? Type { get; }

	internal bool IsEvent => Type == null;

	// The numbers of a value: 1 for float, 2 to 4 for vectors, 4 for colours (R, G, B, A); 0 for an event track.
	internal int Components { get; }

	internal int KeyCount => Times.Length;

	// Non-decreasing.
	internal double[] Times { get; }

	// Components numbers per key.
	internal double[] Values { get; }

	// The slopes (value units per second) a key's segments enter and leave it with, Components per key.
	internal double[] InTangents { get; }

	internal double[] OutTangents { get; }

	// The interpolation of the segment from each key to the next.
	internal TimelineInterpolation[] Interpolations { get; }

	// The components of a curve type; 0 for a type that is no curve type.
	internal static int ComponentsOf(VisualType type) => type.ClrType switch
	{
		{ } clr when clr == typeof(float) => 1,
		{ } clr when clr == typeof(Vector2) => 2,
		{ } clr when clr == typeof(Vector3) => 3,
		{ } clr when clr == typeof(Vector4) || clr == typeof(Color) => 4,
		_ => 0,
	};

	// The curve's value at a time (spec-slice3 section 3.8.3), as the slot of its output; allocates nothing.
	internal Slot Evaluate(double time)
	{
		Span<double> value = stackalloc double[4];
		Evaluate(time, value);
		return Type!.ClrType switch
		{
			{ } clr when clr == typeof(float) => Slot.FromFloat((float)value[0]),
			{ } clr when clr == typeof(Vector2) => Slot.FromVector2(new Vector2((float)value[0], (float)value[1])),
			{ } clr when clr == typeof(Vector3) => Slot.FromVector3(new Vector3((float)value[0], (float)value[1], (float)value[2])),
			{ } clr when clr == typeof(Vector4) => Slot.FromVector4(new Vector4((float)value[0], (float)value[1], (float)value[2], (float)value[3])),
			_ => Slot.FromColor(new Color((float)value[0], (float)value[1], (float)value[2], (float)value[3])),
		};
	}

	// No key: the type default (0, zero vectors, white). Before the first key its value, from the last key on the last's;
	// between keys the segment's interpolation, per component. Keys at one time make a jump: the segment before them ends at
	// the first, the one after starts at the last.
	internal void Evaluate(double time, Span<double> value)
	{
		int count = KeyCount;
		int components = Components;
		if (count == 0)
		{
			value[..components].Fill(Type!.ClrType == typeof(Color) ? 1.0 : 0.0);
			return;
		}
		if (count == 1 || time < Times[0])
		{
			Values.AsSpan(0, components).CopyTo(value);
			return;
		}
		int key = LastKeyAtOrBefore(time);
		if (key >= count - 1)
		{
			Values.AsSpan((count - 1) * components, components).CopyTo(value);
			return;
		}
		double span = Times[key + 1] - Times[key];
		double s = (time - Times[key]) / span;
		int from = key * components;
		int to = from + components;
		switch (Interpolations[key])
		{
			case TimelineInterpolation.Constant:
				Values.AsSpan(from, components).CopyTo(value);
				break;
			case TimelineInterpolation.Linear:
				for (int component = 0; component < components; component++)
				{
					value[component] = Values[from + component] + ((Values[to + component] - Values[from + component]) * s);
				}
				break;
			default:
				double s2 = s * s;
				double s3 = s2 * s;
				double h00 = (2.0 * s3) - (3.0 * s2) + 1.0;
				double h10 = s3 - (2.0 * s2) + s;
				double h01 = (-2.0 * s3) + (3.0 * s2);
				double h11 = s3 - s2;
				for (int component = 0; component < components; component++)
				{
					value[component] = (h00 * Values[from + component]) + (h10 * span * OutTangents[from + component]) +
						(h01 * Values[to + component]) + (h11 * span * InTangents[to + component]);
				}
				break;
		}
	}

	// The largest key index whose time is at most time (the first key's time is at most time).
	private int LastKeyAtOrBefore(double time)
	{
		int low = 0;
		int high = KeyCount - 1;
		while (low < high)
		{
			int middle = low + ((high - low + 1) / 2);
			if (Times[middle] <= time)
			{
				low = middle;
			}
			else
			{
				high = middle - 1;
			}
		}
		return low;
	}

	// Tangents given as NaN are auto: 0 at the first and last keys; at an interior key the slope from the key before to the
	// key after (Catmull-Rom on uneven times), or 0 when they are at one time.
	private double[] Resolve(double[] tangents)
	{
		int count = KeyCount;
		int components = Components;
		double[] resolved = new double[tangents.Length];
		for (int key = 0; key < count; key++)
		{
			for (int component = 0; component < components; component++)
			{
				int index = (key * components) + component;
				if (!double.IsNaN(tangents[index]))
				{
					resolved[index] = tangents[index];
				}
				else if (key > 0 && key < count - 1 && Times[key + 1] - Times[key - 1] > 0.0)
				{
					resolved[index] = (Values[index + components] - Values[index - components]) / (Times[key + 1] - Times[key - 1]);
				}
			}
		}
		return resolved;
	}
}

// The Timeline setting of a Flow.Timeline node (spec-slice3 section 3.8.2): its length, whether it loops, and its tracks in
// order. The event keys of all event tracks are also merged, ordered by time, then track, then key: the order they fire in
// when the timeline plays forward. Immutable.
internal sealed class TimelineDefinition
{
	internal const double MaxLength = 3600.0;
	internal const int MaxTracks = 32;
	internal const int MaxKeys = 1024;

	internal TimelineDefinition(double length, bool loop, IReadOnlyList<TimelineTrack> tracks)
	{
		Length = length;
		Loop = loop;
		Tracks = tracks;
		List<TimelineTrack> curves = [];
		List<TimelineTrack> events = [];
		List<(double Time, int Track, int Key)> keys = [];
		foreach (TimelineTrack track in tracks)
		{
			if (!track.IsEvent)
			{
				curves.Add(track);
				continue;
			}
			for (int key = 0; key < track.KeyCount; key++)
			{
				keys.Add((track.Times[key], events.Count, key));
			}
			events.Add(track);
		}
		keys.Sort();
		Curves = [.. curves];
		Events = [.. events];
		EventTimes = new double[keys.Count];
		EventTracks = new int[keys.Count];
		for (int index = 0; index < keys.Count; index++)
		{
			EventTimes[index] = keys[index].Time;
			EventTracks[index] = keys[index].Track;
		}
	}

	// A timeline without the setting: 1 second, no loop, no tracks.
	internal static TimelineDefinition Empty { get; } = new(1.0, false, []);

	internal double Length { get; }

	internal bool Loop { get; }

	internal IReadOnlyList<TimelineTrack> Tracks { get; }

	internal TimelineTrack[] Curves { get; }

	internal TimelineTrack[] Events { get; }

	// Every event key's time and its track's index among Events, ordered by time, then track, then key.
	internal double[] EventTimes { get; }

	internal int[] EventTracks { get; }
}
