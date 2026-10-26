using System;
using System.Buffers;
using System.Collections.Generic;
using System.Globalization;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Strada.ScriptCore.Tests;

// The fuzz tests of the visual-script VM, as Tests/StradaTests/Source/Fuzzing.h does it for the engine's formats: fixed
// seeds, deterministic damage, and more rounds through STRADA_FUZZ_SCALE.
internal static class Fuzzing
{
	// The rounds a fuzz test runs: its default, times STRADA_FUZZ_SCALE (1 to 1000) when that is set, for deeper local
	// runs; the default keeps the suite fast.
	internal static int GetRounds(int rounds)
	{
		string? scale = System.Environment.GetEnvironmentVariable("STRADA_FUZZ_SCALE");
		int multiplier = int.TryParse(scale, NumberStyles.Integer, CultureInfo.InvariantCulture, out int value) ? value : 1;
		return rounds * Math.Clamp(multiplier, 1, 1000);
	}
}

// Pseudo-random numbers (SplitMix64) that are the same on every platform and runtime, so a fuzz failure reproduces from its
// seed and round.
internal sealed class FuzzRandom
{
	private ulong m_State;

	internal FuzzRandom(ulong seed)
	{
		m_State = seed;
	}

	internal ulong Next()
	{
		ulong value = m_State += 0x9E3779B97F4A7C15UL;
		value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9UL;
		value = (value ^ (value >> 27)) * 0x94D049BB133111EBUL;
		return value ^ (value >> 31);
	}

	// A number below count, which must be positive.
	internal int Pick(int count) => (int)(Next() % (ulong)count);
}

// Damages visual-script documents the way bad files, hand edits and careless tools do: members and elements go missing,
// repeat or change kind; nodes change type or repeat by the thousand, or are disabled; links name wrong pins, missing nodes
// or the node they leave (cycles); numbers become extreme; values nest far too deep; and the text itself is cut short or
// has a byte changed. Deterministic, so a failure reproduces from its seed and round.
internal sealed class DocumentMutator
{
	private static readonly string[] s_Extremes =
	[
		"2147483647", "-2147483648", "2147483648", "4294967295", "4294967296", "9223372036854775807", "-9223372036854775809",
		"18446744073709551616", "-1", "1e300", "-1e300", "1e-300", "0.5", "-0.0", "123456789012345678901234567890",
	];

	private static readonly string[] s_InputExtremes = ["0", "-1", "1", "3", "2147483647", "-2147483648", "0.0001", "1000000", "-1000000"];

	private static readonly string[] s_Typos = ["", "x", "Execute", "Then", "Value", "Target", "Self", "Default", "Case:", "*", "exec"];

	private static readonly int[] s_Depths = [15, 16, 17, 60, 63, 64, 65, 300];

	private static readonly int[] s_Copies = [8, 200, 3000];

	private static readonly string[] s_EnabledStates = ["Disabled", "DevelopmentOnly", "Enabled"];

	private readonly FuzzRandom m_Random;

	internal DocumentMutator(ulong seed)
	{
		m_Random = new FuzzRandom(seed);
	}

	internal int Pick(int count) => m_Random.Pick(count);

	// Applies 1 to maxMutations mutations, more in later rounds.
	internal JsonNode? Damage(JsonNode? original, int round, int maxMutations = 3)
	{
		JsonNode? document = original?.DeepClone();
		for (int mutation = 0; mutation <= round % maxMutations; mutation++)
		{
			document = Mutate(document);
		}
		return document;
	}

	// Applies one or two changes that tend to keep a graph valid but change what it does: links that loop back, links
	// rewired or gone, nodes repeated under new IDs, nodes disabled or enabled, extreme inputs.
	internal JsonNode? Rewire(JsonNode? original, int round)
	{
		JsonNode? document = original?.DeepClone();
		for (int change = 0; change <= round % 2; change++)
		{
			List<Place> places = [];
			Collect(document, null, null, -1, null, places);
			Facts facts = new(places);
			bool changed = Pick(8) switch
			{
				0 => LoopLink(facts),
				1 => LeadBack(facts),
				2 => SwapTargets(facts),
				3 => RemoveLink(facts),
				4 => RepeatNode(facts, 1 + Pick(3)),
				5 => SetEnabled(facts, true),
				_ => MakeExtremeInput(places),
			};
			if (!changed)
			{
				document = Mutate(document);
			}
		}
		return document;
	}

