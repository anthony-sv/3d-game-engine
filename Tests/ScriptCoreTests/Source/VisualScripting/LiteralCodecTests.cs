using System;
using System.Buffers;
using System.Text.Json;
using Strada.Interop;
using Strada.VisualScripting;
using Xunit;

namespace Strada.ScriptCore.Tests;

[Flags]
public enum ProbeAccess
{
	None = 0,
	Read = 1,
	Write = 2,
	Execute = 4,
}

public enum ProbeWide : long
{
	Small = 1,
	Huge = 9000000000,
}

// Literals in documents (spec section 2.1) and the type defaults (spec section 2.2).
public sealed class LiteralCodecTests
{
	private static VisualType Type(string id) => VisualScriptTesting.Library.FindType(id) ?? throw new InvalidOperationException(id);

	private static VisualType EnumType(Type type, bool flags)
	{
		EnumeratorInfo[] enumerators = Array.ConvertAll(Enum.GetNames(type), name => new EnumeratorInfo(name, Enum.Parse(type, name)));
		return new VisualType(type.FullName!, type.Name, TypeKind.Enum, type) { HasLiteral = true, IsFlags = flags, Enumerators = enumerators };
	}

	private static DecodedLiteral Decode(VisualType type, string json)
	{
		using JsonDocument document = JsonDocument.Parse(json);
		return LiteralCodec.Decode(type, document.RootElement.Clone());
	}

	private static object? Value(string type, string json)
	{
		DecodedLiteral decoded = Decode(Type(type), json);
		Assert.True(decoded.IsValid, $"{json} as {type}: {decoded.Message}");
		return decoded.Value;
	}

	private static string Written(VisualType type, object? value)
	{
		ArrayBufferWriter<byte> buffer = new();
		using (Utf8JsonWriter writer = new(buffer))
		{
			LiteralCodec.Write(writer, type, value);
		}
		return System.Text.Encoding.UTF8.GetString(buffer.WrittenSpan);
	}

	[Fact]
	public void TypeDefaultsFollowTheTable()
	{
		Assert.Equal(false, LiteralCodec.DefaultOf(Type("bool")));
		Assert.Equal(0, LiteralCodec.DefaultOf(Type("int")));
		Assert.Equal(0u, LiteralCodec.DefaultOf(Type("uint")));
		Assert.Equal(0L, LiteralCodec.DefaultOf(Type("long")));
		Assert.Equal(0UL, LiteralCodec.DefaultOf(Type("ulong")));
		Assert.Equal(0.0f, LiteralCodec.DefaultOf(Type("float")));
		Assert.Equal(0.0, LiteralCodec.DefaultOf(Type("double")));
		Assert.Equal(string.Empty, LiteralCodec.DefaultOf(Type("string")));
		Assert.Equal(new Vector2(0.0f, 0.0f), LiteralCodec.DefaultOf(Type("Strada.Vector2")));
		Assert.Equal(new Vector3(0.0f, 0.0f, 0.0f), LiteralCodec.DefaultOf(Type("Strada.Vector3")));
		Assert.Equal(new Vector4(0.0f, 0.0f, 0.0f, 0.0f), LiteralCodec.DefaultOf(Type("Strada.Vector4")));
		Assert.Equal(new Quaternion(0.0f, 0.0f, 0.0f, 1.0f), LiteralCodec.DefaultOf(Type("Strada.Quaternion")));
		Assert.Equal(new Color(1.0f, 1.0f, 1.0f, 1.0f), LiteralCodec.DefaultOf(Type("Strada.Color")));
		Assert.Equal(Matrix4.Identity, LiteralCodec.DefaultOf(Type("Strada.Matrix4")));
		Assert.Equal(ForceMode.Force, LiteralCodec.DefaultOf(Type("Strada.ForceMode")));
		Assert.Equal(KeyCode.Unknown, LiteralCodec.DefaultOf(Type("Strada.KeyCode")));
		Assert.Equal(AssetHandle.Invalid, LiteralCodec.DefaultOf(Type("Strada.AssetHandle")));
		Assert.Equal(default(Ray), LiteralCodec.DefaultOf(Type("Strada.Ray")));
		Assert.Equal(default(RaycastHit), LiteralCodec.DefaultOf(Type("Strada.RaycastHit")));
		foreach (string id in new[] { "Strada.Entity", "Strada.Script", "Strada.Material", "Strada.Asset", "Strada.Prefab", "Strada.RigidBodyComponent",
			"Strada.Entity[]", "System.Object" })
		{
			Assert.Null(LiteralCodec.DefaultOf(Type(id)));
		}
	}

