using Strada.Interop;

namespace Strada;

/// <summary>A post-process volume: changes the scene's post-processing (exposure, tonemapper, bloom, ambient occlusion,
/// color grading and lens effects) where the camera is. Each setting applies only while its <c>Override</c> property is
/// set. <see cref="Global"/> volumes apply everywhere; local volumes apply inside their box (the unit cube placed,
/// rotated and scaled by the entity, so the entity's scale is the box's size in meters) and fade out over
/// <see cref="BlendDistance"/> around it. Volumes apply in order of <see cref="Priority"/>, each scaled by its
/// <see cref="Weight"/>: numbers move towards the volume's values, <see cref="Bloom"/> fades in or out through the
/// bloom's intensity, and <see cref="AmbientOcclusion"/> and <see cref="Tonemapper"/> take the volume's value once its
/// blend reaches one half. Animate <see cref="Weight"/> for effects such as a damage flash or an underwater tint.</summary>
[NativeComponent("PostProcess")]
public sealed unsafe class PostProcessComponent : Component
{
	private PostProcessComponent()
	{
	}

	/// <summary>Whether the volume applies everywhere instead of only inside its box.</summary>
	public bool Global
	{
		get => NativeField.GetBool(InternalCalls.PostProcessComponent_GetGlobal, Entity.ID);
		set => NativeField.SetBool(InternalCalls.PostProcessComponent_SetGlobal, Entity.ID, value);
	}

	/// <summary>Local volumes: meters around the box over which the volume fades out.</summary>
	public float BlendDistance
	{
		get => NativeField.Get<float>(InternalCalls.PostProcessComponent_GetBlendDistance, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.PostProcessComponent_SetBlendDistance, Entity.ID, value);
	}

	/// <summary>Volumes with higher priorities apply over those with lower ones.</summary>
	public float Priority
	{
		get => NativeField.Get<float>(InternalCalls.PostProcessComponent_GetPriority, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.PostProcessComponent_SetPriority, Entity.ID, value);
	}

	/// <summary>How much of the volume applies, from 0 (nothing) to 1 (all of it).</summary>
	public float Weight
	{
		get => NativeField.Get<float>(InternalCalls.PostProcessComponent_GetWeight, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.PostProcessComponent_SetWeight, Entity.ID, value);
	}

	/// <summary>Whether the volume's <see cref="EV100"/> applies.</summary>
	public bool OverrideEV100
	{
		get => NativeField.GetBool(InternalCalls.PostProcessComponent_GetOverrideEV100, Entity.ID);
		set => NativeField.SetBool(InternalCalls.PostProcessComponent_SetOverrideEV100, Entity.ID, value);
	}

	/// <summary>Exposure value at ISO 100 (-20 to 20); higher values darken the image.</summary>
	public float EV100
	{
		get => NativeField.Get<float>(InternalCalls.PostProcessComponent_GetEV100, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.PostProcessComponent_SetEV100, Entity.ID, value);
	}

	/// <summary>Whether the volume's <see cref="Tonemapper"/> applies.</summary>
	public bool OverrideTonemapper
	{
		get => NativeField.GetBool(InternalCalls.PostProcessComponent_GetOverrideTonemapper, Entity.ID);
		set => NativeField.SetBool(InternalCalls.PostProcessComponent_SetOverrideTonemapper, Entity.ID, value);
	}

	/// <summary>How HDR colors map to the display.</summary>
	public TonemapOperator Tonemapper
	{
		get => (TonemapOperator)NativeField.Get<int>(InternalCalls.PostProcessComponent_GetTonemapper, Entity.ID);
		set => NativeField.Set<int>(InternalCalls.PostProcessComponent_SetTonemapper, Entity.ID, (int)value);
	}

	/// <summary>Whether the volume switches ambient occlusion as <see cref="AmbientOcclusion"/> says.</summary>
	public bool OverrideAmbientOcclusion
	{
		get => NativeField.GetBool(InternalCalls.PostProcessComponent_GetOverrideAmbientOcclusion, Entity.ID);
		set => NativeField.SetBool(InternalCalls.PostProcessComponent_SetOverrideAmbientOcclusion, Entity.ID, value);
	}

	/// <summary>Whether ambient occlusion darkens indirect light in creases and corners.</summary>
	public bool AmbientOcclusion
	{
		get => NativeField.GetBool(InternalCalls.PostProcessComponent_GetAmbientOcclusion, Entity.ID);
		set => NativeField.SetBool(InternalCalls.PostProcessComponent_SetAmbientOcclusion, Entity.ID, value);
	}

