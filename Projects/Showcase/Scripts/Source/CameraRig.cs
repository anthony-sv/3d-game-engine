using System.Collections.Generic;
using Strada;

namespace Showcase;

/// <summary>The camera rig, which flies or walks. Flying, the right mouse button looks around, WASD moves (Q and E down and up,
/// Shift faster) and a click throws a ball where the cursor points (along the view while looking around); the rig stays over
/// the clearing, where the ground is. F puts the camera on the feet of the walker (a Walker), standing on what is under it
/// (with nothing under it, the rig flies on): the mouse looks around (Esc frees the cursor for the HUD, a click into the scene
/// takes it again), WASD walks, Shift runs, Space jumps and clicks throw along the view; F flies on from there. Clicks on the
/// HUD stay the HUD's. The camera itself is a child (View), so a CameraShake on it adds to the rig's movement, and its field
/// of view widens a little at speed. The tour (Flythrough) takes the rig over while it plays.</summary>
public sealed class CameraRig : Script
{
	[Tooltip("The camera under the rig: it turns screen positions into rays")]
	public Entity? View;

	[Tooltip("A UI element shown at the center of the screen while looking around")]
	public Entity? Crosshair;

	[Tooltip("The first-person walker (a Walker on a character controller) the rig walks as")]
	public Entity? Walker;

	[Tooltip("Meters per second flying")]
	[Range(0.5f, 50.0f)]
	public float MoveSpeed = 5.0f;

	[Tooltip("How much faster Shift flies")]
	[Range(1.0f, 10.0f)]
	public float SprintFactor = 3.0f;

	[Tooltip("Degrees per pixel of mouse movement")]
	[Range(0.01f, 1.0f)]
	public float LookSensitivity = 0.12f;

	[Tooltip("The key that switches between flying and walking")]
	public KeyCode ModeKey = KeyCode.F;

	[Tooltip("The key that frees the cursor while walking")]
	public KeyCode ReleaseKey = KeyCode.Escape;

	[Tooltip("Seconds the view takes to drop to the walker's eyes")]
	[Range(0.05f, 3.0f)]
	public float EyeDropSeconds = 0.6f;

	[Tooltip("Degrees the field of view widens at full speed (Shift)")]
	[Range(0.0f, 30.0f)]
	public float SpeedFovKick = 7.0f;

	[Tooltip("The ball the left mouse button throws")]
	public Prefab? Ball;

	[Tooltip("Meters per second the ball leaves with")]
	[Range(1.0f, 60.0f)]
	public float ThrowSpeed = 16.0f;

	[Tooltip("The camera stays this high above the ground while flying")]
	public float MinimumHeight = 0.4f;

	[Tooltip("The highest the camera flies, in meters")]
	public float MaximumHeight = 1000.0f;

	[Tooltip("Meters from the world's origin, horizontally, within which the camera flies (0: anywhere)")]
	[Range(0.0f, 10000.0f)]
	public float FlightRadius;

	[Tooltip("Balls kept at once: throwing more removes the oldest")]
	[Range(1, 200)]
	public int MaximumBalls = 40;

	private readonly List<Entity> m_Balls = [];
	private float m_Yaw;
	private float m_Pitch;
	private bool m_Controllable = true;
	private bool m_Walking;
	private bool m_Captured;
	private float m_Drop = 1.0f;
	private Vector3 m_DropFrom;
	private float m_BaseFov;
	private float m_Fov;

	/// <summary>Balls thrown so far.</summary>
	public int Thrown { get; private set; }

	/// <summary>Degrees the view is turned around the vertical axis (0 looks along -Z).</summary>
	public float Yaw => m_Yaw;

	/// <summary>Degrees the view looks up (negative: down).</summary>
	public float Pitch => m_Pitch;

	/// <summary>Whether the rig walks (on the walker's feet) rather than flies.</summary>
	public bool IsWalking => m_Walking;

	/// <summary>Whether the mouse looks around while walking (Esc frees the cursor, a click into the scene takes it back).</summary>
	public bool IsCursorCaptured => m_Walking && m_Captured;

	/// <summary>Times the rig switched between flying and walking.</summary>
	public int ModeSwitches { get; private set; }

	/// <summary>Whether mouse and keys move the rig (the tour turns this off while it plays, and the rig flies again).</summary>
	public bool Controllable
	{
		get => m_Controllable;
		set
		{
			m_Controllable = value;
			if (!value)
			{
				SetWalking(false);
				Input.CursorMode = CursorMode.Normal;
			}
		}
	}

	/// <summary>Whether the mouse turns the view this frame.</summary>
	public bool IsLooking => m_Controllable && (IsCursorCaptured || Input.IsMouseButtonDown(MouseButton.Right));

	protected override void OnCreate()
	{
		// The angles of the view as authored, from where it looks: Euler angles read back from a rotation can come with a
		// half turn of roll, which the rig, setting pitch and yaw alone, would turn upside down.
		Vector3 forward = Transform.Forward;
		m_Yaw = Mathf.Atan2(-forward.X, -forward.Z) * Mathf.Rad2Deg;
		m_Pitch = Mathf.Asin(Mathf.Clamp(forward.Y, -1.0f, 1.0f)) * Mathf.Rad2Deg;
		m_BaseFov = View?.GetComponent<CameraComponent>()?.PerspectiveFOV ?? 55.0f;
		m_Fov = m_BaseFov;
	}

