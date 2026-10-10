using System.Buffers;
using System.Collections.Generic;
using System.Text;
using System.Text.Json;
using Strada.Interop;
using Xunit;

namespace Strada.ScriptCore.Tests;

public sealed class InteropTests
{
	private static string Write(ScriptFieldType type, object? value)
	{
		ArrayBufferWriter<byte> buffer = new();
		using (Utf8JsonWriter writer = new(buffer))
		{
			ScriptFieldCodec.WriteValue(writer, type, value);
		}
		return Encoding.UTF8.GetString(buffer.WrittenSpan);
	}

	private static object? Read(ScriptFieldType type, string json, out string? error)
	{
		using JsonDocument document = JsonDocument.Parse(json);
		return ScriptFieldCodec.ReadValue(document.RootElement, type, out error);
	}

	public static TheoryData<string, object?, string> Values => new()
	{
		{ "Bool", true, "true" },
		{ "Int32", -3, "-3" },
		{ "UInt32", 7u, "7" },
		{ "Int64", -9000000000L, "\"-9000000000\"" },
		{ "UInt64", 18000000000000000000UL, "\"18000000000000000000\"" },
		{ "Float", 2.5f, "2.5" },
		{ "Double", 0.125, "0.125" },
		{ "String", "Héllo", "\"H\\u00E9llo\"" },
		{ "Vector2", new Vector2(1.0f, 2.0f), "[1,2]" },
		{ "Vector3", new Vector3(1.0f, 2.0f, 3.0f), "[1,2,3]" },
		{ "Vector4", new Vector4(1.0f, 2.0f, 3.0f, 4.0f), "[1,2,3,4]" },
		{ "Quaternion", Quaternion.Identity, "[0,0,0,1]" },
		{ "Color", new Color(0.5f, 0.25f, 1.0f, 1.0f), "[0.5,0.25,1,1]" },
		{ "Asset", new AssetHandle(77), "\"77\"" },
	};

	[Theory]
	[MemberData(nameof(Values))]
	public void FieldValuesRoundTripInTheSceneFormat(string typeName, object? value, string json)
	{
		ScriptFieldType type = System.Enum.Parse<ScriptFieldType>(typeName);
		Assert.Equal(json, Write(type, value));
		Assert.Equal(value, Read(type, json, out string? error));
		Assert.Null(error);
	}

	[Fact]
	public void EntityFieldsHoldIdsAndNoEntityIsZero()
	{
		Assert.Equal("\"42\"", Write(ScriptFieldType.Entity, new Entity(42)));
		Assert.Equal("\"0\"", Write(ScriptFieldType.Entity, null));
		Assert.Equal(new Entity(42), Read(ScriptFieldType.Entity, "\"42\"", out _));
		Assert.Null(Read(ScriptFieldType.Entity, "\"0\"", out string? error));
		Assert.Null(error);
	}

	[Fact]
	public void MismatchedValuesAreReported()
	{
		Assert.Null(Read(ScriptFieldType.Float, "\"fast\"", out string? error));
		Assert.Equal("expected a Float value, not \"fast\"", error);
		Assert.Null(Read(ScriptFieldType.Vector3, "[1,2]", out error));
		Assert.NotNull(error);
		Assert.Null(Read(ScriptFieldType.Int32, "2.5", out error));
		Assert.NotNull(error);
		Assert.Null(Read(ScriptFieldType.UInt64, "\"-1\"", out error));
		Assert.NotNull(error);
	}

	[Fact]
	public void NonFiniteNumbersAreWrittenAsZero()
	{
		Assert.Equal("0", Write(ScriptFieldType.Float, float.NaN));
		Assert.Equal("[0,1]", Write(ScriptFieldType.Vector2, new Vector2(float.PositiveInfinity, 1.0f)));
	}

	[Fact]
	public void FieldTypesFollowTheFieldsCSharpType()
	{
		Dictionary<System.Type, ScriptFieldType> expected = new()
		{
			[typeof(bool)] = ScriptFieldType.Bool,
			[typeof(int)] = ScriptFieldType.Int32,
			[typeof(ulong)] = ScriptFieldType.UInt64,
			[typeof(string)] = ScriptFieldType.String,
			[typeof(Color)] = ScriptFieldType.Color,
			[typeof(Entity)] = ScriptFieldType.Entity,
			[typeof(AssetHandle)] = ScriptFieldType.Asset,
			[typeof(byte)] = ScriptFieldType.None,
			[typeof(Script)] = ScriptFieldType.None,
		};
		foreach ((System.Type type, ScriptFieldType fieldType) in expected)
		{
			Assert.Equal(fieldType, ScriptFieldCodec.GetFieldType(type));
		}
	}

	[Fact]
	public void EntitiesCompareByID()
	{
		Assert.Equal(new Entity(5), new Entity(5));
		Assert.True(new Entity(5) == new Entity(5));
		Assert.True(new Entity(5) != new Entity(6));
		Entity? none = null;
		Assert.True(none == null);
		Assert.False(new Entity(5) == none);
		Assert.Equal("Entity(5)", new Entity(5).ToString());
	}
}
