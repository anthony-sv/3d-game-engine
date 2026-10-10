using System;
using System.Collections.Generic;
using System.Reflection;

namespace Strada.Interop;

// Engine functions scripts call. Each field is bound at startup to the native function of the same name
// (ScriptBindings.cpp); both sides change together, and a missing or extra binding fails the startup.
internal static unsafe class InternalCalls
{
#pragma warning disable CS0649 // Assigned by Bind through reflection.
	internal static delegate* unmanaged<int, byte*, int, void> Log_Write;

	internal static delegate* unmanaged<ulong, byte> Entity_IsValid;
	internal static delegate* unmanaged<ulong, int*, byte*> Entity_GetName;
	internal static delegate* unmanaged<ulong, byte*, int, void> Entity_SetName;
	internal static delegate* unmanaged<ulong, byte*, int, byte> Entity_HasComponent;
	internal static delegate* unmanaged<byte*, int, ulong> Entity_Create;
	internal static delegate* unmanaged<ulong, void> Entity_Destroy;
	internal static delegate* unmanaged<byte*, int, ulong> Entity_FindByName;

	internal static delegate* unmanaged<ulong, Vector3*, void> TransformComponent_GetTranslation;
	internal static delegate* unmanaged<ulong, Vector3*, void> TransformComponent_SetTranslation;
	internal static delegate* unmanaged<ulong, Quaternion*, void> TransformComponent_GetRotation;
	internal static delegate* unmanaged<ulong, Quaternion*, void> TransformComponent_SetRotation;
	internal static delegate* unmanaged<ulong, Vector3*, void> TransformComponent_GetScale;
	internal static delegate* unmanaged<ulong, Vector3*, void> TransformComponent_SetScale;
#pragma warning restore CS0649

	// Assigns every function pointer field from the native table. Returns null when each field found exactly one
	// function, otherwise the names that did not match.
	internal static string? Bind(Host.NativeBinding* bindings, int count)
	{
		Dictionary<string, nint> functions = new(StringComparer.Ordinal);
		List<string> duplicates = [];
		for (int index = 0; index < count; index++)
		{
			string name = NativeString.FromUtf8(bindings[index].Name, bindings[index].NameLength);
			if (!functions.TryAdd(name, (nint)bindings[index].Function))
			{
				duplicates.Add(name);
			}
		}

		List<string> missing = [];
		foreach (FieldInfo field in typeof(InternalCalls).GetFields(BindingFlags.Static | BindingFlags.NonPublic))
		{
			if (!field.FieldType.IsFunctionPointer)
			{
				continue;
			}
			if (functions.Remove(field.Name, out nint function) && function != 0)
			{
				field.SetValue(null, function);
			}
			else
			{
				missing.Add(field.Name);
			}
		}

		if (missing.Count == 0 && functions.Count == 0 && duplicates.Count == 0)
		{
			return null;
		}
		return $"The engine's script bindings do not match Strada.ScriptCore. Missing: [{string.Join(", ", missing)}]; "
			+ $"unknown: [{string.Join(", ", functions.Keys)}]; duplicated: [{string.Join(", ", duplicates)}]";
	}
}
