using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text.Json;

namespace Strada.VisualScripting;

// A track of a Timeline setting that has a pin: its name, and its type (null for an event track's exec output).
internal sealed record TimelineTrackPin(string Name, VisualType? Type);

// Reads the Timeline setting of a Flow.Timeline node (spec-slice3 section 3.8.2) and reports what is wrong with it, each
// problem with its path in the setting ("Timeline.Tracks[1].Keys[3].Time: ..."). Every number reads as a float, as every
// reader of documents reads it, and is then computed with in double.
internal sealed class TimelineReader
{
	// The pins of the node every timeline has: no track may take one of their names.
	internal static readonly string[] FixedPins =
		["Play", "PlayFromStart", "Reverse", "ReverseFromEnd", "Stop", "SetTime", "NewTime", "Rate", "Update", "Finished", "Time", "Direction", "Playing"];

	private const string TrackTypes = "float, Strada.Vector2, Strada.Vector3, Strada.Vector4, Strada.Color or exec";

	private readonly NodeLibrary m_Library;
	private readonly Action<DiagnosticCode, string> m_Report;
	private bool m_Valid = true;

	private TimelineReader(NodeLibrary library, Action<DiagnosticCode, string> report)
	{
		m_Library = library;
		m_Report = report;
	}

	// The timeline the setting describes; null when it has a problem (reported). pins receives the tracks with a valid
	// name and type, in order, even when other parts of the setting are wrong, so links to them keep their pins.
	internal static TimelineDefinition? Read(JsonElement setting, NodeLibrary library, Action<DiagnosticCode, string> report, List<TimelineTrackPin> pins)
	{
		TimelineReader reader = new(library, report);
		TimelineDefinition? timeline = reader.ReadTimeline(setting, pins);
		return reader.m_Valid ? timeline : null;
	}

	// A number as messages write it: the float's shortest text.
	internal static string Format(double value) => ((float)value).ToString(CultureInfo.InvariantCulture);

	private TimelineDefinition? ReadTimeline(JsonElement setting, List<TimelineTrackPin> pins)
	{
		if (setting.ValueKind != JsonValueKind.Object)
		{
			Fail($"Timeline: expected an object with Length, Loop and Tracks, not {JsonText.Excerpt(setting)}");
			return null;
		}
		double length = 1.0;
		bool loop = false;
		JsonElement? tracks = null;
		foreach (JsonProperty member in setting.EnumerateObject())
		{
			string key = JsonText.TryGetName(member, out string? name) ? name : "?";
			switch (key)
			{
				case "Length":
					if (!ReadNumber(member.Value, out length) || !(length > 0.0) || length > TimelineDefinition.MaxLength)
					{
						Fail(FormattableString.Invariant($"Timeline.Length: expected a number greater than 0 and at most {TimelineDefinition.MaxLength}"));
						length = TimelineDefinition.MaxLength;
					}
					break;
				case "Loop":
					if (member.Value.ValueKind is JsonValueKind.True or JsonValueKind.False)
					{
						loop = member.Value.GetBoolean();
					}
					else
					{
						Fail($"Timeline.Loop: expected true or false, not {JsonText.Excerpt(member.Value)}");
					}
					break;
				case "Tracks":
					tracks = member.Value;
					break;
				default:
					Fail($"Timeline: a timeline has only Length, Loop and Tracks, not {JsonText.Quote(key)}");
					break;
			}
		}
		List<TimelineTrack> read = [];
		if (tracks is { } list)
		{
			if (list.ValueKind != JsonValueKind.Array)
			{
				Fail($"Timeline.Tracks: expected an array of tracks, not {JsonText.Excerpt(list)}");
			}
			else if (list.GetArrayLength() > TimelineDefinition.MaxTracks)
			{
				Fail(FormattableString.Invariant($"Timeline.Tracks: a timeline has at most {TimelineDefinition.MaxTracks} tracks"));
			}
			else
			{
				HashSet<string> names = new(StringComparer.Ordinal);
				int index = 0;
				foreach (JsonElement track in list.EnumerateArray())
				{
					string path = FormattableString.Invariant($"Timeline.Tracks[{index++}]");
					if (ReadTrack(track, path, length, names, pins) is { } valid)
					{
						read.Add(valid);
					}
				}
			}
		}
		return new TimelineDefinition(length, loop, read);
	}

