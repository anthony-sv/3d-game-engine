using Strada;

namespace Showcase;

/// <summary>A first-person walker: the character controller on the entity walks at the input it is given (relative to the
/// view's heading), runs, and jumps, with a jump buffer and coyote time; it speeds up and slows down smoothly, plays a footstep
/// every stride and a thump on landing, and tells the view how its head bobs and dips. It stays on the clearing's ground: at
/// the edge of its area it walks along the edge, not out. Out of use it waits far below the clearing without gravity;
/// <see cref="Enter"/> puts it on its feet (the camera rig does, and then gives it its input).</summary>
public sealed class Walker : Script
{
	[Tooltip("Meters per second walking")]
	[Range(0.5f, 20.0f)]
	public float WalkSpeed = 4.2f;

	[Tooltip("Meters per second running")]
	[Range(0.5f, 30.0f)]
	public float RunSpeed = 7.5f;

	[Tooltip("Meters per second upwards a jump starts with")]
	[Range(0.5f, 20.0f)]
	public float JumpSpeed = 5.4f;

	[Tooltip("Meters per second squared the walker reaches its speed with on the ground")]
	[Range(1.0f, 200.0f)]
	public float GroundAcceleration = 40.0f;

	[Tooltip("Meters per second squared it steers with in the air")]
	[Range(0.0f, 200.0f)]
	public float AirAcceleration = 10.0f;

	[Tooltip("Seconds a jump press is kept, and seconds after leaving the ground in which the walker can still jump")]
	[Range(0.0f, 1.0f)]
	public float JumpGrace = 0.12f;

	[Tooltip("Meters from the feet (the entity's origin) to the eyes")]
	[Range(0.1f, 3.0f)]
	public float EyeHeight = 1.62f;

	[Tooltip("Meters walked between two footsteps")]
	[Range(0.1f, 5.0f)]
	public float StrideLength = 0.8f;

	[Tooltip("Meters the head rises and falls with each stride at full speed")]
	[Range(0.0f, 0.3f)]
	public float BobHeight = 0.045f;

	[Tooltip("An audio source with a footstep, played every stride")]
	public Entity? Footsteps;

	[Tooltip("An audio source with the thump of landing")]
	public Entity? Landing;

	[Tooltip("Where the walker waits while it is not in use (world space, far from everything)")]
	public Vector3 ParkingPlace = new(0.0f, -500.0f, 0.0f);

	[Tooltip("Meters from the world's origin, horizontally, the walker stays within: it walks along the edge, not out (0: anywhere)")]
	[Range(0.0f, 10000.0f)]
	public float AreaRadius;

	private Vector3 m_Velocity;
	private Vector3 m_LastPosition;
	private float m_JumpBuffer;
	private float m_CoyoteTime;
	private float m_Airborne;
	private float m_FallSpeed;
	private float m_Stride;
	private float m_Dip;
	private float m_DipVelocity;
	private float m_GravityFactor = 1.0f;

	/// <summary>Whether the walker is in use (on its feet in the clearing).</summary>
	public bool IsActive { get; private set; }

	/// <summary>Where to walk: X to the right and Y forward, relative to <see cref="Yaw"/>; longer than 1 is shortened.</summary>
	public Vector2 MoveInput { get; set; }

	/// <summary>Whether the walker runs.</summary>
	public bool Running { get; set; }

	/// <summary>Degrees the view is turned about the vertical axis (0 looks along -Z): forward is that way.</summary>
	public float Yaw { get; set; }

	/// <summary>Whether the walker stands on walkable ground.</summary>
	public bool IsGrounded => GetComponent<CharacterControllerComponent>()?.IsGrounded ?? false;

	/// <summary>The walking velocity it moves with now (meters per second, horizontal).</summary>
	public Vector3 WalkVelocity => m_Velocity;

	/// <summary>Where its eyes are, with the head's bob and the dip of landing (world space).</summary>
	public Vector3 EyePosition => Transform.WorldTranslation + new Vector3(0.0f, EyeHeight + HeadOffset, 0.0f);

	/// <summary>Meters the head is above (or below) its rest height: it bobs with the strides and dips on landing.</summary>
	public float HeadOffset { get; private set; }

	/// <summary>Footsteps so far.</summary>
	public int Steps { get; private set; }

	/// <summary>Jumps so far.</summary>
	public int Jumps { get; private set; }

	/// <summary>Landings after a fall or a jump so far.</summary>
	public int Landings { get; private set; }

	protected override void OnCreate()
	{
		if (GetComponent<CharacterControllerComponent>() is { } character)
		{
			m_GravityFactor = character.GravityFactor;
		}
		Park();
	}

	/// <summary>Puts the walker on its feet at a place (world space) and turns it to the heading, at rest.</summary>
	public void Enter(Vector3 feet, float yaw)
	{
		IsActive = true;
		Yaw = yaw;
		m_Velocity = Vector3.Zero;
		m_JumpBuffer = 0.0f;
		m_CoyoteTime = 0.0f;
		m_Airborne = 0.0f;
		m_Stride = 0.0f;
		m_Dip = 0.0f;
		m_DipVelocity = 0.0f;
		HeadOffset = 0.0f;
		if (GetComponent<CharacterControllerComponent>() is { } character)
		{
			character.GravityFactor = m_GravityFactor;
			character.Teleport(feet);
		}
		m_LastPosition = feet;
	}

