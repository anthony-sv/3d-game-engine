using System.Buffers;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
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
	public void PrefabFieldsHoldHandlesAndNoPrefabIsZero()
	{
		Assert.Equal("\"9\"", Write(ScriptFieldType.Prefab, new Prefab(new AssetHandle(9))));
		Assert.Equal("\"0\"", Write(ScriptFieldType.Prefab, null));
		Assert.Equal(new Prefab(new AssetHandle(9)), Read(ScriptFieldType.Prefab, "\"9\"", out _));
		Assert.Null(Read(ScriptFieldType.Prefab, "\"0\"", out string? error));
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
			[typeof(Prefab)] = ScriptFieldType.Prefab,
			[typeof(AssetHandle)] = ScriptFieldType.Asset,
			[typeof(Mesh)] = ScriptFieldType.Asset,
			[typeof(AudioClip)] = ScriptFieldType.Asset,
			[typeof(MaterialAlphaMode)] = ScriptFieldType.Int32,
			[typeof(ByteEnum)] = ScriptFieldType.Int32,
			[typeof(UnsignedEnum)] = ScriptFieldType.UInt32,
			[typeof(LongEnum)] = ScriptFieldType.Int64,
			[typeof(UnsignedLongEnum)] = ScriptFieldType.UInt64,
			[typeof(byte)] = ScriptFieldType.None,
			[typeof(Script)] = ScriptFieldType.None,
		};
		foreach ((System.Type type, ScriptFieldType fieldType) in expected)
		{
			Assert.Equal(fieldType, ScriptFieldCodec.GetFieldType(type));
		}
	}

	[Fact]
	public void NativeStructuresMatchTheEngineLayouts()
	{
		// ScriptBindingsAssets.cpp and ScriptBindingsRuntime.cpp declare the same layouts.
		Assert.Equal(120, Unsafe.SizeOf<NativeMaterialValues>());
		Assert.Equal(64, (int)Marshal.OffsetOf<NativeMaterialValues>(nameof(NativeMaterialValues.BaseColorTexture)));
		Assert.Equal(104, (int)Marshal.OffsetOf<NativeMaterialValues>(nameof(NativeMaterialValues.AlphaMode)));
		Assert.Equal(40, Unsafe.SizeOf<NativeRaycastHit>());
	}

	[Fact]
	public void AssetClassesMapToTheEngineTypes()
	{
		System.Type[] classes = [typeof(Prefab), typeof(Mesh), typeof(Material), typeof(Texture), typeof(EnvironmentMap),
			typeof(AudioClip), typeof(Font)];
		foreach (System.Type assetClass in classes)
		{
			NativeAssetType type = AssetTypes.GetNativeType(assetClass);
			Assert.NotEqual(NativeAssetType.None, type);
			Asset asset = AssetTypes.Create(type, new AssetHandle(3));
			Assert.IsType(assetClass, asset);
			Assert.Equal(new AssetHandle(3), asset.Handle);
		}
		Assert.Equal(NativeAssetType.None, AssetTypes.GetNativeType(typeof(Asset)));
		Assert.Equal(new Mesh(new AssetHandle(3)), new Mesh(new AssetHandle(3)));
		Assert.False(new Mesh(new AssetHandle(3)).Equals(new Material(new AssetHandle(3))));
	}

	private enum ByteEnum : byte
	{
		Low = 1,
		High = 200,
	}

	private enum UnsignedEnum : uint
	{
		Top = uint.MaxValue,
	}

	private enum LongEnum : long
	{
		Negative = -5000000000,
	}

	private enum UnsignedLongEnum : ulong
	{
		Top = ulong.MaxValue,
	}

	[Fact]
	public void EnumsAreStoredAsIntegersOfTheirSize()
	{
		Assert.Equal(200, ScriptFieldCodec.ToStored(ByteEnum.High, ScriptFieldType.Int32));
		Assert.Equal(uint.MaxValue, ScriptFieldCodec.ToStored(UnsignedEnum.Top, ScriptFieldType.UInt32));
		Assert.Equal(-5000000000L, ScriptFieldCodec.ToStored(LongEnum.Negative, ScriptFieldType.Int64));
		Assert.Equal(ulong.MaxValue, ScriptFieldCodec.ToStored(UnsignedLongEnum.Top, ScriptFieldType.UInt64));
		Assert.Equal("\"-5000000000\"", Write(ScriptFieldType.Int64, ScriptFieldCodec.ToStored(LongEnum.Negative, ScriptFieldType.Int64)));

		Assert.Equal(ByteEnum.High, ScriptFieldCodec.FromStored(200, typeof(ByteEnum), out string? error));
		Assert.Null(error);
		Assert.Equal(LongEnum.Negative, ScriptFieldCodec.FromStored(-5000000000L, typeof(LongEnum), out error));
		Assert.Equal(UnsignedLongEnum.Top, ScriptFieldCodec.FromStored(ulong.MaxValue, typeof(UnsignedLongEnum), out error));
		// Values without an enumerator are kept; values out of the enum's range are not.
		Assert.Equal((ByteEnum)7, ScriptFieldCodec.FromStored(7, typeof(ByteEnum), out error));
		Assert.Null(ScriptFieldCodec.FromStored(300, typeof(ByteEnum), out error));
		Assert.Equal("300 is out of the range of ByteEnum (Byte)", error);
		Assert.Null(ScriptFieldCodec.FromStored(-1, typeof(ByteEnum), out error));
		Assert.NotNull(error);
	}

	[Fact]
	public void TypedAssetReferencesAreStoredAsHandles()
	{
		Assert.Equal(new AssetHandle(5), ScriptFieldCodec.ToStored(new Mesh(new AssetHandle(5)), ScriptFieldType.Asset));
		Assert.Equal(AssetHandle.Invalid, ScriptFieldCodec.ToStored(null, ScriptFieldType.Asset));
		Assert.Equal(new Mesh(new AssetHandle(5)), ScriptFieldCodec.FromStored(new AssetHandle(5), typeof(Mesh), out string? error));
		Assert.Null(error);
		Assert.Null(ScriptFieldCodec.FromStored(AssetHandle.Invalid, typeof(Material), out error));
		Assert.Equal(new AssetHandle(5), ScriptFieldCodec.FromStored(new AssetHandle(5), typeof(AssetHandle), out error));
		Assert.Equal(NativeAssetType.Mesh, ScriptFieldCodec.GetAssetType(typeof(Mesh)));
		Assert.Equal(NativeAssetType.None, ScriptFieldCodec.GetAssetType(typeof(Prefab)));
		Assert.Equal(NativeAssetType.None, ScriptFieldCodec.GetAssetType(typeof(AssetHandle)));
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
