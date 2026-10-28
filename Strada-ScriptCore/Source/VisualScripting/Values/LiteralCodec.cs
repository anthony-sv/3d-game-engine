using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Text.Json;
using Strada.Interop;

namespace Strada.VisualScripting;

internal enum LiteralProblem
{
	None,
	// Not a valid literal of the type (Error InvalidValue).
	InvalidValue,
	// An entity other than "0" (Error EntityValue).
	EntityValue,
}

// A decoded literal: the value (boxed as the type's CLR type; null for no entity or asset), or what is wrong with it. Index
// is the element of an array literal the problem is in, -1 for the literal as a whole.
internal readonly record struct DecodedLiteral(object? Value, LiteralProblem Problem, string? Message, bool WasZeroQuaternion, int Index = -1)
{
	internal bool IsValid => Problem == LiteralProblem.None;

	internal static DecodedLiteral Of(object? value) => new(value, LiteralProblem.None, null, false);

	internal static DecodedLiteral Invalid(string message) => new(null, LiteralProblem.InvalidValue, message, false);

	// The problem as a message about the value of what ("Speeds: expected ...", "Value[2]: expected ...").
	internal string Describe(string what) =>
		Index >= 0 ? $"{what}[{Index.ToString(CultureInfo.InvariantCulture)}]: {Message}" : $"{what}: {Message}";
}

// Literals in documents (spec section 2.1) and type defaults (spec section 2.2): the single place that decodes and writes
// the values of variables, locals, Inputs, Literal nodes, switch cases and pin defaults.
internal static class LiteralCodec
{
	internal const string AssetHandleHint = "asset values in graphs are decimal handles: look the handle up with asset.list";
	internal const string EntityHint = "graphs are shared assets: expose a public Entity variable and set it on the component";
	// Spec-slice3 section 3.2.
	internal const int MaxArrayLiteralElements = 4096;
	// The largest magnitude of a float literal.
	private const double MaxFloat = 3.4028235e38;
	// The engine's quaternion decoder: shorter quaternions have no direction, and unit ones within the tolerance stay as they
	// are (JsonTraits<glm::quat>).
	private const double MinQuaternionLength = 1e-6;
	private const double UnitQuaternionTolerance = 1e-5;

	// Whether values of the CLR type have a literal form: arrays have one when their elements do (spec-slice3 section 3.4.4).
	internal static bool HasLiteralForm(Type type)
	{
		if (type.IsSZArray)
		{
			return type.GetElementType() is { IsArray: false } element && HasLiteralForm(element);
		}
		if (type.IsEnum || type == typeof(bool) || type == typeof(int) || type == typeof(uint) || type == typeof(long) || type == typeof(ulong) ||
			type == typeof(float) || type == typeof(double) || type == typeof(string) || type == typeof(Vector2) || type == typeof(Vector3) ||
			type == typeof(Vector4) || type == typeof(Quaternion) || type == typeof(Color) || type == typeof(Matrix4) || type == typeof(Entity) ||
			type == typeof(AssetHandle))
		{
			return true;
		}
		return AssetTypes.GetNativeType(type) != NativeAssetType.None;
	}

