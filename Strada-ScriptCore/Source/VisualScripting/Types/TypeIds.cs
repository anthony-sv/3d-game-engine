using System;

namespace Strada.VisualScripting;

// Type IDs (spec section 3.1): fixed names for exec, the primitives and the wildcard, the CLR full name for every other
// type (nested types with '+'), and "<element>[]" for arrays.
internal static class TypeIds
{
	internal const string Exec = "exec";
	internal const string Wildcard = "*";
	internal const string Bool = "bool";
	internal const string Int = "int";
	internal const string UInt = "uint";
	internal const string Long = "long";
	internal const string ULong = "ulong";
	internal const string Float = "float";
	internal const string Double = "double";
	internal const string String = "string";
	internal const string ArraySuffix = "[]";

	// The type ID of a CLR type; null for types the type system has no ID for (pointers, by-reference types, generic
	// types and parameters, multi-dimensional arrays).
	internal static string? Of(Type type)
	{
		if (type == typeof(bool))
		{
			return Bool;
		}
		if (type == typeof(int))
		{
			return Int;
		}
		if (type == typeof(uint))
		{
			return UInt;
		}
		if (type == typeof(long))
		{
			return Long;
		}
		if (type == typeof(ulong))
		{
			return ULong;
		}
		if (type == typeof(float))
		{
			return Float;
		}
		if (type == typeof(double))
		{
			return Double;
		}
		if (type == typeof(string))
		{
			return String;
		}
		if (type.IsSZArray)
		{
			string? element = Of(type.GetElementType()!);
			return element != null ? element + ArraySuffix : null;
		}
		if (type.IsArray || type.IsPointer || type.IsByRef || type.IsFunctionPointer || type.IsGenericType || type.IsGenericParameter)
		{
			return null;
		}
		return type.FullName;
	}
}
