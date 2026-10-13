using System.Collections.Generic;
using Strada;

namespace Showcase;

/// <summary>A ball launcher (a squat mortar): its key (or the tour) fires a ball from the muzzle along the barrel's elevation,
/// fast enough to land on the target (ballistics with the scene's gravity: the balls fly without drag). Every shot booms,
/// flashes and puffs smoke, shakes the camera and kicks the barrel back.</summary>
public sealed class BallLauncher : Script, ITourCue
{
	[Tooltip("The ball the launcher fires (keep its linear damping at 0, or it falls short)")]
	public Prefab? Ball;

	[Tooltip("Where balls leave the barrel; its forward direction gives the barrel's elevation")]
	public Entity? Muzzle;

	[Tooltip("What the launcher aims at")]
	public Entity? Target;

	[Tooltip("A one-shot particle effect made at the muzzle by every shot: its own particle system is the smoke")]
	public Prefab? Flash;

	[Tooltip("The day and night switch (a DayNight): the smoke of a shot is as bright as the rest of the unlit matter")]
	public Entity? Lighting;

	[Tooltip("The camera every shot shakes (it needs a CameraShake script)")]
	public Entity? Camera;

	[Tooltip("The part that kicks back along the barrel when it fires (moved along its local Z)")]
	public Entity? Recoil;

	[Tooltip("Meters the recoil kicks the barrel back")]
	[Range(0.0f, 2.0f)]
	public float RecoilDistance = 0.3f;

	[Tooltip("The key that fires")]
	public KeyCode Key = KeyCode.D3;

	[Tooltip("Balls kept at once: firing more removes the oldest")]
	[Range(1, 100)]
	public int MaximumBalls = 12;

	private readonly List<Entity> m_Balls = [];
	private Vector3 m_RecoilRest;
	private float m_Kick;

	/// <summary>Shots fired so far.</summary>
	public int Shots { get; private set; }

	/// <summary>The flash and smoke of the last shot (until its particles are gone).</summary>
	public Entity? LastFlash { get; private set; }

	protected override void OnCreate()
	{
		m_RecoilRest = Recoil?.Translation ?? Vector3.Zero;
	}

	protected override void OnUpdate(float deltaTime)
	{
		if (Input.IsKeyPressed(Key))
		{
			Fire();
		}
		if (m_Kick > 0.0f && Recoil is not null)
		{
			m_Kick = Mathf.Max(0.0f, m_Kick - (deltaTime / 0.7f));
			// A sharp kick, then an eased return.
			float offset = m_Kick * m_Kick * RecoilDistance;
			Recoil.Translation = m_RecoilRest + new Vector3(0.0f, 0.0f, offset);
		}
	}

	/// <summary>Where a ball leaves the muzzle.</summary>
	public Vector3 MuzzlePosition => Muzzle?.Transform.WorldTranslation ?? Transform.WorldTranslation;

	/// <summary>The velocity that lands a ball from the muzzle on the target: along the barrel's elevation when it reaches that
	/// high, otherwise on a path of 1.5 seconds.</summary>
	public Vector3 LaunchVelocity
	{
		get
		{
			Vector3 start = MuzzlePosition;
			Vector3 end = Target?.Transform.WorldTranslation ?? (start + (Transform.Forward * 10.0f));
			Vector3 gravity = Physics.Gravity;
			float g = -gravity.Y;
			Vector3 across = end - start;
			float height = across.Y;
			across.Y = 0.0f;
			float distance = across.Length;
			Vector3 barrel = Muzzle?.Transform.Forward ?? Transform.Forward;
			float elevation = Mathf.Asin(Mathf.Clamp(barrel.Y, -1.0f, 1.0f));
			float rise = (distance * Mathf.Tan(elevation)) - height;
			if (g > 0.0f && distance > 0.01f && elevation > 0.0f && elevation < 1.5f && rise > 0.01f)
			{
				float cosine = Mathf.Cos(elevation);
				float speed = Mathf.Sqrt(g * distance * distance / (2.0f * cosine * cosine * rise));
				return ((across.Normalized * cosine) + (Vector3.Up * Mathf.Sin(elevation))) * speed;
			}
			const float flight = 1.5f;
			return ((end - start) - (gravity * (0.5f * flight * flight))) * (1.0f / flight);
		}
	}

	/// <summary>Where a ball fired now is after the seconds (without anything in the way).</summary>
	public Vector3 PredictPosition(float seconds) =>
		MuzzlePosition + (LaunchVelocity * seconds) + (Physics.Gravity * (0.5f * seconds * seconds));

	/// <summary>Fires a ball at the target.</summary>
	public Entity? Fire()
	{
		if (Ball is null)
		{
			return null;
		}
		Vector3 velocity = LaunchVelocity;
		Entity? ball = Entity.Instantiate(Ball, MuzzlePosition, Quaternion.Identity);
		if (ball?.GetComponent<RigidBodyComponent>() is { } body)
		{
			body.LinearVelocity = velocity;
			body.AngularVelocity = new Vector3(Random.Range(-3.0f, 3.0f), Random.Range(-3.0f, 3.0f), Random.Range(-3.0f, 3.0f));
		}
		if (ball is not null)
		{
			m_Balls.RemoveAll(fired => !fired.IsValid);
			m_Balls.Add(ball);
			if (m_Balls.Count > MaximumBalls)
			{
				m_Balls[0].Destroy();
				m_Balls.RemoveAt(0);
			}
		}
		if (GetComponent<AudioSourceComponent>() is { } boom)
		{
			boom.Pitch = Random.Range(0.92f, 1.06f);
			boom.Stop();
			boom.Play();
		}
		if (Flash is not null)
		{
			LastFlash = Entity.Instantiate(Flash, MuzzlePosition, Quaternion.Identity);
			// Smoke is not lit: at night it dims like the campfire's (the fire of the flash glows on).
			if (LastFlash?.GetComponent<ParticleSystemComponent>() is { } smoke && Lighting?.As<DayNight>() is { } lighting)
			{
				smoke.Intensity *= lighting.MatterBrightness;
			}
		}
		Camera?.As<CameraShake>()?.ShakeFrom(MuzzlePosition, 0.55f);
		m_Kick = 1.0f;
		Shots++;
		return ball;
	}

	/// <inheritdoc/>
	public void Perform() => Fire();

	/// <summary>Removes every ball fired so far.</summary>
	public void ClearBalls()
	{
		foreach (Entity ball in m_Balls)
		{
			if (ball.IsValid)
			{
				ball.Destroy();
			}
		}
		m_Balls.Clear();
	}
}