	[Fact]
	public void LiteralsDecodeByType()
	{
		Assert.Equal(true, Value("bool", "true"));
		Assert.Equal(-7, Value("int", "-7"));
		Assert.Equal(4294967295u, Value("uint", "4294967295"));
		Assert.Equal(-9000000000L, Value("long", "\"-9000000000\""));
		Assert.Equal(18446744073709551615UL, Value("ulong", "\"18446744073709551615\""));
		Assert.Equal(0.5f, Value("float", "0.5"));
		Assert.Equal(3.4028235e38f, Value("float", "3.4028235e38"));
		Assert.Equal(1e300, Value("double", "1e300"));
		Assert.Equal("text", Value("string", "\"text\""));
		Assert.Equal(new Vector2(1.0f, -2.0f), Value("Strada.Vector2", "[1, -2]"));
		Assert.Equal(new Vector3(1.0f, 2.0f, 3.0f), Value("Strada.Vector3", "[1.0, 2.0, 3.0]"));
		Assert.Equal(new Vector4(1.0f, 2.0f, 3.0f, 4.0f), Value("Strada.Vector4", "[1, 2, 3, 4]"));
		Assert.Equal(KeyCode.Space, Value("Strada.KeyCode", "\"Space\""));
		Assert.Equal(KeyCode.Space, Value("Strada.KeyCode", "32"));
		Assert.Equal((KeyCode)1000, Value("Strada.KeyCode", "1000"));
		Assert.Null(Value("Strada.Entity", "\"0\""));
		Assert.Null(Value("Strada.Material", "\"0\""));
		Assert.Equal(new AssetHandle(123), Value("Strada.AssetHandle", "\"123\""));
		Material material = Assert.IsType<Material>(Value("Strada.Material", "\"123\""));
		Assert.Equal(new AssetHandle(123), material.Handle);
		Assert.IsType<Prefab>(Value("Strada.Prefab", "\"5\""));
		Assert.IsType<EnvironmentMap>(Value("Strada.EnvironmentMap", "\"5\""));
	}

	[Fact]
	public void InvalidLiteralsAreRefusedNotCoerced()
	{
		(string Type, string Json)[] cases =
		[
			("int", "1.5"), ("int", "2147483648"), ("int", "\"1\""), ("uint", "-1"), ("long", "5"), ("long", "\"5.0\""), ("long", "\"+5\""),
			("ulong", "\"-1\""), ("ulong", "\"18446744073709551616\""), ("float", "1e39"), ("float", "\"1\""), ("double", "\"x\""), ("bool", "1"),
			("bool", "null"), ("string", "5"), ("Strada.Vector3", "[1, 2]"), ("Strada.Vector3", "[1, 2, \"x\"]"), ("Strada.Color", "{}"),
			("Strada.Matrix4", "[1, 2, 3]"), ("Strada.KeyCode", "\"Nope\""), ("Strada.KeyCode", "\"space\""), ("Strada.KeyCode", "1.5"),
			("Strada.Entity", "\"x\""), ("Strada.Entity", "0"), ("Strada.Material", "\"abc\""), ("Strada.Material", "123"), ("Strada.AssetHandle", "\"-1\""),
			("Strada.RigidBodyComponent", "\"0\""), ("Strada.RaycastHit", "[]"), ("System.Object", "1"),
		];
		foreach ((string type, string json) in cases)
		{
			DecodedLiteral decoded = Decode(Type(type), json);
			Assert.False(decoded.IsValid, $"{json} should not be a {type}");
			Assert.Equal(LiteralProblem.InvalidValue, decoded.Problem);
			Assert.NotNull(decoded.Message);
		}
		Assert.Equal(LiteralCodec.AssetHandleHint, Decode(Type("Strada.Mesh"), "\"asset://Meshes/Cube.glb\"").Message);
		Assert.Equal(LiteralCodec.AssetHandleHint, Decode(Type("Strada.AssetHandle"), "\"builtin://Cube\"").Message);
		DecodedLiteral entity = Decode(Type("Strada.Entity"), "\"4815162342\"");
		Assert.Equal(LiteralProblem.EntityValue, entity.Problem);
		Assert.Equal(LiteralCodec.EntityHint, entity.Message);
	}

	[Fact]
	public void QuaternionsAreNormalizedAndColorsAreNot()
	{
		Assert.Equal(new Quaternion(0.0f, 0.6f, 0.0f, 0.8f), Value("Strada.Quaternion", "[0, 3, 0, 4]"));
		// Unit quaternions keep their exact floats; a length below 1e-6 has no direction.
		Assert.Equal(new Quaternion(0.0f, 0.6f, 0.0f, 0.8f), Value("Strada.Quaternion", "[0, 0.6, 0, 0.8]"));
		foreach (string json in new[] { "[0, 0, 0, 0]", "[0, 1e-7, 0, 0]" })
		{
			DecodedLiteral zero = Decode(Type("Strada.Quaternion"), json);
			Assert.True(zero.IsValid);
			Assert.True(zero.WasZeroQuaternion);
			Assert.Equal(Quaternion.Identity, zero.Value);
		}
		Assert.Equal(new Color(2.0f, -1.0f, 0.5f, 1.0f), Value("Strada.Color", "[2, -1, 0.5, 1]"));
		Matrix4 matrix = Assert.IsType<Matrix4>(Value("Strada.Matrix4", "[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]"));
		Assert.Equal(new Vector4(1.0f, 2.0f, 3.0f, 4.0f), matrix.Column0);
		Assert.Equal(new Vector4(13.0f, 14.0f, 15.0f, 16.0f), matrix.Column3);
	}

