namespace Strada.Interop;

// Reads and writes component fields through their InternalCalls getter and setter pairs.
internal static unsafe class NativeField
{
	internal static T Get<T>(delegate* unmanaged<ulong, T*, void> getter, ulong entity)
		where T : unmanaged
	{
		T value;
		getter(entity, &value);
		return value;
	}

	internal static void Set<T>(delegate* unmanaged<ulong, T*, void> setter, ulong entity, T value)
		where T : unmanaged
	{
		setter(entity, &value);
	}

	internal static bool GetBool(delegate* unmanaged<ulong, byte*, void> getter, ulong entity) => Get(getter, entity) != 0;

	internal static void SetBool(delegate* unmanaged<ulong, byte*, void> setter, ulong entity, bool value) =>
		Set(setter, entity, value ? (byte)1 : (byte)0);

	// Light colors are RGB natively.
	internal static Color GetColor(delegate* unmanaged<ulong, Vector3*, void> getter, ulong entity)
	{
		Vector3 color = Get(getter, entity);
		return new Color(color.X, color.Y, color.Z);
	}

	internal static void SetColor(delegate* unmanaged<ulong, Vector3*, void> setter, ulong entity, Color color) =>
		Set(setter, entity, new Vector3(color.R, color.G, color.B));

	internal static AssetHandle GetAsset(delegate* unmanaged<ulong, ulong*, void> getter, ulong entity) => new(Get(getter, entity));

	internal static void SetAsset(delegate* unmanaged<ulong, ulong*, void> setter, ulong entity, Asset? asset) =>
		Set(setter, entity, asset?.Handle.ID ?? 0);

	internal static string GetString(delegate* unmanaged<ulong, int*, byte*> getter, ulong entity)
	{
		int length = 0;
		byte* text = getter(entity, &length);
		return NativeString.FromUtf8(text, length);
	}

	internal static void SetString(delegate* unmanaged<ulong, byte*, int, void> setter, ulong entity, string value)
	{
		byte[] text = NativeString.ToUtf8(value);
		fixed (byte* bytes = text)
		{
			setter(entity, bytes, text.Length);
		}
	}
}
