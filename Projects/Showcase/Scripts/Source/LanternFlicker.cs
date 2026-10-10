using Strada;

namespace Showcase;

/// <summary>Makes the entity's point light flicker like a flame: a slow sway with quick, small jitters.</summary>
public sealed class LanternFlicker : Script
{
	[Tooltip("How far the brightness swings, as a fraction of the light's intensity")]
	[Range(0.0f, 1.0f)]
	public float Amount = 0.18f;

	[Tooltip("Flickers per second")]
	[Range(0.1f, 20.0f)]
	public float Speed = 6.0f;

	private float m_BaseIntensity;
	private float m_Target;
	private float m_Current;
	private float m_UntilNext;

	protected override void OnCreate()
	{
		m_BaseIntensity = GetComponent<PointLightComponent>()?.Intensity ?? 0.0f;
	}

	protected override void OnUpdate(float deltaTime)
	{
		PointLightComponent? light = GetComponent<PointLightComponent>();
		if (light is null)
		{
			return;
		}
		m_UntilNext -= deltaTime;
		if (m_UntilNext <= 0.0f)
		{
			m_Target = Random.Range(-1.0f, 1.0f);
			m_UntilNext = Random.Range(0.5f, 1.5f) / Speed;
		}
		m_Current = Mathf.Lerp(m_Current, m_Target, Mathf.Clamp01(deltaTime * Speed * 2.0f));
		float sway = System.MathF.Sin((float)Time.Elapsed * 1.3f) * 0.3f;
		light.Intensity = m_BaseIntensity * (1.0f + (Amount * ((m_Current * 0.7f) + sway)));
	}
}