	[Fact]
	public void FlagsAndWideEnumsHaveTheirForms()
	{
		// A [Flags] value is an array of names, also when it is one enumerator or none; a name alone is not a value.
		VisualType access = EnumType(typeof(ProbeAccess), true);
		Assert.Equal(ProbeAccess.Read | ProbeAccess.Write, Decode(access, "[\"Read\", \"Write\"]").Value);
		Assert.Equal(ProbeAccess.Write, Decode(access, "[\"Write\"]").Value);
		Assert.Equal(ProbeAccess.None, Decode(access, "[]").Value);
		Assert.Equal((ProbeAccess)8, Decode(access, "8").Value);
		Assert.False(Decode(access, "[\"Read\", \"Nope\"]").IsValid);
		DecodedLiteral name = Decode(access, "\"Write\"");
		Assert.False(name.IsValid);
		Assert.Equal("Strada.ScriptCore.Tests.ProbeAccess is a [Flags] enum: its values are arrays of enumerator names, such as [\"Write\"]", name.Message);
		Assert.False(Decode(access, "\"Nope\"").IsValid);
		Assert.Equal("[\"Read\",\"Write\"]", Written(access, ProbeAccess.Read | ProbeAccess.Write));
		Assert.Equal("[\"Write\"]", Written(access, ProbeAccess.Write));
		Assert.Equal("[]", Written(access, ProbeAccess.None));
		Assert.Equal("8", Written(access, (ProbeAccess)8));

		VisualType wide = EnumType(typeof(ProbeWide), false);
		Assert.Equal(ProbeWide.Huge, Decode(wide, "\"Huge\"").Value);
		Assert.Equal((ProbeWide)5, Decode(wide, "\"5\"").Value);
		Assert.False(Decode(wide, "5").IsValid);
		Assert.Equal("\"5\"", Written(wide, (ProbeWide)5));
	}

	[Fact]
	public void WrittenLiteralsDecodeToTheirValues()
	{
		(string Type, object? Value)[] cases =
		[
			("bool", true), ("int", -3), ("uint", 3000000000u), ("long", long.MinValue), ("ulong", ulong.MaxValue), ("float", 0.1f), ("double", 0.1),
			("string", "a \"quoted\" text"), ("Strada.Vector3", new Vector3(1.5f, -2.0f, 0.25f)), ("Strada.Quaternion", new Quaternion(0.0f, 0.6f, 0.0f, 0.8f)),
			("Strada.Color", new Color(0.5f, 0.25f, 1.0f, 1.0f)), ("Strada.Matrix4", Matrix4.Identity), ("Strada.ForceMode", ForceMode.Impulse),
			("Strada.KeyCode", (KeyCode)1000), ("Strada.Entity", null), ("Strada.AssetHandle", new AssetHandle(42)), ("Strada.Mesh", null),
		];
		foreach ((string id, object? value) in cases)
		{
			VisualType type = Type(id);
			DecodedLiteral decoded = Decode(type, Written(type, value));
			Assert.True(decoded.IsValid, $"{id}: {decoded.Message}");
			Assert.Equal(value, decoded.Value);
		}
		Assert.Equal("\"Impulse\"", Written(Type("Strada.ForceMode"), ForceMode.Impulse));
		Assert.Equal("\"-9000000000\"", Written(Type("long"), -9000000000L));
	}