	private TimelineTrack? ReadTrack(JsonElement track, string path, double length, HashSet<string> names, List<TimelineTrackPin> pins)
	{
		if (track.ValueKind != JsonValueKind.Object)
		{
			Fail($"{path}: expected an object with Name, Type and Keys, not {JsonText.Excerpt(track)}");
			return null;
		}
		JsonElement? name = null;
		JsonElement? type = null;
		JsonElement? keys = null;
		foreach (JsonProperty member in track.EnumerateObject())
		{
			string key = JsonText.TryGetName(member, out string? memberName) ? memberName : "?";
			switch (key)
			{
				case "Name":
					name = member.Value;
					break;
				case "Type":
					type = member.Value;
					break;
				case "Keys":
					keys = member.Value;
					break;
				default:
					Fail($"{path}: a track has only Name, Type and Keys, not {JsonText.Quote(key)}");
					break;
			}
		}
		string? trackName = ReadTrackName(name, path, names);
		bool isEvent = false;
		VisualType? trackType = null;
		if (type is { } typeElement && JsonText.TryGetString(typeElement, out string? id))
		{
			isEvent = id == TypeIds.Exec;
			trackType = isEvent ? null : m_Library.FindType(id) is { } found && TimelineTrack.ComponentsOf(found) > 0 ? found : null;
		}
		if (!isEvent && trackType == null)
		{
			Fail($"{path}.Type: expected {TrackTypes}");
			return null;
		}
		if (trackName == null)
		{
			return null;
		}
		pins.Add(new TimelineTrackPin(trackName, trackType));
		return ReadKeys(keys, path, length, trackName, trackType);
	}

	// A track's name: the name rule of spec section 1.3, unique among the tracks and none of the node's own pins'.
	private string? ReadTrackName(JsonElement? name, string path, HashSet<string> names)
	{
		if (name is not { } element || !JsonText.TryGetString(element, out string? text))
		{
			Fail(name is { } other ? $"{path}.Name: expected a name, not {JsonText.Excerpt(other)}" : $"{path}: a track needs a Name");
			return null;
		}
		if (Names.Check(text) is { } problem)
		{
			Fail($"{path}.Name {JsonText.Quote(text)}: {problem}", DiagnosticCode.InvalidName);
			return null;
		}
		if (Names.IsReserved(text))
		{
			Fail($"{path}.Name {JsonText.Quote(text)}: {text} is a reserved pin name ({Names.ReservedList})", DiagnosticCode.ReservedName);
			return null;
		}
		if (Array.IndexOf(FixedPins, text) >= 0)
		{
			Fail($"{path}.Name: {text} is a pin of the timeline: name the track otherwise");
			return null;
		}
		if (!names.Add(text))
		{
			Fail($"{path}.Name: a track named {text} comes earlier");
			return null;
		}
		return text;
	}

	private TimelineTrack? ReadKeys(JsonElement? keys, string path, double length, string name, VisualType? type)
	{
		int components = type == null ? 0 : TimelineTrack.ComponentsOf(type);
		List<double> times = [];
		List<double> values = [];
		List<double> inTangents = [];
		List<double> outTangents = [];
		List<TimelineInterpolation> interpolations = [];
		bool valid = true;
		if (keys is { } list)
		{
			if (list.ValueKind != JsonValueKind.Array)
			{
				Fail($"{path}.Keys: expected an array of keys, not {JsonText.Excerpt(list)}");
				return null;
			}
			if (list.GetArrayLength() > TimelineDefinition.MaxKeys)
			{
				Fail(FormattableString.Invariant($"{path}.Keys: a track has at most {TimelineDefinition.MaxKeys} keys"));
				return null;
			}
			int index = 0;
			foreach (JsonElement key in list.EnumerateArray())
			{
				string keyPath = FormattableString.Invariant($"{path}.Keys[{index++}]");
				if (!ReadKey(key, keyPath, length, type, components, times, values, inTangents, outTangents, interpolations))
				{
					valid = false;
				}
			}
		}
		return valid ? new TimelineTrack(name, type, [.. times], [.. values], [.. inTangents], [.. outTangents], [.. interpolations]) : null;
	}

