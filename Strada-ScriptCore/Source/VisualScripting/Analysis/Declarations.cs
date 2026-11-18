using System;
using System.Collections.Generic;
using System.Text.Json;
using Strada.Interop;

namespace Strada.VisualScripting;

// The rules for declared types and values (spec sections 2.1 and 3.2, spec-slice3 section 3.4.3): variables, locals,
// function parameters and custom-event parameters.
internal static class Declarations
{
	// Spec-slice3 section 3.4.1.
	internal const string NestedArrayMessage = "arrays of arrays are not types of graphs";

	// The type of a declaration; null when it is unknown or not allowed (exec, the wildcard). A public variable of a type
	// that is not exposable keeps its type (the Error NotExposable is about exposure only). An array is exposable when its
	// element type is: components store it as a list (Docs/Architecture.md §10.6).
	internal static VisualType? ResolveType(AnalysisContext context, string typeId, bool isPublic,
		Action<DiagnosticCode, string, IReadOnlyList<string>?> report)
	{
		NodeLibrary library = context.Library;
		if (typeId is TypeIds.Exec or TypeIds.Wildcard)
		{
			report(DiagnosticCode.InvalidType, $"{typeId} is a pin type: variables and parameters hold values", null);
			return null;
		}
		VisualType? type = library.FindType(typeId);
		if (type == null && IsNestedArray(typeId))
		{
			report(DiagnosticCode.UnknownType, $"{JsonText.Quote(typeId)}: {NestedArrayMessage}", null);
			return null;
		}
		if (type == null)
		{
			List<string> candidates = [];
			foreach (string id in library.TypeIds)
			{
				if (id != TypeIds.Exec)
				{
					candidates.Add(id);
				}
			}
			report(DiagnosticCode.UnknownType, $"unknown type {JsonText.Quote(typeId)}", context.Suggest(typeId, candidates));
			return null;
		}
		if (isPublic && type.VariableFieldType == ScriptFieldType.None)
		{
			report(DiagnosticCode.NotExposable,
				$"{type.Id} cannot be set on components: public variables are numbers, bool, string, vectors, quaternions, colors, entities, assets or enums, or arrays of them",
				null);
		}
		return type;
	}

	// Whether a type ID the library does not have names an array of arrays ("int[][]").
	internal static bool IsNestedArray(string typeId) => typeId.EndsWith(TypeIds.ArraySuffix + TypeIds.ArraySuffix, StringComparison.Ordinal);

	// The initial value of a variable or local: its decoded Value, else the type default. Reports what is wrong with the
	// Value ("Value: ...", "Value[2]: ..." for an element of an array) and gives the type default then.
	internal static object? DecodeValue(VisualType type, JsonElement? value, Action<DiagnosticCode, string> report)
	{
		const string what = "Value";
		if (value is not { } json)
		{
			return LiteralCodec.DefaultOf(type);
		}
		if (!type.HasLiteral)
		{
			report(DiagnosticCode.InvalidValue, $"{what}: {type.Id} has no literal form: leave Value out (the value starts as the type default)");
			return LiteralCodec.DefaultOf(type);
		}
		DecodedLiteral decoded = LiteralCodec.Decode(type, json);
		if (decoded.WasZeroQuaternion)
		{
			report(DiagnosticCode.ZeroQuaternion, $"{what}: a quaternion of length zero is the identity");
		}
		switch (decoded.Problem)
		{
			case LiteralProblem.InvalidValue:
				report(DiagnosticCode.InvalidValue, decoded.Describe(what));
				return LiteralCodec.DefaultOf(type);
			case LiteralProblem.EntityValue:
				report(DiagnosticCode.EntityValue, decoded.Describe(what));
				return LiteralCodec.DefaultOf(type);
			default:
				return decoded.Value;
		}
	}
}
