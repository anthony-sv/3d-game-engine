using System;
using System.Collections.Generic;
using System.Text;
using Strada.Interop;
using Xunit;

namespace Strada.ScriptCore.Tests;

// The managed side of save slots and the player's named values (Docs/Architecture.md §10.4): what Get and Set make of C#
// values, what Set refuses, and the JSON the engine reads and writes. Slots here are read from documents, as SaveSlot.Open
// reads the engine's answer, so no engine runs; the engine's side is StradaTests' Scene/SceneSavesScriptingTests.cpp.
public sealed class SaveSlotTests
{
	private enum Gear : byte
	{
		Neutral,
		First,
		Second,
	}

	private static SaveSlot Empty() => SaveSlot.FromDocument("test", SaveSlotStatus.Missing, []);

	private static SaveSlot Read(string values) =>
		SaveSlot.FromDocument("test", SaveSlotStatus.Present,
			Encoding.UTF8.GetBytes($$"""{ "Strada": { "Version": 1, "Type": "SaveSlot" }, "SaveSlot": { "Values": {{values}} } }"""));

	[Fact]
	public void ValuesAreReadAsTheirTypesAndListsAsArraysOrLists()
	{
		SaveSlot slot = Read("""
			{
				"Best": { "Type": "Float", "Value": 92.5 },
				"Gear": { "Type": "Int32", "Value": 2 },
				"Points": { "Type": "Vector3[]", "Value": [[1, 2, 3], [4, 5, 6]] },
				"Laps": { "Type": "Int32[]", "Value": [1, 2] },
				"Name": { "Type": "String", "Value": "Moto" },
				"Big": { "Type": "UInt64", "Value": "18000000000000000000" }
			}
			""");
		Assert.Equal(SaveSlotStatus.Present, slot.Status);
		Assert.Equal(92.5f, slot.Get("Best", 0.0f));
		Assert.Equal(Gear.Second, slot.Get("Gear", Gear.Neutral));
		Assert.Equal([new Vector3(1.0f, 2.0f, 3.0f), new Vector3(4.0f, 5.0f, 6.0f)], slot.Get("Points", Array.Empty<Vector3>()));
		Assert.Equal([1, 2], slot.Get("Laps", new List<int>()));
		Assert.Equal([1, 2], slot.Get("Laps", Array.Empty<int>()));
		Assert.Equal("Moto", slot.Get("Name", ""));
		Assert.Equal(18_000_000_000_000_000_000UL, slot.Get("Big", 0UL));
		Assert.True(slot.Has("Best"));
		Assert.False(slot.Has("best"));
		Assert.Equal(7.0f, slot.Get("Missing", 7.0f));
	}

	[Fact]
	public void AListReadIsACopy()
	{
		SaveSlot slot = Empty();
		float[] splits = [30.5f, 61.25f];
		slot.Set("Splits", splits);
		splits[0] = 0.0f;
		float[] read = slot.Get("Splits", Array.Empty<float>());
		Assert.Equal([30.5f, 61.25f], read);
		read[1] = 0.0f;
		Assert.Equal([30.5f, 61.25f], slot.Get("Splits", Array.Empty<float>()));
	}

	[Fact]
	public void SetRefusesWhatTheEngineCannotKeep()
	{
		SaveSlot slot = Empty();
		Assert.Throws<ArgumentNullException>(() => slot.Set(null!, 1));
		Assert.Throws<ArgumentException>(() => slot.Set("", 1));
		Assert.Throws<ArgumentException>(() => slot.Set(new string('n', SavedValueCodec.MaxNameBytes + 1), 1));
		Assert.Throws<ArgumentException>(() => slot.Set("Line\nBreak", 1));
		Assert.Throws<ArgumentException>(() => slot.Set("Lone\uD800", 1));
		Assert.Throws<ArgumentException>(() => slot.Set("Number", float.NaN));
		Assert.Throws<ArgumentException>(() => slot.Set("Number", double.PositiveInfinity));
		Assert.Throws<ArgumentException>(() => slot.Set("Vector", new Vector2(float.NaN, 0.0f)));
		Assert.Throws<ArgumentException>(() => slot.Set("Rotation", new Quaternion(0.0f, 0.0f, 0.0f, 0.0f)));
		Assert.Throws<ArgumentException>(() => slot.Set("Rotations", new[] { Quaternion.Identity, new Quaternion(0.0f, 0.0f, 0.0f, 1e-7f) }));
		Assert.Throws<ArgumentException>(() => slot.Set("List", new float[ScriptFieldCodec.MaxListElements + 1]));
		Assert.Throws<ArgumentException>(() => slot.Set("Object", new object()));
		Assert.Throws<ArgumentException>(() => slot.Set("Lists", new List<int[]>()));
		slot.Set("List", new float[ScriptFieldCodec.MaxListElements]);
		slot.Set(new string('n', SavedValueCodec.MaxNameBytes), 1);
		Assert.False(slot.Has("Number"));

		// At most MaxValues values: a new name past them is refused, a value changed in place is not.
		SaveSlot full = Empty();
		for (int index = 0; index < SavedValueCodec.MaxValues; index++)
		{
			full.Set(index.ToString(System.Globalization.CultureInfo.InvariantCulture), index);
		}
		Assert.Throws<ArgumentException>(() => full.Set("One more", 1));
		full.Set("0", 5);
		Assert.Equal(5, full.Get("0", 0));
	}

