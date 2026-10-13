using System.Collections.Generic;
using Strada;

namespace Showcase;

/// <summary>Day and night: its key (or the HUD's day and night button) turns the golden afternoon into a moonlit night and
/// back over a few seconds. The sun dims to moonlight and the sky darkens; a global post-process volume grades the night
/// (cooler, with the exposure of eyes used to the dark), so the lamps, the fire, the neon and the lighthouse carry the scene.
/// The evening zone around the light show fades out (the night needs no evening of its own), and the birds give way to
/// crickets. Particles are not lit, so it also dims those of unlit matter (smoke, water, leaves), which would otherwise glow in
/// the dark.</summary>
public sealed class DayNight : Script
{
	[Tooltip("The sun (a directional light)")]
	public Entity? Sun;

	[Tooltip("The sky (a sky light)")]
	public Entity? Sky;

	[Tooltip("A global post-process volume with the look of the night: its weight follows the night")]
	public Entity? NightGrade;

	[Tooltip("A post-process volume of the day that the night fades out")]
	public Entity? EveningZone;

	[Tooltip("The day's ambience (an audio source), which fades out at night")]
	public Entity? DayAmbience;

	[Tooltip("The night's ambience (an audio source playing from the start), which fades in at night")]
	public Entity? NightAmbience;

	[Tooltip("Smoke to dim at night: this entity's particle system and those of its children")]
	public Entity? Smoke;

	[Tooltip("Water to dim at night: this entity's particle system and those of its children")]
	public Entity? Water;

	[Tooltip("Leaves to dim at night: this entity's particle system and those of its children")]
	public Entity? Leaves;

	[Tooltip("The key that switches between day and night")]
	public KeyCode Key = KeyCode.L;

	[Tooltip("Seconds the change takes")]
	[Range(0.1f, 20.0f)]
	public float Seconds = 3.0f;

	[Tooltip("The moonlight's color (the sun's at night)")]
	public Color MoonColor = new(0.55f, 0.67f, 1.0f);

	[Tooltip("The moonlight's intensity")]
	[Range(0.0f, 10.0f)]
	public float MoonIntensity = 0.35f;

	[Tooltip("The sky light's intensity at night")]
	[Range(0.0f, 5.0f)]
	public float NightSkyIntensity = 0.04f;

	[Tooltip("The night ambience's volume at night")]
	[Range(0.0f, 1.0f)]
	public float NightAmbienceVolume = 0.6f;

	[Tooltip("How bright the smoke, water and leaves stay at night, as a share of their brightness by day")]
	[Range(0.0f, 1.0f)]
	public float NightMatterBrightness = 0.2f;

	private Color m_SunColor = Color.White;
	private float m_SunIntensity;
	private float m_SkyIntensity;
	private float m_ZoneWeight;
	private float m_DayAmbienceVolume;
	private float m_Target;
	private readonly List<(Entity Entity, float Intensity)> m_Matter = [];

	/// <summary>Whether it is night or turning into night.</summary>
	public bool IsNight => m_Target > 0.5f;

	/// <summary>How far the change has come, from 0 (day) to 1 (night).</summary>
	public float Blend { get; private set; }

	/// <summary>Times it switched between day and night.</summary>
	public int Switches { get; private set; }

	/// <summary>The sun's intensity by day.</summary>
	public float DaySunIntensity => m_SunIntensity;

	/// <summary>The sky light's intensity by day.</summary>
	public float DaySkyIntensity => m_SkyIntensity;

	/// <summary>The evening zone's weight by day.</summary>
	public float DayZoneWeight => m_ZoneWeight;

	/// <summary>How bright unlit matter (smoke, water, leaves) is now, as a share of its brightness by day: 1 by day,
	/// <see cref="NightMatterBrightness"/> at night. Effects made later (a shot's smoke) are scaled by it too.</summary>
	public float MatterBrightness { get; private set; } = 1.0f;

	protected override void OnCreate()
	{
		if (Sun?.GetComponent<DirectionalLightComponent>() is { } sun)
		{
			m_SunColor = sun.Color;
			m_SunIntensity = sun.Intensity;
		}
		m_SkyIntensity = Sky?.GetComponent<SkyLightComponent>()?.Intensity ?? 0.0f;
		m_ZoneWeight = EveningZone?.GetComponent<PostProcessComponent>()?.Weight ?? 0.0f;
		m_DayAmbienceVolume = DayAmbience?.GetComponent<AudioSourceComponent>()?.Volume ?? 0.0f;
		CollectMatter(Smoke);
		CollectMatter(Water);
		CollectMatter(Leaves);
		Apply(0.0f);
	}

	/// <summary>Switches from day to night or back.</summary>
	public void Toggle() => SetNight(!IsNight);

	/// <summary>Turns to night (true) or day (false) over <see cref="Seconds"/>.</summary>
	public void SetNight(bool night)
	{
		if (night != IsNight)
		{
			m_Target = night ? 1.0f : 0.0f;
			Switches++;
		}
	}

	protected override void OnUpdate(float deltaTime)
	{
		if (Input.IsKeyPressed(Key))
		{
			Toggle();
		}
		if (Blend != m_Target)
		{
			Blend = Mathf.MoveTowards(Blend, m_Target, deltaTime / Seconds);
			Apply(Mathf.SmoothStep(0.0f, 1.0f, Blend));
		}
	}

	private void Apply(float night)
	{
		if (Sun?.GetComponent<DirectionalLightComponent>() is { } sun)
		{
			sun.Intensity = Fade(m_SunIntensity, MoonIntensity, night);
			sun.Color = Color.Lerp(m_SunColor, MoonColor, night);
		}
		if (Sky?.GetComponent<SkyLightComponent>() is { } sky)
		{
			sky.Intensity = Fade(m_SkyIntensity, NightSkyIntensity, night);
		}
		if (NightGrade?.GetComponent<PostProcessComponent>() is { } grade)
		{
			grade.Weight = night;
		}
		if (EveningZone?.GetComponent<PostProcessComponent>() is { } zone)
		{
			zone.Weight = m_ZoneWeight * (1.0f - night);
		}
		if (DayAmbience?.GetComponent<AudioSourceComponent>() is { } day)
		{
			day.Volume = m_DayAmbienceVolume * (1.0f - night);
		}
		if (NightAmbience?.GetComponent<AudioSourceComponent>() is { } nightAmbience)
		{
			nightAmbience.Volume = NightAmbienceVolume * night;
		}
		MatterBrightness = Fade(1.0f, NightMatterBrightness, night);
		foreach ((Entity entity, float intensity) in m_Matter)
		{
			if (entity.GetComponent<ParticleSystemComponent>() is { } particles)
			{
				particles.Intensity = intensity * MatterBrightness;
			}
		}
	}

	// The particle systems of an entity and of its descendants, with their brightness by day.
	private void CollectMatter(Entity? entity)
	{
		if (entity is null)
		{
			return;
		}
		if (entity.GetComponent<ParticleSystemComponent>() is { } particles)
		{
			m_Matter.Add((entity, particles.Intensity));
		}
		foreach (Entity child in entity.Children)
		{
			CollectMatter(child);
		}
	}

	// Light fades evenly to the eye when it changes by the same factor every moment.
	private static float Fade(float day, float night, float t) =>
		day > 0.0f && night > 0.0f ? day * Mathf.Pow(night / day, t) : Mathf.Lerp(day, night, t);
}
