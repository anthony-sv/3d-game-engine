using Strada;

namespace Showcase;

/// <summary>A neon sign whose tubes stutter on when it is switched on (the tour's first cue, a moment after the screen starts
/// fading in): the frame flickers on first, then the letters, each by a timeline of its own, while the ignition's buzz plays
/// (the entity's sound) and the glow it casts (a point light) follows the letters. A quiet hum goes with the lit tubes. Until
/// it is switched on, the sign shines as authored.</summary>
public sealed class NeonSign : Script, ITourCue
{
	[Tooltip("The letters' tubes: the glow of their material flickers")]
	public Entity? Letters;

	[Tooltip("The frame's tubes")]
	public Entity? Frame;

	[Tooltip("The point light the letters cast on the wall: it follows them")]
	public Entity? Glow;

	[Tooltip("An audio source with the tubes' steady hum (looping): it is as loud as the tubes are lit")]
	public Entity? Hum;

	[Tooltip("Seconds the sign stays dark when the tour switches it on, before its tubes flicker")]
	[Range(0.0f, 10.0f)]
	public float CueDelay = 1.0f;

	// When the tubes are lit after the sign is switched on: (start, end, level) in seconds; the last flicker of each holds. The
	// ignition's sound (GenerateAudio.py's NEON_FRAME and NEON_LETTERS) buzzes by the same timelines.
	private static readonly (float Start, float End, float Level)[] s_Frame =
		[(0.18f, 0.24f, 0.8f), (0.36f, 0.4f, 0.5f), (0.55f, float.PositiveInfinity, 1.0f)];

	private static readonly (float Start, float End, float Level)[] s_Letters =
	[
		(0.78f, 0.86f, 0.7f), (0.98f, 1.02f, 0.45f), (1.12f, 1.24f, 0.9f), (1.33f, 1.36f, 0.5f),
		(1.48f, float.PositiveInfinity, 1.0f)
	];

	private Material? m_Letters;
	private Material? m_Frame;
	private float m_LettersIntensity;
	private float m_FrameIntensity;
	private float m_GlowIntensity;
	private float m_HumVolume;
	private float m_Time;
	private bool m_Igniting;
	private bool m_Buzzed;

	/// <summary>How brightly the letters glow, from 0 (off) to 1 (as authored).</summary>
	public float LettersLevel { get; private set; } = 1.0f;

	/// <summary>How brightly the frame glows, from 0 (off) to 1 (as authored).</summary>
	public float FrameLevel { get; private set; } = 1.0f;

	/// <summary>Whether the sign is dark or its tubes are flickering on.</summary>
	public bool IsIgniting => m_Igniting;

	/// <summary>Times the sign was switched on.</summary>
	public int Ignitions { get; private set; }

	/// <summary>Seconds from switching the sign on until both tubes hold.</summary>
	public static float IgnitionSeconds => Mathf.Max(s_Frame[^1].Start, s_Letters[^1].Start);

	protected override void OnCreate()
	{
		m_Letters = OwnMaterial(Letters, out m_LettersIntensity);
		m_Frame = OwnMaterial(Frame, out m_FrameIntensity);
		m_GlowIntensity = Glow?.GetComponent<PointLightComponent>()?.Intensity ?? 0.0f;
		m_HumVolume = Hum?.GetComponent<AudioSourceComponent>()?.Volume ?? 0.0f;
	}

	/// <summary>Switches the sign off and lets its tubes flicker on again after the delay (seconds).</summary>
	public void Ignite(float delay = 0.0f)
	{
		m_Igniting = true;
		m_Buzzed = false;
		m_Time = -Mathf.Max(0.0f, delay);
		Ignitions++;
		Show(0.0f, 0.0f);
	}

	/// <inheritdoc/>
	public void Perform() => Ignite(CueDelay);

	protected override void OnUpdate(float deltaTime)
	{
		if (!m_Igniting)
		{
			return;
		}
		m_Time += deltaTime;
		if (!m_Buzzed && m_Time >= 0.0f)
		{
			m_Buzzed = true;
			if (GetComponent<AudioSourceComponent>() is { } buzz)
			{
				buzz.Stop();
				buzz.Play();
			}
		}
		if (m_Time >= IgnitionSeconds)
		{
			m_Igniting = false;
			Show(1.0f, 1.0f);
			return;
		}
		Show(LevelAt(s_Frame, m_Time), LevelAt(s_Letters, m_Time));
	}

	private void Show(float frame, float letters)
	{
		FrameLevel = frame;
		LettersLevel = letters;
		if (m_Frame is not null)
		{
			m_Frame.EmissiveIntensity = m_FrameIntensity * frame;
		}
		if (m_Letters is not null)
		{
			m_Letters.EmissiveIntensity = m_LettersIntensity * letters;
		}
		if (Glow?.GetComponent<PointLightComponent>() is { } glow)
		{
			glow.Intensity = m_GlowIntensity * letters;
		}
		if (Hum?.GetComponent<AudioSourceComponent>() is { } hum)
		{
			hum.Volume = m_HumVolume * Mathf.Max(frame, letters);
		}
	}

	private static float LevelAt((float Start, float End, float Level)[] timeline, float time)
	{
		foreach ((float start, float end, float level) in timeline)
		{
			if (time >= start && time < end)
			{
				return level;
			}
		}
		return 0.0f;
	}

	// The tubes get a copy of their material, whose glow the flicker changes.
	private static Material? OwnMaterial(Entity? tubes, out float intensity)
	{
		intensity = 0.0f;
		if (tubes?.GetComponent<MeshComponent>() is not { } mesh || mesh.GetMaterial(0)?.Clone() is not { } material)
		{
			return null;
		}
		mesh.SetMaterial(0, material);
		intensity = material.EmissiveIntensity;
		return material;
	}
}