	/// <summary>Whether the volume's <see cref="AmbientOcclusionIntensity"/> applies.</summary>
	public bool OverrideAmbientOcclusionIntensity
	{
		get => NativeField.GetBool(InternalCalls.PostProcessComponent_GetOverrideAmbientOcclusionIntensity, Entity.ID);
		set => NativeField.SetBool(InternalCalls.PostProcessComponent_SetOverrideAmbientOcclusionIntensity, Entity.ID, value);
	}

	/// <summary>Contrast exponent of ambient occlusion (0.1 to 8).</summary>
	public float AmbientOcclusionIntensity
	{
		get => NativeField.Get<float>(InternalCalls.PostProcessComponent_GetAmbientOcclusionIntensity, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.PostProcessComponent_SetAmbientOcclusionIntensity, Entity.ID, value);
	}

	/// <summary>Whether the volume fades bloom in or out as <see cref="Bloom"/> says.</summary>
	public bool OverrideBloom
	{
		get => NativeField.GetBool(InternalCalls.PostProcessComponent_GetOverrideBloom, Entity.ID);
		set => NativeField.SetBool(InternalCalls.PostProcessComponent_SetOverrideBloom, Entity.ID, value);
	}

	/// <summary>Whether bright areas glow.</summary>
	public bool Bloom
	{
		get => NativeField.GetBool(InternalCalls.PostProcessComponent_GetBloom, Entity.ID);
		set => NativeField.SetBool(InternalCalls.PostProcessComponent_SetBloom, Entity.ID, value);
	}

	/// <summary>Whether the volume's <see cref="BloomIntensity"/> applies.</summary>
	public bool OverrideBloomIntensity
	{
		get => NativeField.GetBool(InternalCalls.PostProcessComponent_GetOverrideBloomIntensity, Entity.ID);
		set => NativeField.SetBool(InternalCalls.PostProcessComponent_SetOverrideBloomIntensity, Entity.ID, value);
	}

	/// <summary>Fraction of the blurred image mixed into the scene (0 to 1).</summary>
	public float BloomIntensity
	{
		get => NativeField.Get<float>(InternalCalls.PostProcessComponent_GetBloomIntensity, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.PostProcessComponent_SetBloomIntensity, Entity.ID, value);
	}

	/// <summary>Whether the volume's <see cref="Contrast"/> applies.</summary>
	public bool OverrideContrast
	{
		get => NativeField.GetBool(InternalCalls.PostProcessComponent_GetOverrideContrast, Entity.ID);
		set => NativeField.SetBool(InternalCalls.PostProcessComponent_SetOverrideContrast, Entity.ID, value);
	}

	/// <summary>Contrast around middle gray (0 to 2); 1 leaves colors unchanged.</summary>
	public float Contrast
	{
		get => NativeField.Get<float>(InternalCalls.PostProcessComponent_GetContrast, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.PostProcessComponent_SetContrast, Entity.ID, value);
	}

	/// <summary>Whether the volume's <see cref="Saturation"/> applies.</summary>
	public bool OverrideSaturation
	{
		get => NativeField.GetBool(InternalCalls.PostProcessComponent_GetOverrideSaturation, Entity.ID);
		set => NativeField.SetBool(InternalCalls.PostProcessComponent_SetOverrideSaturation, Entity.ID, value);
	}

	/// <summary>Color intensity: 0 is grayscale, 1 unchanged, 2 doubles it.</summary>
	public float Saturation
	{
		get => NativeField.Get<float>(InternalCalls.PostProcessComponent_GetSaturation, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.PostProcessComponent_SetSaturation, Entity.ID, value);
	}

	/// <summary>Whether the volume's <see cref="Temperature"/> applies.</summary>
	public bool OverrideTemperature
	{
		get => NativeField.GetBool(InternalCalls.PostProcessComponent_GetOverrideTemperature, Entity.ID);
		set => NativeField.SetBool(InternalCalls.PostProcessComponent_SetOverrideTemperature, Entity.ID, value);
	}

	/// <summary>White balance from cool (-1, blue) to warm (1, orange).</summary>
	public float Temperature
	{
		get => NativeField.Get<float>(InternalCalls.PostProcessComponent_GetTemperature, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.PostProcessComponent_SetTemperature, Entity.ID, value);
	}