	/// <summary>Takes the walker out of the clearing: it waits at the parking place without gravity.</summary>
	public void Park()
	{
		IsActive = false;
		MoveInput = Vector2.Zero;
		Running = false;
		m_Velocity = Vector3.Zero;
		if (GetComponent<CharacterControllerComponent>() is { } character)
		{
			character.GravityFactor = 0.0f;
			character.Teleport(ParkingPlace);
		}
	}

	/// <summary>Jumps as soon as the walker may: now on the ground, a moment after leaving it, or on landing a moment later.</summary>
	public void Jump()
	{
		m_JumpBuffer = JumpGrace;
	}

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		if (!IsActive || GetComponent<CharacterControllerComponent>() is not { } character)
		{
			return;
		}
		bool grounded = character.IsGrounded;

		// The wanted velocity along the ground, reached smoothly.
		float yaw = Yaw * Mathf.Deg2Rad;
		Vector3 forward = new(-Mathf.Sin(yaw), 0.0f, -Mathf.Cos(yaw));
		Vector3 right = new(Mathf.Cos(yaw), 0.0f, -Mathf.Sin(yaw));
		Vector2 input = MoveInput.LengthSquared > 1.0f ? MoveInput.Normalized : MoveInput;
		Vector3 wanted = ((right * input.X) + (forward * input.Y)) * (Running ? RunSpeed : WalkSpeed);
		Vector3 change = wanted - m_Velocity;
		float step = (grounded ? GroundAcceleration : AirAcceleration) * fixedDeltaTime;
		m_Velocity = change.Length <= step ? wanted : m_Velocity + (change.Normalized * step);
		KeepInArea();
		character.Move(m_Velocity);

		// Jumps: a press is kept for a moment, and the ground is too after the walker left it.
		m_CoyoteTime = grounded ? JumpGrace : m_CoyoteTime - fixedDeltaTime;
		if (m_JumpBuffer > 0.0f && m_CoyoteTime > 0.0f)
		{
			character.Velocity = new Vector3(0.0f, JumpSpeed, 0.0f);
			m_JumpBuffer = 0.0f;
			m_CoyoteTime = 0.0f;
			Jumps++;
		}
		m_JumpBuffer -= fixedDeltaTime;

		// Landing after a while in the air thumps and dips the head, by how fast the walker fell.
		if (grounded)
		{
			if (m_Airborne > 0.25f && m_FallSpeed > 2.0f)
			{
				Land(m_FallSpeed);
			}
			m_Airborne = 0.0f;
			m_FallSpeed = 0.0f;
		}
		else
		{
			m_Airborne += fixedDeltaTime;
			m_FallSpeed = Mathf.Max(m_FallSpeed, -character.Velocity.Y);
		}

		// A footstep every stride walked on the ground (as far as the walker got, not as far as it wanted).
		Vector3 position = Transform.WorldTranslation;
		Vector3 moved = position - m_LastPosition;
		m_LastPosition = position;
		if (grounded)
		{
			m_Stride += new Vector2(moved.X, moved.Z).Length;
			if (m_Stride >= StrideLength)
			{
				m_Stride -= StrideLength;
				Steps++;
				PlayStep();
			}
		}
	}

	protected override void OnUpdate(float deltaTime)
	{
		if (!IsActive)
		{
			return;
		}
		// The dip springs back; the bob follows the strides, as high as the walker is fast.
		float stiffness = 90.0f;
		float damping = 2.0f * Mathf.Sqrt(stiffness);
		m_DipVelocity += ((-stiffness * m_Dip) - (damping * m_DipVelocity)) * deltaTime;
		m_Dip += m_DipVelocity * deltaTime;
		float speed = Mathf.Clamp01(m_Velocity.Length / RunSpeed);
		float bob = IsGrounded ? Mathf.Sin(m_Stride / StrideLength * Mathf.PI) * BobHeight * speed : 0.0f;
		HeadOffset = bob - (0.5f * BobHeight * speed) + m_Dip;
	}

	// At the edge of the area the part of the velocity that leads out goes.
	private void KeepInArea()
	{
		Vector3 position = Transform.WorldTranslation;
		Vector3 outward = new(position.X, 0.0f, position.Z);
		if (AreaRadius <= 0.0f || outward.Length < AreaRadius)
		{
			return;
		}
		outward = outward.Normalized;
		float leaving = Vector3.Dot(m_Velocity, outward);
		if (leaving > 0.0f)
		{
			m_Velocity -= outward * leaving;
		}
	}

	private void Land(float fallSpeed)
	{
		Landings++;
		m_DipVelocity -= Mathf.Min(fallSpeed, 12.0f) * 0.15f;
		if (Landing?.GetComponent<AudioSourceComponent>() is { } thump)
		{
			thump.Volume = Mathf.InverseLerp(2.0f, 10.0f, fallSpeed);
			thump.Pitch = Random.Range(0.9f, 1.05f);
			thump.Stop();
			thump.Play();
		}
	}

	private void PlayStep()
	{
		if (Footsteps?.GetComponent<AudioSourceComponent>() is { } step)
		{
			step.Volume = Running ? 0.75f : 0.55f;
			step.Pitch = Random.Range(0.85f, 1.15f);
			step.Stop();
			step.Play();
		}
	}
}