	// The compact text of a document; now and then cut short, with one byte changed or with an escaped surrogate that has no
	// pair.
	internal byte[] Write(JsonNode? document)
	{
		ArrayBufferWriter<byte> buffer = new();
		using (Utf8JsonWriter writer = new(buffer, new JsonWriterOptions { MaxDepth = 4096, Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping }))
		{
			if (document == null)
			{
				writer.WriteNullValue();
			}
			else
			{
				document.WriteTo(writer);
			}
		}
		byte[] text = buffer.WrittenSpan.ToArray();
		switch (Pick(24))
		{
			case 0:
				return text[..Pick(text.Length + 1)];
			case 1:
				text[Pick(text.Length)] = (byte)Pick(256);
				return text;
			case 2:
				return EscapeSurrogate(text);
			default:
				return text;
		}
	}

	// An escaped surrogate without its pair after a quote: inside a string or a member name, or breaking the syntax.
	private byte[] EscapeSurrogate(byte[] text)
	{
		List<int> quotes = [];
		for (int index = 0; index < text.Length; index++)
		{
			if (text[index] == (byte)'"')
			{
				quotes.Add(index + 1);
			}
		}
		if (quotes.Count == 0)
		{
			return text;
		}
		int at = quotes[Pick(quotes.Count)];
		return [.. text[..at], .. @"\uDC00"u8, .. text[at..]];
	}

	// One mutation; returns the document (a new root when the root itself was replaced).
	internal JsonNode? Mutate(JsonNode? document)
	{
		List<Place> places = [];
		Collect(document, null, null, -1, null, places);
		Facts facts = new(places);
		switch (Pick(20))
		{
			case 0:
			case 1:
				if (RemoveChild(places))
				{
					return document;
				}
				break;
			case 2:
				if (DuplicateElement(places))
				{
					return document;
				}
				break;
			case 3:
				if (RetypeNode(facts))
				{
					return document;
				}
				break;
			case 4:
			case 5:
				if (RenamePin(facts))
				{
					return document;
				}
				break;
			case 6:
			case 7:
				if (RetargetLink(facts))
				{
					return document;
				}
				break;
			case 8:
				if (AddLink(facts))
				{
					return document;
				}
				break;
			case 9:
				if (LoopLink(facts))
				{
					return document;
				}
				break;
			case 10:
			case 11:
				if (MakeExtreme(places))
				{
					return document;
				}
				break;
			case 12:
				if (RepeatNode(facts, 1))
				{
					return document;
				}
				break;
			case 13:
				if (RepeatNode(facts, s_Copies[Pick(s_Copies.Length)]))
				{
					return document;
				}
				break;
			case 14:
				if (ShareName(places))
				{
					return document;
				}
				break;
			case 15:
				return Replace(document, places[Pick(places.Count)], Nest(s_Depths[Pick(s_Depths.Length)]));
			case 16:
				if (SetEnabled(facts, false))
				{
					return document;
				}
				break;
			default:
				break;
		}
		return Replace(document, places[Pick(places.Count)], MakeValue());
	}

	private static JsonNode? Replace(JsonNode? document, Place place, JsonNode? value)
	{
		switch (place.Parent)
		{
			case JsonObject parent:
				parent[place.Key!] = value;
				return document;
			case JsonArray parent:
				parent[place.Index] = value;
				return document;
			default:
				return value;
		}
	}

	// A member or element goes missing.
	private bool RemoveChild(List<Place> places)
	{
		List<JsonNode> containers = [];
		foreach (Place place in places)
		{
			if (place.Node is JsonObject { Count: > 0 } or JsonArray { Count: > 0 })
			{
				containers.Add(place.Node);
			}
		}
		if (containers.Count == 0)
		{
			return false;
		}
		switch (containers[Pick(containers.Count)])
		{
			case JsonObject members:
				List<string> keys = [.. ((IDictionary<string, JsonNode?>)members).Keys];
				members.Remove(keys[Pick(keys.Count)]);
				return true;
			case JsonArray elements:
				elements.RemoveAt(Pick(elements.Count));
				return true;
			default:
				return false;
		}
	}

