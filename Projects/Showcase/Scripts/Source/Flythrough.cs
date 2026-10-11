using System;
using System.Collections.Generic;
using Strada;

namespace Showcase;

/// <summary>The opening tour: the screen fades in from black and the camera glides through the shots (the children, each a
/// CameraShot) on a smooth path that eases in and out, shows the title and the shots' captions, cues their exhibits, and
/// then hands the rig over to the free camera. Any key or click skips it (the camera glides to the last shot in a moment);
/// its key plays it again.</summary>
public sealed class Flythrough : Script
{
	[Tooltip("The camera rig the tour moves (a FlyCamera)")]
	public Entity? Rig;

	[Tooltip("Plays the tour when the scene starts")]
	public bool PlayOnStart = true;

	[Tooltip("The key that plays the tour again")]
	public KeyCode Key = KeyCode.C;

	[Tooltip("A screen-space sprite covering the screen, faded out at the start of the tour")]
	public Entity? Fade;

	[Tooltip("Seconds the screen takes to fade in from black")]
	[Range(0.0f, 10.0f)]
	public float FadeInSeconds = 1.5f;

	[Tooltip("Screen text shown large at the start of the tour, with its children (a subtitle, shadows)")]
	public Entity? Title;

	[Tooltip("Seconds into the tour the title appears")]
	[Range(0.0f, 60.0f)]
	public float TitleFrom = 1.0f;

	[Tooltip("Seconds into the tour the title is gone (captions wait for it)")]
	[Range(0.0f, 60.0f)]
	public float TitleUntil = 5.0f;

	[Tooltip("Screen text that shows the caption of the shot the camera is at, shown with its children (the detail line, a band)")]
	public Entity? Caption;

	[Tooltip("Screen text that shows the detail line of the shot")]
	public Entity? Detail;

	[Tooltip("Screen text that says how to skip the tour")]
	public Entity? Hint;

	[Tooltip("The parent of the screen texts and sprites shown while exploring; hidden during the tour")]
	public Entity? Hud;

	[Tooltip("Seconds the camera takes to get up to speed at the start and to come to rest at the end")]
	[Range(0.0f, 5.0f)]
	public float RampSeconds = 1.6f;

	[Tooltip("Seconds the camera takes to glide to the last shot when the tour is skipped")]
	[Range(0.1f, 5.0f)]
	public float SkipSeconds = 1.2f;

	[Tooltip("Seconds texts take to fade in or out")]
	[Range(0.05f, 3.0f)]
	public float FadeSeconds = 0.6f;

	private static readonly KeyCode[] s_Keys = Enum.GetValues<KeyCode>();

	private readonly Dictionary<ulong, Color> m_TextColors = [];
	private readonly Dictionary<ulong, Color> m_SpriteColors = [];
	private CameraShot?[] m_Shots = [];
	private Vector3[] m_Positions = [];
	private Vector3[] m_Angles = [];
	private float[] m_Knots = [];
	private float m_Elapsed;
	private bool[] m_Cued = [];
	private bool m_Skipping;
	private float m_SkipElapsed;
	private Vector3 m_SkipPosition;
	private Vector3 m_SkipAngles;
	private float m_HudFade = 1.0f;

	/// <summary>Whether the tour is playing.</summary>
	public bool IsPlaying { get; private set; }

	/// <summary>Whether the tour is gliding to its end after a skip.</summary>
	public bool IsSkipping => IsPlaying && m_Skipping;

	/// <summary>Seconds the tour lasts when it is not skipped.</summary>
	public float Duration => m_Knots.Length > 0 ? m_Knots[^1] + RampSeconds : 0.0f;

	/// <summary>Seconds since the tour started.</summary>
	public float Elapsed => m_Elapsed;

	/// <summary>The stops of the tour, in order.</summary>
	public int ShotCount => m_Positions.Length;

	/// <summary>Times the tour handed the camera over to the free camera.</summary>
	public int HandOvers { get; private set; }

