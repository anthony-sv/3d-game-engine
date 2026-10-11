using Strada;

namespace Blocks;

/// <summary>Moves the camera around its place. <see cref="Shake"/> adds trauma, which decays: the camera wanders on smooth
/// noise and rolls a little, by amounts that grow with the trauma squared, so small bumps stay subtle and big ones jolt.
/// <see cref="Kick"/> jolts the view in a direction, from which a spring brings it back with a small bounce. While
/// <see cref="Sway"/> is on (the title screen), the camera slowly swings around a point in front of it.</summary>
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

	[Tooltip("The point the camera swings around while it sways")]
	public Vector3 SwayPivot = Vector3.Zero;

	[Tooltip("Degrees the camera swings to each side while it sways")]
	[Range(0.0f, 30.0f)]
	public float SwayDegrees = 4.0f;

	[Tooltip("Seconds of a whole swing")]
	[Range(1.0f, 120.0f)]
	public float SwayPeriod = 16.0f;

	// Seconds the sway takes to start or to settle back.
	private const float SwayEaseSeconds = 1.2f;

	private Vector3 m_Rest;
	private Quaternion m_RestRotation;
	private Vector3 m_Kick;
	private Vector3 m_KickVelocity;
	private float m_Time;
	private float m_Seed;
	private float m_SwayWeight;
	private float m_SwayTime;

	/// <summary>How shaken the camera is, from 0 to 1.</summary>
	public float Trauma { get; private set; }

	/// <summary>Whether the camera slowly swings around <see cref="SwayPivot"/>; it eases in, and back to its place when turned
	/// off.</summary>
	public bool Sway { get; set; }

	/// <summary>How far a kick moved the view from its place (the scene's apparent motion).</summary>
	public Vector3 KickOffset => -m_Kick;

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

	protected override void OnCreate()
	{
		m_Rest = Translation;
		m_RestRotation = Rotation;
		m_Seed = Random.Range(0.0f, 1000.0f);
	}

	protected override void OnUpdate(float deltaTime)
	{
		m_Time += deltaTime;
		Trauma = Mathf.Max(0.0f, Trauma - (Recovery * deltaTime));

		// A damped spring pulls the kick back (semi-implicit Euler, stable at any usual frame rate).
		Vector3 acceleration = (m_Kick * -(KickSpring * KickSpring)) - (m_KickVelocity * (2.0f * KickDamping * KickSpring));
		m_KickVelocity += acceleration * deltaTime;
		m_Kick += m_KickVelocity * deltaTime;

		// The sway starts its swing from the camera's place, so it eases in without a jump.
		m_SwayWeight = Mathf.MoveTowards(m_SwayWeight, Sway ? 1.0f : 0.0f, deltaTime / SwayEaseSeconds);
		m_SwayTime = m_SwayWeight > 0.0f ? m_SwayTime + deltaTime : 0.0f;
		float swing = SwayDegrees * Mathf.Sin(m_SwayTime * 2.0f * Mathf.PI / SwayPeriod) * Mathf.SmoothStep(0.0f, 1.0f, m_SwayWeight);
		Quaternion orbit = Quaternion.AngleAxis(swing, Vector3.Up);
		Vector3 place = SwayPivot + (orbit * (m_Rest - SwayPivot));

		float shake = Trauma * Trauma;
		float time = (m_Time * Frequency) + m_Seed;
		Vector3 wander = new(Noise(time), Noise(time + 71.3f), 0.0f);
		Translation = place + m_Kick + (wander * (shake * MaximumOffset));
		float roll = Noise((time * 0.7f) + 143.9f) * shake * MaximumRoll;
		Rotation = orbit * m_RestRotation * Quaternion.AngleAxis(roll, Vector3.Back);
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