	private bool ReadKey(JsonElement key, string path, double length, VisualType? type, int components, List<double> times, List<double> values,
		List<double> inTangents, List<double> outTangents, List<TimelineInterpolation> interpolations)
	{
		if (key.ValueKind != JsonValueKind.Object)
		{
			Fail($"{path}: expected an object with {(type == null ? "Time" : "Time and Value")}, not {JsonText.Excerpt(key)}");
			return false;
		}
		bool valid = true;
		double? time = null;
		JsonElement? value = null;
		TimelineInterpolation interpolation = TimelineInterpolation.Cubic;
		JsonElement? inTangent = null;
		JsonElement? outTangent = null;
		foreach (JsonProperty member in key.EnumerateObject())
		{
			string name = JsonText.TryGetName(member, out string? memberName) ? memberName : "?";
			if (type == null && name != "Time")
			{
				Fail($"{path}: an event key has only Time, not {JsonText.Quote(name)}");
				valid = false;
				continue;
			}
			switch (name)
			{
				case "Time":
					if (ReadNumber(member.Value, out double seconds) && seconds >= 0.0 && seconds <= length)
					{
						time = seconds;
					}
					else
					{
						Fail($"{path}.Time: expected a number from 0 to {Format(length)}, not {JsonText.Excerpt(member.Value)}");
						valid = false;
					}
					break;
				case "Value":
					value = member.Value;
					break;
				case "Interpolation":
					int choice = JsonText.TryGetString(member.Value, out string? text) ? Array.IndexOf(Enum.GetNames<TimelineInterpolation>(), text) : -1;
					if (choice >= 0)
					{
						interpolation = (TimelineInterpolation)choice;
					}
					else
					{
						Fail($"{path}.Interpolation: expected Cubic, Linear or Constant, not {JsonText.Excerpt(member.Value)}");
						valid = false;
					}
					break;
				case "In":
					inTangent = member.Value;
					break;
				case "Out":
					outTangent = member.Value;
					break;
				default:
					Fail($"{path}: a key has only Time, Value, Interpolation, In and Out, not {JsonText.Quote(name)}");
					valid = false;
					break;
			}
		}
		if (time is not { } keyTime)
		{
			if (valid)
			{
				Fail($"{path}: a key needs a Time");
			}
			return false;
		}
		if (times.Count > 0 && keyTime < times[^1])
		{
			Fail($"{path}.Time: keys are in time order: {Format(keyTime)} comes after {Format(times[^1])}");
			valid = false;
		}
		if (type == null)
		{
			times.Add(keyTime);
			return valid;
		}
		Span<double> decoded = stackalloc double[4];
		if (value is not { } literal)
		{
			Fail($"{path}: a key of a {type.Id} track needs a Value");
			return false;
		}
		DecodedLiteral read = LiteralCodec.Decode(type, literal);
		if (!read.IsValid || read.Value == null)
		{
			Fail($"{path}.Value: {read.Message}");
			return false;
		}
		ComponentsOf(read.Value, decoded);
		valid &= ReadTangent(inTangent, $"{path}.In", components, inTangents);
		valid &= ReadTangent(outTangent, $"{path}.Out", components, outTangents);
		times.Add(keyTime);
		for (int component = 0; component < components; component++)
		{
			values.Add(decoded[component]);
		}
		interpolations.Add(interpolation);
		return valid;
	}

	// A tangent: a number for a float track, an array of one number per component otherwise; absent: auto (NaN).
	private bool ReadTangent(JsonElement? tangent, string path, int components, List<double> tangents)
	{
		if (tangent is not { } element)
		{
			for (int component = 0; component < components; component++)
			{
				tangents.Add(double.NaN);
			}
			return true;
		}
		if (components == 1 && ReadNumber(element, out double slope))
		{
			tangents.Add(slope);
			return true;
		}
		if (components > 1 && element.ValueKind == JsonValueKind.Array && element.GetArrayLength() == components)
		{
			int start = tangents.Count;
			foreach (JsonElement number in element.EnumerateArray())
			{
				if (!ReadNumber(number, out double componentSlope))
				{
					tangents.RemoveRange(start, tangents.Count - start);
					break;
				}
				tangents.Add(componentSlope);
			}
			if (tangents.Count - start == components)
			{
				return true;
			}
		}
		Fail(components == 1 ? $"{path}: expected a number, not {JsonText.Excerpt(element)}"
			: FormattableString.Invariant($"{path}: expected an array of {components} numbers, not {JsonText.Excerpt(element)}"));
		for (int component = 0; component < components; component++)
		{
			tangents.Add(double.NaN);
		}
		return false;
	}

	private static void ComponentsOf(object value, Span<double> components)
	{
		switch (value)
		{
			case float number:
				components[0] = number;
				break;
			case Vector2 vector:
				components[0] = vector.X;
				components[1] = vector.Y;
				break;
			case Vector3 vector:
				components[0] = vector.X;
				components[1] = vector.Y;
				components[2] = vector.Z;
				break;
			case Vector4 vector:
				components[0] = vector.X;
				components[1] = vector.Y;
				components[2] = vector.Z;
				components[3] = vector.W;
				break;
			case Color color:
				components[0] = color.R;
				components[1] = color.G;
				components[2] = color.B;
				components[3] = color.A;
				break;
		}
	}

	private static bool ReadNumber(JsonElement element, out double value)
	{
		bool read = LiteralCodec.TryReadFloat(element, out float number);
		value = number;
		return read;
	}

	private void Fail(string message, DiagnosticCode code = DiagnosticCode.InvalidSetting)
	{
		m_Valid = false;
		m_Report(code, message);
	}
}