	protected override void OnCreate()
	{
		RememberColors(Fade);
		RememberColors(Title);
		RememberColors(Caption);
		RememberColors(Detail);
		RememberColors(Hint);
		RememberColors(Hud);
		SetAlpha(Fade, 0.0f);
		FadeTexts(0.0f, 0.0f, 0.0f);
		if (PlayOnStart)
		{
			Play();
		}
	}

	/// <summary>Starts the tour from its first shot (it needs at least two).</summary>
	public void Play()
	{
		FlyCamera? rig = Rig?.As<FlyCamera>();
		if (rig is null || !GatherShots())
		{
			Log.Warn("Flythrough: the tour needs a FlyCamera rig and at least two shots");
			return;
		}
		IsPlaying = true;
		m_Skipping = false;
		m_Elapsed = 0.0f;
		m_Cued = new bool[m_Knots.Length];
		rig.Controllable = false;
		SetAlpha(Hud, 0.0f);
		m_HudFade = 0.0f;
		SetAlpha(Fade, FadeInSeconds > 0.0f ? 1.0f : 0.0f);
		Apply(rig, m_Positions[0], m_Angles[0]);
	}

	/// <summary>Ends the tour early: the camera glides to the last shot and is handed over.</summary>
	public void Skip()
	{
		if (!IsPlaying || m_Skipping || Rig?.As<FlyCamera>() is not { } rig)
		{
			return;
		}
		m_Skipping = true;
		m_SkipElapsed = 0.0f;
		m_SkipPosition = rig.Translation;
		m_SkipAngles = new Vector3(rig.Pitch, rig.Yaw, 0.0f);
	}

	protected override void OnUpdate(float deltaTime)
	{
		if (!IsPlaying)
		{
			if (m_HudFade < 1.0f)
			{
				m_HudFade = Mathf.Min(1.0f, m_HudFade + (deltaTime / FadeSeconds));
				SetAlpha(Hud, m_HudFade);
			}
			if (Input.IsKeyPressed(Key))
			{
				Play();
			}
			return;
		}
		FlyCamera? rig = Rig?.As<FlyCamera>();
		if (rig is null)
		{
			IsPlaying = false;
			return;
		}
		m_Elapsed += deltaTime;
		SetAlpha(Fade, FadeInSeconds > 0.0f ? 1.0f - Mathf.SmoothStep(0.0f, 1.0f, m_Elapsed / FadeInSeconds) : 0.0f);
		// The press that started the tour does not skip it.
		if (!m_Skipping && m_Elapsed > 0.25f && AnyInput())
		{
			Skip();
		}

		if (m_Skipping)
		{
			m_SkipElapsed += deltaTime;
			float progress = Mathf.SmoothStep(0.0f, 1.0f, m_SkipElapsed / SkipSeconds);
			Vector3 angles = m_Angles[^1];
			angles.Y = m_SkipAngles.Y + Mathf.DeltaAngle(m_SkipAngles.Y, angles.Y);
			Apply(rig, Vector3.Lerp(m_SkipPosition, m_Positions[^1], progress), Vector3.Lerp(m_SkipAngles, angles, progress));
			float fade = 1.0f - Mathf.Clamp01(m_SkipElapsed / FadeSeconds);
			float skippedTitle = TitleAlpha();
			FadeTexts(Mathf.Min(fade, skippedTitle), Mathf.Min(fade, UpdateCaption(TourTime()) * (1.0f - skippedTitle)), fade);
			if (m_SkipElapsed >= SkipSeconds)
			{
				Finish(rig);
			}
			return;
		}

		float time = TourTime();
		int segment = Segment(time);
		Apply(rig, Spline(m_Positions, segment, time), Spline(m_Angles, segment, time));
		// Exhibits go off as the camera nears their shots, each its shot's lead time ahead.
		for (int shot = 0; shot < m_Knots.Length; shot++)
		{
			if (!m_Cued[shot] && m_Knots[shot] - (m_Shots[shot]?.CueLead ?? 0.0f) <= time)
			{
				m_Cued[shot] = true;
				if (m_Shots[shot]?.Cue?.As<Script>() is ITourCue cue)
				{
					cue.Perform();
				}
			}
		}
		// Captions give way to the title while it shows.
		float title = TitleAlpha();
		FadeTexts(title, UpdateCaption(time) * (1.0f - title), Mathf.Clamp01((m_Elapsed - 1.0f) / FadeSeconds) * 0.7f);
		if (m_Elapsed >= Duration)
		{
			Finish(rig);
		}
	}

