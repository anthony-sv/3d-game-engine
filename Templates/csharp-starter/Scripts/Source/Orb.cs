using Strada;

namespace Game;

/// <summary>The orb: WASD, the arrow keys or a gamepad's left stick roll it during a run, it hums with its speed, and a hard
/// hit on a wall thuds.</summary>
public sealed class Orb : Script
{
	[Tooltip("The force the controls push the orb with, in newtons")]
	[Range(0.0f, 100.0f)]
	public float Push = 12.0f;

	[Tooltip("How far the stick must lean before it pushes")]
	[Range(0.0f, 0.9f)]
	public float DeadZone = 0.2f;

	[Tooltip("The child whose looping sound hums with the orb's speed")]
	public Entity? Hum;

	[Tooltip("The floor the orb rolls on without a thud")]
	public Entity? Floor;

	/// <summary>Whether the controls push the orb: only during a run (the run sets it).</summary>
	public bool Rolling { get; set; } = true;

	/// <summary>The way the keys and the stick steer, at most one long: right is +x, up the screen is -z, away from the
	/// camera.</summary>
	public Vector3 Steer()
	{
		float across = Axis(KeyCode.D, KeyCode.Right, KeyCode.A, KeyCode.Left) + Stick(GamepadAxis.LeftX);
		float along = Axis(KeyCode.S, KeyCode.Down, KeyCode.W, KeyCode.Up) + Stick(GamepadAxis.LeftY);
		Vector3 direction = new(across, 0.0f, along);
		// A diagonal is no faster than a straight push.
		return direction.Length > 1.0f ? direction.Normalized : direction;
	}

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		// Each physics step pushes the orb the way it is steered.
		if (Rolling)
		{
			GetComponent<RigidBodyComponent>()?.AddForce(Steer() * Push, ForceMode.Force);
		}
	}

	protected override void OnUpdate(float deltaTime)
	{
		// The faster the orb rolls, the higher and louder it hums.
		if (Hum?.GetComponent<AudioSourceComponent>() is AudioSourceComponent hum)
		{
			float speed = GetSpeed();
			hum.Pitch = 0.7f + (speed * 0.08f);
			hum.Volume = Mathf.Min(0.45f, speed * 0.06f);
		}
	}

	protected override void OnCollisionEnter(Entity other)
	{
		// A wall hit hard thuds, louder the harder.
		float speed = GetSpeed();
		if (other != Floor && speed > 1.5f && GetComponent<AudioSourceComponent>() is AudioSourceComponent bump)
		{
			bump.Volume = Mathf.Min(1.0f, speed / 8.0f);
			bump.Play();
		}
	}

	private float GetSpeed() => GetComponent<RigidBodyComponent>()?.LinearVelocity.Length ?? 0.0f;

	private static float Axis(KeyCode positive, KeyCode otherPositive, KeyCode negative, KeyCode otherNegative) =>
		(Input.IsKeyDown(positive) || Input.IsKeyDown(otherPositive) ? 1.0f : 0.0f) -
		(Input.IsKeyDown(negative) || Input.IsKeyDown(otherNegative) ? 1.0f : 0.0f);

	private float Stick(GamepadAxis axis)
	{
		float lean = Input.GetGamepadAxis(axis);
		return Mathf.Abs(lean) > DeadZone ? lean : 0.0f;
	}
}
