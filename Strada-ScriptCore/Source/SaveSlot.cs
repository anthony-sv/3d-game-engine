using System;
using System.Buffers;
using System.Collections.Generic;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;

namespace Strada;

/// <summary>What a save slot read as when it was opened, or what its last <see cref="SaveSlot.Save"/> or
/// <see cref="SaveSlot.Delete"/> left it.</summary>
public enum SaveSlotStatus
{
	/// <summary>No file: an empty slot, which a save creates.</summary>
	Missing = 0,

	/// <summary>The slot's values were read, or it was saved.</summary>
	Present,

	/// <summary>A file that does not read: the slot is empty, and the file stays on disk until the game saves or deletes the
	/// slot.</summary>
	Damaged,

	/// <summary>A file of a newer version of the engine: the slot is empty, and this game never writes over it or deletes it,
	/// so a player who runs an older build keeps the newer build's save.</summary>
	Newer,

	/// <summary>No game runs (the editor while it edits): the slot is empty, and nothing is read or saved.</summary>
	Unavailable,
}

/// <summary>
/// A save slot: named values the game keeps for its player between runs (a best lap, its splits, progress), in a file of the
/// game's own data folder that this class never names. Open a slot by its name, read values with <see cref="Get{T}"/>, change
/// them with <see cref="Set{T}"/> and <see cref="Remove"/>, and write them with <see cref="Save"/>, which returns once the
/// slot is on disk: the frame that saves waits for the device, so save at a results screen, a menu or between races, never
/// while the player plays, and keep what a ride learns in memory until then. A slot holds numbers, text, vectors,
/// quaternions, colors, entities, prefabs, assets, enums (as their integers) and arrays or lists of any of them of up to 4,096
/// elements, at most 4,096 values, and is written whole, at most 16 MiB. The object holds the values: reading and changing
/// them touches no file, and two objects opened with one name are separate copies, the last to save winning. Main thread.
/// </summary>
public sealed unsafe class SaveSlot
{
	private readonly Dictionary<string, SavedValue> m_Values;
	// Names whose value Get found of another type, warned about once each.
	private readonly HashSet<string> m_Mismatched = new(StringComparer.Ordinal);

	private SaveSlot(string name, SaveSlotStatus status, Dictionary<string, SavedValue> values)
	{
		Name = name;
		Status = status;
		m_Values = values;
	}

	/// <summary>The slot's name as the game's folder spells it, in lower case: <c>Best</c> and <c>best</c> are one slot.</summary>
	public string Name { get; }

	/// <summary>What the slot read as when it was opened, or what its last save or delete left it.</summary>
	public SaveSlotStatus Status { get; private set; }

	/// <summary>Reads a slot. A slot without a file is empty (<see cref="SaveSlotStatus.Missing"/>); a damaged slot, one of a
	/// newer version and any slot while no game runs are empty too, and logged. It reads the disk where the flow passes, so
	/// open a slot once and keep the object.</summary>
	/// <param name="name">1 to 64 ASCII letters, digits, spaces, <c>-</c> and <c>_</c>, without a space at either end and not a
	/// name Windows keeps for a device (<c>CON</c>, <c>NUL</c>, ...); letter case does not matter.</param>
	/// <returns>The slot, never null.</returns>
	/// <exception cref="ArgumentException">The name is not one a slot takes.</exception>
	public static SaveSlot Open(string name)
	{
		ArgumentNullException.ThrowIfNull(name);
		byte[] utf8 = NativeString.ToUtf8(name);
		int status;
		int length;
		byte* text;
		fixed (byte* bytes = utf8)
		{
			text = InternalCalls.SaveSlot_Read(bytes, utf8.Length, &status, &length);
		}
		if (status < 0)
		{
			throw new ArgumentException($"SaveSlot.Open: {NativeString.FromUtf8(text, length)}", nameof(name));
		}
		ReadOnlySpan<byte> document = length > 0 ? new ReadOnlySpan<byte>(text, length) : ReadOnlySpan<byte>.Empty;
		return FromDocument(name.ToLowerInvariant(), (SaveSlotStatus)status, document);
	}

	/// <summary>The names of the slots the game has saved, in lower case and sorted; empty while no game runs. It reads the disk
	/// where the flow passes.</summary>
	public static string[] GetNames()
	{
		int length;
		byte* text = InternalCalls.SaveSlot_GetNames(&length);
		string joined = NativeString.FromUtf8(text, length);
		return joined.Length == 0 ? [] : joined.Split('\n');
	}

	/// <summary>Whether the slot holds a value of the name, of any type.</summary>
	/// <param name="name">The value's name; names differ in letter case.</param>
	[PureNode]
	public bool Has(string name)
	{
		ArgumentNullException.ThrowIfNull(name);
		return m_Values.ContainsKey(name);
	}

	/// <summary>The value of the name, when the slot holds one of <typeparamref name="T"/>'s type (an array and a list of a
	/// type are one type, an enum is its integers); otherwise the fallback, and a value of another type is warned about once
	/// per name.</summary>
	/// <param name="name">The value's name.</param>
	/// <param name="fallback">What a slot without such a value answers.</param>
	[PureNode]
	public T Get<T>(string name, T fallback)
	{
		ArgumentNullException.ThrowIfNull(name);
		if (!m_Values.TryGetValue(name, out SavedValue saved))
		{
			return fallback;
		}
		if (SavedValueCodec.TryFromSaved(saved, out T value, out string? problem))
		{
			return value;
		}
		if (m_Mismatched.Add(name))
		{
			Log.Warn($"SaveSlot.Get: slot '{Name}', value '{name}': {problem}: the fallback answers");
		}
		return fallback;
	}

