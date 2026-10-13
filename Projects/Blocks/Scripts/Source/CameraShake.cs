using Strada;

namespace Blocks;

/// <summary>Moves the camera around its place. <see cref="Shake"/> adds trauma, which decays: the camera wanders on smooth
/// noise and rolls a little, by amounts that grow with the trauma squared, so small bumps stay subtle and big ones jolt.
/// <see cref="Kick"/> jolts the view in a direction, from which a spring brings it back with a small bounce. In
/// <see cref="Attract"/> mode (the title screen) the camera glides to <see cref="AttractView"/> and slowly swings around a
/// point in front of it; it glides back to its place when the mode ends. The view keeps its width on screens narrower than
/// it was composed for (<see cref="ComposedAspect"/>): its field of view widens, so the board and its panels keep their
/// places between the screen's panels.</summary>
public sealed class CameraShake : Script
{
	[Tooltip("Units the camera moves at full trauma")]
	[Range(0.0f, 2.0f)]
	public float MaximumOffset = 0.45f;

	[Tooltip("Degrees the camera rolls at full trauma")]
	[Range(0.0f, 10.0f)]
	public float MaximumRoll = 1.5f;

	[Tooltip("Trauma lost per second")]
	[Range(0.1f, 10.0f)]
	public float Recovery = 1.5f;

	[Tooltip("How fast the shake wanders, in noise cycles per second")]
	[Range(1.0f, 60.0f)]
	public float Frequency = 16.0f;

	[Tooltip("How fast a kicked view springs back (radians per second)")]
	[Range(1.0f, 60.0f)]
	public float KickSpring = 17.0f;

	[Tooltip("Damping ratio of the kick's spring: 1 returns without a bounce, less bounces more")]
	[Range(0.05f, 1.0f)]
	public float KickDamping = 0.55f;

	[Tooltip("Where the camera stands in attract mode: the position and rotation of an entity beside the camera (none keeps its place)")]
	public Entity? AttractView;

	[Tooltip("Seconds the camera takes to glide between its place and the attract view")]
	[Range(0.1f, 5.0f)]
	public float GlideSeconds = 1.4f;

	[Tooltip("The point the camera swings around in attract mode")]
	public Vector3 SwayPivot = Vector3.Zero;

	[Tooltip("Degrees the camera swings to each side in attract mode")]
	[Range(0.0f, 30.0f)]
	public float SwayDegrees = 4.0f;

	[Tooltip("Seconds of a whole swing")]
	[Range(1.0f, 120.0f)]
	public float SwayPeriod = 16.0f;

	[Tooltip("The narrowest screen (width / height) the view is made for: narrower ones get a wider field of view to show as much across")]
	[Range(0.5f, 4.0f)]
	public float ComposedAspect = 1.6f;

	// The spring moves in steps of at most this long. Semi-implicit Euler stays stable only while a step times the spring's
	// frequency stays small (below 1.2 for a damping ratio of 0.55, below 0.8 for 1): these steps keep it at 0.25 or less for
	// any spring the fields allow, however long a frame takes.
	private const float SpringStep = 1.0f / 240.0f;
	// The widest field of view a narrow screen gets (degrees), well inside what a camera takes.
	private const float WidestFieldOfView = 170.0f;

	private CameraComponent? m_Camera;
	// The camera's field of view on screens as wide as the composed one or wider: the one it starts with.
	private float m_FieldOfView;
	private Vector3 m_Rest;
	private Quaternion m_RestRotation;
	private Vector3 m_Kick;
	private Vector3 m_KickVelocity;
	private float m_Time;
	private float m_Seed;
	private float m_Glide;
	private float m_SwayTime;

	/// <summary>How shaken the camera is, from 0 to 1.</summary>
	public float Trauma { get; private set; }

	/// <summary>Whether the camera is in attract mode (the title screen): it glides to <see cref="AttractView"/> and slowly
	/// swings around <see cref="SwayPivot"/>; it glides back to its place when this turns off.</summary>
	public bool Attract { get; set; }

	/// <summary>How far the camera has glided from its place toward the attract view, from 0 to 1 (eased).</summary>
	public float AttractWeight => Mathf.SmoothStep(0.0f, 1.0f, m_Glide);

	/// <summary>How far a kick moved the view from its place (the scene's apparent motion).</summary>
	public Vector3 KickOffset => -m_Kick;

	/// <summary>Raised every frame once the camera stands where it shows the frame from: what is placed by where the camera
	/// shows things (labels over the board) is placed then, without trailing the camera by a frame, whatever order the scripts
	/// update in.</summary>
	public event System.Action? Moved;

	/// <summary>Adds trauma (0 to 1).</summary>
	public void Shake(float amount)
	{
		Trauma = Mathf.Clamp01(Trauma + amount);
	}

	/// <summary>Jolts the view: the scene jumps by the offset (in units) and springs back.</summary>
	public void Kick(Vector3 offset)
	{
		// The camera moves the other way for the scene to move this way.
		m_Kick -= offset;
	}

	/// <summary>Ends the glide between the camera's place and the attract view at once: from its next update the camera stands
	/// where <see cref="Attract"/> puts it, and its swing starts over (a game that starts on the title screen shows no
	/// glide).</summary>
	public void SkipGlide()
	{
		m_Glide = Attract ? 1.0f : 0.0f;
		m_SwayTime = 0.0f;
	}

