namespace Strada.Tests;

// The examples of the scripting guide (Docs/Scripting.md) as it shows them: StradaTests checks that the guide still shows
// exactly these classes, and runs them.

public class Spinner : Script
{
	[Tooltip("Turn speed in degrees per second")]
	public float Speed = 90.0f;

	protected override void OnUpdate(float deltaTime)
	{
		// A little more of a turn around the up axis each frame.
		Transform.Rotation = Quaternion.AngleAxis(Speed * deltaTime, Vector3.Up) * Transform.Rotation;
	}
}

public class FirstPersonController : Script
{
	public float Speed = 5.0f;
	public float JumpSpeed = 5.0f;
	public float MouseSensitivity = 0.15f;
	[Tooltip("Seconds a jump press is kept, and seconds after leaving the ground in which the character can still jump")]
	public float JumpGrace = 0.1f;
	public Entity? Head;

	private float m_Yaw;
	private float m_Pitch;
	private float m_JumpBuffer;
	private float m_CoyoteTime;

	protected override void OnCreate()
	{
		Input.CursorMode = CursorMode.Locked;
		// Start looking where the entity faces.
		Vector3 forward = Transform.Forward;
		m_Yaw = Mathf.Atan2(-forward.X, -forward.Z) * Mathf.Rad2Deg;
	}

	protected override void OnUpdate(float deltaTime)
	{
		// Look around: the body turns left and right, the head up and down.
		Vector2 mouse = Input.MouseDelta * MouseSensitivity;
		m_Yaw -= mouse.X;
		m_Pitch = Mathf.Clamp(m_Pitch - mouse.Y, -85.0f, 85.0f);
		Transform.Rotation = Quaternion.AngleAxis(m_Yaw, Vector3.Up);
		Head!.Transform.Rotation = Quaternion.AngleAxis(m_Pitch, Vector3.Right);
		// Key presses last one frame: keep the jump for the next physics steps.
		if (Input.IsKeyPressed(KeyCode.Space))
		{
			m_JumpBuffer = JumpGrace;
		}
	}

	protected override void OnFixedUpdate(float fixedDeltaTime)
	{
		CharacterControllerComponent character = GetComponent<CharacterControllerComponent>()!;
		Vector3 direction = (Transform.Forward * Axis(KeyCode.W, KeyCode.S)) + (Transform.Right * Axis(KeyCode.D, KeyCode.A));
		character.Move(direction.Normalized * Speed);

		m_CoyoteTime = character.IsGrounded ? JumpGrace : m_CoyoteTime - fixedDeltaTime;
		if (m_JumpBuffer > 0.0f && m_CoyoteTime > 0.0f)
		{
			character.Velocity = new Vector3(0.0f, JumpSpeed, 0.0f);
			m_JumpBuffer = 0.0f;
			m_CoyoteTime = 0.0f;
		}
		m_JumpBuffer -= fixedDeltaTime;
	}

	private static float Axis(KeyCode positive, KeyCode negative) =>
		(Input.IsKeyDown(positive) ? 1.0f : 0.0f) - (Input.IsKeyDown(negative) ? 1.0f : 0.0f);
}