	// Floats are read as the engine reads documents (VisualScriptSerializer::ReadFloat): the double the text denotes, narrowed
	// to float. So the shortest texts of ±7.038531e-26, which lie almost on the midpoint between two floats, read as the
	// neighbouring floats (0x15AE43FE, 0x95AE43FE), as they do in the engine (VisualScriptLiteralsTests pins its side), and
	// every float is written so that it reads back bit for bit: those two as their doubles' texts, -0 as "-0.0" ("-0" is the
	// integer 0 to the engine's JSON reader). Components of vectors are read and written the same way; library pin defaults
	// are written as literals are.
	[Fact]
	public void FloatsReadAsTheDoubleTheirTextDenotesAndAreWrittenToReadBackExactly()
	{
		Assert.Equal(0x15AE43FE, BitConverter.SingleToInt32Bits((float)Value("float", "7.038531e-26")!));
		Assert.Equal(unchecked((int)0x95AE43FE), BitConverter.SingleToInt32Bits((float)Value("float", "-7.038531e-26")!));
		Assert.Equal("7.038530691851209E-26", Written(Type("float"), BitConverter.Int32BitsToSingle(0x15AE43FD)));
		Assert.Equal("-7.038530691851209E-26", Written(Type("float"), BitConverter.Int32BitsToSingle(unchecked((int)0x95AE43FD))));
		Assert.Equal("-0.0", Written(Type("float"), -0.0f));
		Assert.Equal("0.5", Written(Type("float"), 0.5f));
		Assert.Equal("[0,-0.0,1.5]", Written(Type("Strada.Vector3"), new Vector3(0.0f, -0.0f, 1.5f)));

		foreach (int bits in (int[])[0x15AE43FD, unchecked((int)0x95AE43FD), unchecked((int)0x80000000), 0x15AE43FE, 0x00000001, 0x7F7FFFFF,
			0x3E4CCCCD])
		{
			float value = BitConverter.Int32BitsToSingle(bits);
			string text = Written(Type("float"), value);
			Assert.Equal(bits, BitConverter.SingleToInt32Bits((float)Value("float", text)!));
			// The engine's reader: the double the text denotes, narrowed.
			Assert.Equal(bits, BitConverter.SingleToInt32Bits((float)double.Parse(text, System.Globalization.CultureInfo.InvariantCulture)));
			Vector3 vector = (Vector3)Value("Strada.Vector3", Written(Type("Strada.Vector3"), new Vector3(0.0f, value, 0.0f)))!;
			Assert.Equal(bits, BitConverter.SingleToInt32Bits(vector.Y));

			// A library pin's default.
			ArrayBufferWriter<byte> buffer = new();
			using (Utf8JsonWriter writer = new(buffer))
			{
				LibraryWriter.WritePin(writer, new PinDescription("Amount", PinDirection.In, Type("float")) { HasDefault = true, Default = value },
					Type("float"));
			}
			using JsonDocument pin = JsonDocument.Parse(buffer.WrittenMemory);
			JsonElement written = pin.RootElement.GetProperty("Default");
			Assert.Equal(text, written.GetRawText());
			Assert.Equal(bits, BitConverter.SingleToInt32Bits((float)Value("float", written.GetRawText())!));
		}
		// Past float's largest value up to the bound, numbers narrow to it.
		Assert.Equal(float.MaxValue, Value("float", "3.40282349e38"));
	}

	[Fact]
	public void VariablesStartAtTheirValueOrTheTypeDefault()
	{
		AnalyzedDocument document = VisualScriptTesting.Analyze(VisualScriptTesting.Document("", variables: """
			{ "ID": 1, "Name": "Facing", "Type": "Strada.Quaternion", "Public": true },
			{ "ID": 2, "Name": "Tint", "Type": "Strada.Color", "Public": true },
			{ "ID": 3, "Name": "Push", "Type": "Strada.ForceMode", "Value": "Impulse", "Public": true },
			{ "ID": 4, "Name": "Big", "Type": "long", "Value": "-9000000000", "Public": true },
			{ "ID": 5, "Name": "Body", "Type": "Strada.RigidBodyComponent" },
			{ "ID": 6, "Name": "Label", "Type": "string" }
			"""));
		VisualScriptTesting.AssertNoErrors(document);
		Assert.Equal(Quaternion.Identity, document.Variables[0].Value);
		Assert.Equal(new Color(1.0f, 1.0f, 1.0f, 1.0f), document.Variables[1].Value);
		Assert.Equal(ForceMode.Impulse, document.Variables[2].Value);
		Assert.Equal(-9000000000L, document.Variables[3].Value);
		Assert.Null(document.Variables[4].Value);
		Assert.Equal(string.Empty, document.Variables[5].Value);
		Assert.Equal(ScriptFieldType.Int32, document.Variables[2].Type!.FieldType);

		AnalyzedDocument invalid = VisualScriptTesting.Analyze(VisualScriptTesting.Document("", variables: """
			{ "ID": 7, "Name": "Count", "Type": "int", "Value": 1.5 }, { "ID": 8, "Name": "Body", "Type": "Strada.RigidBodyComponent", "Value": "0" }
			"""));
		Diagnostic[] errors = [.. VisualScriptTesting.Errors(invalid)];
		Assert.Equal([(DiagnosticCode.InvalidValue, 7u), (DiagnosticCode.InvalidValue, 8u)], Array.ConvertAll(errors, static error => (error.Code, error.Variable!.Value)));
		Assert.All(errors, static error => Assert.Equal(0u, error.Function));
	}
}
