using Strada;

namespace Blocks;

/// <summary>Shakes the camera around its place: Shake adds trauma, which decays; the offset grows with trauma squared, so
/// small bumps stay subtle and big ones jolt.</summary>
public sealed class CameraShake : Script
{
	[Tooltip("Units the camera moves at full trauma")]
	[Range(0.0f, 2.0f)]
	public float MaximumOffset = 0.35f;

	[Tooltip("Trauma lost per second")]
	[Range(0.1f, 10.0f)]
	public float Recovery = 1.8f;

	private Vector3 m_Rest;

	/// <summary>How shaken the camera is, from 0 to 1.</summary>
	public float Trauma { get; private set; }

	protected override void OnCreate()
	{
		m_Rest = Translation;
	}

	/// <summary>Adds trauma (0 to 1).</summary>
	public void Shake(float amount)
	{
		Trauma = Mathf.Clamp01(Trauma + amount);
	}

	protected override void OnUpdate(float deltaTime)
	{
		Trauma = Mathf.Max(0.0f, Trauma - (Recovery * deltaTime));
		float strength = Trauma * Trauma * MaximumOffset;
		Translation = m_Rest + new Vector3(Random.Range(-1.0f, 1.0f), Random.Range(-1.0f, 1.0f), 0.0f) * strength;
	}
}
