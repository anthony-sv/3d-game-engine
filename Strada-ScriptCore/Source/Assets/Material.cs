using System;
using Strada.Interop;

namespace Strada;

/// <summary>How a material uses the alpha of its base color.</summary>
public enum MaterialAlphaMode
{
	/// <summary>Alpha is ignored.</summary>
	Opaque = 0,
	/// <summary>Pixels with alpha below <see cref="Material.AlphaCutoff"/> are discarded.</summary>
	Mask = 1,
	/// <summary>Alpha blending: transparent.</summary>
	Blend = 2,
}

/// <summary>A material asset: the metallic-roughness surface of meshes (colors are linear). The project's materials are
/// shared by every scene, so scripts change copies made with <see cref="Create"/> or <see cref="Clone"/>; those last until
/// the scene stops. Changing a shared material logs a script error and does nothing.</summary>
public sealed unsafe class Material : Asset
{
	internal Material(AssetHandle handle)
		: base(handle)
	{
	}

	/// <summary>A new material with default parameters (white, not metallic, half rough), or null (logged) without a
	/// running scene.</summary>
	public static Material? Create()
	{
		ulong handle = InternalCalls.Material_Create();
		return handle != 0 ? new Material(new AssetHandle(handle)) : null;
	}

	/// <summary>A copy of the material that scripts can change, or null (logged) when it cannot be made.</summary>
	public Material? Clone()
	{
		ulong handle = InternalCalls.Material_Clone(Handle.ID);
		return handle != 0 ? new Material(new AssetHandle(handle)) : null;
	}

	/// <summary>Whether scripts can change the material: it was made with <see cref="Create"/> or <see cref="Clone"/> by
	/// the running scene.</summary>
	public bool IsEditable => InternalCalls.Material_IsRuntime(Handle.ID) != 0;

	/// <summary>Albedo and opacity.</summary>
	public Color BaseColor
	{
		get => GetValues().BaseColor;
		set
		{
			NativeMaterialValues values = GetValues();
			values.BaseColor = value;
			SetValues(values, "BaseColor"u8);
		}
	}

	/// <summary>0 for dielectrics, 1 for metals.</summary>
	public float Metallic
	{
		get => GetValues().Metallic;
		set
		{
			NativeMaterialValues values = GetValues();
			values.Metallic = value;
			SetValues(values, "Metallic"u8);
		}
	}

	/// <summary>0 for mirrors, 1 for fully diffuse surfaces.</summary>
	public float Roughness
	{
		get => GetValues().Roughness;
		set
		{
			NativeMaterialValues values = GetValues();
			values.Roughness = value;
			SetValues(values, "Roughness"u8);
		}
	}

	/// <summary>The color of emitted light (alpha is ignored).</summary>
	public Color Emissive
	{
		get
		{
			Vector3 color = GetValues().EmissiveColor;
			return new Color(color.X, color.Y, color.Z);
		}
		set
		{
			NativeMaterialValues values = GetValues();
			values.EmissiveColor = new Vector3(value.R, value.G, value.B);
			SetValues(values, "Emissive"u8);
		}
	}

	/// <summary>Multiplier of the emitted light.</summary>
	public float EmissiveIntensity
	{
		get => GetValues().EmissiveIntensity;
		set
		{
			NativeMaterialValues values = GetValues();
			values.EmissiveIntensity = value;
			SetValues(values, "EmissiveIntensity"u8);
		}
	}

	/// <summary>Scales the bumps of the normal map.</summary>
	public float NormalStrength
	{
		get => GetValues().NormalStrength;
		set
		{
			NativeMaterialValues values = GetValues();
			values.NormalStrength = value;
			SetValues(values, "NormalStrength"u8);
		}
	}

	/// <summary>How much the occlusion map darkens indirect light (0 to 1).</summary>
	public float OcclusionStrength
	{
		get => GetValues().OcclusionStrength;
		set
		{
			NativeMaterialValues values = GetValues();
			values.OcclusionStrength = value;
			SetValues(values, "OcclusionStrength"u8);
		}
	}

