using Strada.Interop;

namespace Strada;

/// <summary>Light shining from the entity's position in a cone along its -Z axis.</summary>
[NativeComponent("SpotLight")]
public sealed unsafe class SpotLightComponent : Component
{
	private SpotLightComponent()
	{
	}

	/// <summary>The light's color (the alpha is ignored).</summary>
	public Color Color
	{
		get => NativeField.GetColor(InternalCalls.SpotLightComponent_GetColor, Entity.ID);
		set => NativeField.SetColor(InternalCalls.SpotLightComponent_SetColor, Entity.ID, value);
	}

	/// <summary>Brightness multiplier.</summary>
	public float Intensity
	{
		get => NativeField.Get<float>(InternalCalls.SpotLightComponent_GetIntensity, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.SpotLightComponent_SetIntensity, Entity.ID, value);
	}

	/// <summary>Distance at which the light's contribution reaches zero.</summary>
	public float Range
	{
		get => NativeField.Get<float>(InternalCalls.SpotLightComponent_GetRange, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.SpotLightComponent_SetRange, Entity.ID, value);
	}

	/// <summary>Half-angle of the full-intensity cone, in degrees.</summary>
	public float InnerConeAngle
	{
		get => NativeField.Get<float>(InternalCalls.SpotLightComponent_GetInnerConeAngle, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.SpotLightComponent_SetInnerConeAngle, Entity.ID, value);
	}

	/// <summary>Half-angle of the cone the light fades out at, in degrees.</summary>
	public float OuterConeAngle
	{
		get => NativeField.Get<float>(InternalCalls.SpotLightComponent_GetOuterConeAngle, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.SpotLightComponent_SetOuterConeAngle, Entity.ID, value);
	}

	/// <summary>Whether the light casts shadows.</summary>
	public bool CastShadows
	{
		get => NativeField.GetBool(InternalCalls.SpotLightComponent_GetCastShadows, Entity.ID);
		set => NativeField.SetBool(InternalCalls.SpotLightComponent_SetCastShadows, Entity.ID, value);
	}
}
