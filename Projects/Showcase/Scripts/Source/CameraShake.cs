using Strada;

namespace Showcase;

/// <summary>Shakes the camera it is on around its rest pose: impacts add trauma, which wears off, and the camera turns and
/// moves by smooth noise scaled by trauma squared, so small bumps stay subtle and big ones jolt. The camera sits under a rig
/// that moves it (CameraRig), so the shake adds to whatever the rig does.</summary>
public sealed class CameraShake : Script
{
	[Tooltip("Degrees the camera turns at full trauma")]
	[Range(0.0f, 20.0f)]
	public float MaximumAngle = 3.5f;

	[Tooltip("Meters the camera moves at full trauma")]
	[Range(0.0f, 1.0f)]
	public float MaximumOffset = 0.1f;

	[Tooltip("Trauma lost per second")]
	[Range(0.1f, 10.0f)]
	public float Recovery = 1.3f;

	[Tooltip("How fast the shake wobbles")]
	[Range(1.0f, 60.0f)]
	public float Frequency = 16.0f;

	[Tooltip("Meters from an impact at which it no longer shakes the camera")]
	[Range(1.0f, 200.0f)]
	public float Reach = 28.0f;

	private readonly float[] m_Phases = new float[5];
	private float m_Time;

	/// <summary>How shaken the camera is, from 0 to 1.</summary>
	public float Trauma { get; private set; }

	protected override void OnCreate()
	{
		for (int i = 0; i < m_Phases.Length; i++)
		{
			m_Phases[i] = Random.Range(0.0f, 100.0f);
		}
	}

	/// <summary>Adds trauma (0 to 1).</summary>
	public void Shake(float amount)
	{
		Trauma = Mathf.Clamp01(Trauma + amount);
	}

	/// <summary>Shakes the camera by an impact of the strength (0 to 1) at a world position: less the farther away it is.</summary>
	public void ShakeFrom(Vector3 position, float strength)
	{
		float nearness = 1.0f - Mathf.Clamp01(Vector3.Distance(Transform.WorldTranslation, position) / Reach);
		Shake(strength * nearness * nearness);
	}

	protected override void OnUpdate(float deltaTime)
	{
		Trauma = Mathf.Max(0.0f, Trauma - (Recovery * deltaTime));
		m_Time += deltaTime;
		float amount = Trauma * Trauma;
		Transform.EulerAngles = new Vector3(Noise(0), Noise(1), 0.5f * Noise(2)) * (MaximumAngle * amount);
		Translation = new Vector3(Noise(3), Noise(4), 0.0f) * (MaximumOffset * amount);
	}

	// Smooth noise in [-1, 1]: two sines of unrelated frequencies with a phase of their own per channel.
	private float Noise(int channel)
	{
		float phase = m_Phases[channel];
		float time = m_Time * Frequency;
		return (0.6f * Mathf.Sin(time + phase)) + (0.4f * Mathf.Sin((time * 2.37f) + (phase * 1.7f)));
	}
}
