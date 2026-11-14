using System;
using Strada.Interop;
using Strada.VisualScripting;

namespace Strada;

/// <summary>Access to the project's assets.</summary>
public static unsafe class Assets
{
	/// <summary>Loads an asset by its path in the project's asset directory ("Prefabs/Enemy.sprefab") or by reference
	/// ("builtin://Cube"). Returns null (logged) when there is no such asset of type <typeparamref name="T"/> or it cannot
	/// be loaded.</summary>
	/// <exception cref="ArgumentException"><typeparamref name="T"/> is <see cref="Asset"/> itself.</exception>
	[PureNode]
	public static T? Load<T>(string path)
		where T : Asset
	{
		string typeName = AssetTypes.GetTypeName(typeof(T))
			?? throw new ArgumentException($"{typeof(T).Name} is not an asset type: load a Mesh, Material, Texture, ...");
		int type = AssetTypes.GetNativeType(typeof(T));
		if (type == (int)NativeAssetType.None)
		{
			return null;
		}
		byte[] text = NativeString.ToUtf8(path);
		fixed (byte* bytes = text)
		{
			ulong handle = InternalCalls.Assets_Load(bytes, text.Length, type);
			return handle != 0 ? (T)AssetTypes.Create(typeName, new AssetHandle(handle)) : null;
		}
	}
}