	// An element repeats at the end of its array.
	private bool DuplicateElement(List<Place> places)
	{
		List<JsonArray> arrays = [];
		foreach (Place place in places)
		{
			if (place.Node is JsonArray { Count: > 0 } array)
			{
				arrays.Add(array);
			}
		}
		if (arrays.Count == 0)
		{
			return false;
		}
		JsonArray target = arrays[Pick(arrays.Count)];
		target.Add(target[Pick(target.Count)]?.DeepClone());
		return true;
	}

	// A node takes the type of another node, a misspelt type or its type without the signature.
	private bool RetypeNode(Facts facts)
	{
		if (facts.Nodes.Count == 0)
		{
			return false;
		}
		JsonObject node = facts.Nodes[Pick(facts.Nodes.Count)];
		string type = facts.Types.Count > 0 ? facts.Types[Pick(facts.Types.Count)] : "Flow.Branch";
		node["Type"] = Pick(4) switch
		{
			0 => type + "x",
			1 => type.Contains('(', StringComparison.Ordinal) ? type[..type.IndexOf('(', StringComparison.Ordinal)] : type[..(type.Length / 2)],
			_ => type,
		};
		return true;
	}

	// A node is disabled, development-only or enabled again. When damaging, its Enabled may also be no state at all, or
	// about half of the nodes are disabled, so that nodes the other damage broke are disabled too.
	private bool SetEnabled(Facts facts, bool valid)
	{
		if (facts.Nodes.Count == 0)
		{
			return false;
		}
		if (!valid && Pick(2) == 0)
		{
			foreach (JsonObject each in facts.Nodes)
			{
				if (Pick(2) == 0)
				{
					each["Enabled"] = "Disabled";
				}
			}
			return true;
		}
		JsonObject node = facts.Nodes[Pick(facts.Nodes.Count)];
		node["Enabled"] = valid || Pick(2) == 0 ? s_EnabledStates[Pick(s_EnabledStates.Length)] : MakeValue();
		return true;
	}

	// A link names a pin of the document that its node may not have, or a misspelt one.
	private bool RenamePin(Facts facts)
	{
		if (facts.Endpoints.Count == 0)
		{
			return false;
		}
		JsonArray endpoint = facts.Endpoints[Pick(facts.Endpoints.Count)];
		if (endpoint.Count < 2)
		{
			return false;
		}
		endpoint[1] = Pick(3) switch
		{
			0 => s_Typos[Pick(s_Typos.Length)],
			1 => facts.Pins[Pick(facts.Pins.Count)] + "e",
			_ => facts.Pins[Pick(facts.Pins.Count)],
		};
		return true;
	}

	// A link leads to another node of the document (cycles, other graphs) or to one that does not exist.
	private bool RetargetLink(Facts facts)
	{
		if (facts.Endpoints.Count == 0)
		{
			return false;
		}
		JsonArray endpoint = facts.Endpoints[Pick(facts.Endpoints.Count)];
		if (endpoint.Count < 1)
		{
			return false;
		}
		endpoint[0] = Pick(4) switch
		{
			0 => facts.LargestId + 1 + (long)Pick(1000),
			1 => 0,
			_ => facts.Ids.Count > 0 ? facts.Ids[Pick(facts.Ids.Count)] : 1,
		};
		return true;
	}

	// A new link between two nodes of a graph, through pins the document names.
	private bool AddLink(Facts facts)
	{
		if (facts.Graphs.Count == 0 || facts.Pins.Count == 0)
		{
			return false;
		}
		JsonObject graph = facts.Graphs[Pick(facts.Graphs.Count)];
		List<long> ids = Facts.IdsOf(graph);
		if (ids.Count == 0 || graph["Links"] is not JsonArray links)
		{
			return false;
		}
		links.Add(Link(ids[Pick(ids.Count)], facts.Pins[Pick(facts.Pins.Count)], ids[Pick(ids.Count)], facts.Pins[Pick(facts.Pins.Count)]));
		return true;
	}

	// A link leads back to the node it leaves, or to the node before it: chains that never end without the budget.
	private bool LoopLink(Facts facts)
	{
		if (facts.Links.Count == 0)
		{
			return false;
		}
		JsonObject link = facts.Links[Pick(facts.Links.Count)];
		if (link["From"] is not JsonArray { Count: 2 } from || link["To"] is not JsonArray { Count: 2 } to)
		{
			return false;
		}
		if (Pick(2) == 0)
		{
			to[0] = from[0]?.DeepClone();
			return true;
		}
		// The reverse link: the node after leads back into the node before, through the input the link entered.
		if (link.Parent is JsonArray links)
		{
			links.Add(new JsonObject
			{
				["From"] = new JsonArray(to[0]?.DeepClone(), from[1]?.DeepClone()),
				["To"] = new JsonArray(from[0]?.DeepClone(), to[1]?.DeepClone()),
			});
			return true;
		}
		return false;
	}

