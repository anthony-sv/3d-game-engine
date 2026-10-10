using Strada.Interop;

namespace Strada;

/// <summary>Light from infinitely far away, shining along the entity's -Z axis.</summary>
[NativeComponent("DirectionalLight")]
public sealed unsafe class DirectionalLightComponent : Component
{
	private DirectionalLightComponent()
	{
	}

	/// <summary>The light's color (the alpha is ignored).</summary>
	public Color Color
	{
		get => NativeField.GetColor(InternalCalls.DirectionalLightComponent_GetColor, Entity.ID);
		set => NativeField.SetColor(InternalCalls.DirectionalLightComponent_SetColor, Entity.ID, value);
	}

	/// <summary>Brightness multiplier.</summary>
	public float Intensity
	{
		get => NativeField.Get<float>(InternalCalls.DirectionalLightComponent_GetIntensity, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.DirectionalLightComponent_SetIntensity, Entity.ID, value);
	}

	/// <summary>Whether the light casts shadows.</summary>
	public bool CastShadows
	{
		get => NativeField.GetBool(InternalCalls.DirectionalLightComponent_GetCastShadows, Entity.ID);
		set => NativeField.SetBool(InternalCalls.DirectionalLightComponent_SetCastShadows, Entity.ID, value);
	}

	/// <summary>Apparent angular diameter of the light source in degrees; larger values soften shadows.</summary>
	public float LightSize
	{
		get => NativeField.Get<float>(InternalCalls.DirectionalLightComponent_GetLightSize, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.DirectionalLightComponent_SetLightSize, Entity.ID, value);
	}
}