	private bool GatherShots()
	{
		Entity[] children = Children;
		if (children.Length < 2)
		{
			return false;
		}
		m_Shots = new CameraShot?[children.Length];
		m_Positions = new Vector3[children.Length];
		m_Angles = new Vector3[children.Length];
		m_Knots = new float[children.Length];
		float time = 0.0f;
		for (int i = 0; i < children.Length; i++)
		{
			CameraShot? shot = children[i].As<CameraShot>();
			m_Shots[i] = shot;
			m_Positions[i] = children[i].Transform.WorldTranslation;
			Vector3 forward = children[i].Transform.Forward;
			float yaw = Mathf.Atan2(-forward.X, -forward.Z) * Mathf.Rad2Deg;
			float pitch = Mathf.Asin(Mathf.Clamp(forward.Y, -1.0f, 1.0f)) * Mathf.Rad2Deg;
			// Yaw takes the short way round from one shot to the next.
			if (i > 0)
			{
				yaw = m_Angles[i - 1].Y + Mathf.DeltaAngle(m_Angles[i - 1].Y, yaw);
			}
			m_Angles[i] = new Vector3(pitch, yaw, 0.0f);
			m_Knots[i] = time;
			time += shot?.Seconds ?? 4.0f;
		}
		return true;
	}

	private void Finish(FlyCamera rig)
	{
		IsPlaying = false;
		SetAlpha(Fade, 0.0f);
		m_Skipping = false;
		rig.SetPose(m_Positions[^1], m_Angles[^1].X, m_Angles[^1].Y);
		rig.Controllable = true;
		FadeTexts(0.0f, 0.0f, 0.0f);
		HandOvers++;
	}

	private static void Apply(FlyCamera rig, Vector3 position, Vector3 angles)
	{
		rig.SetPose(position, angles.X, angles.Y);
	}

	// The tour's own time: it starts and ends at rest, gaining and losing speed over RampSeconds.
	private float TourTime()
	{
		float total = m_Knots[^1];
		float ramp = Mathf.Min(RampSeconds, total);
		if (ramp <= 0.0f)
		{
			return Mathf.Min(m_Elapsed, total);
		}
		float elapsed = Mathf.Min(m_Elapsed, total + ramp);
		if (elapsed < ramp)
		{
			return elapsed * elapsed / (2.0f * ramp);
		}
		if (elapsed > total)
		{
			float left = total + ramp - elapsed;
			return total - (left * left / (2.0f * ramp));
		}
		return elapsed - (ramp / 2.0f);
	}

	private int Segment(float time)
	{
		int segment = 0;
		while (segment < m_Knots.Length - 2 && time > m_Knots[segment + 1])
		{
			segment++;
		}
		return segment;
	}

	// Barry and Goldman's pyramid: the Catmull-Rom spline through the points at the knot times, which stays smooth where
	// segments take different times. The ends continue straight on.
	private Vector3 Spline(Vector3[] points, int segment, float time)
	{
		Vector3 p1 = points[segment];
		Vector3 p2 = points[segment + 1];
		float t1 = m_Knots[segment];
		float t2 = m_Knots[segment + 1];
		Vector3 p0 = segment > 0 ? points[segment - 1] : (p1 * 2.0f) - p2;
		float t0 = segment > 0 ? m_Knots[segment - 1] : t1 - (t2 - t1);
		Vector3 p3 = segment + 2 < points.Length ? points[segment + 2] : (p2 * 2.0f) - p1;
		float t3 = segment + 2 < points.Length ? m_Knots[segment + 2] : t2 + (t2 - t1);
		Vector3 a1 = Mix(p0, p1, t0, t1, time);
		Vector3 a2 = Mix(p1, p2, t1, t2, time);
		Vector3 a3 = Mix(p2, p3, t2, t3, time);
		Vector3 b1 = Mix(a1, a2, t0, t2, time);
		Vector3 b2 = Mix(a2, a3, t1, t3, time);
		return Mix(b1, b2, t1, t2, time);
	}

