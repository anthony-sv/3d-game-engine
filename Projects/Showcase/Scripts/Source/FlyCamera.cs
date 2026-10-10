using Strada;

namespace Showcase;

/// <summary>A free camera: hold the right mouse button to look around, WASD to move (Q and E down and up, Shift faster), and
/// click to throw a ball where the cursor points (along the view while looking around).</summary>
public sealed class FlyCamera : Script
{
	[Tooltip("Meters per second")]
	[Range(0.5f, 50.0f)]
	public float MoveSpeed = 4.0f;

	[Tooltip("How much faster Shift moves")]
	[Range(1.0f, 10.0f)]
	public float SprintFactor = 3.0f;

	[Tooltip("Degrees per pixel of mouse movement")]
	[Range(0.01f, 1.0f)]
	public float LookSensitivity = 0.12f;

	[Tooltip("The ball the left mouse button throws")]
	public Prefab? Ball;

	[Tooltip("Meters per second the ball leaves with")]
	[Range(1.0f, 60.0f)]
	public float ThrowSpeed = 16.0f;

	[Tooltip("The camera stays this high above the ground")]
	public float MinimumHeight = 0.4f;

	private float m_Yaw;
	private float m_Pitch;

	/// <summary>Balls thrown so far.</summary>
	public int Thrown { get; private set; }

	protected override void OnCreate()
	{
		Vector3 angles = Transform.EulerAngles;
		m_Pitch = angles.X;
		m_Yaw = angles.Y;
	}

	protected override void OnUpdate(float deltaTime)
	{
		bool looking = Input.IsMouseButtonDown(MouseButton.Right);
		Input.CursorMode = looking ? CursorMode.Locked : CursorMode.Normal;
		if (looking)
		{
			Vector2 delta = Input.MouseDelta;
			m_Yaw -= delta.X * LookSensitivity;
			m_Pitch = Mathf.Clamp(m_Pitch - (delta.Y * LookSensitivity), -85.0f, 85.0f);
			Transform.EulerAngles = new Vector3(m_Pitch, m_Yaw, 0.0f);
		}

		Vector3 move = Vector3.Zero;
		move += Axis(KeyCode.W, KeyCode.S) * Transform.Forward;
		move += Axis(KeyCode.D, KeyCode.A) * Transform.Right;
		move += Axis(KeyCode.E, KeyCode.Q) * Vector3.Up;
		if (move.LengthSquared > 0.0f)
		{
			float speed = MoveSpeed * (Input.IsKeyDown(KeyCode.LeftShift) ? SprintFactor : 1.0f);
			Vector3 position = Translation + (move.Normalized * speed * deltaTime);
			position.Y = System.MathF.Max(position.Y, MinimumHeight);
			Translation = position;
		}

		if (Input.IsMouseButtonPressed(MouseButton.Left))
		{
			Throw(looking ? Transform.Forward : AimAtCursor());
		}
	}

	/// <summary>Throws a ball from the camera along the direction.</summary>
	public Entity? Throw(Vector3 direction)
	{
		if (Ball is null || direction.LengthSquared < 1e-6f)
		{
			return null;
		}
		Vector3 forward = direction.Normalized;
		Entity? ball = Entity.Instantiate(Ball, Transform.WorldTranslation + (forward * 0.6f), Quaternion.Identity);
		RigidBodyComponent? body = ball?.GetComponent<RigidBodyComponent>();
		if (body is not null)
		{
			body.LinearVelocity = forward * ThrowSpeed;
			body.AngularVelocity = new Vector3(Random.Range(-6.0f, 6.0f), Random.Range(-6.0f, 6.0f), Random.Range(-6.0f, 6.0f));
		}
		AudioSourceComponent? whoosh = GetComponent<AudioSourceComponent>();
		if (whoosh is not null)
		{
			whoosh.Pitch = Random.Range(0.9f, 1.15f);
			whoosh.Play();
		}
		Thrown++;
		return ball;
	}

	private Vector3 AimAtCursor()
	{
		CameraComponent? camera = GetComponent<CameraComponent>();
		return camera is null ? Transform.Forward : camera.ScreenToWorldRay(Input.MousePosition).Direction;
	}

	private static float Axis(KeyCode positive, KeyCode negative) =>
		(Input.IsKeyDown(positive) ? 1.0f : 0.0f) - (Input.IsKeyDown(negative) ? 1.0f : 0.0f);
}
