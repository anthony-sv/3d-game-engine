using System.Collections.Generic;
using Strada;

namespace Showcase;

/// <summary>A free camera rig: hold the right mouse button to look around, WASD to move (Q and E down and up, Shift faster),
/// and click to throw a ball where the cursor points (along the view while looking around); clicks on the HUD stay the HUD's.
/// The camera itself is a child (View), so a CameraShake on it adds to the rig's movement. The tour (Flythrough) takes the rig
/// over while it plays.</summary>
public sealed class FlyCamera : Script
{
	[Tooltip("The camera under the rig: it turns screen positions into rays")]
	public Entity? View;

	[Tooltip("A UI element shown at the center of the screen while looking around")]
	public Entity? Crosshair;

	[Tooltip("Meters per second")]
	[Range(0.5f, 50.0f)]
	public float MoveSpeed = 5.0f;

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

	[Tooltip("Balls kept at once: throwing more removes the oldest")]
	[Range(1, 200)]
	public int MaximumBalls = 40;

	private readonly List<Entity> m_Balls = [];
	private float m_Yaw;
	private float m_Pitch;
	private bool m_Controllable = true;

	/// <summary>Balls thrown so far.</summary>
	public int Thrown { get; private set; }

	/// <summary>Degrees the view is turned around the vertical axis (0 looks along -Z).</summary>
	public float Yaw => m_Yaw;

	/// <summary>Degrees the view looks up (negative: down).</summary>
	public float Pitch => m_Pitch;

	/// <summary>Whether mouse and keys move the rig (the tour turns this off while it plays).</summary>
	public bool Controllable
	{
		get => m_Controllable;
		set
		{
			m_Controllable = value;
			if (!value)
			{
				Input.CursorMode = CursorMode.Normal;
			}
		}
	}

	/// <summary>Whether the right mouse button turns the view this frame.</summary>
	public bool IsLooking => m_Controllable && Input.IsMouseButtonDown(MouseButton.Right);

	protected override void OnCreate()
	{
		Vector3 angles = Transform.EulerAngles;
		m_Pitch = angles.X;
		m_Yaw = angles.Y;
	}

	protected override void OnUpdate(float deltaTime)
	{
		bool looking = IsLooking;
		if (Crosshair?.GetComponent<UIRectComponent>() is { } crosshair && crosshair.Visible != looking)
		{
			crosshair.Visible = looking;
		}
		if (!m_Controllable)
		{
			return;
		}
		Input.CursorMode = looking ? CursorMode.Locked : CursorMode.Normal;
		if (looking)
		{
			Vector2 delta = Input.MouseDelta;
			SetPose(Translation, Mathf.Clamp(m_Pitch - (delta.Y * LookSensitivity), -85.0f, 85.0f), m_Yaw - (delta.X * LookSensitivity));
		}

		Vector3 move = Vector3.Zero;
		move += Axis(KeyCode.W, KeyCode.S) * Transform.Forward;
		move += Axis(KeyCode.D, KeyCode.A) * Transform.Right;
		move += Axis(KeyCode.E, KeyCode.Q) * Vector3.Up;
		if (move.LengthSquared > 0.0f)
		{
			float speed = MoveSpeed * (Input.IsKeyDown(KeyCode.LeftShift) ? SprintFactor : 1.0f);
			Vector3 position = Translation + (move.Normalized * speed * deltaTime);
			position.Y = Mathf.Max(position.Y, MinimumHeight);
			Translation = position;
		}

		if (Input.IsMouseButtonPressed(MouseButton.Left) && !Input.IsPointerOverUI)
		{
			Throw(AimDirection);
		}
	}

	/// <summary>Places the rig and turns it to the angles (degrees; pitch is clamped to look at most 85 degrees up or down).</summary>
	public void SetPose(Vector3 position, float pitch, float yaw)
	{
		m_Pitch = Mathf.Clamp(pitch, -85.0f, 85.0f);
		m_Yaw = yaw;
		Translation = position;
		Transform.EulerAngles = new Vector3(m_Pitch, m_Yaw, 0.0f);
	}

	/// <summary>Where a throw goes: along the view while looking around, otherwise towards the cursor.</summary>
	public Vector3 AimDirection
	{
		get
		{
			if (IsLooking)
			{
				return Transform.Forward;
			}
			CameraComponent? camera = View?.GetComponent<CameraComponent>();
			return camera is null ? Transform.Forward : camera.ScreenToWorldRay(Input.MousePosition).Direction;
		}
	}

	/// <summary>Where thrown balls start, for a throw along the direction.</summary>
	public Vector3 ThrowOrigin(Vector3 direction) => Transform.WorldTranslation + (direction.Normalized * 0.6f);

	/// <summary>Throws a ball from the camera along the direction.</summary>
	public Entity? Throw(Vector3 direction)
	{
		if (Ball is null || direction.LengthSquared < 1e-6f)
		{
			return null;
		}
		Vector3 forward = direction.Normalized;
		Entity? ball = Entity.Instantiate(Ball, ThrowOrigin(forward), Quaternion.Identity);
		if (ball is null)
		{
			return null;
		}
		if (ball.GetComponent<RigidBodyComponent>() is { } body)
		{
			body.LinearVelocity = forward * ThrowSpeed;
			body.AngularVelocity = new Vector3(Random.Range(-6.0f, 6.0f), Random.Range(-6.0f, 6.0f), Random.Range(-6.0f, 6.0f));
		}
		if (GetComponent<AudioSourceComponent>() is { } whoosh)
		{
			whoosh.Pitch = Random.Range(0.9f, 1.15f);
			whoosh.Play();
		}
		m_Balls.RemoveAll(thrown => !thrown.IsValid);
		m_Balls.Add(ball);
		if (m_Balls.Count > MaximumBalls)
		{
			m_Balls[0].Destroy();
			m_Balls.RemoveAt(0);
		}
		Thrown++;
		return ball;
	}

	/// <summary>Removes every ball thrown so far.</summary>
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

	private static float Axis(KeyCode positive, KeyCode negative) =>
		(Input.IsKeyDown(positive) ? 1.0f : 0.0f) - (Input.IsKeyDown(negative) ? 1.0f : 0.0f);
}
