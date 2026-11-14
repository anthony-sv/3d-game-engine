using System;
using System.Collections.Generic;

namespace Strada.Interop;

// The engine's built-in AssetType values (Asset.h). Data asset types (DataAssetRegistry, Docs/Architecture.md section 6.4)
// take values from 128 up, assigned when they are registered, so managed code names them and asks the engine for their value.
internal enum NativeAssetType
{
	None = 0,
	Scene,
	Prefab,
	Mesh,
	Material,
	Texture,
	Environment,
	AudioClip,
	Font,
	VisualScript,
}

// The asset classes by the engine's names of their types (AssetTypeToString in Asset.h), which the node library, the script
// field metadata and documents carry, and references of a class made from handles. Only calls into native code use the
// engine's numbers (GetNativeType).
internal static unsafe class AssetTypes
{
	// The first value of data asset types (DataAssetRegistry::FirstAssetTypeValue); the built-in values stay below.
	internal const int FirstDataAssetTypeValue = 128;

	// The values of data asset types the engine gave by name, kept for the process: the engine's own types keep their values
	// for as long as it runs. Cleared when the bindings are bound again (Reset).
	private static readonly Dictionary<string, int> s_DataAssetTypes = [];
	// The names of data asset types' values the engine gave, as s_DataAssetTypes the other way.
	private static readonly Dictionary<int, string> s_DataAssetNames = [];

	// The engine's name of the asset class's type; null for other types (Asset itself included).
	internal static string? GetTypeName(Type type) => type switch
	{
		_ when type == typeof(Prefab) => "Prefab",
		_ when type == typeof(Mesh) => "Mesh",
		_ when type == typeof(Material) => "Material",
		_ when type == typeof(Texture) => "Texture",
		_ when type == typeof(EnvironmentMap) => "Environment",
		_ when type == typeof(AudioClip) => "AudioClip",
		_ when type == typeof(Font) => "Font",
		_ when type == typeof(VisualScript) => "VisualScript",
		_ when type == typeof(Curve) => "Curve",
		_ => null,
	};

	// A reference of the class whose type the engine names typeName.
	internal static Asset Create(string typeName, AssetHandle handle) => typeName switch
	{
		"Prefab" => new Prefab(handle),
		"Mesh" => new Mesh(handle),
		"Material" => new Material(handle),
		"Texture" => new Texture(handle),
		"Environment" => new EnvironmentMap(handle),
		"AudioClip" => new AudioClip(handle),
		"Font" => new Font(handle),
		"VisualScript" => new VisualScript(handle),
		"Curve" => new Curve(handle),
		_ => throw new ArgumentOutOfRangeException(nameof(typeName), typeName, "not an asset class"),
	};

	// The engine's AssetType value of the asset class's type, for calls into native code: fixed for built-in types, asked of
	// the engine once for a data asset type (Assets_FindType). 0 (None) for other types, and when the engine has no type of
	// that name (it logged why).
	internal static int GetNativeType(Type type)
	{
		string? name = GetTypeName(type);
		if (name == null)
		{
			return (int)NativeAssetType.None;
		}
		NativeAssetType builtIn = GetBuiltInType(name);
		if (builtIn != NativeAssetType.None)
		{
			return (int)builtIn;
		}
		if (s_DataAssetTypes.TryGetValue(name, out int value))
		{
			return value;
		}
		byte[] text = NativeString.ToUtf8(name);
		fixed (byte* bytes = text)
		{
			value = InternalCalls.Assets_FindType(bytes, text.Length);
		}
		if (value != (int)NativeAssetType.None)
		{
			s_DataAssetTypes[name] = value;
			s_DataAssetNames[value] = name;
		}
		return value;
	}

	// The engine's name of the type an AssetType value from native code stands for, before Create makes a reference of its
	// class: fixed for built-in types, asked of the engine once for a data asset type's value (Assets_GetTypeName, values from
	// 128 up). Null for None and for a value that is no type (the engine logged why).
	internal static string? GetTypeName(int nativeType)
	{
		if (nativeType < FirstDataAssetTypeValue)
		{
			return GetBuiltInName((NativeAssetType)nativeType);
		}
		if (s_DataAssetNames.TryGetValue(nativeType, out string? name))
		{
			return name;
		}
		int length = 0;
		byte* text = InternalCalls.Assets_GetTypeName(nativeType, &length);
		if (text == null)
		{
			return null;
		}
		name = NativeString.FromUtf8(text, length);
		s_DataAssetNames[nativeType] = name;
		s_DataAssetTypes[name] = nativeType;
		return name;
	}

	// Forgets the data asset types' values: the engine binds the scripting runtime again (Host.Initialize).
	internal static void Reset()
	{
		s_DataAssetTypes.Clear();
		s_DataAssetNames.Clear();
	}

	private static NativeAssetType GetBuiltInType(string name) => name switch
	{
		"Prefab" => NativeAssetType.Prefab,
		"Mesh" => NativeAssetType.Mesh,
		"Material" => NativeAssetType.Material,
		"Texture" => NativeAssetType.Texture,
		"Environment" => NativeAssetType.Environment,
		"AudioClip" => NativeAssetType.AudioClip,
		"Font" => NativeAssetType.Font,
		"VisualScript" => NativeAssetType.VisualScript,
		_ => NativeAssetType.None,
	};

	private static string? GetBuiltInName(NativeAssetType type) => type switch
	{
		NativeAssetType.Scene => "Scene",
		NativeAssetType.Prefab => "Prefab",
		NativeAssetType.Mesh => "Mesh",
		NativeAssetType.Material => "Material",
		NativeAssetType.Texture => "Texture",
		NativeAssetType.Environment => "Environment",
		NativeAssetType.AudioClip => "AudioClip",
		NativeAssetType.Font => "Font",
		NativeAssetType.VisualScript => "VisualScript",
		_ => null,
	};
}
