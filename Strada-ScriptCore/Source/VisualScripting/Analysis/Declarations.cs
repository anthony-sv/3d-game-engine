using System;
using System.Collections.Generic;
using System.Text.Json;
using Strada.Interop;

namespace Strada.VisualScripting;

// The rules for declared types and values (spec sections 2.1 and 3.2): variables, locals, function parameters and
// custom-event parameters.
internal static class Declarations
{
	// The type of a declaration; null when it is unknown or not allowed (exec, the wildcard, arrays). A public variable of a
	// type that is not exposable keeps its type (the Error NotExposable is about exposure only).
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
		if (type.Kind == TypeKind.Array)
		{
			report(DiagnosticCode.InvalidType, $"{type.Id}: arrays cannot be variables or parameters yet", null);
			return null;
		}
		if (isPublic && type.FieldType == ScriptFieldType.None)
		{
			report(DiagnosticCode.NotExposable,
				$"{type.Id} cannot be set on components: public variables are numbers, bool, string, vectors, quaternions, colors, entities, assets or enums",
				null);
		}
		return type;
	}

	// The initial value of a variable or local: its decoded Value, else the type default. Reports what is wrong with the
	// Value and gives the type default then.
	internal static object? DecodeValue(VisualType type, JsonElement? value, Action<DiagnosticCode, string> report)
	{
		if (value is not { } json)
		{
			return LiteralCodec.DefaultOf(type);
		}
		if (!type.HasLiteral)
		{
			report(DiagnosticCode.InvalidValue, $"{type.Id} has no literal form: leave Value out (the value starts as the type default)");
			return LiteralCodec.DefaultOf(type);
		}
		DecodedLiteral decoded = LiteralCodec.Decode(type, json);
		if (decoded.WasZeroQuaternion)
		{
			report(DiagnosticCode.ZeroQuaternion, "a quaternion of length zero is the identity");
		}
		switch (decoded.Problem)
		{
			case LiteralProblem.InvalidValue:
				report(DiagnosticCode.InvalidValue, decoded.Message!);
				return LiteralCodec.DefaultOf(type);
			case LiteralProblem.EntityValue:
				report(DiagnosticCode.EntityValue, decoded.Message!);
				return LiteralCodec.DefaultOf(type);
			default:
				return decoded.Value;
		}
	}
}