	/// <summary>Sets a value of the slot, kept by this object until <see cref="Save"/>. An array or a list is copied.</summary>
	/// <param name="name">1 to 128 bytes of UTF-8 without control characters.</param>
	/// <param name="value">A number (finite), text, a vector, a quaternion (of a length of at least 1e-6; kept of length one), a
	/// color, an entity, a prefab, an asset, an enum, or an array or a list of any of them of up to 4,096 elements.</param>
	/// <exception cref="ArgumentException">The name, the type or the value is not one a slot holds, or the slot holds 4,096
	/// values already.</exception>
	public void Set<T>(string name, T value)
	{
		SavedValueCodec.RequireName("SaveSlot.Set", name);
		SavedValue saved = SavedValueCodec.ToSaved("SaveSlot.Set", value);
		if (m_Values.Count >= SavedValueCodec.MaxValues && !m_Values.ContainsKey(name))
		{
			throw new ArgumentException(FormattableString.Invariant($"SaveSlot.Set: a slot holds at most {SavedValueCodec.MaxValues} values"),
				nameof(name));
		}
		m_Values[name] = saved;
		m_Mismatched.Remove(name);
	}

	/// <summary>Removes a value of the slot; <see cref="Save"/> writes the slot without it.</summary>
	/// <param name="name">The value's name.</param>
	/// <returns>Whether the slot held a value of the name.</returns>
	public bool Remove(string name)
	{
		ArgumentNullException.ThrowIfNull(name);
		m_Mismatched.Remove(name);
		return m_Values.Remove(name);
	}

	/// <summary>Writes every value of the slot to its file and returns once the file is on disk, whole: the old file stays whole
	/// until then, whatever happens. False, logged, while no game runs, for a slot of a newer version and when the slot cannot
	/// be written (larger than 16 MiB, a full disk); <see cref="Status"/> is <see cref="SaveSlotStatus.Present"/> after one
	/// that succeeds.</summary>
	/// <returns>Whether the slot is on disk.</returns>
	public bool Save()
	{
		byte[] name = NativeString.ToUtf8(Name);
		byte[] values = WriteValues();
		byte saved;
		fixed (byte* nameBytes = name)
		fixed (byte* valueBytes = values)
		{
			saved = InternalCalls.SaveSlot_Write(nameBytes, name.Length, valueBytes, values.Length);
		}
		if (saved == 0)
		{
			return false;
		}
		Status = SaveSlotStatus.Present;
		return true;
	}

	/// <summary>Removes the slot's file and every value of this object; a slot without a file is deleted already. False, logged,
	/// while no game runs and for a slot of a newer version, which keep their file and values.</summary>
	/// <returns>Whether the slot has no file now.</returns>
	public bool Delete()
	{
		byte[] name = NativeString.ToUtf8(Name);
		byte deleted;
		fixed (byte* bytes = name)
		{
			deleted = InternalCalls.SaveSlot_Delete(bytes, name.Length);
		}
		if (deleted == 0)
		{
			return false;
		}
		m_Values.Clear();
		m_Mismatched.Clear();
		Status = SaveSlotStatus.Missing;
		return true;
	}

	// A slot of the values a slot document holds (SaveSlot_Read's: { "Strada", "SaveSlot": { "Values": { ... } } }); an empty
	// one for an empty document. A value that does not read is left out, warned about.
	internal static SaveSlot FromDocument(string name, SaveSlotStatus status, ReadOnlySpan<byte> document)
	{
		Dictionary<string, SavedValue> values = new(StringComparer.Ordinal);
		if (!document.IsEmpty)
		{
			Utf8JsonReader reader = new(document);
			using JsonDocument parsed = JsonDocument.ParseValue(ref reader);
			if (parsed.RootElement.TryGetProperty("SaveSlot", out JsonElement content) && content.ValueKind == JsonValueKind.Object &&
				content.TryGetProperty("Values", out JsonElement stored) && stored.ValueKind == JsonValueKind.Object)
			{
				foreach (JsonProperty property in stored.EnumerateObject())
				{
					SavedValue? value = SavedValueCodec.Read(property.Value, out string? problem);
					if (value == null)
					{
						Log.Warn($"SaveSlot.Open: slot '{name}', value '{property.Name}' is left out: {problem}");
						continue;
					}
					values[property.Name] = value.Value;
				}
			}
		}
		return new SaveSlot(name, status, values);
	}

	// The values as a script field map: { "<Name>": { "Type", "Value" } }.
	internal byte[] WriteValues()
	{
		ArrayBufferWriter<byte> buffer = new();
		using (Utf8JsonWriter writer = new(buffer))
		{
			writer.WriteStartObject();
			foreach ((string name, SavedValue value) in m_Values)
			{
				writer.WritePropertyName(name);
				SavedValueCodec.Write(writer, value);
			}
			writer.WriteEndObject();
		}
		return buffer.WrittenSpan.ToArray();
	}
}