	[Fact]
	public void QuaternionsAreKeptOfLengthOne()
	{
		SaveSlot slot = Empty();
		slot.Set("Rotation", new Quaternion(0.0f, 0.0f, 0.0f, 2.0f));
		Assert.Equal(Quaternion.Identity, slot.Get("Rotation", new Quaternion(1.0f, 0.0f, 0.0f, 0.0f)));
	}

	[Fact]
	public void WrittenValuesReadBackAsTheyWereSet()
	{
		SaveSlot slot = Empty();
		slot.Set("Bool", true);
		slot.Set("Long", -5_000_000_000L);
		slot.Set("Double", 0.1);
		slot.Set("Text", "Strada été");
		slot.Set("Color", new Color(1.0f, 0.5f, 0.25f, 1.0f));
		slot.Set("Gear", Gear.First);
		slot.Set("Points", new List<Vector3> { new(1.0f, 2.0f, 3.0f) });
		slot.Set("Asset", new AssetHandle(42));
		slot.Set("Gone", 1);
		Assert.True(slot.Remove("Gone"));
		Assert.False(slot.Remove("Gone"));

		byte[] values = slot.WriteValues();
		SaveSlot read = Read(Encoding.UTF8.GetString(values));
		Assert.True(read.Get("Bool", false));
		Assert.Equal(-5_000_000_000L, read.Get("Long", 0L));
		Assert.Equal(0.1, read.Get("Double", 0.0));
		Assert.Equal("Strada été", read.Get("Text", ""));
		Assert.Equal(new Color(1.0f, 0.5f, 0.25f, 1.0f), read.Get("Color", Color.Black));
		Assert.Equal(Gear.First, read.Get("Gear", Gear.Neutral));
		Assert.Equal([new Vector3(1.0f, 2.0f, 3.0f)], read.Get("Points", Array.Empty<Vector3>()));
		Assert.Equal(new AssetHandle(42), read.Get("Asset", AssetHandle.Invalid));
		Assert.False(read.Has("Gone"));
		Assert.Contains("\"Gear\":{\"Type\":\"Int32\",\"Value\":1}", Encoding.UTF8.GetString(values), StringComparison.Ordinal);
	}

	[Fact]
	public void AValueOfAnotherTypeOrOutOfAnEnumsRangeIsNoValueOfT()
	{
		SavedValue text = new(ScriptFieldType.String, "Moto");
		Assert.False(SavedValueCodec.TryFromSaved(text, out int _, out string? problem));
		Assert.Equal("it holds a value of type String, not Int32", problem);
		SavedValue large = new(ScriptFieldType.Int32, 300);
		Assert.False(SavedValueCodec.TryFromSaved(large, out Gear _, out problem));
		Assert.NotNull(problem);
		Assert.False(SavedValueCodec.TryFromSaved(text, out object _, out _));
	}

	[Theory]
	[InlineData("Float", (int)ScriptFieldType.Float)]
	[InlineData("Vector3[]", (int)ScriptFieldType.Vector3 | ScriptFieldCodec.ListFlag)]
	[InlineData("None", 0)]
	[InlineData("3", 0)]
	[InlineData("Bool, Int32", 0)]
	[InlineData("float", 0)]
	[InlineData("Float[][]", 0)]
	[InlineData("", 0)]
	public void TypeNamesAreTheEnginesOwn(string name, int type)
	{
		Assert.Equal((ScriptFieldType)type, SavedValueCodec.ParseTypeName(name));
	}
}
