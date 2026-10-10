using Strada;

namespace Blocks;

/// <summary>A block of a cleared row flying off: it shrinks away and disappears after its lifetime.</summary>
public sealed class Debris : Script
{
	[Tooltip("Seconds before it disappears")]
	[Range(0.1f, 10.0f)]
	public float Lifetime = 1.3f;

	private float m_Age;
	private Vector3 m_Scale;

	protected override void OnCreate()
	{
		m_Scale = Scale;
	}

	protected override void OnUpdate(float deltaTime)
	{
		m_Age += deltaTime;
		if (m_Age >= Lifetime)
		{
			Destroy();
			return;
		}
		// Full size for the first half, then shrinking to nothing.
		float shrink = Mathf.Clamp01(2.0f * (1.0f - (m_Age / Lifetime)));
		Scale = m_Scale * shrink;
	}
}