	/// <summary>Fits the camera's field of view to the shape of the screen it shows now, or to its fixed aspect ratio when it
	/// has one (see <see cref="FitFieldOfView"/>). Runs on every update.</summary>
	public void FitView()
	{
		if (m_Camera is null)
		{
			return;
		}
		float aspect = m_Camera.FixedAspectRatio ? m_Camera.AspectRatio : (float)Application.WindowWidth / Application.WindowHeight;
		m_Camera.PerspectiveFOV = FitFieldOfView(m_FieldOfView, ComposedAspect, aspect);
	}

	/// <summary>The vertical field of view (degrees) that shows as much across on a screen of an aspect ratio (width over
	/// height) as fieldOfView does on a screen of the composed aspect ratio: fieldOfView itself on screens as wide or wider, a
	/// wider one on narrower screens (170 degrees at most). An aspect ratio that is not a positive number keeps
	/// fieldOfView.</summary>
	public static float FitFieldOfView(float fieldOfView, float composedAspect, float aspect)
	{
		if (!(aspect > 0.0f) || !float.IsFinite(aspect) || aspect >= composedAspect)
		{
			return fieldOfView;
		}
		float halfHeight = Mathf.Tan(0.5f * fieldOfView * Mathf.Deg2Rad) * composedAspect / aspect;
		return Mathf.Min(2.0f * Mathf.Atan(halfHeight) * Mathf.Rad2Deg, WidestFieldOfView);
	}

	/// <summary>Moves a damped spring that pulls an offset back to zero on by a time step, given its frequency (radians per
	/// second) and damping ratio. Long steps are split into short ones, so the spring behaves the same at any frame
	/// rate.</summary>
	public static void StepSpring(ref Vector3 offset, ref Vector3 velocity, float frequency, float damping, float deltaTime)
	{
		int steps = System.Math.Max(1, (int)Mathf.Ceil(deltaTime / SpringStep));
		float step = deltaTime / steps;
		for (int i = 0; i < steps; i++)
		{
			Vector3 acceleration = (offset * -(frequency * frequency)) - (velocity * (2.0f * damping * frequency));
			velocity += acceleration * step;
			offset += velocity * step;
		}
	}

	protected override void OnCreate()
	{
		m_Camera = GetComponent<CameraComponent>();
		m_FieldOfView = m_Camera?.PerspectiveFOV ?? 0.0f;
		m_Rest = Translation;
		m_RestRotation = Rotation;
		m_Seed = Random.Range(0.0f, 1000.0f);
	}

	protected override void OnUpdate(float deltaTime)
	{
		FitView();
		m_Time += deltaTime;
		Trauma = Mathf.Max(0.0f, Trauma - (Recovery * deltaTime));
		StepSpring(ref m_Kick, ref m_KickVelocity, KickSpring, KickDamping, deltaTime);

		// The glide between the camera's place and the attract view, eased at both ends.
		m_Glide = Mathf.MoveTowards(m_Glide, Attract ? 1.0f : 0.0f, deltaTime / GlideSeconds);
		float glide = AttractWeight;
		Vector3 place = m_Rest;
		Quaternion rotation = m_RestRotation;
		if (AttractView is not null)
		{
			place = Vector3.Lerp(m_Rest, AttractView.Translation, glide);
			rotation = Quaternion.Slerp(m_RestRotation, AttractView.Rotation, glide);
		}
		// The swing starts from where the camera is and grows with the glide, so it eases in without a jump.
		m_SwayTime = glide > 0.0f ? m_SwayTime + deltaTime : 0.0f;
		float swing = SwayDegrees * Mathf.Sin(m_SwayTime * 2.0f * Mathf.PI / SwayPeriod) * glide;
		Quaternion orbit = Quaternion.AngleAxis(swing, Vector3.Up);
		place = SwayPivot + (orbit * (place - SwayPivot));

		float shake = Trauma * Trauma;
		float time = (m_Time * Frequency) + m_Seed;
		Vector3 wander = new(Noise(time), Noise(time + 71.3f), 0.0f);
		Translation = place + m_Kick + (wander * (shake * MaximumOffset));
		float roll = Noise((time * 0.7f) + 143.9f) * shake * MaximumRoll;
		Rotation = orbit * rotation * Quaternion.AngleAxis(roll, Vector3.Back);
		Moved?.Invoke();
	}

	// Smooth noise from -1 to 1: Perlin's gradient noise in one dimension.
	private static float Noise(float x)
	{
		float cell = Mathf.Floor(x);
		float offset = x - cell;
		float fade = offset * offset * offset * ((offset * ((offset * 6.0f) - 15.0f)) + 10.0f);
		float start = Gradient((int)cell) * offset;
		float end = Gradient((int)cell + 1) * (offset - 1.0f);
		return 2.0f * Mathf.LerpUnclamped(start, end, fade);
	}

	// A gradient from -1 to 1 for each whole number (an integer hash).
	private static float Gradient(int cell)
	{
		uint hash = unchecked((uint)cell * 0x9E3779B1u);
		hash ^= hash >> 15;
		hash = unchecked(hash * 0x85EBCA77u);
		hash ^= hash >> 13;
		return ((hash & 0xFFFFu) / 32767.5f) - 1.0f;
	}
}
