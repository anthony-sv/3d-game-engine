using Strada;

namespace Showcase;

/// <summary>A thrown ball: it disappears after a while, or when it falls off the world, so throwing never piles up bodies.</summary>
public sealed class Projectile : Script
{
	[Tooltip("Seconds before the ball disappears")]
	[Range(1.0f, 120.0f)]
	public float Lifetime = 25.0f;

	private float m_Age;

	protected override void OnUpdate(float deltaTime)
	{
		m_Age += deltaTime;
		if (m_Age > Lifetime || Transform.WorldTranslation.Y < -20.0f)
		{
			Destroy();
		}
	}
}