	// An exec link leads back to a node earlier in its chain, through an input of the same name: a loop.
	private bool LeadBack(Facts facts)
	{
		if (facts.Links.Count < 2)
		{
			return false;
		}
		JsonObject link = facts.Links[Pick(facts.Links.Count)];
		JsonObject other = facts.Links[Pick(facts.Links.Count)];
		if (link["To"] is not JsonArray { Count: 2 } to || other["From"] is not JsonArray { Count: 2 } from)
		{
			return false;
		}
		to[0] = from[0]?.DeepClone();
		return true;
	}

	// Two links exchange the inputs they enter.
	private bool SwapTargets(Facts facts)
	{
		if (facts.Links.Count < 2)
		{
			return false;
		}
		JsonObject first = facts.Links[Pick(facts.Links.Count)];
		JsonObject second = facts.Links[Pick(facts.Links.Count)];
		if (ReferenceEquals(first, second))
		{
			return false;
		}
		JsonNode? to = first["To"];
		first["To"] = second["To"]?.DeepClone();
		second["To"] = to?.DeepClone();
		return true;
	}

	private bool RemoveLink(Facts facts)
	{
		if (facts.Links.Count == 0)
		{
			return false;
		}
		JsonObject link = facts.Links[Pick(facts.Links.Count)];
		return link.Parent is JsonArray links && links.Remove(link);
	}

	// An input's stored number becomes extreme: no time, endless counts, huge or negative ranges.
	private bool MakeExtremeInput(List<Place> places)
	{
		List<Place> inputs = places.FindAll(static place => place.Container is not null && place.Parent is JsonObject && place.Node is JsonValue value &&
			value.GetValueKind() == JsonValueKind.Number && place.Parent.Parent is JsonObject && place.Parent.GetPropertyName() == "Inputs");
		if (inputs.Count == 0)
		{
			return false;
		}
		Replace(null, inputs[Pick(inputs.Count)], JsonNode.Parse(s_InputExtremes[Pick(s_InputExtremes.Length)]));
		return true;
	}

	// A number becomes extreme: out of every range, huge counts, tiny or negative durations.
	private bool MakeExtreme(List<Place> places)
	{
		List<Place> numbers = places.FindAll(static place => place.Node is JsonValue value && value.GetValueKind() == JsonValueKind.Number);
		if (numbers.Count == 0)
		{
			return false;
		}
		Place target = numbers[Pick(numbers.Count)];
		Replace(null, target, JsonNode.Parse(s_Extremes[Pick(s_Extremes.Length)]));
		return true;
	}

	// A node repeats: once with its own ID (a duplicate), or many times with new IDs, linked into a chain through pins its
	// links use.
	private bool RepeatNode(Facts facts, int copies)
	{
		if (facts.Nodes.Count == 0)
		{
			return false;
		}
		JsonObject node = facts.Nodes[Pick(facts.Nodes.Count)];
		if (node.Parent is not JsonArray nodes)
		{
			return false;
		}
		if (copies == 1)
		{
			nodes.Add(node.DeepClone());
			return true;
		}
		JsonArray? links = node.Parent?.Parent is JsonObject graph ? graph["Links"] as JsonArray : null;
		string output = facts.Pins.Count > 0 ? facts.Pins[Pick(facts.Pins.Count)] : "Then";
		string input = facts.Pins.Count > 0 ? facts.Pins[Pick(facts.Pins.Count)] : "Execute";
		long id = facts.LargestId;
		for (int copy = 0; copy < copies; copy++)
		{
			JsonObject clone = (JsonObject)node.DeepClone();
			clone["ID"] = ++id;
			nodes.Add(clone);
			if (copy > 0)
			{
				links?.Add(Link(id - 1, output, id, input));
			}
		}
		return true;
	}

