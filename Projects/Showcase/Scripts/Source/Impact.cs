using Strada;

namespace Showcase;

/// <summary>Plays the entity's sound when it hits something, louder the faster it was moving (crates knock, balls clang), and
/// shakes the camera on hard hits. With a lifetime the entity disappears after it (thrown balls); anything that falls off the
/// world goes away.</summary>
public class Impact : Script
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

	[Tooltip("The camera that hard hits shake (it needs a CameraShake script); none shakes nothing")]
	public Entity? Camera;

	[Tooltip("How much the hardest hit shakes the camera (0 to 1)")]
	[Range(0.0f, 1.0f)]
	public float ShakeStrength;

	[Tooltip("Seconds before the entity disappears (0: it stays)")]
	[Range(0.0f, 600.0f)]
	public float Lifetime;

	private Vector3 m_Velocity;
	private float m_Quiet;
	private float m_Age;

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
		m_Age += deltaTime;
		if ((Lifetime > 0.0f && m_Age > Lifetime) || Transform.WorldTranslation.Y < -20.0f)
		{
			Destroy();
		}
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
		float loudness = Mathf.InverseLerp(MinimumSpeed, LoudestSpeed, speed);
		sound.Volume = Volume * loudness;
		sound.Pitch = Random.Range(0.85f, 1.15f);
		sound.Stop();
		sound.Play();
		if (ShakeStrength > 0.0f)
		{
			Camera?.As<CameraShake>()?.ShakeFrom(Transform.WorldTranslation, ShakeStrength * loudness);
		}
		// A body bouncing on the ground touches it several times in a row: one sound for them.
		m_Quiet = 0.08f;
		Hits++;
	}
}
