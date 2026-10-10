using Strada;

namespace Showcase;

/// <summary>Plays the entity's sound when it hits something, louder the faster it was moving (crates knock, balls clang).</summary>
public sealed class Impact : Script
{
	[Tooltip("Meters per second below which hits are silent")]
	[Range(0.0f, 5.0f)]
	public float MinimumSpeed = 0.8f;

	[Tooltip("Meters per second at which hits are loudest")]
	[Range(1.0f, 30.0f)]
	public float LoudestSpeed = 8.0f;

	[Tooltip("Volume of the loudest hit")]
	[Range(0.0f, 1.0f)]
	public float Volume = 1.0f;

	private Vector3 m_Velocity;
	private float m_Quiet;

	/// <summary>Hits that played a sound.</summary>
	public int Hits { get; private set; }

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		// The velocity before the step: in OnCollisionEnter the collision has already slowed the body down.
		m_Velocity = GetComponent<RigidBodyComponent>()?.LinearVelocity ?? Vector3.Zero;
	}

	protected override void OnUpdate(float deltaTime)
	{
		m_Quiet -= deltaTime;
	}

	protected override void OnCollisionEnter(Entity other)
	{
		Vector3 otherVelocity = other.GetComponent<RigidBodyComponent>()?.LinearVelocity ?? Vector3.Zero;
		float speed = (m_Velocity - otherVelocity).Length;
		AudioSourceComponent? sound = GetComponent<AudioSourceComponent>();
		if (sound is null || speed < MinimumSpeed || m_Quiet > 0.0f)
		{
			return;
		}
		sound.Volume = Volume * Mathf.InverseLerp(MinimumSpeed, LoudestSpeed, speed);
		sound.Pitch = Random.Range(0.85f, 1.15f);
		sound.Stop();
		sound.Play();
		// A body bouncing on the ground touches it several times in a row: one sound for them.
		m_Quiet = 0.08f;
		Hits++;
	}
}