	// A variable, local, function or parameter takes the name of another.
	private bool ShareName(List<Place> places)
	{
		List<Place> names = places.FindAll(static place => place.Key == "Name" && place.Node is JsonValue value && value.GetValueKind() == JsonValueKind.String);
		if (names.Count < 2)
		{
			return false;
		}
		Place target = names[Pick(names.Count)];
		Replace(null, target, names[Pick(names.Count)].Node!.DeepClone());
		return true;
	}

	private static JsonObject Link(long from, string output, long to, string input) => new()
	{
		["From"] = new JsonArray(from, output),
		["To"] = new JsonArray(to, input),
	};

	// Arrays nested depth levels deep around a number.
	private static JsonNode Nest(int depth)
	{
		JsonNode nested = 1;
		for (int level = 1; level < depth; level++)
		{
			nested = new JsonArray(nested);
		}
		return nested;
	}

	private JsonNode? MakeValue() => Pick(17) switch
	{
		0 => null,
		1 => true,
		2 => false,
		3 => 0,
		4 => -1,
		5 => long.MaxValue,
		6 => ulong.MaxValue,
		7 => -1.0e30,
		8 => 1.0e30,
		9 => 0.5,
		10 => string.Empty,
		11 => "builtin://Nothing",
		12 => "18446744073709551615",
		13 => new JsonArray(1, 2, 3),
		14 => new JsonArray(),
		15 => "Strada.Entity",
		_ => new JsonObject(),
	};

	private static void Collect(JsonNode? node, JsonNode? parent, string? key, int index, string? container, List<Place> places)
	{
		places.Add(new Place(node, parent, key, index, container));
		switch (node)
		{
			case JsonObject members:
				foreach (KeyValuePair<string, JsonNode?> member in members)
				{
					Collect(member.Value, members, member.Key, -1, member.Key, places);
				}
				break;
			case JsonArray elements:
				for (int element = 0; element < elements.Count; element++)
				{
					Collect(elements[element], elements, null, element, container, places);
				}
				break;
		}
	}

	// A value of the document and where it is: the member Key of an object, or the element Index of an array whose member
	// is Container.
	private readonly record struct Place(JsonNode? Node, JsonNode? Parent, string? Key, int Index, string? Container);

	// What graph mutations work with: the nodes, links and graphs of the document, the pin names its links and inputs use,
	// the node types and the node IDs.
	private sealed class Facts
	{
		internal Facts(List<Place> places)
		{
			foreach (Place place in places)
			{
				if (place.Node is not JsonObject element)
				{
					continue;
				}
				if (place.Container == "Nodes" && place.Parent is JsonArray)
				{
					Nodes.Add(element);
					if (element["Type"] is JsonValue type && type.TryGetValue(out string? text))
					{
						Types.Add(text);
					}
					if (element["Inputs"] is JsonObject inputs)
					{
						Pins.AddRange(((IDictionary<string, JsonNode?>)inputs).Keys);
					}
				}
				else if (place.Container == "Links" && place.Parent is JsonArray)
				{
					Links.Add(element);
					foreach (string side in (string[])["From", "To"])
					{
						if (element[side] is JsonArray endpoint)
						{
							Endpoints.Add(endpoint);
							if (endpoint.Count > 1 && endpoint[1] is JsonValue pin && pin.TryGetValue(out string? name))
							{
								Pins.Add(name);
							}
						}
					}
				}
				if (element["Nodes"] is JsonArray && element["Links"] is JsonArray)
				{
					Graphs.Add(element);
				}
				if (element["ID"] is JsonValue id && id.TryGetValue(out long value))
				{
					Ids.Add(value);
					LargestId = Math.Max(LargestId, value);
				}
			}
		}

		internal List<JsonObject> Nodes { get; } = [];

		internal List<JsonObject> Links { get; } = [];

		internal List<JsonArray> Endpoints { get; } = [];

		internal List<JsonObject> Graphs { get; } = [];

		internal List<string> Pins { get; } = [];

		internal List<string> Types { get; } = [];

		internal List<long> Ids { get; } = [];

		internal long LargestId { get; }

		internal static List<long> IdsOf(JsonObject graph)
		{
			List<long> ids = [];
			if (graph["Nodes"] is JsonArray nodes)
			{
				foreach (JsonNode? node in nodes)
				{
					if (node is JsonObject element && element["ID"] is JsonValue id && id.TryGetValue(out long value))
					{
						ids.Add(value);
					}
				}
			}
			return ids;
		}
	}
}