	/// <summary>Whether the volume's <see cref="Tint"/> applies.</summary>
	public bool OverrideTint
	{
		get => NativeField.GetBool(InternalCalls.PostProcessComponent_GetOverrideTint, Entity.ID);
		set => NativeField.SetBool(InternalCalls.PostProcessComponent_SetOverrideTint, Entity.ID, value);
	}

	/// <summary>White balance from green (-1) to magenta (1).</summary>
	public float Tint
	{
		get => NativeField.Get<float>(InternalCalls.PostProcessComponent_GetTint, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.PostProcessComponent_SetTint, Entity.ID, value);
	}

	/// <summary>Whether the volume's <see cref="ColorFilter"/> applies.</summary>
	public bool OverrideColorFilter
	{
		get => NativeField.GetBool(InternalCalls.PostProcessComponent_GetOverrideColorFilter, Entity.ID);
		set => NativeField.SetBool(InternalCalls.PostProcessComponent_SetOverrideColorFilter, Entity.ID, value);
	}

	/// <summary>A linear color (components 0 to 2) every color is multiplied by; the alpha is ignored.</summary>
	public Color ColorFilter
	{
		get => NativeField.GetColor(InternalCalls.PostProcessComponent_GetColorFilter, Entity.ID);
		set => NativeField.SetColor(InternalCalls.PostProcessComponent_SetColorFilter, Entity.ID, value);
	}

	/// <summary>Whether the volume's <see cref="ChromaticAberration"/> applies.</summary>
	public bool OverrideChromaticAberration
	{
		get => NativeField.GetBool(InternalCalls.PostProcessComponent_GetOverrideChromaticAberration, Entity.ID);
		set => NativeField.SetBool(InternalCalls.PostProcessComponent_SetOverrideChromaticAberration, Entity.ID, value);
	}

	/// <summary>Lens color fringes that grow towards the edges of the image (0 to 1).</summary>
	public float ChromaticAberration
	{
		get => NativeField.Get<float>(InternalCalls.PostProcessComponent_GetChromaticAberration, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.PostProcessComponent_SetChromaticAberration, Entity.ID, value);
	}

	/// <summary>Whether the volume's <see cref="Vignette"/> applies.</summary>
	public bool OverrideVignette
	{
		get => NativeField.GetBool(InternalCalls.PostProcessComponent_GetOverrideVignette, Entity.ID);
		set => NativeField.SetBool(InternalCalls.PostProcessComponent_SetOverrideVignette, Entity.ID, value);
	}

	/// <summary>How much the corners of the image darken (0 to 1).</summary>
	public float Vignette
	{
		get => NativeField.Get<float>(InternalCalls.PostProcessComponent_GetVignette, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.PostProcessComponent_SetVignette, Entity.ID, value);
	}

	/// <summary>Whether the volume's <see cref="VignetteSmoothness"/> applies.</summary>
	public bool OverrideVignetteSmoothness
	{
		get => NativeField.GetBool(InternalCalls.PostProcessComponent_GetOverrideVignetteSmoothness, Entity.ID);
		set => NativeField.SetBool(InternalCalls.PostProcessComponent_SetOverrideVignetteSmoothness, Entity.ID, value);
	}

	/// <summary>How gradually the vignette fades in from the center (0.01 to 1).</summary>
	public float VignetteSmoothness
	{
		get => NativeField.Get<float>(InternalCalls.PostProcessComponent_GetVignetteSmoothness, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.PostProcessComponent_SetVignetteSmoothness, Entity.ID, value);
	}

	/// <summary>Whether the volume's <see cref="FilmGrain"/> applies.</summary>
	public bool OverrideFilmGrain
	{
		get => NativeField.GetBool(InternalCalls.PostProcessComponent_GetOverrideFilmGrain, Entity.ID);
		set => NativeField.SetBool(InternalCalls.PostProcessComponent_SetOverrideFilmGrain, Entity.ID, value);
	}

	/// <summary>Strength of the animated film grain (0 to 1).</summary>
	public float FilmGrain
	{
		get => NativeField.Get<float>(InternalCalls.PostProcessComponent_GetFilmGrain, Entity.ID);
		set => NativeField.Set<float>(InternalCalls.PostProcessComponent_SetFilmGrain, Entity.ID, value);
	}
}