	private static Vector3 Mix(Vector3 a, Vector3 b, float timeA, float timeB, float time) =>
		a + ((b - a) * ((time - timeA) / (timeB - timeA)));

	private float TitleAlpha() =>
		Mathf.Clamp01((m_Elapsed - TitleFrom) / FadeSeconds) * Mathf.Clamp01((TitleUntil - m_Elapsed) / FadeSeconds);

	// Shows the caption of the shot nearest in time and returns how visible it is: it fades in and out at the edges of the
	// shot's share of the tour.
	private float UpdateCaption(float time)
	{
		int nearest = NearestShot(time);
		float start = nearest > 0 ? (m_Knots[nearest - 1] + m_Knots[nearest]) / 2.0f : 0.0f;
		float end = nearest < m_Knots.Length - 1 ? (m_Knots[nearest] + m_Knots[nearest + 1]) / 2.0f : m_Knots[^1] + RampSeconds;
		ShowCaption(m_Shots[nearest]);
		if (string.IsNullOrEmpty(m_Shots[nearest]?.Caption))
		{
			return 0.0f;
		}
		return Mathf.Clamp01((time - start) / FadeSeconds) * Mathf.Clamp01((end - time) / FadeSeconds);
	}

	private int NearestShot(float time)
	{
		int nearest = 0;
		for (int i = 1; i < m_Knots.Length; i++)
		{
			if (Mathf.Abs(m_Knots[i] - time) < Mathf.Abs(m_Knots[nearest] - time))
			{
				nearest = i;
			}
		}
		return nearest;
	}

	private void ShowCaption(CameraShot? shot)
	{
		if (Caption?.GetComponent<TextComponent>() is { } caption && caption.Text != (shot?.Caption ?? ""))
		{
			caption.Text = shot?.Caption ?? "";
		}
		if (Detail?.GetComponent<TextComponent>() is { } detail && detail.Text != (shot?.Detail ?? ""))
		{
			detail.Text = shot?.Detail ?? "";
		}
	}

	private void FadeTexts(float title, float caption, float hint)
	{
		SetAlpha(Title, title);
		SetAlpha(Caption, caption);
		SetAlpha(Hint, hint);
	}

	private static bool AnyInput()
	{
		foreach (KeyCode key in s_Keys)
		{
			if (key != KeyCode.Unknown && Input.IsKeyPressed(key))
			{
				return true;
			}
		}
		return Input.IsMouseButtonPressed(MouseButton.Left) || Input.IsMouseButtonPressed(MouseButton.Right) ||
			Input.IsMouseButtonPressed(MouseButton.Middle);
	}

	// The colors the entity's and its descendants' texts and sprites have when the scene starts: fading scales their alpha.
	private void RememberColors(Entity? entity)
	{
		if (entity is null)
		{
			return;
		}
		if (entity.GetComponent<TextComponent>() is { } text)
		{
			m_TextColors[entity.ID] = text.Color;
		}
		if (entity.GetComponent<SpriteRendererComponent>() is { } sprite)
		{
			m_SpriteColors[entity.ID] = sprite.Color;
		}
		foreach (Entity child in entity.Children)
		{
			RememberColors(child);
		}
	}

	// Scales the alpha of the entity's and its descendants' texts and sprites (as it was when the scene started) by the fraction.
	private void SetAlpha(Entity? entity, float alpha)
	{
		if (entity is null)
		{
			return;
		}
		if (m_TextColors.TryGetValue(entity.ID, out Color textColor) && entity.GetComponent<TextComponent>() is { } text)
		{
			text.Color = new Color(textColor.R, textColor.G, textColor.B, textColor.A * alpha);
		}
		if (m_SpriteColors.TryGetValue(entity.ID, out Color spriteColor) && entity.GetComponent<SpriteRendererComponent>() is { } sprite)
		{
			sprite.Color = new Color(spriteColor.R, spriteColor.G, spriteColor.B, spriteColor.A * alpha);
		}
		foreach (Entity child in entity.Children)
		{
			SetAlpha(child, alpha);
		}
	}
}
