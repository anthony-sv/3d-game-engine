using System.Buffers;
using System.Collections.Generic;
using System.Linq;
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
			[typeof(VisualScript)] = ScriptFieldType.Asset,
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
	public void ListTypesAreTheirElementTypesWithTheListBit()
	{
		ScriptFieldType floats = ScriptFieldCodec.MakeListType(ScriptFieldType.Float);
		Assert.True(ScriptFieldCodec.IsList(floats));
		Assert.False(ScriptFieldCodec.IsList(ScriptFieldType.Float));
		Assert.NotEqual(ScriptFieldType.Float, floats);
		Assert.Equal(ScriptFieldType.Float, ScriptFieldCodec.GetElementType(floats));
		Assert.Equal(ScriptFieldType.Float, ScriptFieldCodec.GetElementType(ScriptFieldType.Float));
		Assert.Equal("Float[]", ScriptFieldCodec.GetTypeName(floats));
		Assert.Equal("Entity[]", ScriptFieldCodec.GetTypeName(ScriptFieldCodec.MakeListType(ScriptFieldType.Entity)));
		Assert.Equal("Float", ScriptFieldCodec.GetTypeName(ScriptFieldType.Float));
		// No lists of None and no lists of lists.
		Assert.Equal(ScriptFieldType.None, ScriptFieldCodec.MakeListType(ScriptFieldType.None));
		Assert.Equal(ScriptFieldType.None, ScriptFieldCodec.MakeListType(floats));
	}

	[Fact]
	public void ArraysAndListsOfTypesWithFieldTypesAreListFields()
	{
		Dictionary<System.Type, ScriptFieldType> expected = new()
		{
			[typeof(int[])] = ScriptFieldCodec.MakeListType(ScriptFieldType.Int32),
			[typeof(List<int>)] = ScriptFieldCodec.MakeListType(ScriptFieldType.Int32),
			[typeof(string[])] = ScriptFieldCodec.MakeListType(ScriptFieldType.String),
			[typeof(List<Entity>)] = ScriptFieldCodec.MakeListType(ScriptFieldType.Entity),
			[typeof(Prefab[])] = ScriptFieldCodec.MakeListType(ScriptFieldType.Prefab),
			[typeof(List<Mesh>)] = ScriptFieldCodec.MakeListType(ScriptFieldType.Asset),
			[typeof(AssetHandle[])] = ScriptFieldCodec.MakeListType(ScriptFieldType.Asset),
			[typeof(ByteEnum[])] = ScriptFieldCodec.MakeListType(ScriptFieldType.Int32),
			[typeof(List<UnsignedLongEnum>)] = ScriptFieldCodec.MakeListType(ScriptFieldType.UInt64),
			[typeof(float)] = ScriptFieldType.Float,
			// No arrays of arrays, of lists or of types without a field type, no arrays of more dimensions, no other collections.
			[typeof(int[][])] = ScriptFieldType.None,
			[typeof(List<int[]>)] = ScriptFieldType.None,
			[typeof(List<List<int>>)] = ScriptFieldType.None,
			[typeof(int[,])] = ScriptFieldType.None,
			[typeof(byte[])] = ScriptFieldType.None,
			[typeof(List<Script>)] = ScriptFieldType.None,
			[typeof(Matrix4[])] = ScriptFieldType.None,
			[typeof(Dictionary<string, int>)] = ScriptFieldType.None,
			[typeof(HashSet<int>)] = ScriptFieldType.None,
		};
		foreach ((System.Type type, ScriptFieldType fieldType) in expected)
		{
			Assert.Equal(fieldType, ScriptFieldCodec.GetMemberFieldType(type));
		}
		// A type of single values has no list field type: what graphs' library types are.
		Assert.Equal(ScriptFieldType.None, ScriptFieldCodec.GetFieldType(typeof(int[])));
	}

	[Theory]
	[MemberData(nameof(Values))]
	public void ListValuesRoundTripInTheSceneFormat(string typeName, object? value, string json)
	{
		ScriptFieldType type = ScriptFieldCodec.MakeListType(System.Enum.Parse<ScriptFieldType>(typeName));
		List<object?> list = [value, value];
		Assert.Equal($"[{json},{json}]", Write(type, list));
		Assert.Equal(list, Assert.IsType<List<object?>>(Read(type, $"[{json},{json}]", out string? error)));
		Assert.Null(error);
		Assert.Equal("[]", Write(type, new List<object?>()));
		Assert.Empty(Assert.IsType<List<object?>>(Read(type, "[]", out error)));
		Assert.Null(error);
	}

	[Fact]
	public void ListsRefuseValuesThatAreNoListsOfTheirElements()
	{
		ScriptFieldType counts = ScriptFieldCodec.MakeListType(ScriptFieldType.Int32);
		Assert.Null(Read(counts, "5", out string? error));
		Assert.Equal("expected an array of Int32 values, not 5", error);
		Assert.Null(Read(counts, "[1,\"x\"]", out error));
		Assert.Equal("element 1: expected a Int32 value, not \"x\"", error);
		string longest = $"[{string.Join(",", Enumerable.Repeat("1", ScriptFieldCodec.MaxListElements))}]";
		Assert.Equal(ScriptFieldCodec.MaxListElements, Assert.IsType<List<object?>>(Read(counts, longest, out error)).Count);
		Assert.Null(error);
		Assert.Null(Read(counts, $"[{string.Join(",", Enumerable.Repeat("1", ScriptFieldCodec.MaxListElements + 1))}]", out error));
		Assert.Equal("a list holds at most 4096 elements, not 4097", error);
	}

	[Fact]
	public void ListsAreStoredAsTheirElementsAndBecomeArraysAndListsOfTheFieldsType()
	{
		ScriptFieldType levels = ScriptFieldCodec.MakeListType(ScriptFieldType.Int32);
		// Arrays and List<T> are stored alike; a null one is the empty list.
		Assert.Equal(new List<object?> { 1, 200 }, ScriptFieldCodec.ToStored(new[] { ByteEnum.Low, ByteEnum.High }, levels));
		Assert.Equal(new List<object?> { 1, 200 }, ScriptFieldCodec.ToStored(new List<ByteEnum> { ByteEnum.Low, ByteEnum.High }, levels));
		Assert.Empty(Assert.IsType<List<object?>>(ScriptFieldCodec.ToStored(null, levels)));
		ScriptFieldType shapes = ScriptFieldCodec.MakeListType(ScriptFieldType.Asset);
		Assert.Equal(new List<object?> { new AssetHandle(4), AssetHandle.Invalid },
			ScriptFieldCodec.ToStored(new Mesh?[] { new Mesh(new AssetHandle(4)), null }, shapes));

		// A stored list becomes a new array or List<T> of the field's element type.
		ByteEnum[] array = Assert.IsType<ByteEnum[]>(ScriptFieldCodec.FromStored(new List<object?> { 200, 1 }, typeof(ByteEnum[]), out string? error));
		Assert.Null(error);
		Assert.Equal([ByteEnum.High, ByteEnum.Low], array);
		List<Mesh?> meshes = Assert.IsType<List<Mesh?>>(
			ScriptFieldCodec.FromStored(new List<object?> { new AssetHandle(4), AssetHandle.Invalid }, typeof(List<Mesh?>), out error));
		Assert.Null(error);
		Assert.Equal(2, meshes.Count);
		Assert.Equal(new AssetHandle(4), meshes[0]!.Handle);
		Assert.Null(meshes[1]);
		// An element that does not fit is named.
		Assert.Null(ScriptFieldCodec.FromStored(new List<object?> { 1, 300 }, typeof(ByteEnum[]), out error));
		Assert.Equal("element 1: 300 is out of the range of ByteEnum (Byte)", error);
	}

	[Fact]
	public void NativeStructuresMatchTheEngineLayouts()
	{
		// ScriptBindingsAssets.cpp and ScriptBindingsRuntime.cpp declare the same layouts.
		Assert.Equal(120, Unsafe.SizeOf<NativeMaterialValues>());
		Assert.Equal(64, (int)Marshal.OffsetOf<NativeMaterialValues>(nameof(NativeMaterialValues.BaseColorTexture)));
		Assert.Equal(104, (int)Marshal.OffsetOf<NativeMaterialValues>(nameof(NativeMaterialValues.AlphaMode)));
		Assert.Equal(48, Unsafe.SizeOf<NativeRaycastHit>());
		Assert.Equal(32, (int)Marshal.OffsetOf<NativeRaycastHit>(nameof(NativeRaycastHit.Distance)));
		Assert.Equal(40, (int)Marshal.OffsetOf<NativeRaycastHit>(nameof(NativeRaycastHit.Material)));
		Assert.Equal(24, Unsafe.SizeOf<NativeQueryShape>());
		Assert.Equal(8, (int)Marshal.OffsetOf<NativeQueryShape>(nameof(NativeQueryShape.HalfHeight)));
		Assert.Equal(12, (int)Marshal.OffsetOf<NativeQueryShape>(nameof(NativeQueryShape.HalfExtents)));
		// PhysicsQueryShapeType's values (static_asserted beside the engine's layout).
		Assert.Equal([0, 1, 2], new[] { NativeQueryShape.Sphere, NativeQueryShape.Box, NativeQueryShape.Capsule });
		Assert.Equal(32, Unsafe.SizeOf<NativeCollision>());
		Assert.Equal(16, (int)Marshal.OffsetOf<NativeCollision>(nameof(NativeCollision.Step)));
		Assert.Equal(24, (int)Marshal.OffsetOf<NativeCollision>(nameof(NativeCollision.PointCount)));
		Assert.Equal(56, Unsafe.SizeOf<NativeContactPoint>());
		Assert.Equal(36, (int)Marshal.OffsetOf<NativeContactPoint>(nameof(NativeContactPoint.Impulse)));
		Assert.Equal(40, (int)Marshal.OffsetOf<NativeContactPoint>(nameof(NativeContactPoint.Material)));
		Assert.Equal(48, (int)Marshal.OffsetOf<NativeContactPoint>(nameof(NativeContactPoint.OtherMaterial)));
		// Collision_Read writes up to the engine's MaxContactPoints (4, static_asserted beside its layout) into a buffer of this
		// many.
		Assert.Equal(4, NativeContactPoint.MaxCount);
		Assert.Equal(48, Unsafe.SizeOf<NativePhysicsMaterial>());
		Assert.Equal(24, (int)Marshal.OffsetOf<NativePhysicsMaterial>(nameof(NativePhysicsMaterial.Friction)));
		Assert.Equal(36, (int)Marshal.OffsetOf<NativePhysicsMaterial>(nameof(NativePhysicsMaterial.FrictionCombine)));
		Assert.Equal(44, (int)Marshal.OffsetOf<NativePhysicsMaterial>(nameof(NativePhysicsMaterial.SurfaceLength)));
		Assert.Equal(24, Unsafe.SizeOf<NativeCurveHeader>());
		Assert.Equal(8, (int)Marshal.OffsetOf<NativeCurveHeader>(nameof(NativeCurveHeader.KeyCount)));
		Assert.Equal(16, (int)Marshal.OffsetOf<NativeCurveHeader>(nameof(NativeCurveHeader.PostExtrapolation)));
		Assert.Equal(20, Unsafe.SizeOf<NativeCurveKey>());
		Assert.Equal(8, (int)Marshal.OffsetOf<NativeCurveKey>(nameof(NativeCurveKey.In)));
		Assert.Equal(16, (int)Marshal.OffsetOf<NativeCurveKey>(nameof(NativeCurveKey.Interpolation)));
		Assert.Equal(16, Unsafe.SizeOf<NativeMixerHeader>());
		Assert.Equal(8, (int)Marshal.OffsetOf<NativeMixerHeader>(nameof(NativeMixerHeader.BusCount)));
		Assert.Equal(32, Unsafe.SizeOf<NativeMixerBus>());
		Assert.Equal(16, (int)Marshal.OffsetOf<NativeMixerBus>(nameof(NativeMixerBus.NameLength)));
		Assert.Equal(24, (int)Marshal.OffsetOf<NativeMixerBus>(nameof(NativeMixerBus.VolumeDb)));
		Assert.Equal(28, (int)Marshal.OffsetOf<NativeMixerBus>(nameof(NativeMixerBus.Muted)));
		Assert.Equal(16, Unsafe.SizeOf<NativeRebindAnswer>());
		Assert.Equal(12, (int)Marshal.OffsetOf<NativeRebindAnswer>(nameof(NativeRebindAnswer.ConflictCount)));
		// InputRebindStatus's values (static_asserted beside the engine's layout), and what no scene answers.
		RebindStatus[] statuses = [RebindStatus.None, RebindStatus.Listening, RebindStatus.Done, RebindStatus.Cancelled, RebindStatus.Refused];
		Assert.Equal([0, 1, 2, 3, 4], statuses.Select(static status => (int)status));
		Assert.Equal(-1, NativeRebindAnswer.NoScene);
	}

	[Fact]
	public void AssetClassesMapToTheEngineTypes()
	{
		// By the engine's names of their types (AssetTypeToString); the built-in types also by their fixed values, which
		// need no engine (data asset types' values are the engine's to give).
		(System.Type Class, string Name, NativeAssetType BuiltIn)[] classes = [(typeof(Prefab), "Prefab", NativeAssetType.Prefab),
			(typeof(Mesh), "Mesh", NativeAssetType.Mesh), (typeof(Material), "Material", NativeAssetType.Material),
			(typeof(Texture), "Texture", NativeAssetType.Texture), (typeof(EnvironmentMap), "Environment", NativeAssetType.Environment),
			(typeof(AudioClip), "AudioClip", NativeAssetType.AudioClip), (typeof(Font), "Font", NativeAssetType.Font),
			(typeof(VisualScript), "VisualScript", NativeAssetType.VisualScript), (typeof(Curve), "Curve", NativeAssetType.None),
			(typeof(InputActions), "InputActions", NativeAssetType.None), (typeof(Mixer), "Mixer", NativeAssetType.None),
			(typeof(PhysicsMaterial), "PhysicsMaterial", NativeAssetType.None)];
		foreach ((System.Type assetClass, string name, NativeAssetType builtIn) in classes)
		{
			Assert.Equal(name, AssetTypes.GetTypeName(assetClass));
			if (builtIn != NativeAssetType.None)
			{
				Assert.Equal((int)builtIn, AssetTypes.GetNativeType(assetClass));
			}
			Asset asset = AssetTypes.Create(name, new AssetHandle(3));
			Assert.IsType(assetClass, asset);
			Assert.Equal(new AssetHandle(3), asset.Handle);
		}
		Assert.Null(AssetTypes.GetTypeName(typeof(Asset)));
		Assert.Equal((int)NativeAssetType.None, AssetTypes.GetNativeType(typeof(Asset)));
		Assert.Throws<System.ArgumentOutOfRangeException>(() => AssetTypes.Create("Scene", new AssetHandle(3)));
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
		Assert.Equal("Mesh", ScriptFieldCodec.GetAssetType(typeof(Mesh)));
		Assert.Equal("Curve", ScriptFieldCodec.GetAssetType(typeof(Curve)));
		Assert.Equal(new Curve(new AssetHandle(6)), ScriptFieldCodec.FromStored(new AssetHandle(6), typeof(Curve), out error));
		Assert.Equal("InputActions", ScriptFieldCodec.GetAssetType(typeof(InputActions)));
		Assert.Equal(new InputActions(new AssetHandle(8)), ScriptFieldCodec.FromStored(new AssetHandle(8), typeof(InputActions), out error));
		Assert.Equal("Mixer", ScriptFieldCodec.GetAssetType(typeof(Mixer)));
		Assert.Equal(new Mixer(new AssetHandle(8)), ScriptFieldCodec.FromStored(new AssetHandle(8), typeof(Mixer), out error));
		Assert.Equal("PhysicsMaterial", ScriptFieldCodec.GetAssetType(typeof(PhysicsMaterial)));
		Assert.Equal(new PhysicsMaterial(new AssetHandle(7)), ScriptFieldCodec.FromStored(new AssetHandle(7), typeof(PhysicsMaterial), out error));
		Assert.Null(ScriptFieldCodec.GetAssetType(typeof(Prefab)));
		Assert.Null(ScriptFieldCodec.GetAssetType(typeof(AssetHandle)));
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
