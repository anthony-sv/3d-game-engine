using System;
using Strada;

namespace Showcase;

/// <summary>A lift: a kinematic platform that waits at the bottom, rises <see cref="Rise"/> meters, waits at the top and comes
/// down again, easing in and out of each trip, over and over. Characters ride it (and bodies on it go along). On its way down
/// it stops while a character or a loose body is under it, and goes on once they are clear. The motor's hum (the entity's
/// sound, looping) swells while it moves.</summary>
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

	[Tooltip("Meters under the platform within which a character or a loose body stops it on its way down")]
	[Range(0.05f, 5.0f)]
	public float Clearance = 0.3f;

	[Tooltip("Meters between the rays that look under the platform: less than a character is wide")]
	[Range(0.05f, 2.0f)]
	public float SensorSpacing = 0.3f;

	private Vector3 m_Bottom;
	private Quaternion m_Rotation;
	private float m_HumVolume;

	/// <summary>Seconds into the lift's cycle, which starts with the wait at the bottom.</summary>
	public float Elapsed { get; private set; }

	/// <summary>Meters the platform is above the bottom.</summary>
	public float Height { get; private set; }

	/// <summary>Whether the platform is moving.</summary>
	public bool IsMoving { get; private set; }

	/// <summary>Whether the platform has stopped on its way down because something is under it.</summary>
	public bool IsHeld { get; private set; }

	/// <summary>Times the platform stopped on its way down for something under it.</summary>
	public int Holds { get; private set; }

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
		IsHeld = false;
		if (GetComponent<RigidBodyComponent>() is { } body)
		{
			body.Teleport(m_Bottom, m_Rotation);
		}
	}

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		bool wasMoving = IsMoving;
		bool held = IsDescending && SomethingBelow();
		if (held && !IsHeld)
		{
			Holds++;
		}
		IsHeld = held;
		if (!held)
		{
			Elapsed = Mathf.Repeat(Elapsed + fixedDeltaTime, 2.0f * (WaitSeconds + TravelSeconds));
		}
		Apply();
		if (wasMoving && !IsMoving && !IsHeld)
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

	// Whether the cycle is in its trip down.
	private bool IsDescending => Elapsed >= (2.0f * WaitSeconds) + TravelSeconds;

	// Where the platform is in its cycle: waiting at the bottom, rising, waiting at the top, coming down (or held on the way).
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
		else if (!IsDescending)
		{
			Set(Rise, false);
		}
		else
		{
			Set(Mathf.SmoothStep(Rise, 0.0f, (time - (2.0f * WaitSeconds) - TravelSeconds) / TravelSeconds), !IsHeld);
		}
	}

	private void Set(float height, bool moving)
	{
		Height = height;
		IsMoving = moving;
	}

	// Rays down from a grid over the platform's underside, closer together than a character is wide: whether one of them meets a
	// character or a dynamic body within the clearance.
	private bool SomethingBelow()
	{
		// The platform is a box (a unit cube under its scale): its underside spans its scaled X and Z.
		Vector3 scale = Scale;
		Vector3 right = Transform.Right;
		Vector3 forward = Transform.Forward;
		Vector3 underside = Transform.WorldTranslation - new Vector3(0.0f, (scale.Y * 0.5f) + 0.01f, 0.0f);
		int across = Math.Max(2, (int)Mathf.Ceil(scale.X / SensorSpacing) + 1);
		int along = Math.Max(2, (int)Mathf.Ceil(scale.Z / SensorSpacing) + 1);
		for (int i = 0; i < across; i++)
		{
			for (int j = 0; j < along; j++)
			{
				float x = ((i / (float)(across - 1)) - 0.5f) * scale.X;
				float z = ((j / (float)(along - 1)) - 0.5f) * scale.Z;
				Vector3 origin = underside + (right * x) + (forward * z);
				if (Physics.Raycast(origin, Vector3.Down, Clearance, out RaycastHit hit) && Blocks(hit.Entity))
				{
					return true;
				}
			}
		}
		return false;
	}

	// Characters and bodies that physics moves stop the platform; the ground and what is built there do not.
	private static bool Blocks(Entity entity) =>
		entity.HasComponent<CharacterControllerComponent>() || entity.GetComponent<RigidBodyComponent>()?.Type == RigidBodyType.Dynamic;
}
