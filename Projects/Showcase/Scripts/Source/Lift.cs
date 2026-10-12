using Strada;

namespace Showcase;

/// <summary>A lift: a kinematic platform that waits at the bottom, rises <see cref="Rise"/> meters, waits at the top and comes
/// down again, easing in and out of each trip, over and over. Characters ride it (and bodies on it go along); the motor's hum
/// (the entity's sound, looping) swells while it moves.</summary>
public sealed class Lift : Script
{
	[Tooltip("Meters the platform rises")]
	[Range(0.1f, 50.0f)]
	public float Rise = 3.3f;

	[Tooltip("Seconds a trip up or down takes")]
	[Range(0.5f, 30.0f)]
	public float TravelSeconds = 3.5f;

	[Tooltip("Seconds the platform waits at each end")]
	[Range(0.0f, 30.0f)]
	public float WaitSeconds = 2.5f;

	private Vector3 m_Bottom;
	private Quaternion m_Rotation;
	private float m_HumVolume;

	/// <summary>Seconds into the lift's cycle, which starts with the wait at the bottom.</summary>
	public float Elapsed { get; private set; }

	/// <summary>Meters the platform is above the bottom.</summary>
	public float Height { get; private set; }

	/// <summary>Whether the platform is moving.</summary>
	public bool IsMoving { get; private set; }

	/// <summary>Trips up or down finished so far.</summary>
	public int Trips { get; private set; }

	/// <summary>Seconds from the start of the cycle until the platform is at the top.</summary>
	public float SecondsToTop => WaitSeconds + TravelSeconds;

	protected override void OnCreate()
	{
		m_Bottom = Transform.WorldTranslation;
		m_Rotation = Rotation;
		m_HumVolume = GetComponent<AudioSourceComponent>()?.Volume ?? 0.0f;
		Elapsed = 0.0f;
		Apply();
	}

	/// <summary>Starts the cycle again: the platform waits at the bottom (it is taken there at once).</summary>
	public void Restart()
	{
		Elapsed = 0.0f;
		Height = 0.0f;
		if (GetComponent<RigidBodyComponent>() is { } body)
		{
			body.Teleport(m_Bottom, m_Rotation);
		}
	}

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		float cycle = 2.0f * (WaitSeconds + TravelSeconds);
		bool wasMoving = IsMoving;
		Elapsed = Mathf.Repeat(Elapsed + fixedDeltaTime, cycle);
		Apply();
		if (wasMoving && !IsMoving)
		{
			Trips++;
		}
		// The platform sits under parents that are not moved or turned: its world position is its own.
		GetComponent<RigidBodyComponent>()?.MoveKinematic(m_Bottom + new Vector3(0.0f, Height, 0.0f), m_Rotation);
	}

	protected override void OnUpdate(float deltaTime)
	{
		if (GetComponent<AudioSourceComponent>() is { } hum)
		{
			float target = IsMoving ? m_HumVolume : 0.0f;
			hum.Volume = Mathf.MoveTowards(hum.Volume, target, deltaTime * m_HumVolume * 3.0f);
		}
	}

	// Where the platform is in its cycle: waiting at the bottom, rising, waiting at the top, coming down.
	private void Apply()
	{
		float time = Elapsed;
		if (time < WaitSeconds)
		{
			Set(0.0f, false);
		}
		else if (time < WaitSeconds + TravelSeconds)
		{
			Set(Mathf.SmoothStep(0.0f, Rise, (time - WaitSeconds) / TravelSeconds), true);
		}
		else if (time < (2.0f * WaitSeconds) + TravelSeconds)
		{
			Set(Rise, false);
		}
		else
		{
			Set(Mathf.SmoothStep(Rise, 0.0f, (time - (2.0f * WaitSeconds) - TravelSeconds) / TravelSeconds), true);
		}
	}

	private void Set(float height, bool moving)
	{
		Height = height;
		IsMoving = moving;
	}
}
