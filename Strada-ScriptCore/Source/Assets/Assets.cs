using System;
using Strada.Interop;

namespace Strada;

/// <summary>Access to the project's assets.</summary>
public static unsafe class Assets
{
	/// <summary>Loads an asset by its path in the project's asset directory ("Prefabs/Enemy.sprefab") or by reference
	/// ("builtin://Cube"). Returns null (logged) when there is no such asset of type <typeparamref name="T"/> or it cannot
	/// be loaded.</summary>
	/// <exception cref="ArgumentException"><typeparamref name="T"/> is <see cref="Asset"/> itself.</exception>
	public static T? Load<T>(string path)
		where T : Asset
	{
		NativeAssetType type = AssetTypes.GetNativeType(typeof(T));
		if (type == NativeAssetType.None)
		{
			throw new ArgumentException($"{typeof(T).Name} is not an asset type: load a Mesh, Material, Texture, ...");
		}
		byte[] text = NativeString.ToUtf8(path);
		fixed (byte* bytes = text)
		{
			ulong handle = InternalCalls.Assets_Load(bytes, text.Length, (int)type);
			return handle != 0 ? (T)AssetTypes.Create(type, new AssetHandle(handle)) : null;
		}
	}
}
