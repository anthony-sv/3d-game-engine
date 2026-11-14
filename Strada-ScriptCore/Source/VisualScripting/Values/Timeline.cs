using System;
using System.Collections.Generic;

namespace Strada.VisualScripting;

// A track of a Timeline setting (spec-slice3 section 3.8.2): a curve of float, vector or colour values, which a data output
// of its type follows, or an event track (Type null), whose keys fire its exec output. Times are in seconds, values per
// component, all in double; a key's tangents are resolved (auto tangents computed) when the track is made. Curves evaluate
// through the runtime's curve evaluator (CurveMath), the engine's curve semantics. Immutable.
internal sealed class TimelineTrack
{
	internal TimelineTrack(string name, VisualType? type, double[] times, double[] values, double[] inTangents, double[] outTangents,
		CurveInterpolation[] interpolations)
	{
		Name = name;
		Type = type;
		Components = type == null ? 0 : ComponentsOf(type);
		Times = times;
		Values = values;
		Interpolations = interpolations;
		InTangents = CurveMath.ResolveSlopes(times, values, Components, inTangents);
		OutTangents = CurveMath.ResolveSlopes(times, values, Components, outTangents);
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
	internal CurveInterpolation[] Interpolations { get; }

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

	// No key: the type default (0, zero vectors, white); otherwise the keys' value (CurveMath.Evaluate): before the first key its
	// value, from the last key on the last's; between keys the segment's interpolation, per component. Keys at one time make a
	// jump: the segment before them ends at the first, the one after starts at the last.
	internal void Evaluate(double time, Span<double> value)
	{
		int components = Components;
		if (KeyCount == 0)
		{
			value[..components].Fill(Type!.ClrType == typeof(Color) ? 1.0 : 0.0);
			return;
		}
		CurveMath.Evaluate(Times, Values, InTangents, OutTangents, Interpolations, time, value[..components]);
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