	protected override void OnUpdate(float deltaTime)
	{
		if (m_Controllable && Input.IsKeyPressed(ModeKey))
		{
			SetWalking(!m_Walking);
		}
		if (m_Controllable && Input.IsKeyPressed(ReleaseKey))
		{
			ReleaseCursor();
		}
		// A click into the scene while walking takes the mouse back; it throws nothing.
		bool clicked = m_Controllable && Input.IsMouseButtonPressed(MouseButton.Left) && !Input.IsPointerOverUI;
		bool captured = false;
		if (clicked && m_Walking && !m_Captured)
		{
			m_Captured = true;
			captured = true;
		}

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
			m_Pitch = Mathf.Clamp(m_Pitch - (delta.Y * LookSensitivity), -85.0f, 85.0f);
			m_Yaw -= delta.X * LookSensitivity;
		}

		bool fast = Input.IsKeyDown(KeyCode.LeftShift);
		bool moving;
		if (m_Walking && Walker?.As<Walker>() is { } walker)
		{
			walker.Yaw = m_Yaw;
			walker.MoveInput = new Vector2(Axis(KeyCode.D, KeyCode.A), Axis(KeyCode.W, KeyCode.S));
			walker.Running = fast;
			if (Input.IsKeyPressed(KeyCode.Space))
			{
				walker.Jump();
			}
			moving = walker.WalkVelocity.Length > walker.WalkSpeed + 0.5f;
			// The view sits at the walker's eyes, after dropping there from where the rig flew.
			m_Drop = Mathf.Min(1.0f, m_Drop + (deltaTime / EyeDropSeconds));
			Vector3 eyes = walker.EyePosition;
			Translation = m_Drop < 1.0f ? Vector3.Lerp(m_DropFrom, eyes, Mathf.SmoothStep(0.0f, 1.0f, m_Drop)) : eyes;
			Transform.EulerAngles = new Vector3(m_Pitch, m_Yaw, 0.0f);
		}
		else
		{
			Transform.EulerAngles = new Vector3(m_Pitch, m_Yaw, 0.0f);
			Vector3 move = Vector3.Zero;
			move += Axis(KeyCode.W, KeyCode.S) * Transform.Forward;
			move += Axis(KeyCode.D, KeyCode.A) * Transform.Right;
			move += Axis(KeyCode.E, KeyCode.Q) * Vector3.Up;
			moving = fast && move.LengthSquared > 0.0f;
			if (move.LengthSquared > 0.0f)
			{
				Fly(move.Normalized * (MoveSpeed * (fast ? SprintFactor : 1.0f)), deltaTime);
			}
		}
		KickFieldOfView(moving ? SpeedFovKick : 0.0f, deltaTime);

		if (clicked && !captured)
		{
			Throw(AimDirection);
		}
	}

	/// <summary>Switches between walking and flying; returns whether the rig now moves the way asked. Walking starts on what is
	/// under the camera, the view dropping to the walker's eyes, with the mouse looking around; with nothing under the camera
	/// (beyond the ground) the rig flies on. Flying goes on from the eyes.</summary>
	public bool SetWalking(bool walking)
	{
		if (walking == m_Walking)
		{
			return true;
		}
		Walker? walker = Walker?.As<Walker>();
		if (walking)
		{
			Vector3 from = Transform.WorldTranslation;
			if (walker is null || !Physics.Raycast(from, Vector3.Down, 1000.0f, out RaycastHit ground))
			{
				return false;
			}
			walker.Enter(ground.Point + new Vector3(0.0f, 0.02f, 0.0f), m_Yaw);
			m_DropFrom = from;
			m_Drop = 0.0f;
			m_Captured = true;
		}
		else
		{
			walker?.Park();
			m_Captured = false;
			m_Drop = 1.0f;
			Input.CursorMode = CursorMode.Normal;
		}
		m_Walking = walking;
		ModeSwitches++;
		return true;
	}

	/// <summary>Frees the cursor while walking (Esc does): the mouse stops looking around and can click the HUD; a click into the
	/// scene takes it back.</summary>
	public void ReleaseCursor()
	{
		m_Captured = false;
	}

	/// <summary>Moves the flying rig at a velocity (meters per second, world space) for the seconds, within where it flies: between
	/// <see cref="MinimumHeight"/> and <see cref="MaximumHeight"/> and, horizontally, within <see cref="FlightRadius"/> of the
	/// world's origin, along whose edge it slides. Put farther out (the tour may), it can only come nearer.</summary>
	public void Fly(Vector3 velocity, float seconds)
	{
		Vector3 from = Translation;
		Vector3 position = from + (velocity * seconds);
		position.Y = Mathf.Clamp(position.Y, MinimumHeight, Mathf.Max(MaximumHeight, from.Y));
		if (FlightRadius > 0.0f)
		{
			Vector2 flat = new(position.X, position.Z);
			float limit = Mathf.Max(FlightRadius, new Vector2(from.X, from.Z).Length);
			if (flat.Length > limit)
			{
				flat = flat.Normalized * limit;
				position = new Vector3(flat.X, position.Y, flat.Y);
			}
		}
		Translation = position;
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

	// The field of view widens a little at speed and settles back, over about a third of a second.
	private void KickFieldOfView(float kick, float deltaTime)
	{
		float target = m_BaseFov + kick;
		if (m_Fov == target || View?.GetComponent<CameraComponent>() is not { } camera)
		{
			return;
		}
		m_Fov = Mathf.Abs(target - m_Fov) < 0.01f ? target : Mathf.Lerp(m_Fov, target, 1.0f - Mathf.Exp(-deltaTime * 6.0f));
		camera.PerspectiveFOV = m_Fov;
	}

	private static float Axis(KeyCode positive, KeyCode negative) =>
		(Input.IsKeyDown(positive) ? 1.0f : 0.0f) - (Input.IsKeyDown(negative) ? 1.0f : 0.0f);
}