	/// <summary>Base color texture (sRGB colors, opacity in alpha), or null.</summary>
	public Texture? BaseColorTexture
	{
		get => ToTexture(GetValues().BaseColorTexture);
		set
		{
			NativeMaterialValues values = GetValues();
			values.BaseColorTexture = value?.Handle.ID ?? 0;
			SetValues(values, "BaseColorTexture"u8);
		}
	}

	/// <summary>Tangent-space normal map (+Y up), or null.</summary>
	public Texture? NormalTexture
	{
		get => ToTexture(GetValues().NormalTexture);
		set
		{
			NativeMaterialValues values = GetValues();
			values.NormalTexture = value?.Handle.ID ?? 0;
			SetValues(values, "NormalTexture"u8);
		}
	}

	/// <summary>Roughness (green) and metallic (blue) map multiplied with the factors, or null.</summary>
	public Texture? MetallicRoughnessTexture
	{
		get => ToTexture(GetValues().MetallicRoughnessTexture);
		set
		{
			NativeMaterialValues values = GetValues();
			values.MetallicRoughnessTexture = value?.Handle.ID ?? 0;
			SetValues(values, "MetallicRoughnessTexture"u8);
		}
	}

	/// <summary>Ambient occlusion map (red), or null.</summary>
	public Texture? OcclusionTexture
	{
		get => ToTexture(GetValues().OcclusionTexture);
		set
		{
			NativeMaterialValues values = GetValues();
			values.OcclusionTexture = value?.Handle.ID ?? 0;
			SetValues(values, "OcclusionTexture"u8);
		}
	}

	/// <summary>Emitted color map (sRGB) multiplied with <see cref="Emissive"/> and <see cref="EmissiveIntensity"/>, or
	/// null.</summary>
	public Texture? EmissiveTexture
	{
		get => ToTexture(GetValues().EmissiveTexture);
		set
		{
			NativeMaterialValues values = GetValues();
			values.EmissiveTexture = value?.Handle.ID ?? 0;
			SetValues(values, "EmissiveTexture"u8);
		}
	}

	/// <summary>How the alpha of the base color is used.</summary>
	public MaterialAlphaMode AlphaMode
	{
		get => (MaterialAlphaMode)GetValues().AlphaMode;
		set
		{
			NativeMaterialValues values = GetValues();
			values.AlphaMode = (int)value;
			SetValues(values, "AlphaMode"u8);
		}
	}

	/// <summary>The alpha below which <see cref="MaterialAlphaMode.Mask"/> discards pixels.</summary>
	public float AlphaCutoff
	{
		get => GetValues().AlphaCutoff;
		set
		{
			NativeMaterialValues values = GetValues();
			values.AlphaCutoff = value;
			SetValues(values, "AlphaCutoff"u8);
		}
	}

	/// <summary>Whether back faces are rendered too.</summary>
	public bool DoubleSided
	{
		get => GetValues().DoubleSided != 0;
		set
		{
			NativeMaterialValues values = GetValues();
			values.DoubleSided = value ? 1u : 0u;
			SetValues(values, "DoubleSided"u8);
		}
	}

	/// <summary>Texture coordinate scale.</summary>
	public Vector2 UVTiling
	{
		get => GetValues().UVTiling;
		set
		{
			NativeMaterialValues values = GetValues();
			values.UVTiling = value;
			SetValues(values, "UVTiling"u8);
		}
	}

	/// <summary>Texture coordinate offset.</summary>
	public Vector2 UVOffset
	{
		get => GetValues().UVOffset;
		set
		{
			NativeMaterialValues values = GetValues();
			values.UVOffset = value;
			SetValues(values, "UVOffset"u8);
		}
	}

	private static Texture? ToTexture(ulong handle) => handle != 0 ? new Texture(new AssetHandle(handle)) : null;

	private NativeMaterialValues GetValues()
	{
		NativeMaterialValues values;
		InternalCalls.Material_GetValues(Handle.ID, &values);
		return values;
	}

	// The property's name is for the engine's error message.
	private void SetValues(NativeMaterialValues values, ReadOnlySpan<byte> property)
	{
		fixed (byte* name = property)
		{
			InternalCalls.Material_SetValues(Handle.ID, &values, name, property.Length);
		}
	}
}
