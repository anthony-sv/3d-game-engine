using Strada.Interop;

namespace Strada;

/// <summary>The sky and image-based lighting from an HDR environment map.</summary>
[NativeComponent("SkyLight")]
public sealed unsafe class SkyLightComponent : Component
{
	private SkyLightComponent()
	{
	}

	/// <summary>The environment map, or null for the ambient color.</summary>
	public EnvironmentMap? Environment
	{
		get
		{
			AssetHandle handle = NativeField.GetAsset(InternalCalls.SkyLightComponent_GetEnvironment, Entity.ID);
			return handle.IsValid ? new EnvironmentMap(handle) : null;
		}
		set => NativeField.SetAsset(InternalCalls.SkyLightComponent_SetEnvironment, Entity.ID, value);
	}

	/// <summary>Brightness multiplier.</summary>
	public float Intensity
	{
		get => NativeField.Get<float>(InternalCalls.SkyLightComponent_GetIntensity, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.SkyLightComponent_SetIntensity, Entity.ID, value);
	}

	/// <summary>Rotation of the environment around the world Y axis, in degrees.</summary>
	public float Rotation
	{
		get => NativeField.Get<float>(InternalCalls.SkyLightComponent_GetRotation, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.SkyLightComponent_SetRotation, Entity.ID, value);
	}

	/// <summary>0 draws a sharp sky, 1 a fully blurred one.</summary>
	public float SkyboxBlur
	{
		get => NativeField.Get<float>(InternalCalls.SkyLightComponent_GetSkyboxBlur, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.SkyLightComponent_SetSkyboxBlur, Entity.ID, value);
	}

	/// <summary>Whether the environment is drawn as the background.</summary>
	public bool DrawSkybox
	{
		get => NativeField.GetBool(InternalCalls.SkyLightComponent_GetDrawSkybox, Entity.ID);
		set => NativeField.SetBool(InternalCalls.SkyLightComponent_SetDrawSkybox, Entity.ID, value);
	}

	/// <summary>Ambient light without an environment map (the alpha is ignored).</summary>
	public Color AmbientColor
	{
		get => NativeField.GetColor(InternalCalls.SkyLightComponent_GetAmbientColor, Entity.ID);
		set => NativeField.SetColor(InternalCalls.SkyLightComponent_SetAmbientColor, Entity.ID, value);
	}
}