	internal static DecodedLiteral Decode(VisualType type, JsonElement json)
	{
		Type? clrType = type.ClrType;
		if (clrType == null)
		{
			return DecodedLiteral.Invalid(type.IsExec ? "an exec pin takes no value" : "the pin's type is not resolved");
		}
		if (json.ValueKind == JsonValueKind.Null)
		{
			return DecodedLiteral.Invalid("null is not a value: leave the value out to use the default");
		}
		if (!type.HasLiteral)
		{
			return DecodedLiteral.Invalid($"{type.Id} has no literal form: connect the pin instead");
		}
		if (type.Kind == TypeKind.Array)
		{
			return DecodeArray(type, json);
		}
		if (clrType.IsEnum)
		{
			return DecodeEnum(type, json);
		}
		if (clrType == typeof(bool))
		{
			return json.ValueKind is JsonValueKind.True or JsonValueKind.False ? DecodedLiteral.Of(json.GetBoolean()) : Expected("true or false", json);
		}
		if (clrType == typeof(int))
		{
			return json.ValueKind == JsonValueKind.Number && json.TryGetInt32(out int value) ? DecodedLiteral.Of(value)
				: Expected($"an integer from {Format(int.MinValue)} to {Format(int.MaxValue)}", json);
		}
		if (clrType == typeof(uint))
		{
			return json.ValueKind == JsonValueKind.Number && json.TryGetUInt32(out uint value) ? DecodedLiteral.Of(value)
				: Expected($"an integer from 0 to {Format(uint.MaxValue)}", json);
		}
		if (clrType == typeof(long))
		{
			return JsonText.TryGetString(json, out string? text) && IsDecimal(text, true) &&
				long.TryParse(text, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out long value)
				? DecodedLiteral.Of(value)
				: Expected("a decimal string within the range of long, such as \"-9000000000\"", json);
		}
		if (clrType == typeof(ulong))
		{
			return JsonText.TryGetString(json, out string? text) && IsDecimal(text, false) &&
				ulong.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out ulong value)
				? DecodedLiteral.Of(value)
				: Expected("a decimal string within the range of ulong, such as \"5000000000\"", json);
		}
		if (clrType == typeof(float))
		{
			return TryReadFloat(json, out float value) ? DecodedLiteral.Of(value) : Expected("a finite number of at most 3.4028235e38", json);
		}
		if (clrType == typeof(double))
		{
			return json.ValueKind == JsonValueKind.Number && json.TryGetDouble(out double value) && double.IsFinite(value)
				? DecodedLiteral.Of(value)
				: Expected("a finite number", json);
		}
		if (clrType == typeof(string))
		{
			return JsonText.TryGetString(json, out string? text) ? DecodedLiteral.Of(text) : Expected("a string", json);
		}
		if (clrType == typeof(Entity))
		{
			return DecodeEntity(json);
		}
		if (clrType == typeof(AssetHandle) || AssetTypes.GetNativeType(clrType) != NativeAssetType.None)
		{
			return DecodeAsset(clrType, json);
		}
		return DecodeStruct(clrType, json);
	}

	// Writes a value of the type in its literal form (the type must have one). Non-finite numbers are written as 0.
	internal static void Write(Utf8JsonWriter writer, VisualType type, object? value)
	{
		Type clrType = type.ClrType ?? typeof(object);
		switch (value)
		{
			case null when clrType == typeof(string):
				writer.WriteStringValue(string.Empty);
				break;
			case null when type.Kind == TypeKind.Array:
				writer.WriteStartArray();
				writer.WriteEndArray();
				break;
			case Array array when type.Element is { } element:
				// Element by element, in the canonical forms of the element type.
				writer.WriteStartArray();
				for (int index = 0; index < array.Length; index++)
				{
					Write(writer, element, array.GetValue(index));
				}
				writer.WriteEndArray();
				break;
			case null:
				// No entity, no asset.
				writer.WriteStringValue("0");
				break;
			case Enum enumerator:
				WriteEnum(writer, type, enumerator);
				break;
			case bool flag:
				writer.WriteBooleanValue(flag);
				break;
			case int number:
				writer.WriteNumberValue(number);
				break;
			case uint number:
				writer.WriteNumberValue(number);
				break;
			case long number:
				writer.WriteStringValue(Format(number));
				break;
			case ulong number:
				writer.WriteStringValue(number.ToString(CultureInfo.InvariantCulture));
				break;
			case float number:
				WriteFloat(writer, number);
				break;
			case double number:
				writer.WriteNumberValue(double.IsFinite(number) ? number : 0.0);
				break;
			case string text:
				writer.WriteStringValue(text);
				break;
			case Vector2 vector:
				WriteFloats(writer, vector.X, vector.Y);
				break;
			case Vector3 vector:
				WriteFloats(writer, vector.X, vector.Y, vector.Z);
				break;
			case Vector4 vector:
				WriteFloats(writer, vector.X, vector.Y, vector.Z, vector.W);
				break;
			case Quaternion quaternion:
				WriteFloats(writer, quaternion.X, quaternion.Y, quaternion.Z, quaternion.W);
				break;
			case Color color:
				WriteFloats(writer, color.R, color.G, color.B, color.A);
				break;
			case Matrix4 matrix:
				writer.WriteStartArray();
				foreach (Vector4 column in new[] { matrix.Column0, matrix.Column1, matrix.Column2, matrix.Column3 })
				{
					WriteFloat(writer, column.X);
					WriteFloat(writer, column.Y);
					WriteFloat(writer, column.Z);
					WriteFloat(writer, column.W);
				}
				writer.WriteEndArray();
				break;
			case AssetHandle handle:
				writer.WriteStringValue(handle.ID.ToString(CultureInfo.InvariantCulture));
				break;
			case Asset asset:
				writer.WriteStringValue(asset.Handle.ID.ToString(CultureInfo.InvariantCulture));
				break;
			case Entity entity:
				writer.WriteStringValue(entity.ID.ToString(CultureInfo.InvariantCulture));
				break;
			default:
				throw new ArgumentException($"{clrType.FullName} has no literal form", nameof(value));
		}
	}

	// The type default (spec section 2.2); an array's is its empty array (spec-slice3 section 3.4.2).
	internal static object? DefaultOf(VisualType type)
	{
		Type? clrType = type.ClrType;
		if (type.Kind == TypeKind.Array && clrType != null)
		{
			return type.EmptyArray ?? Array.CreateInstance(clrType.GetElementType()!, 0);
		}
		if (clrType == null || !clrType.IsValueType)
		{
			return clrType == typeof(string) ? string.Empty : null;
		}
		if (clrType.IsEnum)
		{
			return Enum.ToObject(clrType, 0);
		}
		if (clrType == typeof(Quaternion))
		{
			return Quaternion.Identity;
		}
		if (clrType == typeof(Color))
		{
			return new Color(1.0f, 1.0f, 1.0f, 1.0f);
		}
		if (clrType == typeof(Matrix4))
		{
			return Matrix4.Identity;
		}
		// Every other value type (numbers, bool, vectors, AssetHandle, structs) is zero: default(T).
		return RuntimeHelpers.GetUninitializedObject(clrType);
	}

	private static string Format(long value) => value.ToString(CultureInfo.InvariantCulture);

	// An enumerator name; a [Flags] enum's value is an array of names instead ([] is 0). Integers (decimal strings for long and
	// ulong enums), the form of values without an enumerator, are read for any value.
	private static DecodedLiteral DecodeEnum(VisualType type, JsonElement json)
	{
		Type enumType = type.ClrType!;
		TypeCode underlying = Type.GetTypeCode(Enum.GetUnderlyingType(enumType));
		bool wide = underlying is TypeCode.Int64 or TypeCode.UInt64;
		if (JsonText.TryGetString(json, out string? text))
		{
			if (!type.IsFlags && FindEnumerator(type, text) is { } enumerator)
			{
				return DecodedLiteral.Of(enumerator.Value);
			}
			if (wide && TryParseWide(text, underlying, out object? wideValue))
			{
				return DecodedLiteral.Of(Enum.ToObject(enumType, wideValue));
			}
			if (type.IsFlags)
			{
				string example = FindEnumerator(type, text) != null ? $", such as [{JsonText.Quote(text)}]" : string.Empty;
				return DecodedLiteral.Invalid($"{type.Id} is a [Flags] enum: its values are arrays of enumerator names{example}");
			}
			return DecodedLiteral.Invalid($"{JsonText.Quote(text)} is not an enumerator of {type.Id}");
		}
		if (json.ValueKind == JsonValueKind.Number && !wide)
		{
			if (!json.TryGetInt64(out long number))
			{
				return Expected($"an enumerator name of {type.Id} or an integer", json);
			}
			return FitsIn(number, underlying) ? DecodedLiteral.Of(Enum.ToObject(enumType, number))
				: DecodedLiteral.Invalid($"{JsonText.Excerpt(json)} is out of the range of {type.Id}");
		}
		if (json.ValueKind == JsonValueKind.Array && type.IsFlags)
		{
			ulong bits = 0;
			foreach (JsonElement element in json.EnumerateArray())
			{
				EnumeratorInfo? match = JsonText.TryGetString(element, out string? name) ? FindEnumerator(type, name) : null;
				if (match == null)
				{
					return DecodedLiteral.Invalid($"{JsonText.Excerpt(element)} is not an enumerator of {type.Id}");
				}
				bits |= ToBits(match.Value);
			}
			return DecodedLiteral.Of(Enum.ToObject(enumType, bits));
		}
		string form = type.IsFlags ? "an array of enumerator names" : "an enumerator name";
		string number64 = wide ? "a decimal string" : "an integer";
		return Expected($"{form} of {type.Id} or {number64}", json);
	}

	// A JSON array of literals of the element type (spec-slice3 section 3.4.4), at most MaxArrayLiteralElements of them. The
	// first element that does not decode is the problem, with its index; [] is the type's empty array.
	private static DecodedLiteral DecodeArray(VisualType type, JsonElement json)
	{
		VisualType element = type.Element!;
		if (json.ValueKind != JsonValueKind.Array)
		{
			return Expected($"an array of {element.Id} values", json);
		}
		int length = json.GetArrayLength();
		if (length > MaxArrayLiteralElements)
		{
			return DecodedLiteral.Invalid($"an array literal has at most {Format(MaxArrayLiteralElements)} elements");
		}
		if (length == 0)
		{
			return DecodedLiteral.Of(DefaultOf(type));
		}
		Array values = Array.CreateInstance(element.ClrType!, length);
		bool wasZeroQuaternion = false;
		int index = 0;
		foreach (JsonElement item in json.EnumerateArray())
		{
			DecodedLiteral decoded = Decode(element, item);
			if (!decoded.IsValid)
			{
				return decoded with { Index = index };
			}
			wasZeroQuaternion |= decoded.WasZeroQuaternion;
			values.SetValue(decoded.Value, index++);
		}
		return new DecodedLiteral(values, LiteralProblem.None, null, wasZeroQuaternion);
	}

	private static DecodedLiteral DecodeEntity(JsonElement json)
	{
		if (JsonText.TryGetString(json, out string? text) && IsDecimal(text, false))
		{
			return text.TrimStart('0').Length == 0 ? DecodedLiteral.Of(null) : new DecodedLiteral(null, LiteralProblem.EntityValue, EntityHint, false);
		}
		return Expected("\"0\" (no entity)", json);
	}

	private static DecodedLiteral DecodeAsset(Type clrType, JsonElement json)
	{
		if (!JsonText.TryGetString(json, out string? text))
		{
			return Expected("a decimal asset handle (\"0\" for none)", json);
		}
		if (text.StartsWith("asset://", StringComparison.Ordinal) || text.StartsWith("builtin://", StringComparison.Ordinal))
		{
			return DecodedLiteral.Invalid(AssetHandleHint);
		}
		if (!IsDecimal(text, false) || !ulong.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out ulong id))
		{
			return Expected("a decimal asset handle (\"0\" for none)", json);
		}
		AssetHandle handle = new(id);
		if (clrType == typeof(AssetHandle))
		{
			return DecodedLiteral.Of(handle);
		}
		return DecodedLiteral.Of(handle.IsValid ? AssetTypes.Create(AssetTypes.GetNativeType(clrType), handle) : null);
	}

	// The math structs: arrays of their floats.
	private static DecodedLiteral DecodeStruct(Type clrType, JsonElement json)
	{
		int count = clrType == typeof(Vector2) ? 2
			: clrType == typeof(Vector3) ? 3
			: clrType == typeof(Matrix4) ? 16
			: clrType == typeof(Vector4) || clrType == typeof(Color) || clrType == typeof(Quaternion) ? 4
			: 0;
		if (count == 0)
		{
			return DecodedLiteral.Invalid($"{clrType.FullName} has no literal form: connect the pin instead");
		}
		float[] values = new float[count];
		if (json.ValueKind != JsonValueKind.Array || json.GetArrayLength() != count)
		{
			return Expected($"an array of {Format(count)} finite numbers", json);
		}
		int index = 0;
		foreach (JsonElement element in json.EnumerateArray())
		{
			if (!TryReadFloat(element, out values[index++]))
			{
				return Expected($"an array of {Format(count)} finite numbers", json);
			}
		}
		if (clrType == typeof(Vector2))
		{
			return DecodedLiteral.Of(new Vector2(values[0], values[1]));
		}
		if (clrType == typeof(Vector3))
		{
			return DecodedLiteral.Of(new Vector3(values[0], values[1], values[2]));
		}
		if (clrType == typeof(Vector4))
		{
			return DecodedLiteral.Of(new Vector4(values[0], values[1], values[2], values[3]));
		}
		if (clrType == typeof(Color))
		{
			return DecodedLiteral.Of(new Color(values[0], values[1], values[2], values[3]));
		}
		if (clrType == typeof(Matrix4))
		{
			return DecodedLiteral.Of(new Matrix4(new Vector4(values[0], values[1], values[2], values[3]),
				new Vector4(values[4], values[5], values[6], values[7]), new Vector4(values[8], values[9], values[10], values[11]),
				new Vector4(values[12], values[13], values[14], values[15])));
		}
		Quaternion quaternion = Normalize(new Quaternion(values[0], values[1], values[2], values[3]), out bool wasZero);
		return new DecodedLiteral(quaternion, LiteralProblem.None, null, wasZero);
	}

	// Quaternions are normalized as the engine's decoder does it: unit ones are kept bit-exact (renormalizing would move them
	// by an ulp on every round trip), and one too short (or too broken) to have a direction is the identity (a literal gets a
	// Warning ZeroQuaternion for it).
	internal static Quaternion Normalize(Quaternion value, out bool wasZero)
	{
		double length = Math.Sqrt(((double)value.X * value.X) + ((double)value.Y * value.Y) + ((double)value.Z * value.Z) +
			((double)value.W * value.W));
		wasZero = !(length >= MinQuaternionLength) || double.IsInfinity(length);
		if (wasZero)
		{
			return Quaternion.Identity;
		}
		if (Math.Abs(length - 1.0) <= UnitQuaternionTolerance)
		{
			return value;
		}
		return new Quaternion((float)(value.X / length), (float)(value.Y / length), (float)(value.Z / length), (float)(value.W / length));
	}

	// The forms DecodeEnum reads: the enumerator name, an array of names for a [Flags] enum, else the integer.
	private static void WriteEnum(Utf8JsonWriter writer, VisualType type, Enum value)
	{
		ulong bits = ToBits(value);
		if (type.IsFlags)
		{
			if (TryNameFlags(type, bits, out List<string> names))
			{
				writer.WriteStartArray();
				foreach (string name in names)
				{
					writer.WriteStringValue(name);
				}
				writer.WriteEndArray();
				return;
			}
		}
		else if (FindEnumerator(type, bits) is { } enumerator)
		{
			writer.WriteStringValue(enumerator.Name);
			return;
		}
		TypeCode underlying = Type.GetTypeCode(Enum.GetUnderlyingType(type.ClrType!));
		switch (underlying)
		{
			case TypeCode.Int64:
				writer.WriteStringValue(Format(Convert.ToInt64(value, CultureInfo.InvariantCulture)));
				break;
			case TypeCode.UInt64:
				writer.WriteStringValue(Convert.ToUInt64(value, CultureInfo.InvariantCulture).ToString(CultureInfo.InvariantCulture));
				break;
			case TypeCode.UInt32:
				writer.WriteNumberValue(Convert.ToUInt32(value, CultureInfo.InvariantCulture));
				break;
			default:
				writer.WriteNumberValue(Convert.ToInt64(value, CultureInfo.InvariantCulture));
				break;
		}
	}

	// The names of a [Flags] value: none for 0, the enumerator of exactly its bits when there is one, else the enumerators
	// whose bits make it up, when they do exactly.
	private static bool TryNameFlags(VisualType type, ulong bits, out List<string> names)
	{
		if (bits != 0 && FindEnumerator(type, bits) is { } enumerator)
		{
			names = [enumerator.Name];
			return true;
		}
		return TryDecompose(type, bits, out names);
	}

	private static EnumeratorInfo? FindEnumerator(VisualType type, string name)
	{
		foreach (EnumeratorInfo enumerator in type.Enumerators)
		{
			if (enumerator.Name == name)
			{
				return enumerator;
			}
		}
		return null;
	}

	private static EnumeratorInfo? FindEnumerator(VisualType type, ulong bits)
	{
		foreach (EnumeratorInfo enumerator in type.Enumerators)
		{
			if (ToBits(enumerator.Value) == bits)
			{
				return enumerator;
			}
		}
		return null;
	}

	// The names of the enumerators whose bits make up the value, when they do exactly.
	private static bool TryDecompose(VisualType type, ulong bits, out List<string> names)
	{
		names = [];
		ulong covered = 0;
		foreach (EnumeratorInfo enumerator in type.Enumerators)
		{
			ulong enumeratorBits = ToBits(enumerator.Value);
			if (enumeratorBits != 0 && (bits & enumeratorBits) == enumeratorBits)
			{
				names.Add(enumerator.Name);
				covered |= enumeratorBits;
			}
		}
		return covered == bits;
	}

	// The bits of an enum value or enumerator.
	internal static ulong ToBits(object value)
	{
		TypeCode underlying = Type.GetTypeCode(Enum.GetUnderlyingType(value.GetType()));
		return underlying is TypeCode.UInt64 or TypeCode.UInt32 or TypeCode.UInt16 or TypeCode.Byte
			? Convert.ToUInt64(value, CultureInfo.InvariantCulture)
			: unchecked((ulong)Convert.ToInt64(value, CultureInfo.InvariantCulture));
	}

	private static bool TryParseWide(string text, TypeCode underlying, [NotNullWhen(true)] out object? value)
	{
		value = null;
		if (underlying == TypeCode.Int64 && IsDecimal(text, true) &&
			long.TryParse(text, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out long signed))
		{
			value = signed;
			return true;
		}
		if (underlying == TypeCode.UInt64 && IsDecimal(text, false) && ulong.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out ulong unsigned))
		{
			value = unsigned;
			return true;
		}
		return false;
	}

	private static bool FitsIn(long value, TypeCode underlying) => underlying switch
	{
		TypeCode.SByte => value >= sbyte.MinValue && value <= sbyte.MaxValue,
		TypeCode.Byte => value >= byte.MinValue && value <= byte.MaxValue,
		TypeCode.Int16 => value >= short.MinValue && value <= short.MaxValue,
		TypeCode.UInt16 => value >= ushort.MinValue && value <= ushort.MaxValue,
		TypeCode.Int32 => value >= int.MinValue && value <= int.MaxValue,
		TypeCode.UInt32 => value >= uint.MinValue && value <= uint.MaxValue,
		_ => false,
	};

	private static bool TryReadFloat(JsonElement json, out float value)
	{
		value = 0.0f;
		if (json.ValueKind != JsonValueKind.Number || !json.TryGetDouble(out double number) || !double.IsFinite(number) ||
			Math.Abs(number) > MaxFloat)
		{
			return false;
		}
		// As the engine reads documents (VisualScriptSerializer::ReadFloat): the double the text denotes, narrowed to float. So
		// every reader of a document reads every float alike, also the few texts that lie almost on the midpoint between two
		// floats (the shortest texts of ±7.038531e-26), which WriteFloat never writes. Magnitudes up to the bound narrow to at
		// most float's largest value.
		value = (float)number;
		return true;
	}

	// An optional minus sign (when allowed) and at least one decimal digit, nothing else.
	private static bool IsDecimal(string text, bool allowMinus)
	{
		int start = allowMinus && text.StartsWith('-') ? 1 : 0;
		if (start == text.Length)
		{
			return false;
		}
		for (int index = start; index < text.Length; index++)
		{
			if (!char.IsAsciiDigit(text[index]))
			{
				return false;
			}
		}
		return true;
	}

	private static DecodedLiteral Expected(string what, JsonElement json) => DecodedLiteral.Invalid($"expected {what}, not {JsonText.Excerpt(json)}");

	// A float so that every reader of documents, which reads the double a number's text denotes and narrows it to float, reads
	// it back bit for bit (as the engine's literal codec writes floats): its shortest text, unless that text reads back as
	// another float through a double (the shortest texts of ±7.038531e-26), then its double's text; -0 as "-0.0", because "-0"
	// is the integer 0 to the engine's JSON reader. A float that is not finite is written as 0.
	private static void WriteFloat(Utf8JsonWriter writer, float value)
	{
		if (!float.IsFinite(value))
		{
			writer.WriteNumberValue(0.0f);
		}
		else if (value == 0.0f && float.IsNegative(value))
		{
			writer.WriteRawValue("-0.0", skipInputValidation: true);
		}
		else if (BitConverter.SingleToInt32Bits((float)double.Parse(value.ToString("R", CultureInfo.InvariantCulture), CultureInfo.InvariantCulture)) ==
			BitConverter.SingleToInt32Bits(value))
		{
			writer.WriteNumberValue(value);
		}
		else
		{
			writer.WriteNumberValue((double)value);
		}
	}

	private static void WriteFloats(Utf8JsonWriter writer, params ReadOnlySpan<float> values)
	{
		writer.WriteStartArray();
		foreach (float value in values)
		{
			WriteFloat(writer, value);
		}
		writer.WriteEndArray();
	}
}
