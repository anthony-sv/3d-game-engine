using Strada.Interop;

namespace Strada;

/// <summary>Light shining from the entity's position in every direction.</summary>
[NativeComponent("PointLight")]
public sealed unsafe class PointLightComponent : Component
{
	private PointLightComponent()
	{
	}

	/// <summary>The light's color (the alpha is ignored).</summary>
	public Color Color
	{
		get => NativeField.GetColor(InternalCalls.PointLightComponent_GetColor, Entity.ID);
		set => NativeField.SetColor(InternalCalls.PointLightComponent_SetColor, Entity.ID, value);
	}

	/// <summary>Brightness multiplier.</summary>
	public float Intensity
	{
		get => NativeField.Get<float>(InternalCalls.PointLightComponent_GetIntensity, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.PointLightComponent_SetIntensity, Entity.ID, value);
	}

	/// <summary>Distance at which the light's contribution reaches zero.</summary>
	public float Range
	{
		get => NativeField.Get<float>(InternalCalls.PointLightComponent_GetRange, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.PointLightComponent_SetRange, Entity.ID, value);
	}

	/// <summary>Whether the light casts shadows.</summary>
	public bool CastShadows
	{
		get => NativeField.GetBool(InternalCalls.PointLightComponent_GetCastShadows, Entity.ID);
		set => NativeField.SetBool(InternalCalls.PointLightComponent_SetCastShadows, Entity.ID, value);
	}
}
